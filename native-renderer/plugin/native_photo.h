#pragma once
#include <stdint.h>
// Photo mode (kk/src/photo_mode.cpp, task 208): what the game tells the renderer each frame while it is on
// (nr_photo_set). Renderer only: nothing here changes the game. With active = 0 every frame is as before.
typedef struct NrPhotoSettings {
  uint32_t size;      // sizeof(NrPhotoSettings)
  uint32_t active;    // photo mode is on
  uint32_t hide;      // bits: 1 Jack's first-person arms and weapon, 2 the pause's black cover
  uint32_t look;      // 0 none, 1 black and white, 2 sepia, 3 warm, 4 cool, 5 green, 6 rose
  float look_strength;  // 0..1
  float exposure;     // stops, -2..2
  float contrast;     // 0.5..1.5 (1 unchanged)
  float saturation;   // 0..2 (1 unchanged)
  float temperature;  // -1 cool .. 1 warm
  float vignette;     // 0..1
  float grain;        // 0..1
  float letterbox;    // the picture's aspect ratio inside bars (0 none; 1.85, 2.0, 2.39, 1.0 ...)
  uint32_t flip;      // mirror the picture left to right
  float dof;          // depth of field blur, 0 (off) .. 1
  float focus;        // focus distance in metres
  float focus_range;  // metres either side of the focus that stay sharp
  int32_t cel;        // cel shading: -1 the player's setting, 0 off, 1 on
  int32_t sharpness;  // -1 the player's setting, 0 off, 1 low, 2 medium, 3 high
  float brightness;   // -50..50 (the player's kk_brightness meaning); NAN: the player's setting
  float gamma;        // 0.5..1.5 (kk_gamma); NAN: the player's setting
  uint32_t seed;      // the grain's pattern (kept while the picture is still)
} NrPhotoSettings;
#ifdef __cplusplus
namespace nr {
// Thread safe; the render thread reads the latest each frame.
void SetPhotoSettings(const NrPhotoSettings& s);
NrPhotoSettings PhotoSettingsNow();
}  // namespace nr
#endif
