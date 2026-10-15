#pragma once
#include <algorithm>
#include <cmath>

namespace kk::ui {
// Snap the current (possibly save-file rounded) value before stepping. Adding
// to raw floats can leave a displayed integer unchanged or strand an endpoint.
inline float StepSlider(float value, float lo, float hi, float step, int direction) {
  if (!(step > 0) || hi <= lo || !direction) return std::clamp(value, lo, hi);
  const double tick = std::round((double(value) - lo) / step);
  const double next = double(lo) + (tick + (direction > 0 ? 1 : -1)) * double(step);
  const double epsilon = double(step) * 1e-5;
  if (next <= double(lo) + epsilon) return lo;
  if (next >= double(hi) - epsilon) return hi;
  return float(next);
}
struct DirectionRepeat {
  bool held = false;
  double since = 0, next = 0;
  bool Update(bool down, double now) {
    if (!down) { held = false; return false; }
    if (!held) { held = true; since = now; next = now + 0.34; return true; }
    if (now < next) return false;
    // Keep the wall-clock cadence at 30, 60 and higher UI frame rates. Skip
    // missed repeats after a stall rather than emitting a burst of steps.
    next += (std::floor((now - next) / 0.075) + 1.0) * 0.075;
    return true;
  }
};
}  // namespace kk::ui
