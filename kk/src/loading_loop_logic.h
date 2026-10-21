// Task 331 (owner, 20 Oct): the rules of the launcher's two loading loops (loading_loop.h), without a window.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>

namespace kk::loading_loop {

enum class Kind {
  kNone,       // no loop (a launcher restarted for a shader rebuild: its own shader screen)
  kNotice,     // the save notice: the game opened from the desktop
  kReturning,  // "Returning to the main menu": back from the game (any --kk_launcher_open but rebuild)
};

// Which loop a launcher start plays. `open_on`: --kk_launcher_open ("" = opened by the player).
inline Kind KindForStart(const std::string& open_on) {
  if (open_on == "rebuild") return Kind::kNone;
  return open_on.empty() ? Kind::kNotice : Kind::kReturning;
}

constexpr float kMinSeconds = 3.0f;     // on screen at least this long, so the notice can be read (owner)
constexpr float kFadeSeconds = 0.5f;    // the fade into the launcher
// Task 349c (owner, 21 Oct): the loop stays until the live menu scene is up. An attempt that has not got the scene
// up this long after its start failed: the launcher starts again for a second attempt (a new process: the game
// cannot start twice in one), and only when that one fails too do the pictures show.
constexpr float kSceneAttemptSeconds = 30.0f;

enum class SceneStep {
  kWait,      // keep the loop up
  kEnd,       // fade into the launcher (the scene is up, or none was started)
  kRetry,     // this attempt failed: start the launcher again for the second attempt
  kPictures,  // the second attempt failed too: fade into the launcher's pictures
};
// `shown`: seconds the loop has been on screen; `scene_expected`: the live menu scene was started for this launcher;
// `scene_ready`: it is up (menu_scene::Ready); `since_start`: seconds since its start; `second_attempt`: this
// launcher is already the second attempt.
inline SceneStep Step(float shown, bool scene_expected, bool scene_ready, float since_start, bool second_attempt) {
  if (shown < kMinSeconds) return SceneStep::kWait;
  if (!scene_expected || scene_ready) return SceneStep::kEnd;
  if (since_start < kSceneAttemptSeconds) return SceneStep::kWait;
  return second_attempt ? SceneStep::kPictures : SceneStep::kRetry;
}

// The loop's cover (1 opaque .. 0 gone) `since_end` seconds after it started fading (< 0: not fading yet).
inline float Cover(float since_end) {
  if (since_end < 0) return 1.0f;
  return std::clamp(1.0f - since_end / kFadeSeconds, 0.0f, 1.0f);
}

// The tips cycle: the one after `last` (the file's text, "" or junk = before the first) of `count`.
inline int NextTip(const std::string& last, int count) {
  if (count <= 0) return 0;
  char* end = nullptr;
  const long v = std::strtol(last.c_str(), &end, 10);
  if (last.empty() || end == last.c_str() || v < 0) return 0;
  return int((v + 1) % count);
}

}  // namespace kk::loading_loop
