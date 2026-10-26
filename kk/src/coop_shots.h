#pragma once
#include "coop_protocol.h"
#include "coop_mut.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <deque>
#include <map>

namespace kk::coop {
struct PendingShot { ShotIntent intent; InventoryState inventory; };
inline bool ValidPendingShot(const PendingShot& shot) {
  return ValidShotIntent(shot.intent) && ValidInventory(shot.inventory) &&
      shot.inventory.world == shot.intent.world && shot.inventory.weapon == shot.intent.weapon &&
      shot.inventory.time == shot.intent.time;
}
struct ReceivedShot {
  ShotIntent intent;
  double received = 0;
  std::optional<InventoryState> inventory;
  std::optional<PlayerPose> peer; // Historical shooter pose in the shooter's clock domain.
};
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
    case ShotReject::Late: return "late";
    default: return "unknown";
  }
}
struct ShotCounters {
  uint64_t applied = 0, rejected = 0, late = 0;
  ShotReject last = ShotReject::None;
  uint64_t reasons[size_t(ShotReject::Count)]{};
  void Reject(ShotReject r) {
    if (r == ShotReject::Late) { ++late; return; }
    ++rejected; last = r; ++reasons[size_t(r)];
  }
};
inline float ShotDistance(ShotVector a, ShotVector b) { return std::hypot(a.x-b.x, a.y-b.y, a.z-b.z); }
// Conservative policy limits, not claims about the game's exact gun cadence/range.
inline double ShotInterval(uint8_t weapon) {
  switch (weapon) { case 1: return .15; case 2: return .045; case 3: return .6; case 4: return .6; default: return 1; }
}
class ShotValidator {
 public:
  ShotReject Validate(const ReceivedShot& in, EnemyWorld world,
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
    // Reliable pre-fire inventory belongs to this guest and world, never the host Jack.
    if (!in.inventory || !ValidInventory(*in.inventory) || in.inventory->world != world ||
        in.inventory->weapon != s.weapon || in.inventory->time > s.time ||
        s.time-in.inventory->time > .5) return ShotReject::Weapon;
    const auto& inventory = *in.inventory;
    if (inventory_seen_ && inventory.time < inventory_time_) return ShotReject::Weapon;
    if (!inventory_seen_ || inventory.time != inventory_time_ || inventory.weapon != inventory_weapon_) {
      inventory_seen_ = true; inventory_time_ = inventory.time; inventory_weapon_ = inventory.weapon;
      rounds_ = inventory.ammo[s.weapon-1];
    }
    if (!rounds_) return ShotReject::Weapon;
    if (!COOP_MUT_IF("shot_validator") && ShotDistance(s.origin, {peer->x, peer->y, peer->z}) > 4) return ShotReject::Origin;
    // Reliable delivery can batch a quarter second of gunfire after jitter or
    // retransmission. Bound that burst while retaining sender cadence checks.
    const double interval = ShotInterval(s.weapon);
    if (fired_ && (s.time < last_time_ || s.time-last_time_ < interval-.005)) return ShotReject::Rate;
    const double capacity = std::ceil(.5/interval)+2;
    if (!fired_) tokens_ = capacity;
    tokens_ = std::min(capacity, tokens_ + std::max(0., now-last_arrival_)/interval);
    last_arrival_ = now;
    if (tokens_ < 1) return ShotReject::Rate;
    tokens_ -= 1; last_time_ = s.time; fired_ = true; --rounds_;
    return ShotReject::None;
  }
 private:
  bool seen_ = false, fired_ = false;
  uint32_t number_ = 0;
  double last_time_ = 0, last_arrival_ = 0, tokens_ = 2;
  bool inventory_seen_ = false;
  double inventory_time_ = 0;
  uint8_t inventory_weapon_ = 0;
  uint32_t rounds_ = 0;
};
inline ShotReject ValidateShotHit(const ShotIntent& shot, const ShotHit& hit, const EnemyState& target) {
  if (!ValidShotIntent(shot)) return ShotReject::Bounds;
  if (!ValidEnemy(target) || hit.id != target.id || hit.generation != target.generation) return ShotReject::Target;
  const float distance = ShotDistance(shot.origin, hit.point);
  if (!std::isfinite(distance) || distance < .01f || distance > (shot.weapon == 3 ? 80.f : 300.f) ||
      ShotDistance(hit.point, {target.x, target.y, target.z}) > 6) return ShotReject::Distance;
  const float dot = ((hit.point.x-shot.origin.x)*shot.direction.x +
      (hit.point.y-shot.origin.y)*shot.direction.y + (hit.point.z-shot.origin.z)*shot.direction.z)/distance;
  if (dot < (shot.weapon == 3 ? .85f : .98f)) return ShotReject::Aim;
  if (target.life <= 0) return ShotReject::Late;
  return ShotReject::None;
}
// A guest fires at an interpolated host snapshot. Keep a bounded host-owned
// history for latency compensation instead of widening the six-unit hit bound.
class ShotTargetHistory {
 public:
  void Observe(const EnemyState& target, double now) {
    if (!ValidEnemy(target) || !std::isfinite(now)) return;
    if (!targets_.contains(target.id) && targets_.size() >= kMaxEnemies) return;
    auto& samples = targets_[target.id];
    if (!samples.empty() && now-samples.back().time < .04) return;
    samples.push_back({now,target});
    while (samples.size()>24 || now-samples.front().time>1) samples.pop_front();
  }
  ShotReject Validate(const ShotIntent& shot, const ShotHit& hit, const EnemyState& current, double now) const {
    const auto reason = ValidateShotHit(shot,hit,current);
    if (reason != ShotReject::Distance || !std::isfinite(now)) return reason;
    const auto found=targets_.find(hit.id);
    if (found==targets_.end()) return reason;
    for (const auto& sample:found->second) {
      if (now<sample.time || now-sample.time>.75 || sample.target.generation!=current.generation ||
          sample.target.owner!=current.owner) continue;
      auto target=sample.target;
      target.life=current.life; // A delayed shot can never revive or re-kill a tombstone.
      const auto prior=ValidateShotHit(shot,hit,target);
      if (prior==ShotReject::None || prior==ShotReject::Late) return prior;
    }
    return reason;
  }
 private:
  struct Sample { double time; EnemyState target; };
  std::map<EnemyId,std::deque<Sample>> targets_;
};
} // namespace kk::coop
