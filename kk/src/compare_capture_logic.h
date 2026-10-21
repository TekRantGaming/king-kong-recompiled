// Task 354: the comparison capture's logic without the game (compare_capture.cpp; tests/compare_capture_tests.cpp).
// The shot list (compare-shots.txt), the two looks' settings, the Original look's 720p mode for an output size and
// the frame difference that says a picture has settled.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kk::compare {

struct Shot {
  std::string id, feature, hint;
};

// The owner's start list (task 354), used when compare-shots.txt is missing or has no shots.
inline const std::vector<Shot>& DefaultShots() {
  static const std::vector<Shot> shots = {
      {"look-1", "Overall look", "A wide view with sky, scenery and a character: the whole picture at a glance."},
      {"look-2", "Overall look", "A second wide view in another place: jungle, cliffs or water, with Kong or Jack."},
      {"gi", "Global illumination",
       "A lit wall, rock or floor next to a darker corner: light bouncing off it colours the shade."},
      {"ao-1", "Ambient occlusion", "Corners, roots, rocks and where walls meet the ground: soft contact shadows."},
      {"ao-2", "Ambient occlusion", "Close to Kong or a dinosaur on the ground: shading under and around the body."},
      {"shadows", "Shadows", "A character or Kong in strong light, casting a shadow on flat ground or a wall."},
      {"water", "Water reflections", "Still water with Kong, a cliff or bright sky reflected in it."},
      {"shafts", "Light shafts", "Looking towards the sun or a bright opening through trees, fog or smoke."},
      {"fog", "Fog and distance", "A long view into the distance: far cliffs or jungle fading into the haze."},
      {"detail", "Detail (720p vs 4K)", "Close up on a face, foliage or text: fine detail and sharp edges."},
  };
  return shots;
}

inline std::string Trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(b, e - b + 1));
}

// compare-shots.txt: one shot per line, "id | feature | hint". Empty lines and lines starting with # are skipped;
// a line without a feature uses its id. The id goes into a folder name: anything but letters, digits, - and _
// becomes -.
inline std::vector<Shot> ParseShots(std::string_view text) {
  std::vector<Shot> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string line = Trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF && line.size() >= 3) line = Trim(line.substr(3));
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> parts;
    size_t p = 0;
    for (int i = 0; i < 2; ++i) {
      const size_t bar = line.find('|', p);
      if (bar == std::string::npos) break;
      parts.push_back(Trim(std::string_view(line).substr(p, bar - p)));
      p = bar + 1;
    }
    parts.push_back(Trim(std::string_view(line).substr(p)));
    Shot s;
    s.id = parts[0];
    for (char& c : s.id)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '-';
    if (s.id.empty()) continue;
    s.feature = parts.size() > 1 && !parts[1].empty() ? parts[1] : s.id;
    s.hint = parts.size() > 2 ? parts[2] : "";
    out.push_back(std::move(s));
  }
  return out;
}

// The Original look's mode of the bilinear stretch: the native_upscaler_quality whose ratio (1, 1.5, 1.7, 2, 3)
// brings the output height closest to the Xbox 360's 720 lines (3840x2160: ultra_performance, 1280x720).
struct OriginalMode {
  const char* quality;
  float ratio;
};
inline OriginalMode OriginalModeFor(int output_height) {
  static constexpr OriginalMode kModes[] = {
      {"native", 1.0f}, {"quality", 1.5f}, {"balanced", 1.7f}, {"performance", 2.0f}, {"ultra_performance", 3.0f}};
  OriginalMode best = kModes[0];
  float best_err = 1e9f;
  for (const auto& m : kModes) {
    const float err = std::abs(float(output_height) / m.ratio - 720.0f);
    if (err < best_err - 0.01f) best = m, best_err = err;
  }
  return best;
}

// "3840x2160" -> {3840, 2160}; {0, 0} when not a size.
inline std::pair<int, int> ParseSize(std::string_view s) {
  const auto x = s.find('x');
  if (x == std::string_view::npos) return {0, 0};
  const int w = std::atoi(std::string(s.substr(0, x)).c_str()), h = std::atoi(std::string(s.substr(x + 1)).c_str());
  return w > 0 && h > 0 ? std::pair{w, h} : std::pair{0, 0};
}

// The temporal upscaler for the Max look from kk_upscalers_offered ("dlss,fsr3,xess"): DLSS (DLAA), then FSR 4,
// XeSS and FSR 3.1, each at native resolution. FSR 3.1 when nothing is listed (it runs on every GPU).
inline std::string BestUpscaler(std::string_view offered) {
  auto has = [&](std::string_view name) {
    size_t p = 0;
    while (p <= offered.size()) {
      size_t e = offered.find(',', p);
      if (e == std::string_view::npos) e = offered.size();
      if (Trim(offered.substr(p, e - p)) == name) return true;
      p = e + 1;
    }
    return false;
  };
  for (const char* u : {"dlss", "fsr4", "xess", "fsr3"})
    if (has(u)) return u;
  return "fsr3";
}

using Setting = std::pair<const char*, std::string>;

// The Original look (the Xbox 360 as shipped): settings.cpp's OriginalLookSettings that apply while the game runs,
// plus what our renderer added since (texture pack, tessellation) off. The 720p picture is the bilinear stretch of
// a 720p input, unjittered, with the game's own texture bias. Field of view 69 (Jack's, the console's).
inline std::vector<Setting> OriginalSettings(int output_height) {
  return {
      {"native_upscaler", "bilinear"},
      {"native_upscaler_quality", OriginalModeFor(output_height).quality},
      {"native_upscaler_mip_bias", "0"},
      {"native_upscaler_sharpness", "-1"},
      {"native_upscaler_still", "true"},
      {"swap_post_effect", "none"},
      {"anisotropic_override", "-1"},
      {"ao_mode", "0"},
      {"ao_strength", "1"},
      {"native_ao_normal_smoothing", "0"},
      {"native_gi", "off"},
      {"native_shadow_quality", "original"},
      {"native_water_reflections", "original"},
      {"native_volumetrics", "off"},
      {"native_cel_shading", "false"},
      {"native_effect_buffers", "false"},
      {"native_sharpness", "off"},
      {"native_tessellation", "false"},
      {"native_texture_pack", "false"},
      {"kk_motion_blur", "true"},
      {"kk_fog", "true"},
      {"kk_big_blur", "true"},
      {"kk_fov", "69"},
  };
}

// The Max look: the Ultra preset with every effect at its highest (GI High at Full distance and full resolution,
// AO High samples at full resolution, Ultra shadows, screen-space water reflections, Volumetric high light
// shafts, 16x filtering, tessellation on) and the best temporal upscaler at native resolution (DLAA where it runs).
// The game's own effects at the Remastered defaults (motion blur and the 720p screen blur off, fog on); the same
// field of view as the Original look (the same camera in both pictures); the texture pack as the player has it.
inline std::vector<Setting> MaxSettings(const std::string& upscaler, const std::string& texture_pack) {
  return {
      {"native_upscaler", upscaler},
      {"native_upscaler_quality", "native"},
      {"native_upscaler_dlss_preset", "latest"},
      {"native_upscaler_mip_bias", "auto"},
      {"native_upscaler_sharpness", "-1"},
      {"native_upscaler_still", "false"},
      {"swap_post_effect", "fxaa"},
      {"anisotropic_override", "5"},
      {"ao_mode", "1"},
      {"ao_strength", "3"},
      {"native_ao_normal_smoothing", "2"},
      {"native_ao_quality", "full"},
      {"native_ao_samples", "3"},
      {"native_ao_temporal", "true"},
      {"native_gi", "high"},
      {"native_gi_distance", "full"},
      {"native_gi_quality", "full"},
      {"native_shadow_quality", "ultra"},
      {"native_water_reflections", "ssr"},
      {"native_volumetrics", "high"},
      {"native_cel_shading", "false"},
      {"native_effect_buffers", "true"},
      {"native_sharpness", "off"},
      {"native_tessellation", "true"},
      {"native_texture_pack", texture_pack},
      {"kk_motion_blur", "false"},
      {"kk_fog", "true"},
      {"kk_big_blur", "false"},
      {"kk_fov", "69"},
  };
}

// The mean absolute difference of two R8G8B8X8 pictures of the same size (0 to 255 a channel), every `step`th
// pixel of every `step`th row; -1 when the sizes differ.
inline double MeanDifference(const uint8_t* a, const uint8_t* b, int width, int height, size_t stride_a,
                             size_t stride_b, int step = 2) {
  if (width <= 0 || height <= 0) return -1;
  uint64_t sum = 0, n = 0;
  for (int y = 0; y < height; y += step) {
    const uint8_t* pa = a + size_t(y) * stride_a;
    const uint8_t* pb = b + size_t(y) * stride_b;
    for (int x = 0; x < width; x += step) {
      const uint8_t* qa = pa + size_t(x) * 4;
      const uint8_t* qb = pb + size_t(x) * 4;
      sum += uint64_t(std::abs(qa[0] - qb[0]) + std::abs(qa[1] - qb[1]) + std::abs(qa[2] - qb[2]));
      n += 3;
    }
  }
  return n ? double(sum) / double(n) : -1;
}

// How two pictures in a row differ: the mean (as above) and the 90th percentile of each pixel's largest channel
// difference. Rain, spray and birds keep moving while the game is paused but cover few pixels: the percentile
// ignores them, while an image still converging (temporal AA, GI, AO) changes most pixels a little.
struct FrameDifference {
  double mean = -1;
  int p90 = 255;
};
inline FrameDifference Difference(const uint8_t* a, const uint8_t* b, int width, int height, size_t stride_a,
                                  size_t stride_b, int step = 2) {
  FrameDifference out;
  if (width <= 0 || height <= 0) return out;
  uint64_t hist[256] = {}, sum = 0, n = 0;
  for (int y = 0; y < height; y += step) {
    const uint8_t* pa = a + size_t(y) * stride_a;
    const uint8_t* pb = b + size_t(y) * stride_b;
    for (int x = 0; x < width; x += step) {
      const uint8_t* qa = pa + size_t(x) * 4;
      const uint8_t* qb = pb + size_t(x) * 4;
      const int d0 = std::abs(qa[0] - qb[0]), d1 = std::abs(qa[1] - qb[1]), d2 = std::abs(qa[2] - qb[2]);
      sum += uint64_t(d0 + d1 + d2);
      ++hist[std::max({d0, d1, d2})];
      ++n;
    }
  }
  out.mean = double(sum) / double(3 * n);
  uint64_t acc = 0;
  for (int i = 0; i < 256; ++i) {
    acc += hist[i];
    if (acc * 10 >= n * 9) {
      out.p90 = i;
      break;
    }
  }
  return out;
}

}  // namespace kk::compare
