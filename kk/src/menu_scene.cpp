// The live main menu scene (task 330): see menu_scene.h.

#include "menu_scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdlib>
#include <bit>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include "ui_paint.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif


REXCVAR_DEFINE_BOOL(kk_menu_scene_off, false, "KK",
                    "Internal: no live menu scene in this launcher (passed on the command line by a launcher that "
                    "restarts itself for a shader rebuild; the scene starts once the rebuild is done). Never saved.");
REXCVAR_DEFINE_BOOL(kk_menu_scene_retry, false, "KK",
                    "Internal: this launcher is the live menu scene's second attempt (passed on the command line by a "
                    "launcher whose scene did not come up in time). Never saved.");

namespace kk::menu_scene {
namespace {

std::atomic<uint32_t> g_universe{0};  // the universe's variables G, as uni_go last saw them

std::atomic<bool> g_active{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_shown_once{false};
std::mutex g_mutex;
std::filesystem::path g_marker;
int g_failed_starts = 0;               // failed starts in a row before this launcher's (the marker)
std::atomic<bool> g_attempt_failed{false};  // this launcher's attempt ran out of time (NoteAttemptFailed)
#if defined(KK_DEV_TOOLS)
std::chrono::steady_clock::time_point g_begun;
// Task 349c test aids: KK_DEV_SCENE_DELAY=<seconds> holds the scene's Ready that long after its start (a slow
// start); KK_DEV_SCENE_FAIL=first never lets the first attempt's scene be ready, =both neither attempt's.
bool DevHoldsReady() {
  if (const char* d = std::getenv("KK_DEV_SCENE_DELAY"); d && *d &&
      std::chrono::steady_clock::now() - g_begun < std::chrono::duration<double>(std::atof(d)))
    return true;
  if (const char* f = std::getenv("KK_DEV_SCENE_FAIL")) {
    if (std::string_view(f) == "both") return true;
    if (std::string_view(f) == "first" && !SecondAttempt()) return true;
  }
  return false;
}
#endif
Tracker g_tracker;  // game thread only

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return std::byteswap(v);
}

std::filesystem::path MarkerPath(const std::filesystem::path& user_dir) { return user_dir / "menu-scene.txt"; }

void WriteMarker(const std::filesystem::path& path, const std::string& text) {
  if (path.empty()) return;
  std::error_code ec;
  if (text.empty()) {
    std::filesystem::remove(path, ec);
    return;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text << "\n";
}

// The scene has been shown: the marker goes (a later crash is not the scene's start).
void NoteShown() {
  if (g_shown_once.exchange(true)) return;
  std::filesystem::path path;
  {
    std::lock_guard lock(g_mutex);
    path = g_marker;
  }
  WriteMarker(path, "");
  REXLOG_INFO("KK menu scene: the start menu scene is up behind the launcher");
}

// Owner, 20 Oct: the scene draws lighter than play (native-renderer/backend/menu_scene_light.h), through the
// native renderer's nr_set_menu_scene_light; the player's settings are not changed.
void SetRendererLight(bool on) {
  using Fn = void (*)(uint32_t);
  Fn fn = nullptr;
#if defined(_WIN32)
  for (const wchar_t* module : {L"rexgpu-nativerd.dll", L"rexgpu-native.dll", L"rexgpu-natived.dll"})
    if (HMODULE m = GetModuleHandleW(module))
      if ((fn = reinterpret_cast<Fn>(GetProcAddress(m, "nr_set_menu_scene_light")))) break;
#else
  for (const char* module : {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"})
    if (void* m = dlopen(module, RTLD_NOW | RTLD_NOLOAD)) {
      fn = reinterpret_cast<Fn>(dlsym(m, "nr_set_menu_scene_light"));
      dlclose(m);  // still loaded by the runtime
      if (fn) break;
    }
#endif
  if (fn) fn(on ? 1u : 0u);
  REXLOG_INFO("KK menu scene: {}", fn ? (on ? "lighter effects for the scene (GI off, AO half, volumetrics off, original "
                                              "shadows and water, game-size shadow maps, no tessellation, upscaler at most Performance)"
                                           : "the player's own effects for the scene")
                                     : "the renderer has no lighter scene mode");
}

}  // namespace

bool SecondAttempt() {
  static const bool second = [] {
    const bool on = REXCVAR_GET(kk_menu_scene_retry);
    rex::cvar::ResetToDefault("kk_menu_scene_retry");  // never saved
    return on;
  }();
  return second;
}

Reason DecideAtLauncherStart(StartFacts facts, const std::filesystem::path& user_dir) {
  // Task 349c: a launcher restarted for a Rebuild leaves the scene off only while the rebuild runs.
  if (REXCVAR_GET(kk_menu_scene_off) && !facts.shader_job) {
    rex::cvar::ResetToDefault("kk_menu_scene_off");
    REXLOG_INFO("KK menu scene: the shader rebuild is done: the scene may start in this launcher");
  }
  facts.off_this_session = facts.off_this_session || REXCVAR_GET(kk_menu_scene_off);
  const auto marker = MarkerPath(user_dir);
  std::error_code ec;
  if (!Active() && !user_dir.empty() && std::filesystem::exists(marker, ec)) {
    std::ifstream in(marker, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    in.close();
    // Task 349c: one failed start no longer means pictures for the rest of the version: the next start tries
    // again; only kMaxFailedStarts in a row (each with its second attempt) give up for this version.
    const MarkerRead read = ReadMarker(text.str(), KK_VERSION, SecondAttempt());
    g_failed_starts = read.failed_starts;
    if (read.gave_up) {
      facts.failed_this_version = true;
      if (text.str().rfind("starting ", 0) == 0) {
        REXLOG_WARN("KK menu scene: {} starts in a row did not get the scene up: pictures for the rest of v{}",
                    read.failed_starts, KK_VERSION);
        WriteMarker(marker, std::string("failed ") + KK_VERSION);
      }
    } else if (read.failed_starts > 0) {
      REXLOG_WARN("KK menu scene: {} (failed starts in a row: {} of {}): trying again",
                  SecondAttempt() ? "this launcher is the second attempt of the same start"
                                  : "the last launcher did not get the scene up",
                  read.failed_starts, kMaxFailedStarts);
    } else if (!SecondAttempt() || text.str().rfind("starting ", 0) != 0) {
      WriteMarker(marker, "");  // another version's
    }
  }
  const Reason r = Decide(facts);
  REXLOG_INFO("KK menu scene: {}", ReasonText(r));
  return r;
}

void Begin(const std::filesystem::path& user_dir) {
  {
    std::lock_guard lock(g_mutex);
    g_marker = user_dir.empty() ? std::filesystem::path() : MarkerPath(user_dir);
    WriteMarker(g_marker, StartingMarker(KK_VERSION, g_failed_starts));
  }
#if defined(KK_DEV_TOOLS)
  g_begun = std::chrono::steady_clock::now();
#endif
  g_active = true;
  // The game's controller reads would otherwise take the mouse for mouse look with Keyboard & mouse on, hiding the
  // launcher's pointer, and keep it after a change to Controller (the driver is not read then). Play is a new process.
  rex::input::mnk::SetMouseLookActive(false);
  REXLOG_INFO("KK menu scene: starting the game behind the launcher ({} attempt)", SecondAttempt() ? "second" : "first");
  bool light = true;
#if defined(KK_DEV_TOOLS)
  if (const char* full = std::getenv("KK_DEV_SCENE_FULL"); full && *full == '1') light = false;  // measurements
#endif
  SetRendererLight(light);
}

bool Active() { return g_active.load(std::memory_order_relaxed); }
bool Ready() { return g_ready.load(std::memory_order_relaxed); }

namespace {
std::atomic<bool> g_throttled{false};
std::mutex g_audio_mutex;
SDL_AudioStream* g_audio_stream = nullptr;
float g_audio_gain = 0.0f, g_audio_set = -1.0f;
void ApplyGainLocked() {
  if (!g_audio_stream || g_audio_set == g_audio_gain) return;
  SDL_SetAudioStreamGain(g_audio_stream, g_audio_gain);
  g_audio_set = g_audio_gain;
}
}  // namespace
void SetAudioStream(void* stream) {
  std::lock_guard lock(g_audio_mutex);
  g_audio_stream = static_cast<SDL_AudioStream*>(stream);
  g_audio_set = -1.0f;
  ApplyGainLocked();
  REXLOG_INFO("KK menu scene: the game's sound plays at the launcher's music volume ({})",
              stream ? "stream found" : "no stream: at full volume");
}
void SetAudioGain(float gain) {
  std::lock_guard lock(g_audio_mutex);
  g_audio_gain = std::clamp(gain, 0.0f, 1.0f);
  if (std::abs(g_audio_gain - g_audio_set) >= 0.002f || g_audio_gain == 0.0f) ApplyGainLocked();
}
void SetThrottled(bool throttled, const char* why) {
  if (!Active()) return;
  if (g_throttled.exchange(throttled) != throttled)
    REXLOG_INFO("KK menu scene: {} frames a second ({})", SceneFrameCap(60, throttled), why);
}
int FrameCap(int player_cap) {
  return Active() ? SceneFrameCap(player_cap, g_throttled.load(std::memory_order_relaxed)) : player_cap;
}

void NoteAttemptFailed() {
  if (!Active() || g_shown_once || g_attempt_failed.exchange(true)) return;
  REXLOG_WARN("KK menu scene: the scene did not come up in time ({} attempt); the marker keeps this failed start",
              SecondAttempt() ? "second" : "first");
}

void NoteClosing() {
  // A failed attempt keeps the marker: the next launcher counts this start as failed (task 349c).
  if (!Active() || g_shown_once || g_attempt_failed) return;
  // Closed before the scene came up (a quick Play or Quit): not a failure.
  std::lock_guard lock(g_mutex);
  WriteMarker(g_marker, "");
}

void NoteUniverse(uint32_t g) { g_universe.store(g, std::memory_order_relaxed); }

uint32_t GuestPad(uint32_t user, uint8_t* base, uint8_t* state) {
  // The whole state after the packet number: buttons, triggers, sticks.
  std::memset(state + 4, 0, 12);
  if (user != 0) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
  const uint32_t g = g_universe.load(std::memory_order_relaxed);
  if (g) {
    GuestView v;
    v.current_level = Load32(base, g + 0x18);
    v.request = Load32(base, g + 4) != 0;
    v.requested = Load32(base, g + 8);
    const bool was_ready = g_ready.load(std::memory_order_relaxed);
    g_tracker.Step(v);
    bool now_ready = g_tracker.Ready();
#if defined(KK_DEV_TOOLS)
    if (now_ready && DevHoldsReady()) now_ready = false;
#endif
#if defined(KK_DEV_TOOLS)
    static int reads = 0;
    if (std::getenv("KK_DEV_SCENE_TRACE") && ++reads % 30 == 1)
      REXLOG_INFO("KK dev scene: read {} level {} request {} -> {} title reads {} menu reads {} start {}", reads,
                  v.current_level, v.request, v.requested, g_tracker.title_reads, g_tracker.menu_reads,
                  g_tracker.start_pressed);
#endif
    if (g_tracker.start_pressed) state[4] = 0x00, state[5] = 0x10;  // XINPUT_GAMEPAD_START (big-endian word)
#if defined(KK_DEV_TOOLS)
    if (std::getenv("KK_DEV_SCENE_TRACE")) {
      static uint32_t movie = 0;
      const uint32_t player = Load32(base, 0x82CCAE80);
      const uint32_t now = player ? (Load32(base, player + 0x10C) | Load32(base, player + 0x110)) : 0;
      if (now != movie) REXLOG_INFO("KK dev scene: movie playing {} -> {}", movie, now);
      movie = now;
    }
#endif
    if (now_ready != was_ready) {
      g_ready = now_ready;
      if (g_ready) NoteShown();
      else REXLOG_WARN("KK menu scene: the game left the start menu (level {}): the stills again", v.current_level);
      kk::RequestUiPaint();
    }
  }
  return 0;  // ERROR_SUCCESS: pad 1 is connected (the title takes its Start from it)
}

}  // namespace kk::menu_scene

// The start menu's attract movie (task 330): left alone for about two minutes, the start menu plays
// Video/Attract.wmv (gameplay footage) over itself: sub_8272EED8, which runs every frame, calls the movie player
// sub_82723AF0(player, name, 0, 0) with "Attract" once the pad has been idle long enough (it also plays the
// startup logos with it, intros.cpp). The scene never plays it: the start menu stays on the sea.
REX_EXTERN(__imp__sub_82723AF0);
REX_HOOK_RAW(sub_82723AF0) {
  constexpr uint32_t kAttractName = 0x820A43A8;  // "Attract" in the game's image
  if (kk::menu_scene::Active() && ctx.r4.u32 == kAttractName) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXLOG_INFO("KK menu scene: the start menu's attract movie is not played");
    return;
  }
  __imp__sub_82723AF0(ctx, base);
}
