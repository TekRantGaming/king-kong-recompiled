#pragma once
#include "coop_protocol.h"
#include "coop_mut.h"
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
// Exact pairs from Raptor bite8256D2F0 and grab825CF4D8. Zero-strength
// release reactions still have native meaning; they are not invented damage.
inline bool RaptorHit(uint32_t flags, uint32_t magnitude) {
  return (flags == 2048 && (magnitude == 1 || magnitude == 2 || magnitude == 4)) ||
      (flags == 4096 && magnitude == 1) || (flags == 16644 && magnitude == 1000) ||
      (flags == 16388 && (magnitude == 6 || magnitude == 10)) || (flags == 16386 && magnitude == 6) ||
      ((flags == 18960 || flags == 8192 || flags == 2560) && magnitude == 1000) ||
      (flags == 516 && magnitude == 1) || ((flags == 4 || flags == 256) && !magnitude);
}
// The authority captures an already produced native type-4 event. Bounds here
// do not create hits or decide contact, range, immunity or the native reaction.
inline bool NativeHit(EnemyFamily family, uint32_t flags, uint32_t magnitude, bool projectile = false) {
  if (family == EnemyFamily::Raptor && !projectile) return RaptorHit(flags, magnitude);
  // Marine reflex824B42E4 uses512 with magnitude0 after actual timed contact.
  // The native reaction is carried by its flags, not a positive scalar.
  if (family == EnemyFamily::Marine && !projectile && flags == 512 && !magnitude) return true;
  // Scorpion grab82616490 sends256/0 for the native NPC hold and512/0
  // when the hold times out. Do not drop those native controller reactions.
  if (family == EnemyFamily::Scorpion && !projectile && (flags == 256 || flags == 512) && !magnitude) return true;
  // KT neighbor contact8246AC88 emits an impulse with flags1 or33 and no
  // scalar damage. Retain that native reaction through the impact builder.
  if (family == EnemyFamily::VRex && projectile && (flags == 1 || flags == 33) && !magnitude) return true;
  switch (family) {
    case EnemyFamily::Raptor: case EnemyFamily::Crab: case EnemyFamily::Worm: case EnemyFamily::Scorpion:
    case EnemyFamily::Bat: case EnemyFamily::Centipede: case EnemyFamily::Native: case EnemyFamily::NativeKna:
    case EnemyFamily::Marine: case EnemyFamily::VRex: case EnemyFamily::SwampCrawler:
    case EnemyFamily::SwampCrawlerSmall: case EnemyFamily::Spider: case EnemyFamily::Pny:
      return flags && flags <= 0x00FFFFFF && magnitude && magnitude <= 1000000;
    default: return false;
  }
}
inline bool NativeImpactProducer(EnemyFamily family,uint32_t caller) {
  // KT native contact query, charge, skid, held-target strike, colmap exit,
  // proximity contact and falling-body impact. These build type4 directly.
  if (family!=EnemyFamily::VRex) return false;
  switch (caller) {
    case 0x82473380: case 0x8248C9A4: case 0x8248D10C: case 0x824866F0:
    case 0x82487D9C: case 0x824884B8: case 0x82468C40: case 0x8246B024: case 0x8248FC34:
      return true;
    default: return false;
  }
}
struct ProjectileLayout {
  uint32_t source = 0, bytes = 0;
  bool Reference(uint32_t offset) const {
    if (source == 232) return offset == 264 || offset == 364 || offset == 428;
    if (source == 44) return offset == 76 || offset == 176 || offset == 228;
    return false;
  }
};
inline ProjectileLayout NativeProjectile(uint32_t track) {
  // Javelin_launch and KPJ_launch retain real collision and homing targets.
  // The source field is deliberately excluded: a player-owned projectile is
  // not evidence that a story human is available as a partner body.
  if (track == 0x8258C5F8) return {232, 568};
  if (track == 0x824DF570) return {44, 384};
  return {};
}
template <class U32, class U16, class Address, class Source>
bool ProjectileReference(uint32_t actor, uint32_t ai, uint32_t vars, uint32_t offset,
                         U32 u32, U16 u16, Address address, Source source_ready) {
  if (!address(actor,32) || !address(ai,88) || !address(vars,4)) return false;
  const auto ext=u32(actor+24);
  if (!address(ext,8) || u32(ext+4)!=ai || u32(ai+64)!=vars) return false;
  const auto first=u16(ai+80),last=u16(ai+82);
  if (first>last || last>4096 || last-first>=64 || !address(ai,(last+12)*4)) return false;
  for (uint32_t t=first;t<=last;++t) {
    const auto block=u32(ai+(t+6)*4);
    if (!address(block,24)) continue;
    const auto layout=NativeProjectile(u32(block+20));
    if (layout.Reference(offset) && address(vars,layout.bytes) &&
        (COOP_MUT_IF("projectile_source") || source_ready(u32(vars+layout.source)))) return true;
  }
  return false;
}
inline bool RemoteOwner(EnemyOwner owner, EnemyOwner local, uint32_t generation,
                        uint32_t hit_generation, bool frozen) {
  return !frozen && owner <= EnemyOwner::Guest && local <= EnemyOwner::Guest &&
      owner != local && generation == hit_generation;
}
inline bool CandidateReference(uint32_t offset, uint32_t count) {
  return count <= 20 && offset >= 6244 && offset < 6244+4*count && (offset-6244)%4 == 0;
}
struct CandidateTable { uint32_t count = 0, actors = 0, limit = 0; };
inline CandidateTable Candidates(EnemyFamily family) {
  // Native registration helpers: 8246DC58, 825533E0, 82508158,
  // 826E6948 and ScoloInterest27. Offsets are never peer supplied.
  switch (family) {
    case EnemyFamily::Raptor: return {5236, 6244, 20};
    case EnemyFamily::Worm: return {2964, 4056, 20};
    case EnemyFamily::Scorpion: return {3012, 4020, 20};
    case EnemyFamily::Spider: return {552, 1140, 20};
    case EnemyFamily::Centipede: return {1400, 3236, 50};
    default: return {};
  }
}
inline bool CandidateReference(CandidateTable table, uint32_t offset, uint32_t count) {
  return table.limit && count <= table.limit && offset >= table.actors &&
      offset < table.actors+4*count && (offset-table.actors)%4 == 0;
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
} // namespace kk::coop::combat
