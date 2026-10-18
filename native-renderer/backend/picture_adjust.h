// Brightness and gamma (owner, 18 Oct; GitHub issue #44): the player's picture adjustment.
//
// The game renderer applies it in its Present pass through the display gamma ramp (game_passes.cpp): the
// player's curve is put after the game's own ramp, so the game's picture changes and nothing else (the
// launcher, the pause menu and the overlays are drawn over it later by the presenter). It costs nothing
// extra: the ramp lookup is part of the blit that writes the frame image anyway.
//
// At the defaults (brightness 0, gamma 1) the curve is not used at all: the ramp is left exactly as the
// game set it, so the picture is the same, bit for bit, as without this feature.
//
// The curve works on the picture's encoded (SDR) values, 0 to 1, and keeps white at 1: with HDR output the
// presenter still maps white to the paper white and nothing goes above it.
//   brightness b (-1 to 1, the settings' -50 to 50): the black level. Positive raises it (more shows in
//     dark places, black becomes grey), negative lowers it (deeper blacks, the darkest tones go black).
//   gamma g (0.5 to 1.5): the middle tones. Above 1 brightens them, below 1 darkens them; black and white
//     stay where they are.
//   out = pow(clamp((x - lo) / (1 - lo), 0, 1), 1 / g), lo = -b * kBlackLevelRange
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nr {

struct PictureAdjust {
  float brightness = 0.0f;  // -1..1
  float gamma = 1.0f;       // 0.5..1.5

  static constexpr float kBlackLevelRange = 0.2f;  // b = 1: black at 0.2 / 1.2 (17% grey); b = -1: up to 0.2 goes black
  static constexpr float kMinGamma = 0.5f, kMaxGamma = 1.5f;

  // From the settings: brightness -50..50, gamma 0.5..1.5 (anything else is clamped; not a number = default).
  static PictureAdjust FromSettings(double brightness, double gamma) {
    PictureAdjust p;
    if (std::isfinite(brightness)) p.brightness = float(std::clamp(brightness, -50.0, 50.0) / 50.0);
    if (std::isfinite(gamma)) p.gamma = float(std::clamp(gamma, double(kMinGamma), double(kMaxGamma)));
    // A slider's float text ("1.000000", "0.999999") still means the default.
    if (std::fabs(p.brightness) < 0.001f) p.brightness = 0.0f;
    if (std::fabs(p.gamma - 1.0f) < 0.001f) p.gamma = 1.0f;
    return p;
  }
  bool identity() const { return brightness == 0.0f && gamma == 1.0f; }
  bool operator==(const PictureAdjust& o) const { return brightness == o.brightness && gamma == o.gamma; }
  bool operator!=(const PictureAdjust& o) const { return !(*this == o); }

  // The curve for one encoded value (0..1).
  float Apply(float x) const {
    if (identity()) return x;
    const float lo = -brightness * kBlackLevelRange;
    float y = std::clamp((x - lo) / (1.0f - lo), 0.0f, 1.0f);
    if (gamma != 1.0f) y = std::pow(y, 1.0f / gamma);
    return y;
  }

  // The display gamma ramp (D3DGAMMARAMP layout: 256 red, 256 green, 256 blue, 16-bit) with the curve put
  // after it. `game` is the game's ramp; `game_identity` when the game left it alone (or never set it).
  std::array<uint16_t, 768> ComposeRamp(const std::array<uint16_t, 768>& game, bool game_identity) const {
    std::array<uint16_t, 768> out;
    for (uint32_t c = 0; c < 3; ++c)
      for (uint32_t i = 0; i < 256; ++i) {
        const float in = game_identity ? float(i) / 255.0f : float(game[c * 256 + i]) / 65535.0f;
        out[c * 256 + i] = uint16_t(std::lround(std::clamp(Apply(in), 0.0f, 1.0f) * 65535.0f));
      }
    return out;
  }

  // The Present pass's choice (game_passes.cpp), here so tests pin it. Before this feature the ramp was used
  // exactly when the game had changed it (not the identity ramp, not all zero: never set), and then as the
  // game set it; at the defaults that is still so.
  static bool GameRampChanged(const std::array<uint16_t, 768>& game) {
    bool identity = true, all_zero = true;
    for (uint32_t i = 0; i < 256 && identity; ++i)
      identity = game[i] == i * 257 && game[256 + i] == i * 257 && game[512 + i] == i * 257;
    for (uint16_t v : game) all_zero = all_zero && v == 0;
    return !identity && !all_zero;
  }
  static bool PresentUsesRamp(const std::array<uint16_t, 768>& game, const PictureAdjust& p) {
    return GameRampChanged(game) || !p.identity();
  }
  // The ramp the Present pass uploads when it uses one.
  std::array<uint16_t, 768> PresentRamp(const std::array<uint16_t, 768>& game) const {
    return identity() ? game : ComposeRamp(game, !GameRampChanged(game));
  }

  // For a picture already drawn with `from` (8-bit values): the table that makes it look drawn with `to`
  // instead (the pause menu's kept frame, while the sliders move). Values `from` crushed to black or
  // white cannot come back; the rest is exact to the 8-bit step.
  static std::array<uint8_t, 256> RetouchTable(const PictureAdjust& from, const PictureAdjust& to) {
    std::array<uint8_t, 256> lut;
    std::array<float, 256> drawn;  // from's output for every 8-bit input
    for (int i = 0; i < 256; ++i) drawn[i] = from.Apply(float(i) / 255.0f);
    for (int v = 0; v < 256; ++v) {
      // The input that `from` turned into v (the middle of the inputs that give the nearest value).
      const float target = float(v) / 255.0f;
      int best = 0;
      float best_d = 2.0f;
      for (int i = 0; i < 256; ++i) {
        const float d = std::fabs(drawn[i] - target);
        if (d < best_d - 1e-6f) best_d = d, best = i;
      }
      int last = best;
      while (last + 1 < 256 && std::fabs(drawn[last + 1] - drawn[best]) < 1e-6f) ++last;
      const float x = float(best + last) * 0.5f / 255.0f;
      lut[v] = uint8_t(std::lround(std::clamp(to.Apply(x), 0.0f, 1.0f) * 255.0f));
    }
    return lut;
  }
};

}  // namespace nr
