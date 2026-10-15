#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <span>
#include <string>

namespace kk::movies {

// A story boundary only. Boot, Continue, chapter practice, replay, death,
// restarts, menu loads and arbitrary level jumps do not insert a movie.
inline bool StoryBoundary(std::span<const int> levels, int from, int to,
                          bool story, bool starting, bool leaving) {
  if (!story || starting || leaving || from == to) return false;
  const auto it = std::find(levels.begin(), levels.end(), from);
  return it != levels.end() && it + 1 != levels.end() && *(it + 1) == to;
}

// Read the game's registered level catalog rather than embed a copied asset
// table. uni_init registers numeric levels at +0x24, world keys at +0x280,
// count at +0xBEC. The 151-word columns also include menus and test maps.
template <typename Read>
uint32_t WorldForLevel(uint32_t catalog, int level, Read read) {
  if (catalog < 0x10000u || catalog > 0xFFFFF000u || (catalog & 3) || level <= 0) return 0;
  const uint32_t count = read(catalog + 0xBEC);
  if (!count || count > 151) return 0;
  uint32_t result = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (read(catalog + 0x24 + i * 4) != uint32_t(level)) continue;
    const auto world = read(catalog + 0x280 + i * 4);
    if (result && result != world) return 0;  // ambiguous catalog: leave the load alone
    result = world;
  }
  return result;
}

inline std::string MovieName(uint32_t world) {
  if (!(world & 0xFFFF)) return {};
  char name[32];
  std::snprintf(name, sizeof(name), "BK_%04X.mp4", unsigned(world & 0xFFFF));
  return name;
}

// The supplied clips have a single mixed soundtrack, so either music or
// effects mute silences it. There are no separable music/effects stems.
inline float MovieGain(float master, float music, float effects) {
  if (!std::isfinite(master) || !std::isfinite(music) || !std::isfinite(effects)) return 0;
  return std::clamp(master, 0.0f, 100.0f) *
         std::clamp(std::min(music, effects), 0.0f, 100.0f) / 10000.0f;
}

inline bool FrameDue(int64_t pts, double seconds) {
  return pts <= int64_t(std::max(0.0, seconds) * 10000000.0);
}

inline int64_t RelativePts(int64_t pts, int64_t first) {
  if (pts <= first) return 0;
  const uint64_t delta = uint64_t(pts) - uint64_t(first);
  return int64_t(std::min<uint64_t>(delta, std::numeric_limits<int64_t>::max()));
}

struct FitRect { float x, y, w, h; };
inline FitRect AspectFit(float vw, float vh, float fw, float fh) {
  if (vw <= 0 || vh <= 0 || fw <= 0 || fh <= 0) return {};
  const float scale = std::min(vw / fw, vh / fh);
  return {(vw - fw * scale) * .5f, (vh - fh * scale) * .5f, fw * scale, fh * scale};
}

}  // namespace kk::movies
