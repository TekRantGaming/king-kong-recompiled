// Host-authoritative creature replication. Guest memory is accessed on the game thread only.
#include "coop_enemies_game.h"
#include "coop_enemies.h"
#include "coop_runtime.h"
#include "coop_pose.h"
#include "hfr.h"
#include "settings.h"
#include "play_internal.h"
#include "photo_mode.h"
#include "dev_puppet.h"
#include <rex/logging.h>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <utility>

REX_EXTERN(__imp__sub_8271D498);
REX_EXTERN(__imp__sub_8271CCB8);
REX_EXTERN(__imp__sub_82715AC8);
REX_EXTERN(__imp__sub_827117E0);
REX_EXTERN(__imp__sub_82711278);
REX_EXTERN(__imp__sub_82401AA8);
REX_EXTERN(__imp__sub_82850100);
REX_EXTERN(__imp__sub_826FA3B0);
#if defined(KK_DEV_TOOLS)
namespace kk {
void DevSampleTick(PPCContext*, uint8_t*, uint32_t);
}
#endif

namespace kk::coop {
namespace {
using namespace hfr;
using namespace enemies;
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint32_t Ptr(const uint8_t* b, uint32_t p, uint32_t offset, uint32_t size) {
  if (!PoseAddress(p, offset+4)) return 0;
  const auto q = Load32(b, p+offset);
  return PoseAddress(q, size) ? q : 0;
}
bool Table(const uint8_t* b, uint32_t w, uint32_t& begin, uint32_t& end, bool all_objects = false) {
  if (!PoseAddress(w, 308)) return false;
  begin = Load32(b, w+(all_objects ? 40 : 304)); end = Load32(b, w+(all_objects ? 28 : 292));
  return PoseAddress(begin) && PoseAddress(end) && end >= begin && (end-begin)%8 == 0 && end-begin <= 8*20000;
}
bool Member(const uint8_t* b, uint32_t w, uint32_t actor) {
  uint32_t begin, end; if (!Table(b, w, begin, end)) return false;
  unsigned found = 0;
  for (auto p = begin; p < end; p += 8) found += Load32(b, p) == actor;
  return found == 1;
}
EnemyFamily Tracks(const uint8_t* b, uint32_t ai, uint32_t& state, bool& spawner, bool* initializing = nullptr) {
  if (!PoseAddress(ai, 88)) return EnemyFamily::None;
  const auto first = Load16(b, ai+80), last = Load16(b, ai+82);
  if (first > last || last > 4096 || last-first >= 64 || !PoseAddress(ai, (last+12)*4)) return EnemyFamily::None;
  EnemyFamily family = EnemyFamily::None;
  for (uint32_t t = first; t <= last; ++t) {
    const auto block = Ptr(b, ai, (t+6)*4, 24);
    if (!block) continue;
    const auto fn = Load32(b, block+20);
    if (initializing && (fn == 0x825B9020 || fn == 0x82516820 || fn == 0x826055F0)) *initializing = true;
    spawner |= fn == 0x826EB8C0 || fn == 0x826E1EE8 || fn == 0x826F8BC8;
    const auto f = Family(fn);
    if (f != EnemyFamily::None) {
      if (family != EnemyFamily::None && family != f) return EnemyFamily::None;
      family = f;
    }
    if (t == first+2) state = fn;
  }
  return family;
}
bool Read(const uint8_t* b, uint32_t actor, Binding& bind, EnemyState& e) {
  if (!PoseAddress(actor, 32) || actor == Load32(b, 0x82CCBF7C) || (Load32(b, actor)&0x00400001)) return false;
  bind.actor = actor;
  bind.ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
  bool spawner = false, initializing = false;
  const auto family = Tracks(b, bind.ai, e.state, spawner, &initializing);
  const auto life = LifeOffset(family);
  if (!life || initializing) return false;
  bind.vars = Ptr(b, bind.ai, 64, life+4); bind.matrix = Ptr(b, actor, 12, 64);
  if (!bind.vars || !bind.matrix) return false;
  const auto hero = Load32(b, 0x82CCBF7C);
  const auto hero_ai = Ptr(b, Ptr(b, hero, 24, 8), 4, 88);
  if (bind.ai == hero_ai || bind.vars == Ptr(b, hero_ai, 64, 4) || bind.matrix == Ptr(b, hero, 12, 64)) return false;
  e.id.family = family;
  e.x = LoadF(b, bind.matrix+48); e.y = LoadF(b, bind.matrix+52); e.z = LoadF(b, bind.matrix+56);
  const float fx = -LoadF(b, bind.matrix+16), fy = -LoadF(b, bind.matrix+20);
  if (!std::isfinite(fx) || !std::isfinite(fy) || std::hypot(fx, fy) < .001f) return false;
  e.facing = std::atan2(fy, fx); e.life = LoadF(b, bind.vars+life);
  e.action = 0xFFFF; // No animation component must not force action zero on a matched rig.
  const auto anim = Ptr(b, Ptr(b, actor, 20, 4), 0, 164);
  if (anim) { e.action = Load16(b, anim+156); e.item = b[anim+160]; }
  // Supply a temporary nonzero key only to validate the value fields.
  e.id.source = 1;
  return ValidEnemy(e);
}
struct Authored { uint64_t key; uint32_t ai, matrix; };
struct Pending { EnemyId id; uint32_t ai, matrix; };
struct Spawner { uint64_t key; uint32_t ai, vars, sequence = 0; std::map<uint32_t, uint64_t> sources; };
struct State {
  uint32_t world = 0, game = 0;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> visits;
  bool capture = false, prepared = false, load_refused_logged = false;
  EnemyWorld identity;
  EnemyLink link;
  std::map<uint32_t, Authored> authored;
  std::map<uint32_t, Spawner> spawners;
  std::map<uint32_t, Binding> bindings;
  std::map<uint32_t, Pending> pending;
  std::map<EnemyId, Record> records;
  std::map<uint32_t, Record> driving;
  std::set<EnemyId> unmatched_logged;
  std::set<EnemyId> death_applied, death_refused;
  std::set<uint32_t> orphan_logged;
  uint32_t unbound = 0;
  Counters counts;
  double next_log = 0, prepared_at = 0;
  void ResetWorld() {
    world = game = 0; capture = prepared = false; identity = {};
    load_refused_logged = false;
    authored.clear(); spawners.clear(); bindings.clear(); pending.clear(); records.clear(); driving.clear(); unmatched_logged.clear(); counts = {};
    death_applied.clear(); death_refused.clear();
    orphan_logged.clear(); unbound = 0;
  }
};
State& Get() { static State s; return s; }
bool Connected() { return EnemyConnectionReady(); }
bool Environment(const uint8_t* b, uint32_t w) {
  const auto& s = Get();
  return s.world == w && PoseAddress(s.game, 28) && Load32(b, 0x82CC97C0) == s.game &&
      !Load32(b, s.game+4) && Load32(b, s.game+0x18) == s.identity.level && !play::StartLevelLoading() && !photo::Active();
}
uint64_t KeyAt(const uint8_t* b, uint32_t actor, uint32_t family) {
  const auto m = Ptr(b, actor, 12, 64);
  if (!m) return 0;
  return AuthoredKey(family, LoadF(b, m+48), LoadF(b, m+52), LoadF(b, m+56));
}
void Capture(const uint8_t* b, uint32_t w) {
  auto& s = Get();
  // The full registry includes dormant actors before they enter the scheduled AI table.
  uint32_t begin, end; if (!Table(b, w, begin, end, true)) return;
  s.world = w; s.capture = false;
  std::map<uint64_t, unsigned> counts;
  for (uint32_t p = begin; p < end; p += 8) {
    const auto actor = Load32(b, p);
    if (!PoseAddress(actor, 32) || (Load32(b, actor)&0x00400001)) continue;
    const auto ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
    bool spawner = false; uint32_t state = 0;
    const auto family = Tracks(b, ai, state, spawner);
    if (!spawner && family == EnemyFamily::None) continue;
    const auto key = KeyAt(b, actor, spawner ? 0x53504157u : uint32_t(family));
    if (!key || s.authored.contains(actor)) continue;
    ++counts[key];
    s.authored[actor] = {key, ai, Ptr(b, actor, 12, 64)};
    const auto vars = Ptr(b, ai, 64, 1780);
    if (!spawner || !vars) continue;
    Spawner sp{key, ai, vars, 0, {}};
    for (const auto off : {1592u, 1776u}) {
      const auto source = Ptr(b, vars, off, 32);
      if (source) sp.sources[source] = Mix(key, off); // authored template selector, not source pointer
    }
    s.spawners[actor] = std::move(sp);
  }
  // Co-located same-family actors or hash collisions are ambiguous. Refuse all
  // participants rather than use a heap address or a mutable table ordinal.
  std::erase_if(s.authored, [&](const auto& item) { return counts[item.second.key] != 1; });
  std::erase_if(s.spawners, [&](const auto& item) { return counts[item.second.key] != 1; });
  REXLOG_INFO("ENEMIES identity level={} start={} serial={} authored={} spawners={}",
      s.identity.level, s.identity.start, s.identity.serial, s.authored.size(), s.spawners.size());
}
void Scan(const uint8_t* b) {
  auto& s = Get();
  uint32_t begin, end; if (!Table(b, s.world, begin, end)) return;
  std::set<EnemyId> present;
  s.unbound = 0;
  auto unbound = [&](uint32_t actor, EnemyFamily family) {
    ++s.unbound;
    if (s.orphan_logged.insert(actor).second)
      REXLOG_INFO("ENEMIES unmatched local actor={:08X} family={} reason=no-unique-authored-or-spawn-identity leave-alone=true", actor, uint16_t(family));
  };
  for (auto p = begin; p < end; p += 8) {
    const auto actor = Load32(b, p);
    Binding binding; EnemyState e;
    if (!Read(b, actor, binding, e)) continue;
    auto existing = s.bindings.find(actor);
    if (existing != s.bindings.end()) {
      const auto& old = existing->second;
      if (old.ai != binding.ai || old.vars != binding.vars || old.matrix != binding.matrix || old.id.family != e.id.family) {
        unbound(actor, e.id.family); continue;
      }
      binding.id = old.id;
    } else {
      const auto spawned = s.pending.find(actor);
      if (spawned != s.pending.end()) {
        if (spawned->second.ai != binding.ai || spawned->second.matrix != binding.matrix || spawned->second.id.family != e.id.family) {
          unbound(actor, e.id.family); continue;
        }
        binding.id = spawned->second.id;
      } else {
        const auto it = s.authored.find(actor);
        if (it == s.authored.end() || it->second.ai != binding.ai || it->second.matrix != binding.matrix) {
          unbound(actor, e.id.family); continue;
        }
        binding.id = {it->second.key, 0, e.id.family};
      }
      if (s.records.contains(binding.id) || s.records.size() >= kMaxEnemies) { unbound(actor, e.id.family); continue; }
      s.bindings[actor] = binding;
      s.pending.erase(actor);
    }
    e.id = binding.id; present.insert(e.id);
    auto it = s.records.find(e.id);
    if (it != s.records.end() && it->second.kind == EnemyEventKind::Removed) continue;
    s.records[e.id] = {e, e.life <= 0 ? EnemyEventKind::Died : EnemyEventKind::Spawned};
  }
  // Leaving the scheduled AI table can mean sleep/deactivation, not destruction.
  // Only disappearance from the full registry or changed mutable identity retires an ID.
  uint32_t full_begin, full_end;
  if (!Table(b, s.world, full_begin, full_end, true)) return;
  std::set<uint32_t> registered;
  for (auto p = full_begin; p < full_end; p += 8) registered.insert(Load32(b, p));
  for (auto binding_it = s.bindings.begin(); binding_it != s.bindings.end();) {
    const auto& [actor, binding] = *binding_it;
    if (present.contains(binding.id)) { ++binding_it; continue; }
    const bool same = registered.contains(actor) &&
        Ptr(b, Ptr(b, actor, 24, 8), 4, 88) == binding.ai &&
        Ptr(b, binding.ai, 64, LifeOffset(binding.id.family)+4) == binding.vars &&
        Ptr(b, actor, 12, 64) == binding.matrix;
    if (!same) {
      if (auto it = s.records.find(binding.id); it != s.records.end()) it->second.kind = EnemyEventKind::Removed;
      binding_it = s.bindings.erase(binding_it);
    } else ++binding_it;
  }
}
bool ValidBinding(const uint8_t* b, const Binding& binding) {
  Binding now; EnemyState e;
  return Member(b, Get().world, binding.actor) && Read(b, binding.actor, now, e) &&
      now.ai == binding.ai && now.vars == binding.vars && now.matrix == binding.matrix && e.id.family == binding.id.family;
}
// Same engine transform/controller route and scratch discipline as the puppet.
struct Scratch {
  PPCContext& ctx; uint8_t* base; PPCContext saved; uint32_t start; uint8_t bytes[16384];
  static bool CanUse(const PPCContext& c) { return !(c.r1.u32&15) && c.r1.u32 >= 0x40014000 && c.r1.u32 < 0xA0000000; }
  Scratch(PPCContext& c, uint8_t* b) : ctx(c), base(b), saved(c), start(c.r1.u32-sizeof(bytes)) {
    std::memcpy(bytes, b+start, sizeof(bytes)); ctx.r1.u64 = start+12288;
    std::memset(b+start+12288, 0, 4096); Store32(b, start+12288, saved.r1.u32);
  }
  ~Scratch() { std::memcpy(base+start, bytes, sizeof(bytes)); ctx = saved; }
  uint32_t Vec(float x, float y, float z, uint32_t offset) {
    const auto p = start+12288+offset; StoreF(base, p, x); StoreF(base, p+4, y); StoreF(base, p+8, z); return p;
  }
};
bool ControllerSafe(const uint8_t* b, uint32_t actor) {
  const auto ext = Ptr(b, actor, 24, 8); if (!ext) return false;
  auto p = Load32(b, ext); if (!p) return true;
  if (!PoseAddress(p, 8)) return false;
  p = Load32(b, p+4); if (!p) return true;
  if (!PoseAddress(p, 60)) return false;
  p = Load32(b, p+56); return PoseAddress(p, 60);
}
void DeathTransition(PPCContext& ctx, uint8_t* b, const Binding& binding) {
  auto& s = Get();
  if (binding.id.family == EnemyFamily::Raptor || s.death_applied.contains(binding.id)) return;
  // Normal Worm PAF -> MORT (.74:19839) and Scorpion PAF -> MORT (.44:22945)
  // use the engine track setter. Never invoke a function address from the wire.
  const uint32_t slot = binding.id.family == EnemyFamily::Worm ? 0x82CC9318 : 0x82CC8C5C;
  const uint32_t expected = binding.id.family == EnemyFamily::Worm ? 0x82534858 : 0x82619FD8;
  const auto block = Ptr(b, slot, 0, 24);
  if (!block || Load32(b, block+20) != expected || !Scratch::CanUse(ctx)) {
    if (s.death_refused.insert(binding.id).second)
      REXLOG_INFO("ENEMIES death transition refused family={} reason=unverified-loaded-death-block life-zero-only=true", uint16_t(binding.id.family));
    return;
  }
  Scratch scratch(ctx, b);
  // 826FA3B0 targets the saved current AI/object (BF68/BF70), then saves BF64/BF6C.
  // Supply this exact local binding and restore the caller's dispatcher context.
  constexpr uint32_t globals[] = {0x82CCBF64, 0x82CCBF68, 0x82CCBF6C, 0x82CCBF70};
  uint32_t saved[4];
  for (unsigned i = 0; i < 4; ++i) saved[i] = Load32(b, globals[i]);
  Store32(b, globals[0], binding.ai); Store32(b, globals[1], binding.ai);
  Store32(b, globals[2], binding.actor); Store32(b, globals[3], binding.actor);
  ctx.r3.u64 = 2; ctx.r4.u64 = block;
  __imp__sub_826FA3B0(ctx, b);
  for (unsigned i = 0; i < 4; ++i) Store32(b, globals[i], saved[i]);
  s.death_applied.insert(binding.id);
}
bool Drive(PPCContext& ctx, uint8_t* b, const Binding& binding, const EnemyState& e) {
  if (!ValidBinding(b, binding) || !ControllerSafe(b, binding.actor) || !Scratch::CanUse(ctx)) return false;
  Scratch scratch(ctx, b);
  const auto xyz = scratch.Vec(e.x, e.y, e.z, 0x100);
  const auto forward = scratch.Vec(std::cos(e.facing), std::sin(e.facing), 0, 0x110);
  auto call = [&](auto fn, uint32_t p) { ctx.r3.u64 = binding.actor; ctx.r4.u64 = p; fn(ctx, b); };
  call(__imp__sub_827117E0, xyz);
  if (!ValidBinding(b, binding)) return false;
  call(__imp__sub_82711278, forward);
  if (!ValidBinding(b, binding)) return false;
  call(__imp__sub_82401AA8, xyz);
  if (!ValidBinding(b, binding)) return false;
  const auto anim = Ptr(b, Ptr(b, binding.actor, 20, 4), 0, 164);
  const auto table = Ptr(b, anim, 88, 16);
  if (table && e.action < 0x8000 && e.action < Load16(b, table) &&
      (e.action == 0 || Ptr(b, table, 4*(uint32_t(e.action)+4), 4)) &&
      (Load16(b, anim+156) != e.action || b[anim+160] != e.item)) {
    ctx.r3.u64 = binding.actor; ctx.r4.u64 = e.action; ctx.r5.u64 = e.item; ctx.r6.u64 = 0;
    __imp__sub_82850100(ctx, b);
  }
  return true;
}
void Guest(PPCContext& ctx, uint8_t* b, bool drive) {
  auto& s = Get(); s.driving.clear(); s.counts = {};
  s.counts.unmatched = s.unbound;
  if (s.link.remote.world != s.identity) { s.counts.unmatched += uint32_t(s.bindings.size()); return; }
  std::vector<Binding> bindings;
  for (const auto& [actor, binding] : s.bindings) bindings.push_back(binding);
  std::set<uint32_t> matched;
  for (const auto& r : s.link.remote.records) {
    const auto* binding = Match(r.enemy.id, bindings);
    if (!binding || !ValidBinding(b, *binding)) {
      if (r.kind != EnemyEventKind::Spawned || r.enemy.life <= 0) continue; // already locally gone
      ++s.counts.unmatched;
      if (s.unmatched_logged.insert(r.enemy.id).second)
        REXLOG_INFO("ENEMIES unmatched host source={:016X} sequence={} family={} leave-alone=true",
            r.enemy.id.source, r.enemy.id.sequence, uint16_t(r.enemy.id.family));
      continue;
    }
    matched.insert(binding->actor); ++s.counts.matched;
    const bool dead = r.kind != EnemyEventKind::Spawned || r.enemy.life <= 0;
    if (!dead && (!ControllerSafe(b, binding->actor) || !Scratch::CanUse(ctx))) continue;
    // Host life is authority. Dead creatures run their normal local death dispatches.
    StoreF(b, binding->vars+LifeOffset(binding->id.family), dead ? 0 : r.enemy.life);
    if (dead) { DeathTransition(ctx, b, *binding); continue; }
    if (!drive || Drive(ctx, b, *binding, r.enemy)) {
      s.driving[binding->actor] = r;
      if (drive) ++s.counts.driven;
    }
  }
  for (const auto& binding : bindings) if (!matched.contains(binding.actor) && ValidBinding(b, binding)) {
    ++s.counts.unmatched;
    if (s.unmatched_logged.insert(binding.id).second)
      REXLOG_INFO("ENEMIES unmatched guest source={:016X} sequence={} family={} leave-alone=true",
          binding.id.source, binding.id.sequence, uint16_t(binding.id.family));
  }
}
void BeforeAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
  auto& s = Get(); s.prepared = false; s.driving.clear();
  if (!Connected()) return;
  s.link = EnemyLinkSnapshot();
  if (uint32_t(ctx.lr) != 0x82881CF0) return;
  if (!s.game && !s.load_refused_logged) {
    s.load_refused_logged = true;
    REXLOG_INFO("ENEMIES waiting reason=no-connected-load-identity restart-both-and-load-while-connected=true");
  }
  if (s.capture) Capture(b, w);
  if (!Environment(b, w)) return;
  Scan(b); s.prepared = true; s.prepared_at = Now();
  if (s.link.role == Role::Guest) Guest(ctx, b, false);
}
void AfterAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
  auto& s = Get();
  if (!s.prepared || !Connected() || !Environment(b, w)) { s.driving.clear(); return; }
  if (s.link.role == Role::Guest) Guest(ctx, b, true);
  else {
    Scan(b);
    s.counts = {}; s.counts.unmatched = s.unbound;
    Frame frame{s.identity, Now(), {}};
    for (const auto& [id, r] : s.records) frame.records.push_back(r);
    PublishEnemyFrame(frame);
  }
  s.counts.sent = s.link.sent; SetEnemyCounters(s.counts);
#if defined(KK_DEV_TOOLS)
  if (Now() >= s.next_log) {
    s.next_log = Now()+5;
    REXLOG_INFO("ENEMIES status role={} sent={} matched={} unmatched={} driven={} tracked={}",
        s.link.role == Role::Host ? "host" : "guest", s.link.sent, s.counts.matched,
        s.counts.unmatched, s.counts.driven, s.records.size());
  }
#endif
}
bool Hold(PPCContext& ctx, uint8_t* b) {
  auto& s = Get();
  if (!s.prepared || s.link.role != Role::Guest || s.driving.empty() || uint32_t(ctx.lr) != 0x8271D304 ||
      Now()-s.prepared_at > .5 || !Connected()) return false;
  const auto actor = Load32(b, 0x82CCBF6C), ai = Load32(b, 0x82CCBF64);
  const auto it = s.driving.find(actor); if (it == s.driving.end()) return false;
  const auto binding = s.bindings.find(actor);
  if (binding == s.bindings.end() || binding->second.ai != ai || !Connected() ||
      !Environment(b, s.world) || !ValidBinding(b, binding->second)) return false;
  StoreF(b, binding->second.vars+LifeOffset(binding->second.id.family), it->second.enemy.life);
  ctx.r3.u64 = 0; return true;
}
void Spawned(const uint8_t* b, uint32_t spawner, uint32_t source, uint32_t actor) {
  auto& s = Get();
  if (!s.prepared || !Connected() || !PoseAddress(actor, 32)) return;
  auto it = s.spawners.find(spawner); if (it == s.spawners.end()) return;
  auto& sp = it->second;
  if (Ptr(b, Ptr(b, spawner, 24, 8), 4, 88) != sp.ai || Ptr(b, sp.ai, 64, 1780) != sp.vars) return;
  const auto template_it = sp.sources.find(source);
  if (template_it == sp.sources.end() || sp.sequence == UINT32_MAX) return;
  ++sp.sequence;
  bool is_spawner = false; uint32_t state = 0;
  const auto family = Tracks(b, Ptr(b, Ptr(b, source, 24, 8), 4, 88), state, is_spawner);
  const auto id = SpawnId(sp.key, template_it->second, sp.sequence, family);
  const auto ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88), matrix = Ptr(b, actor, 12, 64);
  if (!id.source || !ai || !matrix || s.records.size()+s.pending.size() >= kMaxEnemies || s.records.contains(id)) return;
  // A pooled pointer is a new generation. Preserve a tombstone for its previous ID.
  if (auto old = s.bindings.find(actor); old != s.bindings.end()) {
    auto record = s.records.find(old->second.id);
    if (record != s.records.end()) record->second.kind = EnemyEventKind::Removed;
    s.bindings.erase(old);
  }
  // The new copy may join the scheduled AI table on a later pass. Bind only
  // once that exact AI/matrix is observed there, after its normal init.
  s.pending[actor] = {id, ai, matrix};
}
} // namespace
void EnemyBeforeLoad() {
  if (!REXCVAR_GET(kk_coop_enabled)) return;
  Get().ResetWorld(); PublishEnemyFrame({}); SetEnemyCounters({});
}
void EnemyAfterLoad(const uint8_t* b, uint32_t game) {
  // Record every load while co-op is on, not only while Connected: the host usually loads before the guest
  // joins (Claude, run c18 17:53: "no-connected-load-identity"). Matching still waits for Connected.
  if (!REXCVAR_GET(kk_coop_enabled) || !PoseAddress(game, 28)) return;
  auto& s = Get();
  const auto level = Load32(b, game+8), start = Load32(b, game+12);
  if (level < 100 || level > 100000 || start > 10000) return;
  s.game = game; s.identity = {level, start, ++s.visits[{level, start}]};
  s.capture = ValidEnemyWorld(s.identity);
}
void EnemyGameFrame() {
  if (!Connected()) {
    // Release immediately on loss, but retain this load's identity so recovery
    // can revalidate it. The synchronous load boundary owns pointer teardown.
    auto& s = Get(); s.prepared = false; s.driving.clear(); SetEnemyCounters({});
  }
}
} // namespace kk::coop

// One owner for each shared hook in both dev and production builds.
REX_HOOK_RAW(sub_8271D498) {
  const uint32_t world = ctx.r3.u32;
#if defined(KK_DEV_TOOLS)
  kk::DevSampleTick(&ctx, base, world);
  kk::DevPuppetBeforeAi(ctx, base, world);
#endif
  kk::coop::BeforeAi(ctx, base, world);
  __imp__sub_8271D498(ctx, base);
#if defined(KK_DEV_TOOLS)
  kk::DevPuppetAfterAi(ctx, base, world);
#endif
  kk::coop::AfterAi(ctx, base, world);
}
REX_HOOK_RAW(sub_8271CCB8) {
  if (kk::coop::Hold(ctx, base)) return;
#if defined(KK_DEV_TOOLS)
  if (kk::PuppetHold(ctx, base)) return;
#endif
  __imp__sub_8271CCB8(ctx, base);
}
REX_HOOK_RAW(sub_82715AC8) {
  const bool observe = uint32_t(ctx.lr) == 0x826F9240 && kk::coop::Connected();
  const auto source = ctx.r3.u32;
  const auto spawner = observe ? kk::hfr::Load32(base, 0x82CCBF6C) : 0;
  __imp__sub_82715AC8(ctx, base);
  if (observe && ctx.r3.u32) kk::coop::Spawned(base, spawner, source, ctx.r3.u32);
}
