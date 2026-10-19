#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace kk::coop::puppet {
enum class HumanKind { Unknown, Ally, Background };
struct AutoCandidate {
  uint32_t actor = 0;
  HumanKind kind = HumanKind::Unknown;
  float distance = 0;
  bool natural_h = false, stable = false, rig_valid = false;
  bool hero = false, ann = false, dead = false, grabbed = false;
  bool cinematic = false, following = false, script_reference = false;
  bool script_evidence_complete = false;
};
inline const char* AutoReject(const AutoCandidate& c) {
  if (!c.actor || !c.natural_h) return "not-natural-H";
  if (c.hero) return "hero-or-alias";
  if (c.ann) return "Ann";
  if (!c.rig_valid) return "invalid-rig-or-membership";
  if (!c.stable) return "not-stable-one-second";
  if (c.dead) return "dead";
  if (c.grabbed) return "grabbed-or-carried";
  if (c.cinematic) return "cinematic";
  if (c.following) return "following-Jack";
  if (!c.script_evidence_complete) return "script-evidence-unavailable";
  if (c.script_reference) return "mission-or-trigger-reference";
  if (!std::isfinite(c.distance) || c.distance < 0) return "invalid-distance";
  return nullptr;
}
inline int AutoScore(const AutoCandidate& c) {
  if (AutoReject(c)) return -1;
  // Distance is only a preference. A distant human can be teleported. Kind is
  // supplied only by positive model evidence, never an invented character ID.
  return (c.kind == HumanKind::Background ? 3000 : c.kind == HumanKind::Ally ? 1000 : 2000) +
      int(std::fmin(c.distance, 500.f));
}
inline std::optional<std::size_t> SelectAuto(std::span<const AutoCandidate> candidates) {
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const int score = AutoScore(candidates[i]);
    if (score < 0) continue;
    if (!best || score > AutoScore(candidates[*best]) ||
        (score == AutoScore(candidates[*best]) && candidates[i].actor < candidates[*best].actor)) best = i;
  }
  return best;
}
}  // namespace kk::coop::puppet
