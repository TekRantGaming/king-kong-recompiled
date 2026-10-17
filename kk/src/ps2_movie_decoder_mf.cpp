// The PS2 cutscene decoder on Windows: Media Foundation (unchanged from the Windows-only player; moved here
// from ps2_movies.cpp so that Linux can use the same player with FFmpeg, ps2_movie_decoder_ffmpeg.cpp).
#include "ps2_movie_decoder.h"
#include "ps2_movies_logic.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "ole32.lib")

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace kk::movies {
namespace {
using Microsoft::WRL::ComPtr;
constexpr DWORD kVideo = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAudio = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr int kRate = kAudioRate, kChannels = kAudioChannels, kFrameBytes = kAudioFrameBytes;

void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw std::runtime_error(std::string(what) + " HRESULT=" + std::to_string(uint32_t(hr)));
}

// COM and Media Foundation for the calling (decoder) thread, for as long as a reader exists.
struct MfScope {
  HRESULT com, mf;
  MfScope() : com(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), mf(MFStartup(MF_VERSION)) {
    if (FAILED(com) || FAILED(mf)) {
      Release();
      Check(FAILED(com) ? com : mf, FAILED(com) ? "COM" : "Media Foundation startup");
    }
  }
  ~MfScope() { Release(); }
  void Release() {
    if (SUCCEEDED(mf)) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    mf = com = E_FAIL;
  }
};

ComPtr<IMFSourceReader> Reader(const std::filesystem::path& path, bool video) {
  ComPtr<IMFAttributes> attributes;
  Check(MFCreateAttributes(&attributes, 2), "reader attributes");
  if (video) {
    Check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE), "video resizing");
  }
  ComPtr<IMFSourceReader> reader;
  Check(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader), "open movie");
  Check(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE), "select streams");
  Check(reader->SetStreamSelection(video ? kVideo : kAudio, TRUE), "select stream");
  ComPtr<IMFMediaType> type;
  Check(MFCreateMediaType(&type), "media type");
  Check(type->SetGUID(MF_MT_MAJOR_TYPE, video ? MFMediaType_Video : MFMediaType_Audio), "major type");
  Check(type->SetGUID(MF_MT_SUBTYPE, video ? MFVideoFormat_RGB32 : MFAudioFormat_PCM), "subtype");
  if (video) {
    ComPtr<IMFMediaType> native;
    UINT32 width = 0, height = 0;
    Check(reader->GetNativeMediaType(kVideo, 0, &native), "native video type");
    Check(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height), "native video dimensions");
    if (!width || !height || width > 4096 || height > 4096) throw std::runtime_error("unsupported source dimensions");
    UINT32 rate_n = 0, rate_d = 0;
    if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &rate_n, &rate_d)) && rate_n && rate_d)
      Check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, rate_n, rate_d), "unchanged source frame rate");
    Check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height), "unchanged source dimensions");
  } else {
    Check(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels), "audio channels");
    Check(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate), "audio rate");
    Check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "audio bits");
    Check(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kFrameBytes), "audio alignment");
    Check(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kFrameBytes), "audio byte rate");
  }
  Check(reader->SetCurrentMediaType(video ? kVideo : kAudio, nullptr, type.Get()), "decode format");
  return reader;
}
}  // namespace

bool DecoderAvailable() { return true; }
const char* DecoderName() { return "Media Foundation"; }

// Audio is small (under 3 MB for these clips). The player decodes it before starting the clock so that 4K
// video decoding can never starve the soundtrack's queue.
std::vector<uint8_t> DecodeAudio(const std::filesystem::path& file, const std::atomic<bool>& cancel) {
  MfScope scope;
  auto reader = Reader(file, false);
  std::vector<uint8_t> pcm;
  LONGLONG origin = 0;
  bool have_origin = false;
  for (int count = 0; count < 32768 && !cancel; ++count) {
    DWORD flags = 0;
    LONGLONG pts = 0;
    ComPtr<IMFSample> sample;
    Check(reader->ReadSample(kAudio, 0, nullptr, &flags, &pts, &sample), "audio sample");
    if (flags & MF_SOURCE_READERF_ERROR) throw std::runtime_error("audio decoder error");
    if (sample) {
      if (!have_origin) { origin = pts; have_origin = true; }
      pts = RelativePts(pts, origin);
      ComPtr<IMFMediaBuffer> buffer;
      Check(sample->ConvertToContiguousBuffer(&buffer), "audio buffer");
      BYTE* bytes = nullptr;
      DWORD length = 0;
      Check(buffer->Lock(&bytes, nullptr, &length), "lock audio");
      // Preserve gaps/trim overlaps in the source timestamps.
      const size_t offset = size_t(std::max<LONGLONG>(0, pts)) * kRate / 10000000 * kFrameBytes;
      if (offset + length > size_t(kRate) * kFrameBytes * 60) {
        buffer->Unlock();
        throw std::runtime_error("soundtrack exceeds the private clip limit (60 s)");
      }
      if (offset > pcm.size()) pcm.resize(offset, 0);
      const size_t skip = std::min<size_t>(length, pcm.size() > offset ? pcm.size() - offset : 0);
      pcm.insert(pcm.end(), bytes + skip, bytes + length);
      buffer->Unlock();
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return pcm;
  }
  if (!cancel) throw std::runtime_error("audio sample limit");
  return {};
}

struct VideoReader::Impl {
  MfScope scope;
  ComPtr<IMFSourceReader> reader;
  ComPtr<IMFSample> sample;
  UINT32 w = 0, h = 0, par_x = 1, par_y = 1, raw_stride = 0;
  double seconds = 0;
  bool eof = false;
};

VideoReader::VideoReader(const std::filesystem::path& file) : impl_(std::make_unique<Impl>()) {
  auto& m = *impl_;
  m.reader = Reader(file, true);
  ComPtr<IMFMediaType> format;
  Check(m.reader->GetCurrentMediaType(kVideo, &format), "video format");
  Check(MFGetAttributeSize(format.Get(), MF_MT_FRAME_SIZE, &m.w, &m.h), "video dimensions");
  MFGetAttributeRatio(format.Get(), MF_MT_PIXEL_ASPECT_RATIO, &m.par_x, &m.par_y);
  m.raw_stride = m.w * 4;
  format->GetUINT32(MF_MT_DEFAULT_STRIDE, &m.raw_stride);
  PROPVARIANT duration;
  PropVariantInit(&duration);
  if (SUCCEEDED(m.reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) &&
      duration.vt == VT_UI8) m.seconds = double(duration.uhVal.QuadPart) / 1e7;
  PropVariantClear(&duration);
}
VideoReader::~VideoReader() = default;
uint32_t VideoReader::width() const { return impl_->w; }
uint32_t VideoReader::height() const { return impl_->h; }
float VideoReader::pixel_aspect() const { return impl_->par_y ? float(impl_->par_x) / impl_->par_y : 1; }
double VideoReader::duration() const { return impl_->seconds; }

bool VideoReader::Next(int64_t& pts) {
  auto& m = *impl_;
  m.sample.Reset();
  // ReadSample may return no sample (a stream tick); only a sample or the end counts.
  for (int count = 0; count < 15000 && !m.eof; ++count) {
    DWORD flags = 0;
    LONGLONG at = 0;
    ComPtr<IMFSample> sample;
    Check(m.reader->ReadSample(kVideo, 0, nullptr, &flags, &at, &sample), "video sample");
    if (flags & MF_SOURCE_READERF_ERROR) throw std::runtime_error("video decoder error");
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      // The H.264 decoder announces its negotiated stride on its first
      // sample. Accept that notification, but reject a real size/subtype
      // change before interpreting any bytes with the old layout.
      ComPtr<IMFMediaType> changed;
      UINT32 next_w = 0, next_h = 0;
      GUID subtype{};
      Check(m.reader->GetCurrentMediaType(kVideo, &changed), "changed video format");
      Check(MFGetAttributeSize(changed.Get(), MF_MT_FRAME_SIZE, &next_w, &next_h), "changed dimensions");
      Check(changed->GetGUID(MF_MT_SUBTYPE, &subtype), "changed subtype");
      if (next_w != m.w || next_h != m.h || subtype != MFVideoFormat_RGB32)
        throw std::runtime_error("unsupported mid-movie format change");
      changed->GetUINT32(MF_MT_DEFAULT_STRIDE, &m.raw_stride);
    }
    // A last sample can come with the end-of-stream flag: hand it out, and end on the next call.
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) m.eof = true;
    if (sample) {
      m.sample = std::move(sample);
      pts = at;
      return true;
    }
  }
  if (!m.eof) throw std::runtime_error("video sample limit");
  return false;
}

void VideoReader::Pixels(std::vector<uint8_t>& rgba) {
  auto& m = *impl_;
  if (!m.sample) throw std::runtime_error("no video frame");
  const UINT32 w = m.w, h = m.h;
  ComPtr<IMFMediaBuffer> buffer;
  Check(m.sample->ConvertToContiguousBuffer(&buffer), "video buffer");
  ComPtr<IMF2DBuffer> two_d;
  BYTE* scan = nullptr;
  LONG stride = LONG(m.raw_stride);
  bool locked_2d = SUCCEEDED(buffer.As(&two_d)) && SUCCEEDED(two_d->Lock2D(&scan, &stride));
  DWORD length = 0;
  if (!locked_2d) {
    Check(buffer->Lock(&scan, nullptr, &length), "lock video");
    const uint64_t bytes = uint64_t(std::abs(stride)) * h;
    if (std::abs(stride) < LONG(w * 4) || bytes > length) {
      buffer->Unlock();
      throw std::runtime_error("invalid RGB stride/size");
    }
    if (stride < 0) scan += size_t(h-1) * size_t(-stride);
  }
  rgba.resize(size_t(w) * h * 4);
  for (UINT32 y = 0; y < h; ++y) {
    const BYTE* src = scan + ptrdiff_t(y) * stride;
    auto* dst = rgba.data() + size_t(y) * w * 4;
    for (UINT32 x = 0; x < w; ++x) {
      dst[x*4] = src[x*4+2]; dst[x*4+1] = src[x*4+1];
      dst[x*4+2] = src[x*4]; dst[x*4+3] = 255;
    }
  }
  if (locked_2d) two_d->Unlock2D(); else buffer->Unlock();
}
}  // namespace kk::movies
