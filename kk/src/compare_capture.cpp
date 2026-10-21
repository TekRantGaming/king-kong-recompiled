// Task 354: the owner's comparison capture for the v2.0.0 website's before/after sliders (private build aid; see
// compare_capture.h). Nothing here runs, draws or reads keys unless the game started with --kk_compare_capture=true.
//
// Keys (in play and in photo mode; chosen because nothing else uses them: F2 metrics, F3 SDK overlay, F6/F7/F8 the
// graphics panel, F9/F10 free camera and HUD, photo mode's keys, the game's keyboard controls):
//   Insert     preview: the live game flips between ORIGINAL and MAX (a label at the top left says which)
//   Home       capture the shot on the card: freeze, ORIGINAL picture, MAX picture, back to the look shown, resume
//   Page Down  skip to the next shot on the card
//   Page Up    back to the previous shot
//
// The freeze is photo mode's (the game's own pause; photo_mode.cpp): Home presses Start for the player, waits for
// the pause and opens photo mode quietly (no panel); its free camera starts at the game's view, so the camera does
// not move. Photo mode leaves out the HUD and Jack's first-person arms and weapon. Pressed in photo mode, Home takes
// photo mode's frozen view as it is and stays in photo mode afterwards. Start skips a cutscene, so a capture in a
// cutscene would skip it: no pause page appears there and the capture gives up (the card says so).
//
// Settling: after each look is put on, at least kMinFrames guest frames (the upscaler made again, the GI, AO and
// temporal histories refilled), then pictures two frames apart until two in a row differ by at most kSettled (mean
// of every channel, 0 to 255); the closest pair after kMaxExtraFrames otherwise (rain and water keep moving while
// paused), noted in settings.txt.
//
// Pictures: <user data>/screenshots/compare/<id>-<YYYYMMDD-HHMMSS>/original.png, max.png (the guest output: full
// output resolution, never the UI) and settings.txt (both looks' settings as they were for each picture).

#include "compare_capture.h"
#include "compare_capture_logic.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "capture_png.h"
#include "freecam.h"
#include "frame_stats.h"
#include "launcher_ui.h"
#include "mods.h"
#include "hooks/kk_native_ui.h"
#include "photo_mode.h"
#include "play_internal.h"
#include "play_session.h"
#include "settings.h"
#include "ui_paint.h"

#if defined(KK_DEV_TOOLS) && defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
namespace kk {
double DevSecondsSinceMenu();
}
#endif

REXCVAR_DEFINE_BOOL(kk_compare_capture, false, "KK/Debug",
                    "Private: the comparison capture (task 354): Original/Max looks, shot card, capture key");

namespace kk::compare {
namespace {

constexpr int kOriginal = 0, kMax = 1;
constexpr uint64_t kMinFramesOriginal = 30;  // the bilinear stretch: no history; the effects switched off
constexpr uint64_t kMinFramesMax = 120;      // DLAA/FSR/XeSS history, GI and AO temporal accumulation
constexpr uint64_t kMaxExtraFrames = 360;
constexpr int kSettled = 1;  // 90% of the pixels of two pictures in a row differ by at most this (0 to 255)

using Clock = std::chrono::steady_clock;

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }

std::mutex g_mutex;  // the grabber, the baselines, the status
std::function<bool(rex::ui::RawImage&)> g_grab;
std::map<std::string, bool, std::less<>> g_baselined;
std::atomic<int> g_look{kMax};
std::atomic<bool> g_capturing{false};
std::atomic<int64_t> g_press_until_ms{0};
std::string g_upscaler;    // the Max look's (fixed at the start)
std::string g_uhd_folder;  // the Kong UHD Textures pack's folder (with pack.txt); empty: not installed
std::map<std::string, std::string, std::less<>> g_owner;  // every setting changed: the player's own value
std::string g_status;
Clock::time_point g_status_until{};
std::filesystem::path g_user_dir;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void SetStatus(std::string text, int seconds = 8) {
  std::lock_guard lock(g_mutex);
  g_status = std::move(text);
  g_status_until = Clock::now() + std::chrono::seconds(seconds);
  RequestUiPaint();
}

// A setting changed for this run only: every save of the port keeps the player's own value (settings.h).
void Set(const char* name, const std::string& value) {
  if (!rex::cvar::GetFlagInfo(name)) return;
  {
    std::lock_guard lock(g_mutex);
    if (g_baselined.try_emplace(name, true).second) {
      SetTemporarySettingBaseline(name, Get(name));
      g_owner.try_emplace(name, Get(name));
    }
  }
  if (Get(name) != value && rex::cvar::SetFlagByName(name, value)) SetTemporarySettingTrialValue(name, Get(name));
}

int OutputHeight() {
  const auto size = ParseSize(Get("native_upscaler_output"));
  return size.second > 0 ? size.second : 720;
}

std::vector<Setting> LookSettings(int look) {
  return look == kOriginal ? OriginalSettings(OutputHeight(), g_uhd_folder) : MaxSettings(g_upscaler, g_uhd_folder);
}

// Leaving the tool (the play session ended): the player's own settings again, the texture pack included.
void RestoreOwner() {
  std::map<std::string, std::string, std::less<>> owner;
  {
    std::lock_guard lock(g_mutex);
    owner = g_owner;
  }
  for (const auto& [name, value] : owner)
    if (Get(name.c_str()) != value && rex::cvar::SetFlagByName(name, value)) SetTemporarySettingTrialValue(name, value);
  REXLOG_INFO("KK compare: the player's own settings back ({} settings)", owner.size());
}

constexpr const char* kNoUhd = "UHD pack not installed: MAX uses the game's textures";

void ApplyLook(int look) {
  for (const auto& [name, value] : LookSettings(look)) Set(name, value);
#if defined(KK_DEV_TOOLS) && defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  REXLOG_INFO("KK compare: {} look on (frame {}, worst frame {:.1f} ms, private {} MiB, working set {} MiB)",
              look == kOriginal ? "Original" : "Max", GuestFrameCount(), GuestWorstFrameMs(),
              pmc.PrivateUsage >> 20, pmc.WorkingSetSize >> 20);
#else
  REXLOG_INFO("KK compare: {} look on", look == kOriginal ? "Original" : "Max");
#endif
}

std::string LookLabel(int look) {
  const std::string out = Get("native_upscaler_output");
  if (look == kOriginal) {
    const auto size = ParseSize(out);
    const float r = OriginalModeFor(OutputHeight()).ratio;
    char in[48];
    std::snprintf(in, sizeof(in), "%dx%d", int(std::lround(size.first / r)), int(std::lround(size.second / r)));
    return "ORIGINAL  " + std::string(in) + " stretched to " + out;
  }
  std::string up = g_upscaler;
  std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return "MAX  " + out + " native, " + up + (g_upscaler == "dlss" ? " (DLAA)" : " native AA") +
         (g_uhd_folder.empty() ? std::string("  (") + kNoUhd + ")" : std::string(", UHD textures"));
}

// ------------------------------------------------------------------ the shot list ---

std::mutex g_shots_mutex;
std::vector<Shot> g_shots;
int g_index = 0;
std::filesystem::file_time_type g_shots_time{};
bool g_shots_from_file = false;
Clock::time_point g_shots_checked{};
std::map<std::string, int> g_taken;  // pictures already in screenshots/compare, by id

std::filesystem::path ShotsFile() { return rex::filesystem::GetExecutableFolder() / "compare-shots.txt"; }
std::filesystem::path CompareDir() { return g_user_dir / "screenshots" / "compare"; }

void CountTaken() {
  std::map<std::string, int> taken;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(CompareDir(), ec)) {
    if (!e.is_directory(ec)) continue;
    const std::string name = e.path().filename().string();
    const auto dash = name.size() > 16 ? name.size() - 16 : std::string::npos;  // "-YYYYMMDD-HHMMSS"
    if (dash != std::string::npos && name[dash] == '-') ++taken[name.substr(0, dash)];
  }
  std::lock_guard lock(g_shots_mutex);
  g_taken = std::move(taken);
}

// Read again when the file changed (checked once a second): the owner edits it while the game runs.
void RefreshShots() {
  {
    std::lock_guard lock(g_shots_mutex);
    if (Clock::now() - g_shots_checked < std::chrono::seconds(1) && !g_shots.empty()) return;
    g_shots_checked = Clock::now();
  }
  std::error_code ec;
  const auto path = ShotsFile();
  const auto time = std::filesystem::last_write_time(path, ec);
  std::vector<Shot> shots;
  bool from_file = false;
  if (!ec) {
    {
      std::lock_guard lock(g_shots_mutex);
      if (g_shots_from_file && time == g_shots_time && !g_shots.empty()) return;
    }
    std::ifstream f(path, std::ios::binary);
    std::stringstream text;
    text << f.rdbuf();
    shots = ParseShots(text.str());
    from_file = !shots.empty();
  }
  if (shots.empty()) shots = DefaultShots();
  std::lock_guard lock(g_shots_mutex);
  if (!from_file && !g_shots_from_file && !g_shots.empty()) return;
  g_shots = std::move(shots);
  g_shots_from_file = from_file;
  g_shots_time = time;
  g_index = std::clamp(g_index, 0, int(g_shots.size()) - 1);
  REXLOG_INFO("KK compare: {} shots ({})", g_shots.size(), from_file ? path.string() : "the built-in list");
}

void StepShot(int delta) {
  std::lock_guard lock(g_shots_mutex);
  if (g_shots.empty()) return;
  g_index = std::clamp(g_index + delta, 0, int(g_shots.size()) - 1);
}

// ------------------------------------------------------------------ capturing ---

bool Grab(rex::ui::RawImage& image) {
  std::function<bool(rex::ui::RawImage&)> grab;
  {
    std::lock_guard lock(g_mutex);
    grab = g_grab;
  }
  return grab && grab(image) && image.width && image.height;
}

// Waits for `frames` more guest frames (false after `timeout_ms` without them: the game stopped drawing).
bool WaitFrames(uint64_t frames, int64_t timeout_ms = 15000) {
  const uint64_t until = GuestFrameCount() + frames;
  const int64_t end = NowMs() + timeout_ms;
  while (GuestFrameCount() < until) {
    if (NowMs() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

template <typename Pred>
bool WaitFor(Pred pred, int64_t timeout_ms) {
  const int64_t end = NowMs() + timeout_ms;
  while (!pred()) {
    if (NowMs() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

struct Settled {
  rex::ui::RawImage image;
  bool settled = false;
  bool pack_done = true;    // the texture pack had nothing left to stream in (Max with the UHD pack)
  uint64_t pack_ready = 0;  // pack replacements uploaded meanwhile
  uint64_t frames = 0;  // guest frames from the look's change to the picture
  FrameDifference difference;
};

// The texture pack is quiet once the renderer reports nothing pending (no reads queued or in flight, no image
// waiting, no upload) for kPackQuietFrames frames in a row: longer than its rescan (every 60 frames at most), so
// textures first drawn meanwhile have been asked for too. The GI, lighting and upscaler workers must be done too.
constexpr uint64_t kPackQuietFrames = 75;
constexpr uint64_t kMaxPackFrames = 2400;  // 40 s at 60 frames a second

Settled Settle(uint64_t min_frames, bool wait_pack) {
  Settled out;
  const uint64_t start = GuestFrameCount();
  const uint64_t ready0 = nr::kk::EffectTimes().pack_ready;
  uint64_t quiet_since = start;
  bool quiet = !wait_pack;
  auto pack_quiet = [&] {
    if (!wait_pack) return true;
    const NrEffectStats st = nr::kk::EffectTimes();
    if (!st.size || st.pack_pending || st.preparing) {
      quiet_since = GuestFrameCount();
      quiet = false;
    } else {
      quiet = GuestFrameCount() - quiet_since >= kPackQuietFrames;
    }
    return quiet;
  };
  if (!WaitFrames(min_frames)) return out;
  if (wait_pack) {
    const uint64_t give_up = GuestFrameCount() + kMaxPackFrames;
    while (!pack_quiet() && GuestFrameCount() < give_up) WaitFrames(1, 5000);
    out.pack_done = quiet;
    out.pack_ready = nr::kk::EffectTimes().pack_ready - ready0;
  }
  rex::ui::RawImage prev;
  if (!Grab(prev)) return out;
  FrameDifference best;
  best.mean = 1e9;
  const uint64_t end = GuestFrameCount() + kMaxExtraFrames;
  while (GuestFrameCount() < end) {
    if (!WaitFrames(2)) break;
    rex::ui::RawImage cur;
    if (!Grab(cur)) break;
    if (cur.width != prev.width || cur.height != prev.height) {
      prev = std::move(cur);
      continue;
    }
    const FrameDifference d = Difference(prev.data.data(), cur.data.data(), int(cur.width), int(cur.height),
                                         prev.stride, cur.stride);
    if (d.p90 < best.p90 || (d.p90 == best.p90 && d.mean < best.mean)) {
      best = d;
      out.image = cur;
      out.frames = GuestFrameCount() - start;
      out.difference = d;
    }
    if (d.p90 <= kSettled && pack_quiet()) {
      out.settled = true;
      break;
    }
    prev = std::move(cur);
  }
  return out;
}

std::string Timestamp(const char* format) {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char text[64];
  std::strftime(text, sizeof(text), format, &tm);
  return text;
}

// The settings that stay the same in both looks (restart-only ones, the player's tuning), for settings.txt.
constexpr const char* kContext[] = {
    "native_backend", "native_upscaler_output", "native_render_scale", "native_upscaler_point", "kk_render_quality",
    "present_effect", "resolution_scale", "kk_original_look", "native_texture_pack_folder", "kk_brightness",
    "kk_gamma", "kk_hdr", "kk_frame_rate", "native_gi_intensity", "native_ssr_strength", "native_ssr_ripple",
    "native_volumetric_strength", "native_shadow_softness", "native_upscaler_reactive", "native_ao_method",
    "native_ao_apply"};

std::string SettingsText(int look) {
  std::string text;
  for (const auto& [name, value] : LookSettings(look)) {
    (void)value;
    text += std::string(name) + " = " + Get(name) + "\n";
  }
  return text;
}

void SaveSettingsText(const std::filesystem::path& path, const Shot& shot, const std::string& original,
                      const std::string& max, const Settled& o, const Settled& m) {
  std::ostringstream t;
  auto settle = [](const Settled& s) {
    char line[200];
    std::snprintf(line, sizeof(line),
                  "%s after %llu frames (two pictures in a row: 90%% of pixels within %d of 255, mean %.3f)",
                  s.settled ? "settled" : "NOT fully settled (closest pair taken)", (unsigned long long)s.frames,
                  s.difference.p90, s.difference.mean);
    return std::string(line);
  };
  t << "King Kong Recompiled comparison capture (task 354)\n"
    << "shot: " << shot.id << " | " << shot.feature << "\n"
    << "taken: " << Timestamp("%Y-%m-%d %H:%M:%S") << "\n"
    << "pictures: " << o.image.width << "x" << o.image.height << " (original), " << m.image.width << "x"
    << m.image.height << " (max)\n"
    << "original: " << settle(o) << "\n"
    << "max: " << settle(m) << "\n"
    << "max texture pack: "
    << (g_uhd_folder.empty() ? std::string(kNoUhd)
                             : "Kong UHD Textures (" + g_uhd_folder + "): " +
                                   (m.pack_done ? "finished streaming in" : "NOT finished streaming in (gave up waiting)") +
                                   ", " + std::to_string(m.pack_ready) + " replacements uploaded while waiting")
    << "\noriginal texture pack: off (the game's textures)\n\n[original]\n"
    << original << "\n[max]\n"
    << max << "\n[the same in both]\n";
  for (const char* name : kContext)
    if (rex::cvar::GetFlagInfo(name)) t << name << " = " << Get(name) << "\n";
  std::ofstream(path, std::ios::binary | std::ios::trunc) << t.str();
}

void CaptureThread(Shot shot, int index, int shown_look) {
  bool froze = false;
  const bool photo_before = photo::Active();
  auto finish = [&](const std::string& status) {
    ApplyLook(shown_look);
    photo::SetQuiet(false);
    if (froze) photo::Exit(true);
    g_capturing = false;
    SetStatus(status);
    REXLOG_INFO("KK compare: {}", status);
  };
  REXLOG_INFO("KK compare: capture {} ({})", shot.id, photo_before ? "in photo mode" : "pausing the game");
  if (!photo_before) {
    g_press_until_ms = NowMs() + 250;
    if (!WaitFor([] { return photo::Available(); }, 4000))
      return finish("Could not freeze here: the game did not pause (a cutscene, a loading screen or a menu?)");
    photo::SetQuiet(true);
    photo::Enter();
    if (!WaitFor([] { return photo::Active(); }, 6000)) return finish("Could not freeze here: photo mode did not open");
    froze = true;
  } else {
    photo::SetQuiet(true);
  }
  WaitFrames(8);  // the free camera has the game's view, the pause's dimming is taken back

  ApplyLook(kOriginal);
  Settled o = Settle(kMinFramesOriginal, false);
  const std::string original_settings = SettingsText(kOriginal);
  ApplyLook(kMax);
  Settled m = Settle(kMinFramesMax, !g_uhd_folder.empty());
  const std::string max_settings = SettingsText(kMax);
  if (!o.image.width || !m.image.width) return finish("Capture failed: no picture from the game");

  std::error_code ec;
  const auto dir = CompareDir() / (shot.id + "-" + Timestamp("%Y%m%d-%H%M%S"));
  std::filesystem::create_directories(dir, ec);
  const bool ok = art::detail::SaveCapturePng(o.image, dir / "original.png") &&
                  art::detail::SaveCapturePng(m.image, dir / "max.png");
  SaveSettingsText(dir / "settings.txt", shot, original_settings, max_settings, o, m);
  REXLOG_INFO("KK compare: {} original {} frames p90 {} mean {:.3f}{}, max {} frames p90 {} mean {:.3f}{}", dir.string(),
              o.frames, o.difference.p90, o.difference.mean, o.settled ? "" : " (not settled)", m.frames,
              m.difference.p90, m.difference.mean, m.settled ? "" : " (not settled)");
  if (!ok) return finish("Capture failed: could not write " + dir.string());
  CountTaken();
  {
    std::lock_guard lock(g_shots_mutex);
    if (g_index == index && index + 1 < int(g_shots.size())) ++g_index;
  }
  finish("Saved " + dir.filename().string() +
         (o.settled && m.settled && m.pack_done ? "" : "  (not fully settled: see settings.txt)"));
}

// ------------------------------------------------------------------ the card ---

class Card final : public rex::ui::ImGuiDialog, public rex::ui::WindowInputListener {
 public:
  Card(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) : ImGuiDialog(drawer), window_(window) {
    window_->AddInputListener(this, 1250);  // above the SDK's keyboard driver and the graphics panel
  }
  ~Card() override { window_->RemoveInputListener(this); }

  bool NeedsContinuousPaint() override { return play::SessionActive() || g_capturing.load(); }

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    using rex::ui::VirtualKey;
    const VirtualKey k = e.virtual_key();
    if (k != VirtualKey::kInsert && k != VirtualKey::kHome && k != VirtualKey::kPrior && k != VirtualKey::kNext) return;
    if (!InPlay()) return;
    e.set_handled(true);
    if (e.prev_state() || g_capturing.load()) return;
    if (k == VirtualKey::kInsert) {
      const int look = g_look.load() == kOriginal ? kMax : kOriginal;
      g_look = look;
      ApplyLook(look);
    } else if (k == VirtualKey::kHome) {
      Shot shot;
      int index = 0;
      {
        std::lock_guard lock(g_shots_mutex);
        if (g_shots.empty()) return;
        index = g_index;
        shot = g_shots[size_t(index)];
      }
      g_capturing = true;
      RequestUiPaint();
      std::thread(CaptureThread, shot, index, g_look.load()).detach();
    } else {
      StepShot(k == VirtualKey::kNext ? 1 : -1);
    }
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    using rex::ui::VirtualKey;
    const VirtualKey k = e.virtual_key();
    if ((k == VirtualKey::kInsert || k == VirtualKey::kHome || k == VirtualKey::kPrior || k == VirtualKey::kNext) &&
        InPlay())
      e.set_handled(true);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
#if defined(KK_DEV_TOOLS) && defined(_WIN32)
    DevKeys();
#endif
    if (!play::SessionActive() && applied_ && !g_capturing.load()) {  // the tool is left: the player's own again
      applied_ = false;
      RestoreOwner();
    }
    if (!InPlay()) return;
    if (!applied_) {  // the plugin's settings exist now: the look again, complete
      applied_ = true;
      ApplyLook(g_look.load());
      CountTaken();
    }
    RefreshShots();
    if (g_capturing.load() || freecam::HideHud()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = std::clamp(vp->Size.y / 720.0f, 0.75f, 4.0f);
    const auto fonts = ui::GetFonts();
    ImFont* big = fonts.semibold ? fonts.semibold : ImGui::GetFont();
    ImFont* text = fonts.text ? fonts.text : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // The look, top left.
    const std::string label = LookLabel(g_look.load());
    const float ls = 15 * s;
    const ImVec2 lsize = ui::TextSize(text, ls, label);
    const ImVec2 lp(vp->Pos.x + 12 * s, vp->Pos.y + 12 * s);
    dl->AddRectFilled(ImVec2(lp.x - 6 * s, lp.y - 4 * s), ImVec2(lp.x + lsize.x + 6 * s, lp.y + lsize.y + 4 * s),
                      IM_COL32(0, 0, 0, 170), 4 * s);
    dl->AddText(text, ls, lp, g_look.load() == kOriginal ? IM_COL32(255, 210, 120, 255) : IM_COL32(140, 220, 255, 255),
                label.c_str());

    // The card, top centre.
    Shot shot;
    int index = 0, count = 0, taken = 0;
    {
      std::lock_guard lock(g_shots_mutex);
      count = int(g_shots.size());
      if (count) {
        index = g_index;
        shot = g_shots[size_t(index)];
        const auto it = g_taken.find(shot.id);
        taken = it != g_taken.end() ? it->second : 0;
      }
    }
    if (!count) return;
    const float width = std::min(vp->Size.x * 0.62f, 980 * s);
    const float pad = 12 * s;
    const std::string title = "Shot " + std::to_string(index + 1) + " of " + std::to_string(count) + ": " + shot.feature;
    const std::string keys = "Insert: Original / Max    Home: capture    Page Down: skip    Page Up: back" +
                             (taken ? "    Taken: " + std::to_string(taken) : std::string()) +
                             (g_uhd_folder.empty() ? std::string("\n") + kNoUhd : std::string());
    std::string status;
    {
      std::lock_guard lock(g_mutex);
      if (Clock::now() < g_status_until) status = g_status;
    }
    const float ts = 26 * s, hs = 18 * s, ks = 14 * s;
    const ImVec2 tsize = ui::TextSize(big, ts, title, width - 2 * pad);
    const ImVec2 hsize = shot.hint.empty() ? ImVec2(0, 0) : ui::TextSize(text, hs, shot.hint, width - 2 * pad);
    const ImVec2 ksize = ui::TextSize(text, ks, keys, width - 2 * pad);
    const ImVec2 ssize = status.empty() ? ImVec2(0, 0) : ui::TextSize(text, ks, status, width - 2 * pad);
    const float gap = 6 * s;
    const float height = pad * 2 + tsize.y + (hsize.y ? gap + hsize.y : 0) + gap + ksize.y + (ssize.y ? gap + ssize.y : 0);
    const ImVec2 p0(vp->Pos.x + (vp->Size.x - width) * 0.5f, vp->Pos.y + 12 * s);
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), IM_COL32(10, 12, 16, 200), 8 * s);
    float y = p0.y + pad;
    dl->AddText(big, ts, ImVec2(p0.x + pad, y), IM_COL32(255, 255, 255, 255), title.c_str(), nullptr, width - 2 * pad);
    y += tsize.y;
    if (hsize.y) {
      y += gap;
      dl->AddText(text, hs, ImVec2(p0.x + pad, y), IM_COL32(225, 225, 225, 255), shot.hint.c_str(), nullptr,
                  width - 2 * pad);
      y += hsize.y;
    }
    y += gap;
    dl->AddText(text, ks, ImVec2(p0.x + pad, y), IM_COL32(170, 190, 210, 255), keys.c_str(), nullptr, width - 2 * pad);
    y += ksize.y;
    if (ssize.y) {
      y += gap;
      dl->AddText(text, ks, ImVec2(p0.x + pad, y), IM_COL32(150, 230, 150, 255), status.c_str(), nullptr,
                  width - 2 * pad);
    }
  }

 private:
#if defined(KK_DEV_TOOLS) && defined(_WIN32)
  // KK_DEV_COMPARE_KEYS="<s>:<key>,..." (insert, home, pgdn, pgup) on the script clock (seconds since the chapter's
  // level loaded): the keys through the same handler, without focus (test runs in a background window).
  void DevKeys() {
    static const char* plan = std::getenv("KK_DEV_COMPARE_KEYS");
    static size_t pos = 0;
    if (!plan || !plan[pos]) return;
    const double t = DevSecondsSinceMenu();
    if (t < 0) return;
    const std::string rest(plan + pos);
    const size_t colon = rest.find(':');
    if (colon == std::string::npos) return;
    if (t < std::atof(rest.substr(0, colon).c_str())) return;
    const size_t comma = rest.find(',');
    const std::string key = rest.substr(colon + 1, comma == std::string::npos ? std::string::npos : comma - colon - 1);
    pos += comma == std::string::npos ? rest.size() : comma + 1;
    using rex::ui::VirtualKey;
    const VirtualKey vk = key == "insert" ? VirtualKey::kInsert : key == "home" ? VirtualKey::kHome
                        : key == "pgdn" ? VirtualKey::kNext : key == "pgup" ? VirtualKey::kPrior : VirtualKey::kNone;
    rex::ui::KeyEvent down(window_, vk, 1, false, false, false, false, false);
    OnKeyDown(down);
    rex::ui::KeyEvent up(window_, vk, 1, true, false, false, false, false);
    OnKeyUp(up);
    REXLOG_INFO("KK compare dev key: {} at {:.1f} s (handled {})", key, t, down.is_handled());
  }
#endif
  // In play, or in photo mode over the game's pause; not the launcher, the pause menu or a cover screen.
  static bool InPlay() { return play::SessionActive() && !play::CoverScreen() && !play::PauseMenuOpen(); }

  rex::ui::Window* window_;
  bool applied_ = false;
};

}  // namespace

bool Enabled() { return REXCVAR_GET(kk_compare_capture); }

void ApplyAtGameStart(const std::filesystem::path& user_data) {
  if (!Enabled()) return;
  // The Kong UHD Textures pack (content.h kUhd): the Workshop installs it as the texture pack mod of that name.
  g_uhd_folder.clear();
  for (const auto& m : mods::Scan(user_data))
    if (m.type == mods::Type::kTexturePack && m.Usable() && !m.content.empty() &&
        (m.name == "Kong UHD Textures" || m.folder == "Kong UHD Textures")) {
      g_uhd_folder = m.content.string();
      break;
    }
  g_upscaler = BestUpscaler(Get("kk_upscalers_offered"));
  // Restart-only: the Remastered look (the Original one is put on live instead), no FSR 1, the native render
  // quality, and a temporal upscaler from the start, so the frame is the output size in both looks.
  Set("kk_original_look", "false");
  Set("present_effect", "bilinear");
  Set("kk_render_quality", "native");
  ApplyLook(g_look.load());
  REXLOG_INFO("KK compare: on (Max upscaler {}, UHD pack {})", g_upscaler,
              g_uhd_folder.empty() ? std::string("not installed: the game's textures") : g_uhd_folder);
}

void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, const std::filesystem::path& user_dir) {
  if (!Enabled() || !drawer || !window) return;
  static bool created = false;
  if (created) return;
  created = true;
  g_user_dir = user_dir;
  RefreshShots();
  new Card(drawer, window);  // lives for the rest of the session
}

void SetGrabber(std::function<bool(rex::ui::RawImage&)> grab) {
  std::lock_guard lock(g_mutex);
  g_grab = std::move(grab);
}

bool PressStart() { return NowMs() < g_press_until_ms.load(); }

}  // namespace kk::compare
