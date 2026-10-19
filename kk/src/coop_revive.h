#pragma once
#include "coop_protocol.h"
#include <cmath>

namespace kk::coop::revive {
// Native audit, generated/default/king_kong_recomp.N.cpp (G.N):
// G.29:25711+ 826A0920 consumes a type-4 event for the current H. The indexed
// B+7092+4*i cell uses constants dead=820B0C98, hurt=828D28E8, recovered=828E3AC0.
// The player branch kills an already hurt Jack (G.29:26120); fatal flag 0x200
// also stores the dead sentinel at 826A0FC8. These are states, not linear HP.
// G.20:30164+ 826CA0E8 accumulates V+12068 while dead. At 826CB338, >dead
// timer selects *82CC880C via 826FA3B0(2,block), or 826F9E70 on native track 2.
// G.63:25742+ 826B95A0 enters V+2712=701; entry creates the black screen effect
// via 825873A8 (826B97BC/97E8/97FC), retaining its native timing/variant gates.
// 826B9CBC and 826BA2B4 call 82413298 to copy death statistics into the snapshot,
// then set 82CCAA80=1 and 82CCBFD4=30. G.1:33405+ 82857F28 calls 8272CA20
// (our frame hook) BEFORE consuming AA80 and reinitializing via 8271D698.
// Ordinary hurt recovery is only a timed cell update (G.20:30944..30998), with
// no inverse for death's state/control/weapon/camera side effects. Use a native
// private uni_go load, then 82538B70; never call recovery out of track context.
constexpr double kDelay = 3, kPeerAge = 1.5, kDeathTimeout = 15, kLoadTimeout = 120;
// The connection epoch is checked by CoopSession. Load generation, life and
// sequence are checked again here before affecting game policy or a notice.
class PeerLife {
 public:
  bool Accept(const Message& m, const FlowEvent& world, double now) {
    const auto& p = m.life;
    const bool down = m.type == MessageType::PlayerDowned;
    if (!IsLifeMessage(m.type) || !ValidPlayerLife(p) || !std::isfinite(now) ||
        p.world.generation != world.generation || p.world.level != world.level || p.world.start != world.start ||
        p.incarnation < life.incarnation || (p.incarnation == life.incarnation &&
          (p.number <= life.number || (downed && !down)))) return false;
    if ((life.incarnation && down != downed) || (!life.incarnation && down)) ++notice;
    life = p; downed = down; received = now; return true;
  }
  bool Fresh(double now) const {
    return life.incarnation && std::isfinite(now) && now >= received && now-received <= kPeerAge;
  }
  PlayerLife life;
  bool downed = false;
  double received = -1;
  uint64_t notice = 0;
};
enum class Phase { Alive, Down, Loading, Placing, ReloadBoth };
enum class Action { None, Wait, LoadLocal, Place, ReloadBoth };
class Recovery {
 public:
  Action Step(bool dead, bool peer_alive_here, bool peer_dead, bool native_reset, double now) {
    if (!std::isfinite(now)) return Action::None;
    if (phase == Phase::Alive) {
      if (!dead) return Action::None;
      phase = Phase::Down; since_ = now;
    }
    // A peer death always wins, including while a private load is in flight.
    if (peer_dead || !peer_alive_here || now < since_) phase = Phase::ReloadBoth;
    if (phase == Phase::ReloadBoth) return Action::ReloadBoth;
    reset_ = reset_ || native_reset;
    if (phase == Phase::Down) {
      if (now-since_ > kDeathTimeout) { phase = Phase::ReloadBoth; return Action::ReloadBoth; }
      if (reset_ && now-since_ >= kDelay) { phase = Phase::Loading; loaded_at_ = now; return Action::LoadLocal; }
    } else if (now-loaded_at_ > kLoadTimeout) {
      phase = Phase::ReloadBoth; return Action::ReloadBoth;
    }
    return phase == Phase::Placing ? Action::Place : Action::Wait;
  }
  void Loaded() { if (phase == Phase::Loading) phase = Phase::Placing; }
  void Placed() { if (phase == Phase::Placing) *this = {}; }
  bool Downed() const { return phase != Phase::Alive; }
  Phase phase = Phase::Alive;
 private:
  double since_ = 0, loaded_at_ = 0;
  bool reset_ = false;
};
inline ShotVector Beside(const PlayerPose& peer) {
  return {peer.x-1.5f*std::cos(peer.facing), peer.y-1.5f*std::sin(peer.facing), peer.z};
}
} // namespace kk::coop::revive
