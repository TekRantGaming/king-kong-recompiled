#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace kk::coop::grid_guard {

// sub_82556890 receives each Vec3 as packed x/y and z/padding in two GPRs.
// Only the upper word of the second GPR belongs to the vector.
struct Line {
  uint64_t from_xy, from_z, to_xy, to_z;
  double shorten;

  std::array<float, 6> Points() const {
    return {std::bit_cast<float>(uint32_t(from_xy >> 32)),
            std::bit_cast<float>(uint32_t(from_xy)),
            std::bit_cast<float>(uint32_t(from_z >> 32)),
            std::bit_cast<float>(uint32_t(to_xy >> 32)),
            std::bit_cast<float>(uint32_t(to_xy)),
            std::bit_cast<float>(uint32_t(to_z >> 32))};
  }
};

inline bool Refuse(bool enabled, const Line& line) {
  if (!enabled) return false;
  if (!std::isfinite(line.shorten)) return true;
  for (float value : line.Points())
    if (!std::isfinite(value)) return true;
  return false;
}

// Invalid queries can recur every frame. Keep a count without flooding the log.
struct Reports {
  uint64_t refused = 0;
  double next = 0;
  bool Note(double now) {
    ++refused;
    if (now < next) return false;
    next = now + 30;
    return true;
  }
};

}  // namespace kk::coop::grid_guard
