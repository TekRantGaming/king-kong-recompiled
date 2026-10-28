#pragma once
#include <cmath>
#include <cstdint>
#include "coop_mut.h"

// C117: crabs stalled next to the host after his private reload (c115-beach-a-20261028-113810, 65 s).
//   * A crew follower hit a crab (H_ETAT_IA_frappe from help), so the crab took that crew member as its target
//     (CR_ETAT_FIGHT sub 1, vars+1056). Single player does the same (c117-solo-held1).
//   * The crab charges in a straight line (anim 42/46 then 43/47, vars+1404 = 23 while the distance stays at or above
//     3 m). The partner body (1.2 m from the crab) and the host (1.9 m) stood between it and the crew member, so it
//     pushed against them, 3.75 m from its target, without moving, for over a minute.
//   * CR_exec_CHECK_Target keeps a sub 1 target while it exists (have_choice gate), so the crab never looks at the
//     two players in front of it. The game's own way to give up a crew target is in CR_ETAT_FIGHT (decomp
//     crab_states_b, the vars+4420 branch before CR_exec_CHECK_Target): it sets vars+3532 and vars+4420 and names
//     the target in vars+4424, so the next target check refuses it (fn_82463A58) and takes the player in
//     vars+2040 instead; CR_ETAT_FIGHT clears vars+4424 right after that check.
// This watch finds a crab held like that (game time, so a pause does not count) and asks for that native give up.
// Only with a partner body (co-op); single player is unchanged.
namespace kk::coop::crab_unstick {

constexpr float kStuckSeconds = 6.0f;   // the reported stalls lasted 22 to 65 s
constexpr float kMoveLimit = 0.5f;      // planar metres the crab may drift and still count as held
constexpr float kPlayerNear = 3.0f;     // a player this close (crab centre) is in the way and reachable
constexpr float kRangeMargin = 0.25f;   // beyond the crab's own reach for this target
constexpr uint32_t kVarsTarget = 1056, kVarsSub = 3496, kVarsNpcRange = 1412;
constexpr uint32_t kVarsZone = 3532, kVarsGaveUp = 4420, kVarsExcluded = 4424;

struct Sample {
  float now = 0, x = 0, y = 0;    // game time, crab planar position
  uint32_t sub = 0;               // vars+3496
  uint32_t target = 0, hero = 0, body = 0;
  float distance = 0, range = 0;  // to its target; vars+1412
  float player_near = 1e9f;       // nearest of host and partner body
};
struct Watch {
  uint32_t target = 0;
  float since = 0, ax = 0, ay = 0;
};

inline bool PlayerTarget(const Sample& s) {
  if (COOP_MUT_IF("crab_unstick_player_target")) return false;
  return s.target == s.hero || (s.body && s.target == s.body);
}
// One crab frame. True when the crab should give its target up now.
inline bool Step(Watch& w, const Sample& s) {
  const bool held = s.sub == 1 && s.target && !PlayerTarget(s) &&
                    (s.body || COOP_MUT_IF("crab_unstick_single_player")) &&
                    s.distance > s.range + kRangeMargin && s.player_near <= kPlayerNear;
  if (!held || w.target != s.target || s.now < w.since) {
    w = {held ? s.target : 0u, s.now, s.x, s.y};
    return false;
  }
  if (std::hypot(s.x - w.ax, s.y - w.ay) > kMoveLimit && !COOP_MUT_IF("crab_unstick_ignore_move")) {
    w.since = s.now; w.ax = s.x; w.ay = s.y;
    return false;
  }
  if (s.now - w.since < kStuckSeconds) return false;
  w.since = s.now; w.ax = s.x; w.ay = s.y;
  return true;
}
// The writes of the native crew give up (CR_ETAT_FIGHT, before CR_exec_CHECK_Target).
struct GiveUp { uint32_t zone = 1, gave_up = 1, excluded = 0; };
inline GiveUp NativeGiveUp(uint32_t target) {
  GiveUp g; g.excluded = COOP_MUT_IF("crab_unstick_no_exclude") ? 0u : target;
  return g;
}

}  // namespace kk::coop::crab_unstick
