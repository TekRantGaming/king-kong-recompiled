// The live main menu scene (task 330): the rules, without the game or the launcher (tested in
// kk/tests/menu_scene_tests.cpp). menu_scene.h says how it works.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace kk::menu_scene {

// What the launcher knows as it opens.
struct StartFacts {
  bool files_ok = true;          // the game files are installed
  bool native_renderer = true;   // the game runs on the native renderer (its shaders are prepared once)
  bool shaders_prepared = true;  // this version's one shader preparation has run (shader_prepared.h)
  bool shader_job = false;       // a shader job is running or about to (setup, update, Rebuild)
  bool setup = false;            // the first-run setup or the New in this update screen is up
  bool low_end = false;          // kk_hardware_tier "low" (a Steam Deck, a GPU under 4 GB, ...)
  bool off_this_session = false; // --kk_menu_scene_off (a launcher restarted for a Rebuild)
  bool failed_this_version = false;  // the scene did not come up last time in this version
};

enum class Reason {
  kLive,
  kNoFiles,
  kNotNative,
  kShadersNotPrepared,
  kShaderJob,
  kSetup,
  kLowEnd,
  kOffThisSession,
  kFailedBefore,
};

inline Reason Decide(const StartFacts& f) {
  if (!f.files_ok) return Reason::kNoFiles;
  if (!f.native_renderer) return Reason::kNotNative;
  if (!f.shaders_prepared) return Reason::kShadersNotPrepared;
  if (f.shader_job) return Reason::kShaderJob;
  if (f.setup) return Reason::kSetup;
  if (f.low_end) return Reason::kLowEnd;
  if (f.off_this_session) return Reason::kOffThisSession;
  if (f.failed_this_version) return Reason::kFailedBefore;
  return Reason::kLive;
}

inline const char* ReasonText(Reason r) {
  switch (r) {
    case Reason::kLive: return "live";
    case Reason::kNoFiles: return "no game files";
    case Reason::kNotNative: return "not the native renderer";
    case Reason::kShadersNotPrepared: return "shaders not prepared for this version";
    case Reason::kShaderJob: return "a shader job runs";
    case Reason::kSetup: return "setup or the New in this update screen";
    case Reason::kLowEnd: return "low-end hardware tier";
    case Reason::kOffThisSession: return "off for this launcher (restarted for a shader rebuild)";
    case Reason::kFailedBefore: return "the scene did not come up last time in this version";
  }
  return "?";
}

// The stills only for now: a fresh install's game files, its setup and shader preparation (or an update's) are
// still to come in this launcher. Once they are done (the launcher is back on Home) the scene starts after all.
inline bool MayStartLater(Reason r) {
  return r == Reason::kNoFiles || r == Reason::kShadersNotPrepared || r == Reason::kShaderJob || r == Reason::kSetup;
}

// The failure marker (<user data>/menu-scene.txt) holds "starting <version>" from the scene's start until it has
// been shown, then nothing. A launcher that finds "starting" of its own version did not survive the scene's
// start: the stills for the rest of this version ("failed <version>").
enum class MarkerState { kNone, kFailedThisVersion };
inline MarkerState ReadMarker(std::string_view text, std::string_view version) {
  for (std::string_view word : {std::string_view("starting "), std::string_view("failed ")}) {
    if (text.substr(0, word.size()) == word) {
      std::string_view v = text.substr(word.size());
      while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.remove_suffix(1);
      if (v == version) return MarkerState::kFailedThisVersion;
    }
  }
  return MarkerState::kNone;
}

// The game thread's view of the boot, from the universe's variables (frames counted in pad reads).
// The title (level 2) waits for Start; the start menu (level 3) is the scene: the boat on the water.
constexpr uint32_t kTitleLevel = 2, kStartMenuLevel = 3;
// Start held for 3 reads in every 20, from the 10th read on the title, until level 3 is asked for.
inline bool PressStartOnTitle(int title_reads) { return title_reads >= 10 && (title_reads - 10) % 20 < 3; }
// Shown once the start menu has been up this many reads (about 1.5 s at 30): its fade in is over.
constexpr int kReadyReads = 45;

struct GuestView {
  uint32_t current_level = 0;  // G+0x18
  bool request = false;        // G+4: a level change is waiting
  uint32_t requested = 0;      // G+8
};

struct Tracker {
  int title_reads = 0;
  int menu_reads = 0;
  bool start_pressed = false;  // this read: Start is down
  // The scene is up (the start menu's level, nothing else loading).
  bool Ready() const { return menu_reads >= kReadyReads; }
  void Step(const GuestView& g) {
    start_pressed = false;
    if (g.current_level == kStartMenuLevel && !g.request) {
      menu_reads = std::min(menu_reads + 1, 1 << 20);
      return;
    }
    menu_reads = 0;
    if (g.current_level == kTitleLevel && !(g.request && g.requested == kStartMenuLevel)) {
      start_pressed = PressStartOnTitle(title_reads);
      ++title_reads;
    }
  }
};

// The scene's frame rate cap (frame_stats.cpp's limiter; kk_frame_rate itself is never changed): the player's cap
// up to 60 (unlimited counts as 60: a menu needs no more), and 10 while the launcher's window is minimised or the
// launcher is handing over to the game's new process (the scene then costs next to nothing).
constexpr int kSceneMaxFps = 60, kSceneThrottledFps = 10;
inline int SceneFrameCap(int player_cap, bool throttled) {
  if (throttled) return kSceneThrottledFps;
  return player_cap <= 0 || player_cap > kSceneMaxFps ? kSceneMaxFps : player_cap;
}

// The launcher's mix of the scene over the stills (0 stills .. 1 scene): about 0.6 s either way.
inline float StepMix(float mix, bool show, float dt) {
  const float step = std::clamp(dt, 0.0f, 0.1f) / 0.6f;
  return std::clamp(show ? mix + step : mix - step, 0.0f, 1.0f);
}

}  // namespace kk::menu_scene
