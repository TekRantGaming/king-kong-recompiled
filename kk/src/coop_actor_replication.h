#pragma once
#include "coop_protocol.h"
#include "coop_mut.h"
#include <algorithm>
#include <cmath>

namespace kk::coop::actors {
constexpr float kRadius = 160.f, kSnapDistance = 5.f;
constexpr double kInterval = .05, kBuffer = .1, kRefresh = .25;
inline double SmoothClock(double previous,double observed,double elapsed) {
  if (COOP_MUT_IF("actor_clock_filter")) return observed;
  const double limit=std::max(0.,elapsed)*.05;
  return previous+std::clamp((observed-previous)*.1,-limit,limit);
}
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
inline bool InScope(bool enabled,bool terminal,const EnemyState& e,const PlayerPose* local,const PlayerPose* peer) {
  if (COOP_MUT_IF("actor_relevance_all")) return true;
  return !enabled || terminal || !local || Relevant(e,*local) || (peer && Relevant(e,*peer));
}
inline bool Freeze(bool enabled, bool guest, bool connected, bool world,
                   bool binding, bool player, bool initialized,bool retiring=false,
                   bool prepared=true,double prepared_age=0) {
  if (COOP_MUT_IF("actor_freeze_off")) return false;
  // Authority survives a long render or loading stall. Preparation freshness
  // guards writes elsewhere, but cannot give native AI back to a replica.
  if (COOP_MUT_IF("actor_freeze_prepared") && (!prepared || prepared_age>.5)) return false;
  return enabled && (guest || COOP_MUT_IF("actor_freeze_host")) && connected && world &&
      binding && !player && initialized && (!retiring || COOP_MUT_IF("actor_freeze_retire"));
}
template<class ReplicaHold,class PartnerHold,class Recheck>
inline bool HoldDispatch(ReplicaHold replica,PartnerHold partner,Recheck recheck) {
  if (replica() || partner()) return true;
  // The partner service can release an authored donor inside its hold call.
  // That donor is a replica again before any native script may execute.
  return !COOP_MUT_IF("actor_freeze_release") && recheck();
}
inline bool Initialize(bool enabled,bool guest,bool fresh,bool exact,bool player,bool initializer,
                       uint16_t first=0,uint16_t last=0) {
  return enabled && guest && fresh && (exact || COOP_MUT_IF("actor_init_exact")) && !player && initializer &&
      (COOP_MUT_IF("actor_init_tracks") || (first==0 && last==0));
}
// Digest pages describe an older instant than the buffered actor stream.
// Keep their diagnostics, but let snapshots alone repair replicated actors.
inline bool DigestRepairs(bool enabled) {
  return !enabled || COOP_MUT_IF("actor_digest_repair");
}
inline bool WorldAuthority(bool session,bool flow_active,bool playing,bool together) {
  if (COOP_MUT_IF("actor_pose_release")) return true;
  return session && (!flow_active || (playing && together));
}
inline bool SnapPose(const EnemyState& a,const EnemyState& b) {
  if (COOP_MUT_IF("actor_interpolation_no_snap")) return false;
  // Damage can start native knockback between publications. Blending an old
  // approach pose through that step invents a path the host never followed.
  if ((a.life!=b.life || a.secondary_life!=b.secondary_life) &&
      !COOP_MUT_IF("actor_hit_interpolation")) return true;
  return std::hypot(b.x-a.x,b.y-a.y,b.z-a.z)>kSnapDistance;
}
inline bool MissingPoseSamples(double span) {
  // Normal 20 Hz publications can alternate native frame intervals. Beyond
  // one and a half intervals, missing motion must not become a guessed curve.
  return span>kInterval*1.5 && !COOP_MUT_IF("actor_gap_interpolation");
}
inline EnemyState Interpolate(const EnemyState& a, const EnemyState& b, float t) {
  if (COOP_MUT_IF("actor_interpolation_latest")) return b;
  if (SnapPose(a,b)) return b;
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
