#include "ps2_movies.h"
#include "freecam.h"
#include <rex/filesystem.h>
#include "ps2_movie_decoder.h"
#include "ps2_movies_logic.h"
#if defined(KK_DEV_TOOLS)
#include "dev_chapter_prepare.h"
#endif

// The restored PS2 cutscenes' player, the same on Windows and Linux: the decoder is Media Foundation on
// Windows and the bundled LGPL FFmpeg on Linux (ps2_movie_decoder.h).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#pragma comment(lib, "shell32.lib")
#else
#include <cstdio>
#endif

#include <SDL3/SDL.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
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

#include "benchmark.h"
#include "glyphs.h"
#include "launcher_text.h"
#include "launcher_art.h"
#include "launcher_ui.h"
#include "play_internal.h"
#include "settings.h"
#include "ui_paint.h"

// Task 293 (REPORT-269's idea): at a story chapter boundary, load the next level while the PS2 cutscene plays
// instead of after it (removes the game's LOADING screen after the clip). Off: the loader waits for the clip
// first, as before. On by default (owner, 19 Oct; REPORT-294: all game checks passed).
REXCVAR_DEFINE_BOOL(kk_ps2_cutscene_load_during, true, "KK/Gameplay",
                    "Load the next chapter while its PS2 cutscene plays (task 293; off: after the cutscene, as before). "
                    "The game's own sound is held silent until the cutscene ends");
// The SDK's live mute (its audio callback reads it every buffer): the game's sound while the level loads under
// the cutscene. Never saved while held (SetAudioMuteHeld, settings.cpp).
REXCVAR_DECLARE(bool, audio_mute);

namespace kk::movies {
namespace {
constexpr int kRate = kAudioRate, kChannels = kAudioChannels;

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
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
  uint32_t width = 0, height = 0;
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

bool White(const Frame& frame) {
  for (size_t i = 0; i + 3 < frame.rgba.size(); i += 4)
    if (frame.rgba[i] < 250 || frame.rgba[i+1] < 250 || frame.rgba[i+2] < 250) return false;
  return true;
}

void Decode(const std::shared_ptr<Job>& job) {
  try {
    // Audio is small (under 3 MB for these clips). Decode it before starting the
    // clock so that 4K video decoding can never starve the soundtrack's queue.
    std::vector<uint8_t> pcm;
    try { pcm = DecodeAudio(job->file, job->done); }
    catch (const std::exception& e) { REXLOG_WARN("PS2 movie: audio unavailable: {}", e.what()); }
    if (job->done) throw std::runtime_error("cancelled while opening");
    VideoReader reader(job->file);
    const uint32_t w = reader.width(), h = reader.height();
    if (!w || !h || w > 4096 || h > 4096) throw std::runtime_error("unsupported video dimensions");
    const double seconds = reader.duration();
    if (!(seconds > 0 && seconds <= 60)) throw std::runtime_error("invalid movie duration (limit 60 s)");
    {
      std::lock_guard lock(job->mutex);
      job->width = w; job->height = h;
      job->pixel_aspect = reader.pixel_aspect();
      job->duration = seconds;
      job->audio = std::move(pcm);
    }
    std::error_code ec;
    const bool compact_file = std::filesystem::file_size(job->file, ec) < 1024 * 1024 && !ec;
    bool all_white = compact_file;
    int64_t video_origin = 0;
    bool have_video_origin = false;
    for (int count = 0; count < 15000 && !job->done; ++count) {
      int64_t pts = 0;
      if (!reader.Next(pts)) {
        job->eof = true;
        if (!job->primed) End(job, "ignored: no usable video frames");
        RequestUiPaint();
        break;
      }
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
        ++job->late_samples;  // late: skipped without converting it
        continue;
      }
      Frame frame;
      frame.pts = pts;
      reader.Pixels(frame.rgba);
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
        continue;
      }
      job->frames.push_back(std::move(frame));
      if (!all_white || job->decoded >= 3) job->primed = true;
      lock.unlock();
      RequestUiPaint();
    }
    if (!job->done && !job->eof) End(job, "video sample limit");
  } catch (const std::exception& e) {
    if (!job->done) REXLOG_WARN("PS2 movie: {}: {}", job->file.filename().string(), e.what());
    End(job, "decoder stopped; continuing load");
  }
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
    if (freecam::HideHud()) return;
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
  if (!value.empty()) {
    const auto path = std::filesystem::u8path(value);
    return path.is_absolute() ? path : rex::filesystem::GetExecutableFolder() / path;
  }
  return {};
}

bool PlaybackAvailable() { return DecoderAvailable(); }

namespace {
std::mutex override_mutex;
std::vector<std::filesystem::path> override_folders;
}  // namespace

void SetOverrideFolders(std::vector<std::filesystem::path> folders) {
  std::lock_guard lock(override_mutex);
  override_folders = std::move(folders);
}

#if !defined(_WIN32)
// Linux: the desktop's own folder picker (zenity on GNOME and SteamOS desktop mode, kdialog on KDE), when
// one is installed. The downloaded cutscenes set the folder themselves; this is for the player's own files.
std::string BrowseFolder() {
  struct Picker { const char* tool; const char* command; };
  const Picker pickers[] = {
      {"zenity", "env -u LD_LIBRARY_PATH zenity --file-selection --directory "
                 "--title='Select the PS2 movie folder' 2>/dev/null"},
      {"kdialog", "env -u LD_LIBRARY_PATH kdialog --getexistingdirectory \"$HOME\" "
                  "--title 'Select the PS2 movie folder' 2>/dev/null"},
  };
  for (const auto& picker : pickers) {
    if (std::system((std::string("command -v ") + picker.tool + " >/dev/null 2>&1").c_str()) != 0) continue;
    FILE* pipe = popen(picker.command, "r");
    if (!pipe) continue;
    std::string result;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;  // empty if the player cancelled
  }
  REXLOG_INFO("PS2 movie: no folder picker (zenity or kdialog) on this system");
  return {};
}
#else

std::string BrowseFolder() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  std::string result;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(L"Select the PS2 movie folder");
    if (SUCCEEDED(dialog->Show(nullptr))) {
      Microsoft::WRL::ComPtr<IShellItem> item;
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
#endif

void CreateMovieDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                  const std::filesystem::path& glyph_dir) {
  if (dialog_ready || !drawer || !immediate) return;
  new MovieDialog(drawer, immediate, glyph_dir);
  dialog_ready = true;
}

namespace {
std::atomic<uint32_t> loader_waits_ended{0};
std::atomic<bool> loader_waiting{false};
}  // namespace
uint32_t LoaderWaitsEnded() { return loader_waits_ended.load(); }
bool LoaderWaiting() { return loader_waiting.load(); }

namespace {
// Task 293: the clip the level is loading under (kk_ps2_cutscene_load_during): started by BeforeLoad, waited
// for by AfterLoad. Both run on the universe thread (the uni_go hook), so no lock.
struct PendingClip {
  std::shared_ptr<Job> job;
  std::thread worker;
  std::chrono::steady_clock::time_point started;
};
PendingClip pending;
std::atomic<bool> holding_audio{false};

void HoldGameAudio(bool hold) {
  if (hold) {
    if (REXCVAR_GET(audio_mute)) return;  // the player's own mute: left as it is
    holding_audio = true;
    SetAudioMuteHeld(true);  // settings.h: never saved while held
    REXCVAR_SET(audio_mute, true);
  } else if (holding_audio.exchange(false)) {
    REXCVAR_SET(audio_mute, false);
    SetAudioMuteHeld(false);
  }
}

// The loader's wait for the clip's end (75 s watchdog from the clip's start), then lets the decoder go.
void WaitForClip(const std::shared_ptr<Job>& job, std::thread& worker, std::chrono::steady_clock::time_point started) {
  std::unique_lock lock(job->mutex);
  const bool ended = job->cv.wait_until(lock, started + std::chrono::seconds(75), [&] { return job->done.load(); });
  lock.unlock();
  loader_waits_ended.fetch_add(1);  // before clearing `waiting`: never a moment with neither (telemetry)
  loader_waiting = false;
  if (!ended) End(job, "watchdog: continuing load");
  // Cancellation must immediately release the mission load even if an OS
  // decoder call is still returning; the worker retains its own shared state.
  if (worker.joinable()) worker.detach();
}
}  // namespace


bool BeforeLoad(const uint8_t* base, uint32_t g, bool starting, bool leaving) {
#if defined(KK_DEV_TOOLS)
  DevStartPreparationTrace();
#endif
  std::vector<std::filesystem::path> overrides;
  {
    std::lock_guard lock(override_mutex);
    overrides = override_folders;
  }
  const bool ps2 = REXCVAR_GET(kk_ps2_cutscenes);
  if (!dialog_ready || shutting_down || !DecoderAvailable() || (!ps2 && overrides.empty()) || !g ||
      !Word(base,g+4)) return false;
  const int from = int(Word(base,g+0x18)), to = int(Word(base,g+8));
  std::vector<int> levels;
  for (const auto& chapter : play::Chapters()) levels.push_back(chapter.level_key);
  // G+0x5668: a chapter-select run; its end shows the movie too (MovieRun).
  if (!StoryBoundary(levels, from, to, MovieRun(Word(base,g+0x5668)!=0, benchmark::Active()), starting, leaving))
    return false;
  const uint32_t catalog = Word(base,0x82CC9354);
  const auto world = WorldForLevel(catalog, to, [&](uint32_t at) { return Word(base, at); });
  const auto name = MovieName(world);
  if (name.empty()) return false;
  // Linux file names are case-sensitive and the clips' are not consistent (BK_9bb9.mp4 for BK_9BB9).
  // A cutscene mod's clip first (the last in the load order wins), then the PS2 folder.
  std::filesystem::path file;
  for (auto it = overrides.rbegin(); it != overrides.rend() && file.empty(); ++it) file = FindMovie(*it, name);
  if (!file.empty()) REXLOG_INFO("PS2 movie: {} from a cutscene mod ({})", name, file.parent_path().string());
  const auto folder = Folder();
  if (file.empty() && ps2 && !folder.empty()) file = FindMovie(folder, name);
  if (file.empty()) {
    REXLOG_INFO("PS2 movie: no local {} for {} -> {}; normal load", name, from, to);
    return false;
  }
  auto job = std::make_shared<Job>();
  job->file = file;
  { std::lock_guard lock(job_mutex); current_job = job; }
  REXLOG_INFO("PS2 movie: story {} -> {} world {:08X} {} ({})", from, to, world, file.filename().string(),
              DecoderName());
  RequestUiPaint();
  std::thread worker([job] { Decode(job); });
  const auto started = std::chrono::steady_clock::now();
  // Only the level loader waits. The presenter continues painting on its UI
  // thread. No game time/AI dt is changed, and the original request is retained.
  loader_waiting = true;
  // Task 293: story play only; a chapter-select run's end (the Score pages, task 256) and everything else
  // keep waiting before the load.
  if (REXCVAR_GET(kk_ps2_cutscene_load_during) && Word(base, g + 0x5668) == 0) {
    HoldGameAudio(true);
    // Task 294: let the decoder go now (it keeps its own Job, as WaitForClip does). A joinable std::thread
    // parked in this static would call std::terminate if the program ended while the level loads.
    worker.detach();
    pending = PendingClip{job, std::thread{}, started};
    REXLOG_INFO("PS2 movie: the level loads during the clip (kk_ps2_cutscene_load_during){}",
                holding_audio ? "; the game's sound is held until it ends" : "");
    return true;
  }
  WaitForClip(job, worker, started);
  return true;
}

void AfterLoad() {
  if (!pending.job) return;
  PendingClip clip = std::move(pending);
  pending = PendingClip{};
  const auto loaded = std::chrono::steady_clock::now();
  const bool clip_over = clip.job->done.load();
  WaitForClip(clip.job, clip.worker, clip.started);
  HoldGameAudio(false);
  const auto now = std::chrono::steady_clock::now();
  REXLOG_INFO("PS2 movie: level loaded {:.2f} s into the clip; {}", std::chrono::duration<double>(loaded - clip.started).count(),
              clip_over ? "the clip had already ended (the loading screen showed after it)"
                        : fmt::format("the game went on {:.2f} s later, when the clip ended",
                                      std::chrono::duration<double>(now - loaded).count()));
}

void Shutdown() {
#if defined(KK_DEV_TOOLS)
  DevStopPreparationTrace();
#endif
  shutting_down = true;
  std::shared_ptr<Job> job;
  { std::lock_guard lock(job_mutex); job = current_job; }
  End(job, "shutdown");
}
}  // namespace kk::movies
