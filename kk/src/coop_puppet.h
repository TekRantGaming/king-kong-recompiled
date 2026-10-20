#pragma once

// Pure policy shared by the shipping/dev driver and SDK/game-free unit tests.
#include "coop_session.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <optional>
#include <span>

namespace kk::coop::puppet {
// Native animation-event lookup (823F8FB0) reads layer+4 -> descriptor+0
// -> header+4. A live layer can temporarily have no header during a switch.
template<class Pointer>
uint32_t AnimationLayerHeader(uint32_t layer, Pointer pointer) {
  const auto descriptor = pointer(layer, 4, 4);
  return pointer(descriptor, 0, 6);
}
template<class Pointer>
uint32_t AnimationEventHeader(uint32_t animation, Pointer pointer) {
  return AnimationLayerHeader(pointer(animation, 132, 8), pointer);
}
// 82850100 checks the action's byte item count before indexing 20-byte items.
template<class Byte, class Pointer>
uint32_t AnimationItemHeader(uint32_t items, uint32_t item, Byte byte, Pointer pointer) {
  if (!items || item >= byte(items)) return 0;
  return pointer(pointer(items,20*item+4,8),0,20);
}
// Shared resources (AI model/action table) are intentionally absent. Every
// mutable component of an engine-created body must belong to the new body.
using MutableRig = std::array<uint32_t, 7>;  // actor, AI, vars, matrix, rig, anim, layer
inline bool Independent(const MutableRig& copy, const MutableRig& original) {
  for (const auto a : copy) {
    if (!a) return false;
    for (const auto b : original) if (a == b) return false;
  }
  for (std::size_t i = 0; i < copy.size(); ++i)
    for (std::size_t j = i + 1; j < copy.size(); ++j) if (copy[i] == copy[j]) return false;
  return true;
}
inline std::size_t FilterActors(std::span<uint32_t> actors, std::span<const uint32_t> hidden) {
  std::size_t kept = 0;
  for (const auto actor : actors) {
    bool hide = false;
    for (const auto body : hidden) hide |= actor == body;
    if (!hide) actors[kept++] = actor;
  }
  return kept;
}
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
// H_exec_select_action's NPC branches (826A9B30, 826AA83C, 826A918C).
// The player-only 40/50 series is deliberately excluded. Callers resolve the
// loaded rig's action/item resource, including on independent native copies.
inline uint16_t HumanAction(uint8_t weapon, bool moving, bool crouched, bool firing) {
  if (crouched) {
    if (firing && weapon) return weapon >= 3 ? 38 : 28;
    return moving ? (weapon >= 3 ? 37 : 27) : (weapon >= 3 ? 36 : 26);
  }
  if (firing && weapon) return weapon >= 3 ? 162 : weapon==2 ? 164 : 14;
  return moving ? 10 : 0;
}
// Try the body's own weapon first, then another loaded gun's recoil, then aim.
// Item resolution belongs to the rig; never reuse an item from another bank.
template<class LoadedItem>
std::optional<Action> FireAction(uint8_t weapon, bool crouched, LoadedItem loaded) {
  const uint16_t preferred = HumanAction(weapon ? weapon : 1,false,crouched,true);
  if (auto item=loaded(preferred)) return Action{preferred,*item,0};
  for (const uint16_t action : {14,164,162,28,38,13,161,11,165,150,26,36})
    if (auto item=loaded(action)) return Action{action,*item,0};
  return {};
}
struct Gait { Action clip; float yaw = 0; bool crouched = false; };
struct GaitChoice { Action clip; float facing = 0; bool directional = false; };
inline std::optional<GaitChoice> ChooseGait(float vx, float vy, float view, bool crouched,
                                          std::span<const Gait> gaits) {
  const float speed=std::hypot(vx,vy), direction=std::atan2(vy,vx);
  if (!std::isfinite(speed) || speed<kIdleSpeed || !std::isfinite(view)) return {};
  std::optional<GaitChoice> best;
  float score=1000;
  for (const auto& gait : gaits) {
    if (gait.crouched!=crouched || !std::isfinite(gait.clip.speed) || gait.clip.speed<kIdleSpeed) continue;
    const float error=std::abs(std::remainder(direction-view-gait.yaw,6.283185307f));
    const float rank=error+0.1f*std::abs(std::log(speed/gait.clip.speed));
    if (rank>=score) continue;
    score=rank;
    // Without a matching strafe/back clip, face the travel direction. The root
    // clip's own heading is subtracted, so its feet still move the correct way.
    best=GaitChoice{gait.clip,error<.65f ? view : direction-gait.yaw,error<.65f};
  }
  return best;
}
struct AnimationDrive {
  float speed = 0, fire_left = 0, land_left = 0;
  bool moving = false, have_fire = false, new_shot = false, was_airborne = false, have_airborne = false;
  uint32_t last_fire = 0;
  void Update(const PlayerPose& p, float dt) {
    dt = std::clamp(dt,0.f,.1f);
    const float target = std::min(std::hypot(p.vx,p.vy),20.f);
    speed += (target-speed)*(1-std::exp(-dt*14.f));
    moving = speed > (moving ? .08f : .18f);
    fire_left = std::max(0.f,fire_left-dt); new_shot = false;
    land_left=std::max(0.f,land_left-dt);
    if (have_airborne && was_airborne && !p.airborne) land_left=.5f;
    was_airborne=p.airborne; have_airborne=true;
    if (have_fire && p.fire != last_fire && uint32_t(p.fire-last_fire) < 0x80000000u) {
      fire_left = .35f; new_shot = true; last_fire = p.fire;
    }
    if (!have_fire) last_fire = p.fire;
    have_fire = true;
  }
};
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
