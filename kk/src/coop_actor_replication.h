#pragma once
#include "coop_protocol.h"
#include "coop_mut.h"
#include <cmath>

namespace kk::coop::actors {
constexpr float kRadius = 160.f, kSnapDistance = 5.f;
constexpr double kInterval = .05, kBuffer = .1, kRefresh = .25;
// Only native runtime flags, never component ownership or scheduler marks.
constexpr uint32_t kFlagsMask = 0x00200001; // hidden (8270BED8), collision (8284BDA8).
inline bool IndependentTrack(uint32_t fn) {
  switch (fn) {
    case 0x826ECDF8: case 0x826F9788: case 0x826CC8E8: case 0x826F2D20:
    case 0x826F2E30: case 0x826F34B0: case 0x826F3BE8: case 0x826F4450:
    case 0x826F45A8: case 0x826F7AD0: case 0x826F7C78: return true;
    default: return false;
  }
}
inline uint32_t TargetOffset(EnemyFamily family) {
  switch (family) {
    case EnemyFamily::Bat: return 848;
    case EnemyFamily::Native: return 4088;
    case EnemyFamily::NativeKna: return 4084;
    case EnemyFamily::Marine: return 348;
    case EnemyFamily::VRex: return 516;
    case EnemyFamily::SwampCrawler: return 556;
    case EnemyFamily::SwampCrawlerSmall: return 1912;
    case EnemyFamily::Human: return 0xC60;
    case EnemyFamily::Independent: return 0x290;
    default: return 0;
  }
}
inline bool Relevant(const EnemyState& e, const PlayerPose& player) {
  return std::hypot(e.x-player.x,e.y-player.y,e.z-player.z)<=kRadius;
}
inline bool Freeze(bool enabled, bool guest, bool connected, bool world,
                   bool binding, bool player, bool initialized) {
  if (COOP_MUT_IF("actor_freeze_off")) return false;
  return enabled && (guest || COOP_MUT_IF("actor_freeze_host")) && connected && world &&
      binding && !player && initialized;
}
inline EnemyState Interpolate(const EnemyState& a, const EnemyState& b, float t) {
  if (COOP_MUT_IF("actor_interpolation_latest")) return b;
  if (!COOP_MUT_IF("actor_interpolation_no_snap") && std::hypot(b.x-a.x,b.y-a.y,b.z-a.z)>kSnapDistance) return b;
  auto out=a;
  out.x+=(b.x-a.x)*t; out.y+=(b.y-a.y)*t; out.z+=(b.z-a.z)*t;
  out.facing=std::remainder(a.facing+std::remainder(b.facing-a.facing,6.2831853f)*t,6.2831853f);
  if (a.action==b.action && a.item==b.item) {
    if (b.animation_time>=a.animation_time) out.animation_time+=(b.animation_time-a.animation_time)*t;
    else if (a.animation_duration>0 && a.animation_duration==b.animation_duration &&
             a.animation_time<=a.animation_duration && b.animation_time>=0)
      out.animation_time=std::fmod(a.animation_time+(a.animation_duration-a.animation_time+b.animation_time)*t,a.animation_duration);
  }
  return out;
}
// A record delta is absolute and repeated after a short interval. It never
// depends on the delivery of an earlier unreliable record.
inline bool Same(const EnemyState& a,const EnemyState& b) {
  return a.id==b.id && a.generation==b.generation && a.owner==b.owner &&
      a.x==b.x && a.y==b.y && a.z==b.z && a.facing==b.facing && a.life==b.life &&
      a.secondary_life==b.secondary_life && a.state==b.state && a.action==b.action &&
      a.item==b.item && a.animation_time==b.animation_time && a.animation_duration==b.animation_duration && a.actor_flags==b.actor_flags &&
      a.target==b.target && a.target_crew==b.target_crew;
}
}
