#pragma once
#include "coop_protocol.h"
#include <cmath>

namespace kk::coop::combat {
constexpr size_t kQueueLimit = 32;
constexpr double kMaxAge = 1.0;
inline bool Fresh(double received, double now) {
  return std::isfinite(received) && std::isfinite(now) && now >= received && now-received <= kMaxAge;
}
inline bool DamageClock(double hit_time, double host_pose_time) {
  // Both times belong to the host. No assumption about clock synchronization
  // between machines; pose interpolation accounts for up to 100 ms of lag.
  return std::isfinite(hit_time) && std::isfinite(host_pose_time) &&
      hit_time >= host_pose_time-kMaxAge && hit_time <= host_pose_time+.5;
}
// Exact flags observed in Raptor 8256D2F0's non-grab branches (.17).
inline bool RaptorHit(uint32_t flags, uint32_t magnitude) {
  return (flags == 4096 && magnitude == 1) || (flags == 16644 && magnitude == 1000) ||
      (flags == 16388 && (magnitude == 6 || magnitude == 10)) || (flags == 16386 && magnitude == 6);
}
inline bool CandidateReference(uint32_t offset, uint32_t count) {
  return count <= 20 && offset >= 6244 && offset < 6244+4*count && (offset-6244)%4 == 0;
}
enum class VictimAction { Pass, ProtectBody, SuppressDuplicate };
inline VictimAction FilterVictim(uint32_t type, uint32_t victim, uint32_t body,
                                uint32_t jack, bool guest, bool matched_source, bool injecting) {
  if (type != 4 || !victim) return VictimAction::Pass;
  if (body && victim == body) return VictimAction::ProtectBody;
  if (guest && victim == jack && matched_source && !injecting) return VictimAction::SuppressDuplicate;
  return VictimAction::Pass;
}
// Advance before native enqueue. Retrying a packet may never repeat a hit, even
// if the pool was full or Jack's native immunity rejected the first attempt.
class DamageReplay {
 public:
  void Reset(uint64_t epoch, EnemyWorld world, uint64_t incarnation) {
    epoch_ = epoch; world_ = world; incarnation_ = incarnation; last_ = 0;
  }
  bool Accept(const Message& m, double received, double now) {
    if (m.type != MessageType::DamageEvent || !epoch_ || m.epoch != epoch_ ||
        !ValidDamageEvent(m.damage) || m.damage.world != world_ ||
        m.damage.incarnation != incarnation_ || m.damage.number <= last_ || !Fresh(received, now)) return false;
    last_ = m.damage.number; return true;
  }
  uint32_t Last() const { return last_; }
 private:
  uint64_t epoch_ = 0, incarnation_ = 0;
  EnemyWorld world_;
  uint32_t last_ = 0;
};
class StatusReplay {
 public:
  void Reset(uint64_t epoch, EnemyWorld world) { epoch_ = epoch; world_ = world; status_ = {}; }
  bool Accept(const Message& m, double received, double now) {
    const auto& p = m.player_status;
    if (m.type != MessageType::PlayerStatus || !epoch_ || m.epoch != epoch_ ||
        !ValidPlayerStatus(p) || p.world != world_ || !Fresh(received, now) ||
        p.incarnation < status_.incarnation ||
        (p.incarnation == status_.incarnation && (p.number <= status_.number ||
          p.last_damage < status_.last_damage ||
          (status_.condition == PlayerCondition::Dead && p.condition != PlayerCondition::Dead)))) return false;
    status_ = p; return true;
  }
  const PlayerStatus& Status() const { return status_; }
 private:
  uint64_t epoch_ = 0;
  EnemyWorld world_;
  PlayerStatus status_;
};
// The native teleport path is known, but a safe reset of death tracks/control is
// not. Production therefore retains C24's both-reload fallback. Keep the delay
// policy explicit so a future audited native revive cannot revive both-dead.
enum class DeathAction { None, Wait, Revive, ReloadBoth };
class DeathPolicy {
 public:
  DeathAction Step(bool local_dead, bool partner_dead, double now, bool revive_safe = false) {
    if (!std::isfinite(now)) return DeathAction::None;
    if (!local_dead) { since_ = -1; return DeathAction::None; }
    if (partner_dead || !revive_safe) return DeathAction::ReloadBoth;
    if (since_ < 0 || now < since_) since_ = now;
    return now-since_ >= 3 ? DeathAction::Revive : DeathAction::Wait;
  }
 private:
  double since_ = -1;
};
} // namespace kk::coop::combat
