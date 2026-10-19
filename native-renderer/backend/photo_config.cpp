// Photo mode in the renderer: see photo_config.h.

#include "backend/photo_config.h"

#include <algorithm>
#include <mutex>

namespace nr {

namespace {

// Jack's first-person arms (index count, then GeometryContentHash). Found with
// the material census (REX_DEV_FRAME_LOG + REX_DEV_MATERIAL_DUMP) and checked by leaving them out in game
// captures (task 208). The same mesh is drawn twice a frame (the depth pass, then the colour pass).
constexpr PhotoHiddenMesh kHidden[] = {
    {0x7EB7E914947CEC40ull, 14310, "Jack's arms"},
    {0xD69009DC0D6F9433ull, 2928, "Jack's sleeves"},
};

std::mutex g_mutex;
NrPhotoSettings g_settings = NeutralPhotoSettings();

float Clamp(float v, float lo, float hi, float neutral) { return std::isfinite(v) ? std::clamp(v, lo, hi) : neutral; }

float Smooth(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

const PhotoHiddenMesh* PhotoHiddenMeshes(size_t* count) {
  if (count) *count = sizeof(kHidden) / sizeof(kHidden[0]);
  return kHidden;
}

bool PhotoMeshCountCandidate(uint32_t index_count) {
  for (const auto& m : kHidden)
    if (m.count == index_count) return true;
  return false;
}

bool PhotoHidesMesh(uint64_t mesh, uint32_t index_count) {
  if (!mesh) return false;
  for (const auto& m : kHidden)
    if (m.mesh == mesh && m.count == index_count) return true;
  return false;
}

NrPhotoSettings NeutralPhotoSettings() {
  NrPhotoSettings s{};
  s.size = sizeof(NrPhotoSettings);
  s.look_strength = 1.0f;
  s.contrast = 1.0f;
  s.saturation = 1.0f;
  s.focus = 5.0f;
  s.focus_range = 2.0f;
  s.cel = -1;
  s.sharpness = -1;
  s.brightness = NAN;
  s.gamma = NAN;
  s.undim = 1.0f;
  return s;
}

NrPhotoSettings SanitizePhotoSettings(const NrPhotoSettings& in) {
  NrPhotoSettings s = in;
  s.size = sizeof(NrPhotoSettings);
  s.active = in.active ? 1u : 0u;
  s.hide = in.hide & 3u;
  s.look = in.look <= 6 ? in.look : 0u;
  s.look_strength = Clamp(in.look_strength, 0.0f, 1.0f, 1.0f);
  s.exposure = Clamp(in.exposure, -3.0f, 3.0f, 0.0f);
  s.contrast = Clamp(in.contrast, 0.25f, 2.0f, 1.0f);
  s.saturation = Clamp(in.saturation, 0.0f, 2.0f, 1.0f);
  s.temperature = Clamp(in.temperature, -1.0f, 1.0f, 0.0f);
  s.vignette = Clamp(in.vignette, 0.0f, 1.0f, 0.0f);
  s.grain = Clamp(in.grain, 0.0f, 1.0f, 0.0f);
  s.letterbox = Clamp(in.letterbox, 0.0f, 4.0f, 0.0f);
  if (s.letterbox < 0.5f) s.letterbox = 0.0f;
  s.flip = in.flip ? 1u : 0u;
  s.dof = Clamp(in.dof, 0.0f, 1.0f, 0.0f);
  s.focus = Clamp(in.focus, 0.1f, 500.0f, 5.0f);
  s.focus_range = Clamp(in.focus_range, 0.05f, 200.0f, 2.0f);
  s.cel = in.cel < 0 ? -1 : (in.cel ? 1 : 0);
  s.sharpness = in.sharpness < 0 ? -1 : std::min(in.sharpness, 3);
  s.brightness = std::isfinite(in.brightness) ? std::clamp(in.brightness, -50.0f, 50.0f) : NAN;
  s.gamma = std::isfinite(in.gamma) ? std::clamp(in.gamma, 0.5f, 1.5f) : NAN;
  s.undim = Clamp(in.undim, 1.0f, 6.0f, 1.0f);  // the paused game's dimming taken back (task 272)
  return s;
}

bool PhotoGradeNeeded(const NrPhotoSettings& s) {
  if (!s.active) return false;
  return (s.look && s.look_strength > 0.0f) || s.exposure != 0.0f || s.contrast != 1.0f || s.saturation != 1.0f ||
         s.temperature != 0.0f || s.vignette > 0.0f || s.grain > 0.0f || s.letterbox > 0.0f || s.flip ||
         s.dof > 0.0f || s.undim != 1.0f;
}

float PhotoExposureGain(const NrPhotoSettings& s) {
  const float undim = std::isfinite(s.undim) && s.undim > 1.0f ? std::min(s.undim, 6.0f) : 1.0f;
  return std::exp2(s.exposure) * std::pow(undim, 2.2f);
}

std::array<float, 3> PhotoLookColour(uint32_t look) {
  switch (look) {
    case 1: return {1.0f, 1.0f, 1.0f};     // black and white
    case 2: return {1.0f, 0.82f, 0.60f};   // sepia
    case 3: return {1.0f, 0.88f, 0.72f};   // warm
    case 4: return {0.78f, 0.90f, 1.0f};   // cool
    case 5: return {0.84f, 1.0f, 0.78f};   // green
    case 6: return {1.0f, 0.80f, 0.86f};   // rose
    default: return {1.0f, 1.0f, 1.0f};
  }
}

std::array<float, 3> PhotoGradeColour(const NrPhotoSettings& s, std::array<float, 3> c) {
  constexpr float kW[3] = {0.2126f, 0.7152f, 0.0722f};
  const float gain = PhotoExposureGain(s);
  const float warm[3] = {1.0f + 0.10f * s.temperature, 1.0f + 0.01f * s.temperature, 1.0f - 0.10f * s.temperature};
  for (int i = 0; i < 3; ++i) {
    float lin = std::pow(std::max(c[i], 0.0f), 2.2f) * gain * warm[i];
    c[i] = std::pow(std::max(lin, 0.0f), 1.0f / 2.2f);
    c[i] = (c[i] - 0.5f) * s.contrast + 0.5f;
  }
  float l = c[0] * kW[0] + c[1] * kW[1] + c[2] * kW[2];
  for (int i = 0; i < 3; ++i) c[i] = l + (c[i] - l) * s.saturation;
  if (s.look) {
    const auto t = PhotoLookColour(s.look);
    const float tl = t[0] * kW[0] + t[1] * kW[1] + t[2] * kW[2];
    l = c[0] * kW[0] + c[1] * kW[1] + c[2] * kW[2];
    for (int i = 0; i < 3; ++i) {
      // Black and white and sepia: the luminance in the look's colour; the tints keep the colours.
      const float target = (s.look <= 2 ? l : c[i]) * t[i] / tl;
      c[i] = c[i] + (target - c[i]) * s.look_strength;
    }
  }
  for (float& v : c) v = std::max(v, 0.0f);
  return c;
}

float PhotoVignette(float amount, float u, float v, float aspect) {
  const float x = (u - 0.5f) * aspect, y = v - 0.5f;
  const float r = std::sqrt(x * x + y * y) / std::sqrt(0.25f * aspect * aspect + 0.25f);
  return 1.0f - amount * 0.85f * Smooth(0.35f, 1.0f, r);
}

bool PhotoUnderBars(float target_aspect, float u, float v, float width, float height) {
  if (target_aspect <= 0.0f || width <= 0.0f || height <= 0.0f) return false;
  const float aspect = width / height;
  if (target_aspect > aspect) {
    const float bar = 0.5f * (1.0f - aspect / target_aspect);
    return v < bar || v > 1.0f - bar;
  }
  const float bar = 0.5f * (1.0f - target_aspect / aspect);
  return u < bar || u > 1.0f - bar;
}

void SetPhotoSettings(const NrPhotoSettings& s) {
  const NrPhotoSettings clean = SanitizePhotoSettings(s);
  std::lock_guard lock(g_mutex);
  g_settings = clean;
}

NrPhotoSettings PhotoSettingsNow() {
  std::lock_guard lock(g_mutex);
  return g_settings;
}

}  // namespace nr
