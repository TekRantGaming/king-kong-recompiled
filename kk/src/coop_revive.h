#pragma once
#include "coop_protocol.h"
#include "coop_mut.h"
#include <cmath>
#include <cstdint>
#include <optional>

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
// C104 placement. The old spot (1.5 behind at partner height) was never
// checked: a checkpoint start usually faces into the level with its back to
// the edge, so "behind" was often over nothing. Native rule, decomp
// H_exec_read_world 826A1178: a player more than 0.2 off the ground whose ray
// from 0.5 up and 8.5 down meets nothing sets V+0x3C50 (void) and dies once
// the fall passes 3 (runs c89b-bat-strike302 05:20:49, c89b-centipede 08:45:07).
// A spot is used only when the native ground ray finds a flat floor within a
// step of the partner's floor, the head is clear, nothing stands between the
// spot and the partner, and the native nav cell may be stood on.
constexpr float kRadii[] = {1.5f, 1.0f, 2.5f};
// C111: the sides first, then behind. A partner fighting faces the enemies, so
// a spot 1.5 straight behind put the partner's body between them and the
// revived player: melee enemies (crabs, 101/4) pushed into that body 1.3 to
// 2.7 from the player, out of their 2.25 strike range, and nobody was hit for
// about 90 s (runs c111-beach-b/c). Side spots keep both players reachable.
constexpr float kTurns[] = {1.57079633f, -1.57079633f, 2.35619449f, -2.35619449f, 3.14159265f,
                            .78539816f, -.78539816f, 0.f};
// The pre C111 order (behind first), kept for the placement_side twin.
constexpr float kBehindFirstTurns[] = {3.14159265f, 1.57079633f, -1.57079633f, 2.35619449f, -2.35619449f,
                                       .78539816f, -.78539816f, 0.f};
constexpr unsigned kCandidates = 24;
constexpr float kMaxStep = 1.f, kMinNormalZ = .7f;
constexpr double kPlaceWait = 2, kGuardSeconds = 3;
inline float CandidateTurn(unsigned i) {
  return COOP_MUT_IF("placement_side") ? kBehindFirstTurns[i%8] : kTurns[i%8];
}
inline ShotVector Candidate(const PlayerPose& peer, float floor_z, unsigned i) {
  const float r = kRadii[(i/8)%3], a = peer.facing+CandidateTurn(i);
  return {peer.x+r*std::cos(a), peer.y+r*std::sin(a), floor_z};
}
// Native nav cell byte (nav_cell_at 8284D638, sign extended). nav_grid.c:
// bits 6 and 7 block AI routes, kind 15 costs 0 (not walkable). fn_826D5348:
// cell 1 or 12 is blocked. fn_826D8020: kinds 3, 10 and 12 around the player
// refuse a stand. A player can stand where AI may not route (run
// c104-fix302: the host stood on cell 0x40), so every rule is relative to the
// partner's own cell: only marks and kinds the partner does not stand on are
// refused. Cell 0 is also "off the cells": taken only when the partner's is 0.
inline bool NavStandable(int8_t cell, int8_t partner) {
  const auto c = uint8_t(cell), p = uint8_t(partner), kind = uint8_t(c&15);
  if (((c&0xC0) & ~(p&0xC0)) || kind == 15) return false;
  if ((c == 1 || c == 12) && c != p) return false;
  if (kind == 3 || kind == 10 || kind == 12) return kind == (p&15);
  if (c == 0) return p == 0;
  return true;
}
struct Probe {
  bool floor = false;     // native ground ray met a face (not Jack, not the partner body)
  float floor_z = 0, normal_z = 0;
  bool headroom = false;  // nothing above the floor up to head height
  bool sight = false;     // nothing between the spot and the partner
  int8_t nav = 0;
};
enum class Verdict : uint8_t { Ok, NoFloor, Steep, Step, Head, Sight, Nav };
inline Verdict Judge(const Probe& p, float partner_floor, int8_t partner_nav) {
  if (!COOP_MUT_IF("placement_floor") && (!p.floor || !std::isfinite(p.floor_z))) return Verdict::NoFloor;
  if (!(p.normal_z >= kMinNormalZ) && p.floor) return Verdict::Steep;
  if (!(std::abs(p.floor_z-partner_floor) <= kMaxStep) && p.floor) return Verdict::Step;
  if (!p.headroom) return Verdict::Head;
  if (!COOP_MUT_IF("placement_sight") && !p.sight) return Verdict::Sight;
  if (!NavStandable(p.nav, partner_nav)) return Verdict::Nav;
  return Verdict::Ok;
}
inline const char* VerdictName(Verdict v) {
  switch (v) {
    case Verdict::Ok: return "ok"; case Verdict::NoFloor: return "no-floor"; case Verdict::Steep: return "steep";
    case Verdict::Step: return "step"; case Verdict::Head: return "head"; case Verdict::Sight: return "sight";
    case Verdict::Nav: return "nav";
  }
  return "?";
}
// Repeated placement attempts (one per frame). No valid spot for kPlaceWait
// seconds: keep Jack on the native respawn spot the private load gave him.
enum class Place : uint8_t { Wait, Spot, Stay };
class Placement {
 public:
  Place Step(bool found, double now) {
    if (!std::isfinite(now)) return Place::Wait;
    if (first_ < 0 || now < last_ || now-last_ > 1) first_ = now;
    last_ = now;
    if (found) { *this = {}; return Place::Spot; }
    if (now-first_ >= kPlaceWait) { *this = {}; return Place::Stay; }
    return Place::Wait;
  }
 private:
  double first_ = -1, last_ = -1;
};
// Safety net after any co-op placement: the native void flag means the fall
// will kill. Within kGuardSeconds, put Jack back once on the grounded spot he
// stood on before the move.
class VoidGuard {
 public:
  void Arm(const ShotVector& rescue, bool grounded, double now) {
    armed_ = grounded && std::isfinite(now) && std::isfinite(rescue.x) && std::isfinite(rescue.y) &&
        std::isfinite(rescue.z);
    rescue_ = rescue; since_ = now;
  }
  std::optional<ShotVector> Check(bool void_flag, double now) {
    if (!armed_ || !std::isfinite(now) || now < since_ || now-since_ > kGuardSeconds) { armed_ = false; return {}; }
    if (COOP_MUT_IF("void_guard") || !void_flag) return {};
    armed_ = false; return rescue_;
  }
  bool Armed() const { return armed_; }
 private:
  bool armed_ = false;
  ShotVector rescue_{};
  double since_ = 0;
};
} // namespace kk::coop::revive
