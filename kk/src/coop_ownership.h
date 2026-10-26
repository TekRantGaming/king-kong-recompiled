#pragma once
#include "coop_protocol.h"
#include "coop_mut.h"
#include <cmath>
#include <optional>

namespace kk::coop::enemies {
inline EnemyOwner Other(EnemyOwner owner) {
  return owner == EnemyOwner::Host ? EnemyOwner::Guest : EnemyOwner::Host;
}
// Distances are measured in native world units. The return threshold is wider
// than the grant threshold, so equal-distance movement cannot ping-pong AI.
class Proximity {
 public:
  std::optional<EnemyOwner> Decide(EnemyOwner owner, float host, float guest, double now, bool eligible) {
    bool change = eligible && std::isfinite(host) && std::isfinite(guest) &&
        host >= 0 && guest >= 0 && std::isfinite(now) &&
        (owner == EnemyOwner::Host ? guest < .6f*host && guest < 25.f :
         (host < .8f*guest && host < 25.f) || (guest > 30.f && host < guest));
    if (!change) { since_ = -1; return {}; }
    if (since_ < 0 || candidate_ != owner || now < since_) { since_ = now; candidate_ = owner; }
    if (now-since_ <= 1.) return {};
    since_ = -1; return Other(owner);
  }
 private:
  double since_ = -1;
  EnemyOwner candidate_ = EnemyOwner::Host;
};

// Game-thread handover state. A prepare freezes the old owner before Ready is
// made. Commit is applied locally only after the reliable send succeeds. No AI
// reinitialization or native script addresses are part of this state machine.
class Authority {
 public:
  EnemyOwner owner = EnemyOwner::Host;
  uint32_t generation = 0;
  bool dead = false, matched = false;
  EnemyState last;
  std::optional<OwnershipChange> pending;
  Proximity proximity;
  bool Frozen() const { return pending.has_value(); }
  bool Runs(EnemyOwner side) const { return !Frozen() && owner == side; }
  bool Accepts(const EnemyState& e) const {
    return e.owner == owner && (COOP_MUT_IF("ownership_generation") || e.generation == generation) && (!last.id.source || e.id == last.id);
  }
  bool Observe(const EnemyState& e) {
    if (!ValidEnemy(e) || !Accepts(e) || Frozen() || dead) return false;
    last = e; dead = e.life <= 0; return true;
  }
  std::optional<OwnershipChange> Begin(EnemyWorld world, EnemyOwner target, const EnemyState& e) {
    if (!ValidEnemyWorld(world) || target > EnemyOwner::Guest || !matched || dead || Frozen() || target == owner || generation == UINT32_MAX ||
        !Accepts(e) || !ValidEnemy(e) || e.life <= 0) return {};
    pending = OwnershipChange{world, OwnershipPhase::Prepare, generation+1, target, e};
    last = e; return pending;
  }
  std::optional<OwnershipChange> Prepare(const OwnershipChange& c, const EnemyState& local) {
    if (!ValidOwnershipChange(c) || c.phase != OwnershipPhase::Prepare ||
        c.generation != generation+1 || !Accepts(c.enemy) || (pending && pending->generation != c.generation)) return {};
    if (!pending) {
      // Only the guest can be the old remote owner. Host-owned poses come from
      // Prepare; a guest owner returns its final locally sampled state instead.
      auto final = owner == EnemyOwner::Guest ? local : c.enemy;
      if (!ValidEnemy(final) || !Accepts(final)) return {};
      pending = c; pending->enemy = final; last = final;
    }
    auto ready = *pending; ready.phase = OwnershipPhase::Ready; return ready;
  }
  std::optional<OwnershipChange> Ready(const OwnershipChange& c) const {
    if (!pending || !ValidOwnershipChange(c) || c.phase != OwnershipPhase::Ready ||
        c.world != pending->world || c.generation != pending->generation || c.owner != pending->owner || !Accepts(c.enemy)) return {};
    auto commit = c; commit.phase = OwnershipPhase::Commit;
    commit.enemy = owner == EnemyOwner::Host ? pending->enemy : c.enemy;
    // A final native hit can kill the old owner just before Prepare arrives.
    // Publish that death without transferring the corpse to the other peer.
    if (commit.enemy.life <= 0) commit.owner = owner;
    commit.enemy.owner = commit.owner; commit.enemy.generation = commit.generation;
    return commit;
  }
  bool Commit(const OwnershipChange& c) {
    if (!pending || !ValidOwnershipChange(c) || c.phase != OwnershipPhase::Commit ||
        c.world != pending->world || c.enemy.id != last.id || c.generation != pending->generation ||
        (c.owner != pending->owner && !(c.owner == owner && c.enemy.life <= 0))) return false;
    owner = c.owner; generation = c.generation; last = c.enemy;
    dead = dead || last.life <= 0; pending.reset(); return true;
  }
};
// An in-flight shot belongs to precisely one authority generation. On handover
// it expires instead of being applied twice or forwarded back and forth.
inline bool RoutesShot(const Authority& a, EnemyOwner recipient, const ShotHit& hit) {
  return a.Runs(recipient) && hit.generation == a.generation && hit.id == a.last.id;
}
} // namespace kk::coop::enemies
