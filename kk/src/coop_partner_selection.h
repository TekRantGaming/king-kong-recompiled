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
  // Tier zero requires the full resident-owner audit, not just the legacy
  // scheduled-table evidence. Unknown pool ownership is not an unused actor.
  bool scheduled = true, registry_unclaimed = false;
  bool unscheduled_setters_and_draw_confirmed = false;
  bool other_human = false, current_script_use = false;
  uint32_t mission_distance = 104;
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
  if (c.current_script_use) return "current-mission-or-wait-use";
  if (!c.script_evidence_complete) return "script-evidence-unavailable";
  if (c.script_reference) return "mission-or-trigger-reference";
  if (!c.scheduled && !c.registry_unclaimed) return "unscheduled-not-unclaimed";
  if (!c.scheduled && !c.unscheduled_setters_and_draw_confirmed)
    return "unscheduled-movement-and-draw-unconfirmed";
  if (!std::isfinite(c.distance) || c.distance < 0) return "invalid-distance";
  return nullptr;
}
inline int AutoTier(const AutoCandidate& c) {
  if (AutoReject(c)) return -1;
  if (c.registry_unclaimed) return 0;
  return c.kind == HumanKind::Background ? 1 : c.kind == HumanKind::Ally ? 3 : 2;
}
inline const char* AutoTierReason(const AutoCandidate& c) {
  return AutoTier(c) == 0 ? "full-registry-initialized-intact-unclaimed" :
      c.kind == HumanKind::Background ? "scheduled-background" :
      c.kind == HumanKind::Ally ? "scheduled-observed-ally" : "scheduled-unknown";
}
inline int AutoScore(const AutoCandidate& c) {
  if (AutoReject(c)) return -1;
  // Distance is only a preference. A distant human can be teleported. Kind is
  // supplied only by positive model evidence, never an invented character ID.
  return (4 - AutoTier(c)) * 1000 +
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
// Craig's C50 fallback policy. Existing safe tiers retain their priority.
// Only positively identified PNJInd rigs join H; unknown rigs and KAnn do not.
inline const char* FallbackReject(const AutoCandidate& c) {
  if (!c.actor || (!c.natural_h && !c.other_human)) return "unknown-human-family";
  if (c.hero) return "hero-or-alias";
  if (c.ann) return "Ann";
  if (!c.rig_valid || !c.scheduled) return "invalid-rig-or-membership";
  if (!c.stable) return "not-stable-one-second";
  if (c.dead) return "dead";
  if (c.grabbed) return "grabbed-or-carried";
  if (c.current_script_use) return "current-mission-or-wait-use";
  if (!std::isfinite(c.distance) || c.distance < 0) return "invalid-distance";
  return nullptr;
}
inline int BodyTier(const AutoCandidate& c) {
  const int safe = AutoTier(c);
  if (safe >= 0) return safe;
  return FallbackReject(c) ? -1 : c.other_human ? 5 : 4;
}
// Keep an authored human in place when the native copy path is available.
// A failed copy still permits Craig's story-human lease as the last resort.
inline bool PreferStoryCopy(const AutoCandidate& c, bool copy_failed) {
  return !copy_failed && BodyTier(c) == 4;
}
inline int BodyScore(const AutoCandidate& c) {
  const int tier = BodyTier(c);
  if (tier < 0) return -1;
  // Each tier dominates all preferences. Avoid active story states first,
  // then prefer references farther from the current PRG record.
  return (6-tier)*1000000 + (tier < 4 ? AutoScore(c) :
      (!c.cinematic && !c.following ? 200000 : 0) +
      int(c.mission_distance > 104 ? 104 : c.mission_distance)*1000 +
      int(std::fmin(c.distance, 500.f)));
}
inline std::optional<std::size_t> SelectBody(std::span<const AutoCandidate> candidates) {
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const int score = BodyScore(candidates[i]);
    if (score >= 0 && (!best || score > BodyScore(candidates[*best]) ||
        (score == BodyScore(candidates[*best]) && candidates[i].actor < candidates[*best].actor))) best = i;
  }
  return best;
}
}  // namespace kk::coop::puppet
