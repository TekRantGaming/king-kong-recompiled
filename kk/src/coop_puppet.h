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
// H_callback_tir 82696B08: the character's weapon uses the native global
// variable bank and its slot, independently of the local Jack loadout.
template<class Word,class Address>
uint32_t HumanWeapon(uint32_t slot,Word word,Address address) {
  if (slot<1 || slot>12) return 0;
  const auto bank=word(0x82CC8848);
  if (!address(bank,13176)) return 0;
  const auto weapon=word(bank+4*(slot+3281));
  return weapon<=4 ? weapon : 0;
}
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
template<class Byte, class Pointer>
uint32_t AnimationItemDescriptor(uint32_t items, uint32_t item, Byte byte, Pointer pointer) {
  // The layer holds the address of the resource pointer, not its value.
  return AnimationItemHeader(items,item,byte,pointer) ? items+20*item+4 : 0;
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
  float speed = 0;  // Native horizontal speed, normalized to the driver's 60 fps layer rate.
  float yaw = 0;    // Natural travel heading relative to body forward, for observed gaits.
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
// Loaded action numbers are not a semantic capability check: unarmed H banks
// reuse the gun IDs for story gestures. Preserve their proven locomotion.
inline uint8_t AnimationWeapon(uint8_t remote,uint8_t donor) {
  return remote>=1 && remote<=4 && !(donor>=1 && donor<=4) ? 0 : remote;
}
inline uint16_t HumanAction(uint8_t weapon, bool moving, bool crouched, bool firing, bool aiming=false) {
  if (weapon>=1 && weapon<=4 && !crouched) {
    // H npc_idle: 0x91..0x94; npc_walk: 0x0B/0xA5/0x96.
    // The shot itself belongs to anim2, not a repeated main-clip restart.
    return moving ? (weapon>=3 ? 150 : weapon==2 ? 165 : 11) : uint16_t(144+weapon);
  }
  if (weapon>=6 && weapon<=8 && !crouched) return moving ? 11 : aiming ? 75 : 96;
  if (crouched) {
    if (firing && weapon) return weapon >= 3 ? 36 : 26;
    return moving ? (weapon >= 3 ? 37 : 27) : (weapon >= 3 ? 36 : 26);
  }
  // 826A928C selects the stationary armed stance. 14/164/162 and 28/38
  // belong to the vars+2976 hit-reaction branch, not to gunfire.
  if (firing && weapon) return weapon >= 3 ? 150 : weapon==2 ? 165 : 11;
  return moving ? 10 : 0;
}
// H_callback_after_blend's tag 125/126/127 variant selection. These are
// native equipment kinds, including intact/broken spears and bones.
inline uint8_t WeaponVariant(uint8_t weapon, bool right) {
  if (weapon>=1 && weapon<=4) {
    constexpr uint8_t gun[]={0,4,2,6,5};
    return gun[weapon];
  }
  if (!right) return weapon>=5 && weapon<=10 ? 3 : 0;
  constexpr uint8_t held[]={0,4,2,6,5,3,10,7,9,11,12};
  return weapon<=10 ? held[weapon] : 0;
}
inline uint16_t WeaponAimAction(uint8_t weapon) {
  constexpr uint16_t aim[]={0,143,146,147,144};
  return weapon<=4 ? aim[weapon] : 0;
}
// Use the native armed stance on a confirmed shot, falling back to another
// loaded gun's aim. Recoil events are separate from the base action selector.
// Item resolution belongs to the rig; never reuse an item from another bank.
template<class LoadedItem>
std::optional<Action> FireAction(uint8_t weapon, bool crouched, LoadedItem loaded) {
  // An unarmed human can load these action numbers for unrelated gestures
  // (Ann raises her arms in 165). Availability alone does not mean gunfire.
  if (weapon<1 || weapon>4) return {};
  const uint16_t preferred = HumanAction(weapon,false,crouched,true);
  if (auto item=loaded(preferred)) return Action{preferred,*item,0};
  for (const uint16_t action : {11,165,150,26,36})
    if (auto item=loaded(action)) return Action{action,*item,0};
  return {};
}
struct Gait { Action clip; float yaw = 0; bool crouched = false; };
struct GaitChoice { Action clip; float facing = 0; bool directional = false; };
inline std::optional<GaitChoice> ChooseGait(float vx, float vy, float view, bool crouched,
                                          std::span<const Gait> gaits,
                                          uint32_t current = UINT32_MAX) {
  const float speed=std::hypot(vx,vy), direction=std::atan2(vy,vx);
  if (!std::isfinite(speed) || speed<kIdleSpeed || !std::isfinite(view)) return {};
  std::optional<GaitChoice> best;
  float score=1000;
  for (const auto& gait : gaits) {
    if (gait.crouched!=crouched || !std::isfinite(gait.clip.speed) || gait.clip.speed<kIdleSpeed) continue;
    const float error=std::abs(std::remainder(direction-view-gait.yaw,6.283185307f));
    // Keep the current clip through small speed/direction changes. Network
    // jitter at the boundary between two gaits must not rewind both clips.
    const float rank=error+.35f*std::abs(std::log(speed/gait.clip.speed))-
        (current==((uint32_t(gait.clip.action)<<8)|gait.clip.item) ? .06f : 0.f);
    if (rank>=score) continue;
    score=rank;
    // Without a matching strafe/back clip, face the travel direction. The root
    // clip's own heading is subtracted, so its feet still move the correct way.
    best=GaitChoice{gait.clip,error<.65f ? view : direction-gait.yaw,error<.65f};
  }
  return best;
}
struct AnimationDrive {
  float speed = 0, vx = 0, vy = 0, stop_left = 0, fire_left = 0, recoil_left = 0, land_left = 0, use_left = 0, aim_weight = 0;
  bool moving = false, have_fire = false, new_shot = false, new_use = false, was_airborne = false, have_airborne = false;
  uint32_t last_fire = 0;
  uint16_t last_action = 0, use_action = 0;
  double action_time = -1;
  bool crouched = false, airborne = false;
  uint8_t weapon = 0, aiming = 0;
  static bool FullBodyEquipmentAction(uint16_t action) {
    // H pickup actions 0x22/0x23 and H_ETAT_IA_lance's throws 0x56/0x5C.
    return action==0x22 || action==0x23 || action==0x56 || action==0x5C;
  }
  static bool EquipmentAction(uint16_t action) {
    return FullBodyEquipmentAction(action) || (action>=0x73 && action<=0x76);
  }
  float GesturePitch() const {
    // Small upper-body gestures also work on an unarmed rig without a gun
    // clip. Evaluated from time, never accumulated onto last frame's bones.
    return .16f*std::sin(3.141592654f*std::clamp(recoil_left/.22f,0.f,1.f))-
        .18f*std::sin(3.141592654f*std::clamp(use_left/.65f,0.f,1.f));
  }
  void Update(const PlayerPose& p, float dt) {
    dt = std::clamp(dt,0.f,.1f);
    const float target = std::min(std::hypot(p.vx,p.vy),20.f);
    const float blend = 1-std::exp(-dt*10.f);
    speed += (target-speed)*blend;
    const float scale = target>0 ? target/std::hypot(p.vx,p.vy) : 0;
    vx += (p.vx*scale-vx)*blend; vy += (p.vy*scale-vy)*blend;
    // A short zero-velocity sample is common between physics/network updates.
    // Require a sustained stop, but still enter locomotion promptly.
    stop_left = target>.12f ? .22f : std::max(0.f,stop_left-dt);
    moving = moving ? speed>.08f || stop_left>0 : speed>.22f;
    fire_left = std::max(0.f,fire_left-dt); new_shot = false;
    recoil_left=std::max(0.f,recoil_left-dt);
    use_left=std::max(0.f,use_left-dt);
    new_use=false;
    land_left=std::max(0.f,land_left-dt);
    // The interpolated remote timeline can briefly move backward when a new
    // packet arrives. Never replay an equipment/landing edge from that rewind.
    if (p.time>=action_time) {
      action_time=p.time;
      if (p.action!=last_action && EquipmentAction(p.action)) { use_action=p.action; use_left=.65f; new_use=true; }
      last_action=p.action; crouched=p.crouched; airborne=p.airborne;
      weapon=p.weapon; aiming=p.aiming;
      if (have_airborne && was_airborne && !airborne) land_left=.5f;
      was_airborne=airborne; have_airborne=true;
    }
    if (have_fire && p.fire != last_fire && uint32_t(p.fire-last_fire) < 0x80000000u) {
      fire_left = .35f; recoil_left=.22f; new_shot = true; last_fire = p.fire;
    }
    if (!have_fire) last_fire = p.fire;
    have_fire = true;
    const bool gun=weapon>=1 && weapon<=4;
    const bool reload=gun && last_action==weapon+0x72;
    const float aim_target=gun && !reload && (aiming || fire_left>0) ? 1.f : 0.f;
    aim_weight+=(aim_target-aim_weight)*(1-std::exp(-dt*15.f));
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
