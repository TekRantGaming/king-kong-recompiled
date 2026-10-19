#pragma once

// Pure policy shared by the dev driver and SDK/game-free unit tests.
#include "coop_session.h"
#include <cmath>
#include <optional>
#include <span>

namespace kk::coop::puppet {
constexpr float kIdleSpeed = .1f;
struct Action {
  uint16_t action = 0;
  uint8_t item = 0;
  float speed = 0;  // Horizontal speed actually observed with this tuple on this rig.
};
inline std::optional<Action> ChooseAction(float speed, std::span<const Action> observed) {
  if (!std::isfinite(speed)) return {};
  speed = std::abs(speed);
  std::optional<Action> best;
  for (const auto& a : observed) {
    if (!std::isfinite(a.speed) || a.speed < 0 ||
        (a.speed < kIdleSpeed) != (speed < kIdleSpeed)) continue;
    if (!best || std::abs(a.speed - speed) < std::abs(best->speed - speed)) best = a;
  }
  return best;
}
struct Point { float x = 0, y = 0, z = 0; };
struct Step { Point target; bool teleport = false; };
inline std::optional<Step> ClampStep(Point from, Point target) {
  for (float v : {from.x, from.y, from.z, target.x, target.y, target.z})
    if (!std::isfinite(v) || std::abs(v) >= 1000000) return {};
  const float distance = std::hypot(target.x-from.x, target.y-from.y, target.z-from.z);
  // The interpolation already smooths normal motion. More than five units is
  // an explicit teleport, permitted to snap rather than crawl through geometry.
  return Step{target, distance > 5.0f};
}
struct Conditions {
  bool enabled = false;
  SessionState state = SessionState::Idle;
  bool fresh = false, local = false, partner = false, same_level = false;
  bool photo = false, loading = false, environment = false;
};
inline const char* ReleaseReason(const Conditions& c) {
  if (!c.enabled) return "coop-disabled";
  if (c.loading) return "load";
  if (c.photo) return "photo-mode";
  if (c.state == SessionState::Lost) return "lost";
  if (c.state == SessionState::Ended) return "ended";
  if (c.state != SessionState::Connected) return "not-connected";
  if (!c.fresh) return "snapshot-expired";
  if (!c.local) return "local-pose-unavailable";
  if (!c.partner) return "partner-pose-unavailable";
  if (!c.same_level) return "different-level";
  if (!c.environment) return "world-level-or-hero-changed";
  return nullptr;
}
}  // namespace kk::coop::puppet
