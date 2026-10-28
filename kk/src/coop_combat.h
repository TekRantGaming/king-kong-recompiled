#pragma once
#include "coop_protocol.h"
#include "coop_mut.h"
#include <cmath>
#include <optional>

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
enum class GrabFeedback { Pending, Accepted, Release };
inline bool RaptorViewRead(uint32_t caller) {
  if (COOP_MUT_IF("raptor_camera")) return true;
  // Only the selected camera-reference reads in824641B0. Its target,
  // attacker and jaw-anchor reads keep their physical positions.
  return caller==0x82464320 || caller==0x82464374 || caller==0x824643E0 || caller==0x8246446C;
}
inline std::optional<float> RaptorViewHeight(EnemyFamily family,uint32_t kind,uint32_t caller,
                                           ShotVector foot,ShotVector view,bool local_crouched,bool peer_crouched) {
  if (family!=EnemyFamily::Raptor || kind!=16 || !RaptorViewRead(caller) || local_crouched!=peer_crouched) return {};
  const float x=view.x-foot.x,y=view.y-foot.y,z=view.z-foot.z;
  // Use the actual native first person camera offset, never a guessed eye
  // height or a remote address. Detached/cinematic cameras are not an anchor.
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::hypot(x,y)>1 || z<.05f || z>3) return {};
  return z;
}
inline bool NativePlayerScriptTarget(uint32_t authored,uint32_t selected,uint32_t hero,uint32_t body) {
  if (COOP_MUT_IF("native_script_target")) return true;
  // An authored nonplayer target belongs to the story. Only alias an existing
  // player target to the player already accepted by the native selector.
  return hero && body && hero!=body && (authored==hero || authored==body) &&
      (selected==hero || selected==body);
}
inline bool NativeScorpionGrabStart(const DamageEvent& d) {
  if (COOP_MUT_IF("grab_start")) return true;
  // Scorpion82616490 uses3/10 for a player body and256/0 for an NPC body.
  // Both are real grab starts. Preserve the zero scalar NPC reaction.
  return d.enemy.family==EnemyFamily::Scorpion && !d.projectile &&
      ((d.flags==3 && d.magnitude==10) || (d.flags==256 && !d.magnitude));
}
inline bool NativeAttackSound(const DamageEvent& d) {
  if (COOP_MUT_IF("attack_sound")) return true;
  // Scorpion82616490 plays sound8 immediately before producing5/10.
  // Bat82652FF8 plays sound10 immediately after its player contact2/2.
  return !d.projectile && ((d.enemy.family==EnemyFamily::Scorpion && d.flags==5 && d.magnitude==10) ||
      (d.enemy.family==EnemyFamily::Bat && d.flags==2 && d.magnitude==2));
}
inline uint32_t NativeAttackSoundResource(const DamageEvent& d) {
  return NativeAttackSound(d) ? (d.enemy.family==EnemyFamily::Bat ? 10:8):0;
}
inline bool BatContactSound(const DamageEvent& d,EnemyId source,uint32_t generation,
                            uint32_t caller,uint32_t resource,double now) {
  if (COOP_MUT_IF("bat_contact_sound")) return true;
  // Only the successful sound call immediately following the actual player
  // contact may annotate that still unsent event. The separate grab cue at
  //8265408C is not evidence of a player strike.
  return !d.attack_sound && d.enemy.family==EnemyFamily::Bat && NativeAttackSound(d) &&
      d.enemy==source && d.generation==generation && caller==0x82654038 && resource==10 &&
      Fresh(d.time,now) && now-d.time<=.1;
}
inline bool AttackSoundObserved(const DamageEvent& d,EnemyId source,uint32_t generation,
                                double observed,double now,bool consumed) {
  return NativeAttackSound(d) && d.enemy==source && d.generation==generation && !consumed &&
      Fresh(observed,now) && now-observed<=.1;
}
inline bool NativeGrabOwnerReleased(const PlayerStatus& p,EnemyWorld world,uint64_t incarnation,
                                   EnemyId enemy,uint32_t generation,uint32_t first_damage) {
  return ValidPlayerStatus(p) && p.world==world && p.peer_grab_incarnation==incarnation &&
      first_damage && p.peer_grab_number>=first_damage &&
      (p.peer_grab_enemy!=enemy || p.peer_grab_generation!=generation);
}
inline GrabFeedback NativeGrabFeedback(const PlayerStatus& p, EnemyWorld world,uint64_t incarnation,
                                      EnemyId enemy,uint32_t generation,uint32_t first_damage,
                                      double started,double received,double now) {
  if (COOP_MUT_IF("grab_feedback")) return GrabFeedback::Accepted;
  if (!ValidPlayerStatus(p) || p.world!=world || p.incarnation!=incarnation ||
      p.condition==PlayerCondition::Dead || (!p.grab_ready && !p.grab_enemy.source) || !Fresh(received,now) ||
      !std::isfinite(started) || now<started || !first_damage) return GrabFeedback::Release;
  if (p.grab_enemy==enemy && p.grab_generation==generation && p.grab_kind==25) return GrabFeedback::Accepted;
  if (p.grab_enemy.source || p.last_damage>=first_damage || now-started>kMaxAge) return GrabFeedback::Release;
  return GrabFeedback::Pending;
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
inline uint32_t ImpactChild(uint32_t native) {
  // 82634428 tests -1, then uses only the low byte for group_instance_find.
  // Javelin_launch can leave float tolerance bits in this native word when
  // the collision is with an H model. Carry the effective child selector,
  // rather than rejecting that real hit or transporting unrelated high bits.
  return native==UINT32_MAX || COOP_MUT_IF("impact_selector") ? native : native&0xFF;
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
