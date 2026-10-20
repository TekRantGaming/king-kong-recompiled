// The live main menu scene (task 330): see menu_scene.h.

#include "menu_scene.h"

#include <algorithm>
#include <atomic>
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
#include <rex/logging.h>

#include "ui_paint.h"

#include <SDL3/SDL.h>


REXCVAR_DEFINE_BOOL(kk_menu_scene_off, false, "KK",
                    "Internal: no live menu scene in this launcher (passed on the command line by a launcher that "
                    "restarts itself for a shader rebuild). Never saved.");

namespace kk::menu_scene {
namespace {

std::atomic<uint32_t> g_universe{0};  // the universe's variables G, as uni_go last saw them

std::atomic<bool> g_active{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_shown_once{false};
std::mutex g_mutex;
std::filesystem::path g_marker;
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

}  // namespace

Reason DecideAtLauncherStart(StartFacts facts, const std::filesystem::path& user_dir) {
  facts.off_this_session = facts.off_this_session || REXCVAR_GET(kk_menu_scene_off);
  const auto marker = MarkerPath(user_dir);
  std::error_code ec;
  if (!user_dir.empty() && std::filesystem::exists(marker, ec)) {
    std::ifstream in(marker, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    if (ReadMarker(text.str(), KK_VERSION) == MarkerState::kFailedThisVersion) {
      facts.failed_this_version = true;
      if (text.str().rfind("starting ", 0) == 0) {
        REXLOG_WARN("KK menu scene: the last launcher did not get the scene up (it closed or failed while it "
                    "started): pictures for the rest of v{}", KK_VERSION);
        WriteMarker(marker, std::string("failed ") + KK_VERSION);
      }
    } else {
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
    WriteMarker(g_marker, std::string("starting ") + KK_VERSION);
  }
  g_active = true;
  REXLOG_INFO("KK menu scene: starting the game behind the launcher");
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

void NoteClosing() {
  if (!Active() || g_shown_once) return;
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
    const bool was_ready = g_tracker.Ready();
    g_tracker.Step(v);
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
    if (g_tracker.Ready() != was_ready) {
      g_ready = g_tracker.Ready();
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
