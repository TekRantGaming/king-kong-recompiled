// The PS2 cutscene decoder on Linux: FFmpeg's own H.264 and AAC decoders and its MP4 demuxer, nothing else.
// The libraries are LGPL 2.1 builds (no GPL or non-free parts) made by tools/build_ffmpeg_linux.sh with the
// name suffix "-kk" (libavcodec-kk.so.58 ...), linked dynamically and shipped beside the program, so a
// player can swap them; THIRD-PARTY-NOTICES.txt says where their source is.
// It gives the player (ps2_movies.cpp) the same things as the Windows decoder (ps2_movie_decoder_mf.cpp):
// the soundtrack as 48 kHz 16-bit stereo with timestamp gaps kept, and RGBA frames at the video's own size
// with their presentation times in 100 ns units.
#include "ps2_movie_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace kk::movies {
namespace {
constexpr AVRational k100ns{1, 10000000};

std::string Error(int code) {
  char text[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, text, sizeof(text));
  return text;
}
void Check(int code, const char* what) {
  if (code < 0) throw std::runtime_error(std::string(what) + ": " + Error(code));
}
void Quiet() {
  static std::once_flag once;
  std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

// One stream of a file, opened with its decoder.
struct Stream {
  AVFormatContext* format = nullptr;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  int index = -1;
  bool draining = false;
  int bad_packets = 0;

  Stream(const std::filesystem::path& file, AVMediaType type) {
    Quiet();
    try {
      Check(avformat_open_input(&format, file.string().c_str(), nullptr, nullptr), "open movie");
      Check(avformat_find_stream_info(format, nullptr), "read movie streams");
      index = av_find_best_stream(format, type, -1, -1, nullptr, 0);
      Check(index, type == AVMEDIA_TYPE_VIDEO ? "no video stream" : "no audio stream");
      const AVCodecParameters* par = format->streams[index]->codecpar;
      const AVCodec* decoder = avcodec_find_decoder(par->codec_id);
      if (!decoder) throw std::runtime_error(std::string("unsupported codec ") + avcodec_get_name(par->codec_id));
      codec = avcodec_alloc_context3(decoder);
      if (!codec) throw std::runtime_error("decoder context");
      Check(avcodec_parameters_to_context(codec, par), "decoder parameters");
      codec->pkt_timebase = format->streams[index]->time_base;
      if (type == AVMEDIA_TYPE_VIDEO) {
        // 4K H.264 needs several cores; leave some for the game's own threads.
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        codec->thread_count = int(std::clamp(cores > 2 ? cores - 2 : 1u, 1u, 8u));
        codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
      }
      Check(avcodec_open2(codec, decoder, nullptr), "open decoder");
      packet = av_packet_alloc();
      frame = av_frame_alloc();
      if (!packet || !frame) throw std::runtime_error("out of memory");
    } catch (...) {
      Free();
      throw;
    }
  }
  ~Stream() { Free(); }
  void Free() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
  }
  AVRational time_base() const { return format->streams[index]->time_base; }

  // The next decoded frame into `frame`; false at the end.
  bool Receive() {
    for (int reads = 0; reads < 100000; ++reads) {
      const int got = avcodec_receive_frame(codec, frame);
      if (got == 0) return true;
      if (got == AVERROR_EOF) return false;
      if (got != AVERROR(EAGAIN)) Check(got, "decode");
      if (draining) return false;
      const int read = av_read_frame(format, packet);
      if (read == AVERROR_EOF) {
        Check(avcodec_send_packet(codec, nullptr), "finish decoding");
        draining = true;
        continue;
      }
      Check(read, "read movie");
      if (packet->stream_index == index) {
        const int sent = avcodec_send_packet(codec, packet);
        // A damaged packet costs a frame, not the movie (Media Foundation conceals them too).
        if (sent < 0 && sent != AVERROR(EAGAIN) && !(sent == AVERROR_INVALIDDATA && ++bad_packets <= 30))
          Check(sent, "decode packet");
      }
      av_packet_unref(packet);
    }
    throw std::runtime_error("decoder made no progress");
  }
};

int64_t FrameTime(const AVFrame* frame) {
  const int64_t pts = frame->best_effort_timestamp;
  return pts == AV_NOPTS_VALUE ? frame->pts : pts;
}
}  // namespace

bool DecoderAvailable() { return true; }
const char* DecoderName() { return "FFmpeg " LIBAVCODEC_IDENT " (LGPL)"; }

std::vector<uint8_t> DecodeAudio(const std::filesystem::path& file, const std::atomic<bool>& cancel, AudioStats* stats) {
  AudioStats local;
  AudioStats& st = stats ? *stats : local;
  Stream audio(file, AVMEDIA_TYPE_AUDIO);
  SwrContext* swr = nullptr;
  struct FreeSwr { SwrContext** p; ~FreeSwr() { swr_free(p); } } free_swr{&swr};
  std::vector<uint8_t> pcm;
  std::vector<uint8_t> out;
  constexpr size_t kLimit = size_t(kAudioRate) * kAudioFrameBytes * 60;
  // Small timestamp differences are the resampler's own delay; only a real gap or overlap (over 10 ms) is
  // filled with silence or trimmed, as the Windows decoder does.
  constexpr int64_t kSlack = kAudioRate / 100;
  int64_t origin = AV_NOPTS_VALUE;
  int64_t trim = 0;
  int in_rate = 0, in_format = -1;
  uint64_t in_layout = 0;
  auto append = [&](int samples) {
    size_t bytes = size_t(samples) * kAudioFrameBytes;
    size_t skip = std::min<size_t>(bytes, size_t(trim) * kAudioFrameBytes);
    trim -= int64_t(skip / kAudioFrameBytes);
    if (pcm.size() + bytes - skip > kLimit) throw std::runtime_error("soundtrack exceeds the private clip limit (60 s)");
    pcm.insert(pcm.end(), out.data() + skip, out.data() + bytes);
  };
  for (int count = 0; count < 100000 && !cancel; ++count) {
    if (!audio.Receive()) {
      if (swr) {  // what the resampler still holds
        const int left = swr_get_out_samples(swr, 0);
        if (left > 0) {
          out.resize(size_t(left) * kAudioFrameBytes);
          uint8_t* dst[1] = {out.data()};
          const int got = swr_convert(swr, dst, left, nullptr, 0);
          if (got > 0) append(got);
        }
      }
      return pcm;
    }
    AVFrame* f = audio.frame;
    const uint64_t layout = f->channel_layout ? f->channel_layout : uint64_t(av_get_default_channel_layout(f->channels));
    if (!swr) {
      swr = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, kAudioRate, int64_t(layout),
                               AVSampleFormat(f->format), f->sample_rate, 0, nullptr);
      if (!swr) throw std::runtime_error("audio resampler");
      Check(swr_init(swr), "audio resampler");
      in_rate = f->sample_rate; in_format = f->format; in_layout = layout;
    } else if (f->sample_rate != in_rate || f->format != in_format || layout != in_layout) {
      throw std::runtime_error("unsupported mid-movie audio format change");
    }
    const int64_t at = FrameTime(f);
    if (at != AV_NOPTS_VALUE) {
      if (origin == AV_NOPTS_VALUE) origin = at;
      const int64_t want = at <= origin ? 0 : av_rescale_q(at - origin, audio.time_base(), AVRational{1, kAudioRate});
      const int64_t have = int64_t(pcm.size() / kAudioFrameBytes) + swr_get_delay(swr, kAudioRate) - trim;
      ++st.packets;
      if (want - have > kSlack) {
        if (size_t(want) * kAudioFrameBytes > kLimit) throw std::runtime_error("soundtrack exceeds the private clip limit (60 s)");
        pcm.resize(pcm.size() + size_t(want - have) * kAudioFrameBytes, 0);
        ++st.gaps;
        st.gap_frames += want - have;
      } else if (have - want > kSlack) {
        trim += have - want;
        ++st.overlaps;
        st.overlap_frames += have - want;
      } else if (want != have) {
        ++st.jitter;
        st.jitter_frames += want > have ? want - have : have - want;
      }
    }
    const int room = swr_get_out_samples(swr, f->nb_samples);
    out.resize(size_t(std::max(room, 0)) * kAudioFrameBytes);
    uint8_t* dst[1] = {out.data()};
    const int got = swr_convert(swr, dst, room, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
    Check(got, "audio conversion");
    append(got);
  }
  if (!cancel) throw std::runtime_error("audio sample limit");
  return {};
}

struct VideoReader::Impl {
  Stream stream;
  SwsContext* sws = nullptr;
  uint32_t w = 0, h = 0;
  float aspect = 1;
  double seconds = 0;
  bool have_frame = false;
  explicit Impl(const std::filesystem::path& file) : stream(file, AVMEDIA_TYPE_VIDEO) {}
  ~Impl() { sws_freeContext(sws); }
};

VideoReader::VideoReader(const std::filesystem::path& file) : impl_(std::make_unique<Impl>(file)) {
  auto& m = *impl_;
  const AVStream* s = m.stream.format->streams[m.stream.index];
  m.w = uint32_t(std::max(0, s->codecpar->width));
  m.h = uint32_t(std::max(0, s->codecpar->height));
  const AVRational par = av_guess_sample_aspect_ratio(m.stream.format, const_cast<AVStream*>(s), nullptr);
  m.aspect = par.num > 0 && par.den > 0 ? float(av_q2d(par)) : 1.0f;
  if (m.stream.format->duration != AV_NOPTS_VALUE && m.stream.format->duration > 0)
    m.seconds = double(m.stream.format->duration) / AV_TIME_BASE;
  else if (s->duration != AV_NOPTS_VALUE && s->duration > 0)
    m.seconds = double(s->duration) * av_q2d(s->time_base);
}
VideoReader::~VideoReader() = default;
uint32_t VideoReader::width() const { return impl_->w; }
uint32_t VideoReader::height() const { return impl_->h; }
float VideoReader::pixel_aspect() const { return impl_->aspect; }
double VideoReader::duration() const { return impl_->seconds; }

bool VideoReader::Next(int64_t& pts) {
  auto& m = *impl_;
  m.have_frame = false;
  if (!m.stream.Receive()) return false;
  const AVFrame* f = m.stream.frame;
  if (uint32_t(f->width) != m.w || uint32_t(f->height) != m.h)
    throw std::runtime_error("unsupported mid-movie format change");
  const int64_t at = FrameTime(f);
  pts = at == AV_NOPTS_VALUE ? 0 : av_rescale_q(at, m.stream.time_base(), k100ns);
  m.have_frame = true;
  return true;
}

void VideoReader::Pixels(std::vector<uint8_t>& rgba) {
  auto& m = *impl_;
  if (!m.have_frame) throw std::runtime_error("no video frame");
  const AVFrame* f = m.stream.frame;
  m.sws = sws_getCachedContext(m.sws, f->width, f->height, AVPixelFormat(f->format), f->width, f->height,
                               AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!m.sws) throw std::runtime_error("video conversion");
  // The same colours as Windows: BT.709 for HD video unless the file says otherwise, studio range unless
  // the file says full range.
  int matrix = SWS_CS_DEFAULT;
  switch (f->colorspace) {
    case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: matrix = SWS_CS_ITU601; break;
    case AVCOL_SPC_SMPTE240M: matrix = SWS_CS_SMPTE240M; break;
    case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: matrix = SWS_CS_BT2020; break;
    default: matrix = f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601; break;
  }
  const int* table = sws_getCoefficients(matrix);
  sws_setColorspaceDetails(m.sws, table, f->color_range == AVCOL_RANGE_JPEG, table, 1, 0, 1 << 16, 1 << 16);
  rgba.resize(size_t(m.w) * m.h * 4);
  uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
  int dst_stride[4] = {int(m.w * 4), 0, 0, 0};
  if (sws_scale(m.sws, f->data, f->linesize, 0, f->height, dst, dst_stride) != f->height)
    throw std::runtime_error("video conversion");
}
}  // namespace kk::movies
