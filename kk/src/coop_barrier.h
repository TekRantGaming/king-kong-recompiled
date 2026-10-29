#pragma once
#include "coop_mut.h"
#include <array>
#include <cmath>
#include <optional>

namespace kk::coop::barrier {
using Direction=std::array<float,3>;
// IOB Go calls 824AA588, which divides by the planar length without a zero
// check. A vertical vector is just as unsafe as a zero or nonfinite vector.
inline bool Valid(const Direction& d) {
  for (float v:d) if (!std::isfinite(v) || std::abs(v)>1000) return false;
  return std::hypot(d[0],d[1])>1e-6f;
}
// Keep native hit values exactly. The native event builder 82634428 also uses
// an actor's forward when a planar direction is degenerate. Do that only for
// a degenerate input, never replace a usable native direction with a constant.
inline std::optional<Direction> Resolve(const Direction& hit,const Direction& forward) {
  if (Valid(hit)) return hit;
  if (Valid(forward)) return forward;
  return {};
}
inline Direction Replay(const Direction& d) {
  if (COOP_MUT_IF("barrier_direction")) return {};
  if (COOP_MUT_IF("barrier_fixed_direction")) return {0,1,0};
  return d;
}
}
