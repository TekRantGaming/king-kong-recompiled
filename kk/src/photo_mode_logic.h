// Photo mode's settings and their translation for the renderer (photo_mode.cpp; tests: photo_mode_tests.cpp).
// No game, window or renderer here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include "plugin/native_photo.h"

namespace kk::photo {

// The letterbox choices (the picture's aspect ratio inside the bars; 0 none).
inline constexpr float kLetterboxes[] = {0.0f, 1.85f, 2.0f, 2.39f, 4.0f / 3.0f, 1.0f};
inline constexpr int kLetterboxCount = 6;

// The camera's limit around where photo mode started (metres; the free camera loads nothing new).
inline constexpr float kRadius = 15.0f;

// What the player set in photo mode. Everything goes back to these defaults each time photo mode opens;
// nothing is saved, and the player's own settings are never changed (the renderer uses these instead while
// photo mode lasts).
struct Values {
  // Camera
  float fov = 0;          // degrees; 0 until the game's view gave its own (the default)
  float default_fov = 0;  // the game's (with the player's field of view setting)
  float roll = 0;         // degrees
  float speed = 3;        // metres a second
  bool hide_arms = true;
  // Colour
  int look = 0;           // 0 none, 1 black and white, 2 sepia, 3 warm, 4 cool, 5 green, 6 rose
  float look_strength = 100;  // %
  float exposure = 0;     // stops
  float contrast = 100;   // %
  float saturation = 100; // %
  float temperature = 0;  // -100 cool .. 100 warm
  // Effects
  float dof = 0;          // %
  float focus = 5;        // metres
  float focus_range = 2;  // metres
  float vignette = 0;     // %
  float grain = 0;        // %
  int letterbox = 0;      // kLetterboxes
  bool flip = false;
  // Picture (the player's settings while photo mode lasts; the defaults are the player's own)
  int cel = 0, player_cel = 0;              // 0 off, 1 on
  int sharpness = 0, player_sharpness = 0;  // 0 off .. 3 high
  float brightness = 0, player_brightness = 0;  // -50..50
  float gamma = 1, player_gamma = 1;            // 0.5..1.5
  uint32_t seed = 1;
};

// A fresh set: the player's picture settings as the defaults of the Picture rows.
inline Values Fresh(int player_cel, int player_sharpness, float player_brightness, float player_gamma, uint32_t seed) {
  Values v;
  v.cel = v.player_cel = std::clamp(player_cel, 0, 1);
  v.sharpness = v.player_sharpness = std::clamp(player_sharpness, 0, 3);
  v.brightness = v.player_brightness = std::isfinite(player_brightness) ? std::clamp(player_brightness, -50.f, 50.f) : 0.f;
  v.gamma = v.player_gamma = std::isfinite(player_gamma) ? std::clamp(player_gamma, 0.5f, 1.5f) : 1.f;
  v.seed = seed ? seed : 1;
  return v;
}

// The field of view the scene view gets (radians): the player's photo value, else the game's own.
inline float FieldOfViewRadians(const Values& v, float game_radians) {
  if (v.fov <= 0) return game_radians;
  return std::clamp(v.fov, 10.f, 140.f) * 3.14159265f / 180.f;
}

inline NrPhotoSettings ToRenderer(const Values& v, bool active) {
  NrPhotoSettings s{};
  s.size = sizeof(NrPhotoSettings);
  s.active = active ? 1u : 0u;
  s.hide = active ? ((v.hide_arms ? 1u : 0u) | 2u) : 0u;
  s.look = uint32_t(std::clamp(v.look, 0, 6));
  s.look_strength = std::clamp(v.look_strength, 0.f, 100.f) / 100.f;
  s.exposure = std::clamp(v.exposure, -3.f, 3.f);
  s.contrast = std::clamp(v.contrast, 25.f, 200.f) / 100.f;
  s.saturation = std::clamp(v.saturation, 0.f, 200.f) / 100.f;
  s.temperature = std::clamp(v.temperature, -100.f, 100.f) / 100.f;
  s.vignette = std::clamp(v.vignette, 0.f, 100.f) / 100.f;
  s.grain = std::clamp(v.grain, 0.f, 100.f) / 100.f;
  s.letterbox = kLetterboxes[std::clamp(v.letterbox, 0, kLetterboxCount - 1)];
  s.flip = v.flip ? 1u : 0u;
  s.dof = std::clamp(v.dof, 0.f, 100.f) / 100.f;
  s.focus = std::max(v.focus, 0.1f);
  s.focus_range = std::max(v.focus_range, 0.05f);
  // The player's own picture settings stay in charge unless a row was changed.
  s.cel = v.cel != v.player_cel ? v.cel : -1;
  s.sharpness = v.sharpness != v.player_sharpness ? v.sharpness : -1;
  s.brightness = v.brightness != v.player_brightness ? v.brightness : NAN;
  s.gamma = v.gamma != v.player_gamma ? v.gamma : NAN;
  s.seed = v.seed;
  s.undim = 1.f;  // photo_mode.cpp sets the paused game's own (task 272)
  return s;
}

// "kk-photo-20261018-153012.png" for a local time.
inline std::string PictureName(const std::tm& t, int ms) {
  char name[64];
  std::snprintf(name, sizeof(name), "kk-photo-%04d%02d%02d-%02d%02d%02d-%03d.png", t.tm_year + 1900, t.tm_mon + 1,
                t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, std::clamp(ms, 0, 999));
  return name;
}

}  // namespace kk::photo
