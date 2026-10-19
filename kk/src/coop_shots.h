#pragma once
#include "coop_protocol.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace kk::coop {
struct ReceivedShot { ShotIntent intent; double received = 0; };
enum class ShotReject : uint8_t { None, Bounds, World, Sequence, Stale, Weapon, Rate, Origin, Target, Distance, Aim, Engine, Queue, Count };
inline const char* ShotRejectName(ShotReject r) {
  switch (r) {
    case ShotReject::None: return "none";
    case ShotReject::Bounds: return "bounds";
    case ShotReject::World: return "world";
    case ShotReject::Sequence: return "sequence";
    case ShotReject::Stale: return "stale";
    case ShotReject::Weapon: return "weapon-owned";
    case ShotReject::Rate: return "rate";
    case ShotReject::Origin: return "origin";
    case ShotReject::Target: return "target";
    case ShotReject::Distance: return "distance";
    case ShotReject::Aim: return "aim";
    case ShotReject::Engine: return "engine";
    case ShotReject::Queue: return "queue";
    default: return "unknown";
  }
}
struct ShotCounters {
  uint64_t applied = 0, rejected = 0;
  ShotReject last = ShotReject::None;
  uint64_t reasons[size_t(ShotReject::Count)]{};
  void Reject(ShotReject r) { ++rejected; last = r; ++reasons[size_t(r)]; }
};
inline float ShotDistance(ShotVector a, ShotVector b) { return std::hypot(a.x-b.x, a.y-b.y, a.z-b.z); }
// Conservative policy limits, not claims about the game's exact gun cadence/range.
inline double ShotInterval(uint8_t weapon) {
  switch (weapon) { case 1: return .15; case 2: return .045; case 3: return .6; case 4: return .6; default: return 1; }
}
class ShotValidator {
 public:
  ShotReject Validate(const ReceivedShot& in, EnemyWorld world, uint8_t owned_weapon,
      const std::optional<PlayerPose>& peer, double now) {
    const auto& s = in.intent;
    if (!ValidShotIntent(s)) return ShotReject::Bounds;
    if (s.world != world) return ShotReject::World;
    if (seen_ && int32_t(s.number-number_) <= 0) return ShotReject::Sequence;
    // Consume the number even on rejection. A failed shot cannot later be replayed.
    seen_ = true; number_ = s.number;
    if (!std::isfinite(now) || !std::isfinite(in.received) || in.received < 0 || now < in.received || now-in.received > .5 ||
        !peer || peer->level != world.level || !std::isfinite(peer->time) ||
        !std::isfinite(peer->x) || !std::isfinite(peer->y) || !std::isfinite(peer->z) ||
        std::abs(s.time-peer->time) > 1) return ShotReject::Stale;
    if (s.weapon != owned_weapon) return ShotReject::Weapon;
    if (ShotDistance(s.origin, {peer->x, peer->y, peer->z}) > 4) return ShotReject::Origin;
    // Two-shot burst permits network batching; sustained fire is bounded by both clocks.
    const double interval = ShotInterval(s.weapon);
    if (fired_ && (s.time < last_time_ || s.time-last_time_ < interval-.005)) return ShotReject::Rate;
    tokens_ = std::min(2., tokens_ + std::max(0., now-last_arrival_)/interval);
    last_arrival_ = now;
    if (tokens_ < 1) return ShotReject::Rate;
    tokens_ -= 1; last_time_ = s.time; fired_ = true;
    return ShotReject::None;
  }
 private:
  bool seen_ = false, fired_ = false;
  uint32_t number_ = 0;
  double last_time_ = 0, last_arrival_ = 0, tokens_ = 2;
};
inline ShotReject ValidateShotHit(const ShotIntent& shot, const ShotHit& hit, const EnemyState& target) {
  if (!ValidShotIntent(shot)) return ShotReject::Bounds;
  if (!ValidEnemy(target) || hit.id != target.id || target.life <= 0) return ShotReject::Target;
  const float distance = ShotDistance(shot.origin, hit.point);
  if (!std::isfinite(distance) || distance < .01f || distance > (shot.weapon == 3 ? 80.f : 300.f) ||
      ShotDistance(hit.point, {target.x, target.y, target.z}) > 6) return ShotReject::Distance;
  const float dot = ((hit.point.x-shot.origin.x)*shot.direction.x +
      (hit.point.y-shot.origin.y)*shot.direction.y + (hit.point.z-shot.origin.z)*shot.direction.z)/distance;
  if (dot < (shot.weapon == 3 ? .85f : .98f)) return ShotReject::Aim;
  return ShotReject::None;
}
} // namespace kk::coop
