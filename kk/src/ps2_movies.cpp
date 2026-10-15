#include "ps2_movies.h"
#include "ps2_movies_logic.h"

#if defined(_WIN32)
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
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")

#include <SDL3/SDL.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "glyphs.h"
#include "launcher_text.h"
#include "launcher_art.h"
#include "launcher_ui.h"
#include "play_internal.h"
#include "settings.h"
#include "ui_paint.h"

namespace kk::movies {
namespace {
using Microsoft::WRL::ComPtr;
constexpr DWORD kVideo = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAudio = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr int kRate = 48000, kChannels = 2, kFrameBytes = 4;

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw std::runtime_error(std::string(what) + " HRESULT=" + std::to_string(uint32_t(hr)));
}
uint32_t Word(const uint8_t* base, uint32_t at) {
  return uint32_t(base[at]) << 24 | uint32_t(base[at+1]) << 16 | uint32_t(base[at+2]) << 8 | base[at+3];
}
struct Frame {
  int64_t pts = 0;
  std::vector<uint8_t> rgba;
};
struct Job {
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> done{false}, primed{false}, eof{false};
  std::atomic<double> started{0};
  std::filesystem::path file;
  std::deque<Frame> frames;
  std::vector<uint8_t> audio;
  UINT32 width = 0, height = 0;
  float pixel_aspect = 1;
  double duration = 0;
  int decoded = 0;
  std::atomic<int> late_samples{0};
};
std::mutex job_mutex;
std::shared_ptr<Job> current_job;
bool dialog_ready = false;
std::atomic<bool> shutting_down{false};

void End(const std::shared_ptr<Job>& job, const char* why) {
  if (job && !job->done.exchange(true)) {
    REXLOG_INFO("PS2 movie: {} {}", job->file.filename().string(), why);
    job->cv.notify_all();
    RequestUiPaint();
  }
}

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

// Audio is small (under 3 MB for these clips). Decode it before starting the
// clock so that 4K video decoding can never starve the soundtrack's queue.
std::vector<uint8_t> ReadAudio(const std::shared_ptr<Job>& job) {
  auto reader = Reader(job->file, false);
  std::vector<uint8_t> pcm;
  LONGLONG origin = 0;
  bool have_origin = false;
  for (int count = 0; count < 32768 && !job->done; ++count) {
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
  if (!job->done) throw std::runtime_error("audio sample limit");
  return {};
}

Frame Pixels(IMFSample* sample, UINT32 w, UINT32 h, LONG fallback_stride, int64_t pts) {
  ComPtr<IMFMediaBuffer> buffer;
  Check(sample->ConvertToContiguousBuffer(&buffer), "video buffer");
  ComPtr<IMF2DBuffer> two_d;
  BYTE* scan = nullptr;
  LONG stride = fallback_stride;
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
  Frame frame;
  frame.pts = pts;
  frame.rgba.resize(size_t(w) * h * 4);
  for (UINT32 y = 0; y < h; ++y) {
    const BYTE* src = scan + ptrdiff_t(y) * stride;
    auto* dst = frame.rgba.data() + size_t(y) * w * 4;
    for (UINT32 x = 0; x < w; ++x) {
      dst[x*4] = src[x*4+2]; dst[x*4+1] = src[x*4+1];
      dst[x*4+2] = src[x*4]; dst[x*4+3] = 255;
    }
  }
  if (locked_2d) two_d->Unlock2D(); else buffer->Unlock();
  return frame;
}

bool White(const Frame& frame) {
  for (size_t i = 0; i + 3 < frame.rgba.size(); i += 4)
    if (frame.rgba[i] < 250 || frame.rgba[i+1] < 250 || frame.rgba[i+2] < 250) return false;
  return true;
}

void Decode(const std::shared_ptr<Job>& job) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const HRESULT mf = MFStartup(MF_VERSION);
  try {
    Check(com, "COM"); Check(mf, "Media Foundation startup");
    std::vector<uint8_t> pcm;
    try { pcm = ReadAudio(job); }
    catch (const std::exception& e) { REXLOG_WARN("PS2 movie: audio unavailable: {}", e.what()); }
    if (job->done) throw std::runtime_error("cancelled while opening");
    auto reader = Reader(job->file, true);
    ComPtr<IMFMediaType> format;
    Check(reader->GetCurrentMediaType(kVideo, &format), "video format");
    UINT32 w = 0, h = 0, par_x = 1, par_y = 1;
    Check(MFGetAttributeSize(format.Get(), MF_MT_FRAME_SIZE, &w, &h), "video dimensions");
    if (!w || !h || w > 4096 || h > 4096) throw std::runtime_error("unsupported video dimensions");
    MFGetAttributeRatio(format.Get(), MF_MT_PIXEL_ASPECT_RATIO, &par_x, &par_y);
    UINT32 raw_stride = w * 4;
    format->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride);
    PROPVARIANT duration;
    PropVariantInit(&duration);
    double seconds = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8) seconds = double(duration.uhVal.QuadPart) / 1e7;
    PropVariantClear(&duration);
    if (!(seconds > 0 && seconds <= 60)) throw std::runtime_error("invalid movie duration (limit 60 s)");
    {
      std::lock_guard lock(job->mutex);
      job->width = w; job->height = h;
      job->pixel_aspect = par_y ? float(par_x) / par_y : 1;
      job->duration = seconds;
      job->audio = std::move(pcm);
    }
    std::error_code ec;
    const bool compact_file = std::filesystem::file_size(job->file, ec) < 1024 * 1024 && !ec;
    bool all_white = compact_file;
    LONGLONG video_origin = 0;
    bool have_video_origin = false;
    for (int count = 0; count < 15000 && !job->done; ++count) {
      DWORD flags = 0;
      LONGLONG pts = 0;
      ComPtr<IMFSample> sample;
      Check(reader->ReadSample(kVideo, 0, nullptr, &flags, &pts, &sample), "video sample");
      if (flags & MF_SOURCE_READERF_ERROR) throw std::runtime_error("video decoder error");
      if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        // The H.264 decoder announces its negotiated stride on its first
        // sample. Accept that notification, but reject a real size/subtype
        // change before interpreting any bytes with the old layout.
        ComPtr<IMFMediaType> changed;
        UINT32 next_w = 0, next_h = 0;
        GUID subtype{};
        Check(reader->GetCurrentMediaType(kVideo, &changed), "changed video format");
        Check(MFGetAttributeSize(changed.Get(), MF_MT_FRAME_SIZE, &next_w, &next_h), "changed dimensions");
        Check(changed->GetGUID(MF_MT_SUBTYPE, &subtype), "changed subtype");
        if (next_w != w || next_h != h || subtype != MFVideoFormat_RGB32)
          throw std::runtime_error("unsupported mid-movie format change");
        changed->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride);
      }
      if (sample) {
        if (!have_video_origin) {
          video_origin = pts;
          have_video_origin = true;
          REXLOG_INFO("PS2 movie: video timestamp origin {:.3f}ms normalized", double(video_origin) / 10000);
        }
        // MF's H.264 decoder can report a reorder offset on the first
        // sample (125 ms in the supplied files). These standalone clips
        // start both streams at zero; preserve cadence, remove the offset.
        pts = RelativePts(pts, video_origin);
        const double start = job->started;
        if (start > 0 && double(pts) / 1e7 < Now() - start - .08) {
          ++job->late_samples;
          if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { job->eof = true; RequestUiPaint(); break; }
          continue;
        }
        Frame frame = Pixels(sample.Get(), w, h, LONG(raw_stride), pts);
        all_white = all_white && White(frame);
        std::unique_lock lock(job->mutex);
        ++job->decoded;
        // Reject the supplied white-placeholder type without embedding an
        // asset identifier. A normal fade in a larger movie remains valid.
        if (compact_file && all_white && job->decoded == 3) {
          lock.unlock();
          End(job, "ignored: small uniform-white placeholder");
          break;
        }
        job->cv.wait(lock, [&] { return job->done || job->frames.size() < 3; });
        if (job->done) break;
        if (start > 0 && double(pts) / 1e7 < Now() - start - .08) {
          ++job->late_samples;
          lock.unlock();
          if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { job->eof = true; RequestUiPaint(); break; }
          continue;
        }
        job->frames.push_back(std::move(frame));
        if (!all_white || job->decoded >= 3) job->primed = true;
        lock.unlock();
        RequestUiPaint();
      }
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        job->eof = true;
        if (!job->primed) End(job, "ignored: no usable video frames");
        RequestUiPaint();
        break;
      }
    }
    if (!job->done && !job->eof) End(job, "video sample limit");
  } catch (const std::exception& e) {
    if (!job->done) REXLOG_WARN("PS2 movie: {}: {}", job->file.filename().string(), e.what());
    End(job, "decoder stopped; continuing load");
  }
  if (SUCCEEDED(mf)) MFShutdown();
  if (SUCCEEDED(com)) CoUninitialize();
}

class MovieDialog final : public rex::ui::ImGuiDialog {
 public:
  MovieDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
              const std::filesystem::path& glyph_dir)
      : ImGuiDialog(drawer), immediate_(immediate), glyphs_(immediate, glyph_dir) {}
  ~MovieDialog() override { End(job_, "dialog closed"); CloseAudio(); }
  bool NeedsContinuousPaint() override {
    std::lock_guard lock(job_mutex);
    return current_job && !current_job->done;
  }
 protected:
  void OnDraw(ImGuiIO& io) override {
    std::shared_ptr<Job> next;
    { std::lock_guard lock(job_mutex); next = current_job; }
    if (next != job_) {
      CloseAudio(); texture_.reset(); input_.reset();
      job_ = std::move(next);
      glyphs_.AddSet("xbox360", art::LauncherArtDir(play::UserDir()) / "glyphs" / "xbox360");
      shown_frame_ = ImGui::GetFrameCount();
      displayed_ = dropped_ = 0;
      max_lateness_ = 0;
      if (job_ && !job_->done) input_ = std::make_unique<ui::Input>();
    }
    if (!job_) return;
    if (job_->done) {
      CloseAudio(); texture_.reset(); input_.reset();
      { std::lock_guard lock(job_->mutex); job_->frames.clear(); job_->audio.clear(); }
      { std::lock_guard lock(job_mutex); if (current_job == job_) current_job.reset(); }
      job_.reset();
      return;
    }
    input_->Update();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const auto* vp = ImGui::GetMainViewport();
    const ImVec2 origin = vp->Pos, end(origin.x + vp->Size.x, origin.y + vp->Size.y);
    dl->AddRectFilled(origin, end, IM_COL32_BLACK);
    // Always present, including while the decoder is opening, with both a
    // controller glyph in the selected style and the keyboard key.
    Prompt(dl, origin, vp->Size);
    io.WantCaptureKeyboard = io.WantCaptureMouse = true;
#if defined(KK_DEV_TOOLS)
    if (const char* skip = std::getenv("KK_DEV_PS2_SKIP_AFTER"); skip && job_->started > 0 &&
        Now() - job_->started >= std::atof(skip)) {
      input_->Inject(ui::Action::kAccept, ui::Device::kKeyboard);
    }
#endif
    if (ImGui::GetFrameCount() > shown_frame_ + 1 &&
        (input_->Pressed(ui::Action::kAccept) || input_->Pressed(ui::Action::kBack) ||
         ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
      CloseAudio();
      End(job_, "skipped");
      return;
    }
    if (!job_->primed) return;
    Frame frame;
    bool have_frame = false;
    {
      std::lock_guard lock(job_->mutex);
      if (job_->started == 0 && !job_->frames.empty()) {
        OpenAudio(job_->audio);
        job_->started = Now();
        REXLOG_INFO("PS2 movie: clock started {} {}x{} duration {:.3f}s audio {} bytes",
                    job_->file.filename().string(), job_->width, job_->height, job_->duration, job_->audio.size());
      }
      const double elapsed = Now() - job_->started;
      while (!job_->frames.empty() && FrameDue(job_->frames.front().pts, elapsed)) {
        if (have_frame) ++dropped_;
        frame = std::move(job_->frames.front()); job_->frames.pop_front(); have_frame = true;
      }
      if (have_frame) max_lateness_ = std::max(max_lateness_, elapsed - double(frame.pts) / 1e7);
      // Media time, rather than decoder backlog, ends a clip. A slow
      // decoder may discard frames but must never extend the soundtrack.
      if (elapsed >= job_->duration) {
        const int queued = stream_ ? SDL_GetAudioStreamQueued(stream_) : -1;
        CloseAudio();
        REXLOG_INFO("PS2 movie: completed at {:.3f}s, presented {}, superseded {}, decoder late {}, max frame lateness {:.1f}ms, audio queued {} bytes",
                    elapsed, displayed_, dropped_, job_->late_samples.load(), max_lateness_ * 1000, queued);
        End(job_, "completed");
      }
    }
    job_->cv.notify_all();
    if (have_frame) {
      texture_ = immediate_->CreateTexture(job_->width, job_->height,
          rex::ui::ImmediateTextureFilter::kLinear, false, frame.rgba.data());
      if (!texture_) { CloseAudio(); End(job_, "texture upload failed"); return; }
      ++displayed_;
    }
    if (stream_) {
      const auto volume = play::ReadProfileOptions(play::UserDir(), -1);
      SDL_SetAudioStreamGain(stream_, volume.available ? MovieGain(volume.master, volume.music, volume.effects) : 1);
    }
    if (texture_) {
      const auto rect = AspectFit(vp->Size.x, vp->Size.y, job_->width * job_->pixel_aspect, float(job_->height));
      dl->AddImage(ui::Tex(texture_.get()), ImVec2(origin.x+rect.x, origin.y+rect.y),
          ImVec2(origin.x+rect.x+rect.w, origin.y+rect.y+rect.h));
      Prompt(dl, origin, vp->Size);  // draw over the image too
    }
  }
 private:
  void OpenAudio(const std::vector<uint8_t>& pcm) {
    if (pcm.empty()) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { REXLOG_WARN("PS2 movie: SDL audio unavailable"); return; }
    audio_ready_ = true;
    const SDL_AudioSpec spec{SDL_AUDIO_S16LE, kChannels, kRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) { REXLOG_WARN("PS2 movie: audio device: {}", SDL_GetError()); return; }
    const auto volume = play::ReadProfileOptions(play::UserDir(), -1);
    SDL_SetAudioStreamGain(stream_, volume.available ? MovieGain(volume.master, volume.music, volume.effects) : 1);
    if (!SDL_PutAudioStreamData(stream_, pcm.data(), int(pcm.size())) || !SDL_ResumeAudioStreamDevice(stream_)) {
      REXLOG_WARN("PS2 movie: audio start: {}", SDL_GetError()); CloseAudio();
    }
  }
  void CloseAudio() {
    if (stream_) { SDL_DestroyAudioStream(stream_); stream_ = nullptr; }
    if (audio_ready_) { SDL_QuitSubSystem(SDL_INIT_AUDIO); audio_ready_ = false; }
  }
  void Prompt(ImDrawList* dl, ImVec2 origin, ImVec2 size) {
    const float s = std::max(.65f, std::min(size.x / 1280, size.y / 720));
    std::string style = ButtonPromptStyle();
    if (style == "keyboard") style = DetectedPadPrompts();
    float aspect = 1;
    auto* glyph = glyphs_.Get(ui::Action::kAccept, style, &aspect);
    const float gh = 27*s;
    const char* fallback = style == "ps2" || style == "ps5" ? "Cross" : "A";
    const std::string caption = std::string("/ Esc   ") + text::T("Skip");
    ImFont* font = ImGui::GetFont();
    const float glyph_width = glyph ? gh * aspect : font->CalcTextSizeA(19*s, FLT_MAX, 0, fallback).x;
    const float caption_width = font->CalcTextSizeA(19*s, FLT_MAX, 0, caption.c_str()).x;
    const float width = glyph_width + caption_width + 44*s;
    const ImVec2 p(origin.x + size.x - width - 20*s, origin.y + size.y - 66*s);
    dl->AddRectFilled(p, ImVec2(origin.x+size.x-20*s, origin.y+size.y-20*s), IM_COL32(0,0,0,220), 5*s);
    if (glyph) dl->AddImage(ui::Tex(glyph), ImVec2(p.x+12*s,p.y+9*s), ImVec2(p.x+12*s+gh*aspect,p.y+9*s+gh));
    else dl->AddText(font, 19*s, ImVec2(p.x+12*s,p.y+12*s), IM_COL32_WHITE, fallback);
    dl->AddText(font, 19*s, ImVec2(p.x+glyph_width+28*s,p.y+12*s), IM_COL32_WHITE, caption.c_str());
  }
  rex::ui::ImmediateDrawer* immediate_;
  ui::Glyphs glyphs_;
  std::shared_ptr<Job> job_;
  std::unique_ptr<ui::Input> input_;
  std::unique_ptr<rex::ui::ImmediateTexture> texture_;
  SDL_AudioStream* stream_ = nullptr;
  bool audio_ready_ = false;
  double max_lateness_ = 0;
  int shown_frame_ = 0, displayed_ = 0, dropped_ = 0;
};
}  // namespace

std::filesystem::path Folder() {
  const auto value = REXCVAR_GET(kk_ps2_movie_folder);
  if (!value.empty()) return std::filesystem::u8path(value);
  PWSTR desktop = nullptr;
  std::filesystem::path folder;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DONT_VERIFY, nullptr, &desktop))) folder = desktop;
  CoTaskMemFree(desktop);
  return folder.empty() ? std::filesystem::path() : folder / "kk_upscaled_movies";
}

std::string BrowseFolder() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ComPtr<IFileOpenDialog> dialog;
  std::string result;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(L"Select the PS2 movie folder");
    if (SUCCEEDED(dialog->Show(nullptr))) {
      ComPtr<IShellItem> item;
      PWSTR path = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        const auto utf8 = std::filesystem::path(path).u8string();
        result.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
      }
      CoTaskMemFree(path);
    }
  }
  dialog.Reset();
  if (SUCCEEDED(com)) CoUninitialize();
  return result;
}

void CreateMovieDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                  const std::filesystem::path& glyph_dir) {
  if (dialog_ready || !drawer || !immediate) return;
  new MovieDialog(drawer, immediate, glyph_dir);
  dialog_ready = true;
}

bool BeforeLoad(const uint8_t* base, uint32_t g, bool starting, bool leaving) {
  if (!dialog_ready || shutting_down || !REXCVAR_GET(kk_ps2_cutscenes) || !g || !Word(base,g+4)) return false;
  const int from = int(Word(base,g+0x18)), to = int(Word(base,g+8));
  std::vector<int> levels;
  for (const auto& chapter : play::Chapters()) levels.push_back(chapter.level_key);
  if (!StoryBoundary(levels, from, to, Word(base,g+0x5668)==0, starting, leaving)) return false;
  const uint32_t catalog = Word(base,0x82CC9354);
  const auto world = WorldForLevel(catalog, to, [&](uint32_t at) { return Word(base, at); });
  const auto name = MovieName(world);
  const auto folder = Folder();
  if (name.empty() || folder.empty()) return false;
  const auto file = folder / name;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) {
    REXLOG_INFO("PS2 movie: no local {} for {} -> {}; normal load", name, from, to);
    return false;
  }
  auto job = std::make_shared<Job>();
  job->file = file;
  { std::lock_guard lock(job_mutex); current_job = job; }
  REXLOG_INFO("PS2 movie: story {} -> {} world {:08X} {}", from, to, world, name);
  RequestUiPaint();
  std::thread worker([job] { Decode(job); });
  // Only the level loader waits. The presenter continues painting on its UI
  // thread. No game time/AI dt is changed, and the original request is retained.
  std::unique_lock lock(job->mutex);
  const bool ended = job->cv.wait_for(lock, std::chrono::seconds(75), [&] { return job->done.load(); });
  lock.unlock();
  if (!ended) End(job, "watchdog: continuing load");
  // Cancellation must immediately release the mission load even if an OS
  // decoder call is still returning; the worker retains its own shared state.
  worker.detach();
  return true;
}

void Shutdown() {
  shutting_down = true;
  std::shared_ptr<Job> job;
  { std::lock_guard lock(job_mutex); job = current_job; }
  End(job, "shutdown");
}
}  // namespace kk::movies

#else
namespace kk::movies {
std::filesystem::path Folder() { return {}; }
std::string BrowseFolder() { return {}; }
void CreateMovieDialog(rex::ui::ImGuiDrawer*, rex::ui::ImmediateDrawer*, const std::filesystem::path&) {}
bool BeforeLoad(const uint8_t*, uint32_t, bool, bool) { return false; }
void Shutdown() {}
}  // namespace kk::movies
#endif
