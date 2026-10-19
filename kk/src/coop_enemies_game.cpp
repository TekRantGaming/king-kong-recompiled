// Host-authoritative creature replication. Guest memory is accessed on the game thread only.
#include "coop_enemies_game.h"
#include "coop_enemies.h"
#include "coop_runtime.h"
#include "coop_pose.h"
#include "hfr.h"
#include "settings.h"
#include "play_internal.h"
#include "photo_mode.h"
#include "coop_partner_body.h"
#include <algorithm>
#include <rex/logging.h>
#include <chrono>
#include <cstdlib>
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
REX_EXTERN(__imp__sub_82696A28);
REX_EXTERN(__imp__sub_8243A3C0);
REX_EXTERN(__imp__sub_82634428);
REX_EXTERN(__imp__sub_82419C38);
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_8250EE50);
REX_EXTERN(__imp__sub_8250EDE8);
namespace kk {
void DevSampleTick(PPCContext*, uint8_t*, uint32_t);
double DevSecondsSinceMenu();
bool DevTeleportJack(PPCContext&, uint8_t*, uint32_t, coop::ShotVector, float, float);
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
bool Member(const uint8_t* b, uint32_t w, uint32_t actor, bool all = false) {
  uint32_t begin, end; if (!Table(b, w, begin, end, all)) return false;
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
#if defined(KK_DEV_TOOLS)
struct DevEnemy {
  uint32_t actor = 0, state = 0, ai = 0, matrix = 0;
  EnemyFamily family = EnemyFamily::None;
  ShotVector position;
  float distance = 0, life = -1;
  bool scheduled = false, initializing = false;
};
const char* DevFamilyName(EnemyFamily family) {
  switch (family) {
    case EnemyFamily::Raptor: return "raptor";
    case EnemyFamily::Worm: return "worm";
    case EnemyFamily::Scorpion: return "scorpion";
    default: return "none";
  }
}
bool DevWorld(const uint8_t* b, uint32_t w) {
  const auto game = Load32(b, 0x82CC97C0);
  return w == Load32(b, 0x828E8388) && PoseAddress(game, 13652) &&
      !Load32(b, game+4) && !play::StartLevelLoading() && !photo::Active();
}
// Read the full registry independently of replication identity/connection. Init
// actors have a position but their life is not yet meaningful. "Dormant" here
// means absent from the scheduled AI table, not proof of a specific trigger.
std::vector<DevEnemy> DevEnemies(const uint8_t* b, uint32_t w, ShotVector origin) {
  std::vector<DevEnemy> out;
  uint32_t begin, end, active_begin, active_end;
  if (!DevWorld(b, w) || !Table(b, w, begin, end, true) || !Table(b, w, active_begin, active_end)) return out;
  std::set<uint32_t> active;
  for (auto p = active_begin; p < active_end; p += 8) active.insert(Load32(b, p));
  for (auto p = begin; p < end; p += 8) {
    DevEnemy e; e.actor = Load32(b, p);
    if (!PoseAddress(e.actor, 32) || e.actor == Load32(b, 0x82CCBF7C) || (Load32(b, e.actor)&0x00400001)) continue;
    const auto ai = Ptr(b, Ptr(b, e.actor, 24, 8), 4, 88);
    bool spawner = false;
    e.family = Tracks(b, ai, e.state, spawner, &e.initializing);
    const auto matrix = Ptr(b, e.actor, 12, 64);
    if (spawner || e.family == EnemyFamily::None || !matrix) continue;
    e.ai = ai; e.matrix = matrix;
    e.position = {LoadF(b, matrix+48), LoadF(b, matrix+52), LoadF(b, matrix+56)};
    if (!AuthoredKey(uint32_t(e.family), e.position.x, e.position.y, e.position.z)) continue;
    if (!e.initializing) {
      Binding binding; EnemyState observed;
      if (!Read(b, e.actor, binding, observed)) continue;
      e.life = observed.life;
    }
    e.scheduled = active.contains(e.actor);
    e.distance = ShotDistance(origin, e.position);
    out.push_back(e);
  }
  return out;
}
// C22: opt-in, one native volume visit per process, on the solo/host only.
// C20's level-203 Gao at 01BD96A3 has an identity basis, XYZ (-48,146,12)
// at +78 and local bounds (-9.213,-1.490,-1.337)..(.346,4.247,1.503)
// at +98. Visit the bounds' midpoint rather than the nearby wave event pivot.
// This is a source-backed candidate for Scorpion_00 at start 5, not proof of
// the script's prerequisites or activation. The logs must establish those.
// Other starts have no recovered route here and are explicitly refused.
struct DevWake {
  enum Phase { Waiting, Visiting, Observing, Done } phase = Waiting;
  bool enabled = false;
  double due = 0, next = 0, finish = 0;
  uint32_t game = 0, world = 0, hero = 0;
  DevEnemy target;
  ShotVector home;
  float fx = 0, fy = 1;
  unsigned activity = 0;
  DevWake() {
    const char* value = std::getenv("KK_DEV_WAKE");
    if (!value || !*value) return;
    char* end = nullptr;
    due = std::strtod(value, &end);
    enabled = end != value && !*end && std::isfinite(due) && due >= 0;
    if (!enabled) REXLOG_WARN("KK dev wake: invalid KK_DEV_WAKE, expected nonnegative finite seconds");
  }
};
DevWake& Wake() { static DevWake wake; return wake; }
void DevWakeBeforeLoad() {
  auto& wake = Wake();
  if (wake.phase == DevWake::Visiting || wake.phase == DevWake::Observing) {
    wake.phase = DevWake::Done;
    REXLOG_WARN("KK dev wake: load interrupted visit/observation; discarded saved placement");
  }
}
void DevWakeActivity(uint32_t actor, uint32_t off, const char* name) {
  auto& wake = Wake();
  if (!wake.enabled || (wake.phase != DevWake::Visiting && wake.phase != DevWake::Observing) ||
      wake.activity >= 32) return;
  ++wake.activity;
  REXLOG_INFO("KK dev wake: native {} actor={:08X} off={} selected-target={}",
      name, actor, off, actor == wake.target.actor);
}
void DevWakeTick(PPCContext& ctx, uint8_t* b, uint32_t w) {
  auto& wake = Wake();
  if (!wake.enabled || wake.phase == DevWake::Done) return;
  const double t = kk::DevSecondsSinceMenu();
  if (t < wake.due || !DevWorld(b, w)) return;
  const auto game = Load32(b, 0x82CC97C0);
  const auto jack = ReadJackPose([&](uint32_t p) { return Load32(b, p); },
      [&](uint32_t p) { return Load16(b, p); }, [&](uint32_t p) { return LoadF(b, p); }, t);
  if (!jack.pose) return;
  const auto& p = *jack.pose;
  const ShotVector origin{p.x, p.y, p.z};
  const auto hero = Load32(b, 0x82CCBF7C);
  if (wake.phase == DevWake::Waiting) {
    if (REXCVAR_GET(kk_coop_enabled)) {
      const auto link = EnemyLinkSnapshot();
      if (!link.connected) {
        if (t > wake.due+10) {
          wake.phase = DevWake::Done;
          REXLOG_WARN("KK dev wake: expired waiting for connected host authority");
        }
        return;
      }
      if (link.role != Role::Host) {
        wake.phase = DevWake::Done;
        REXLOG_WARN("KK dev wake: refused on guest, host authority only");
        return;
      }
    }
    const auto all = DevEnemies(b, w, origin);
    const DevEnemy* nearest = nullptr;
    for (const auto& e : all)
      if (!e.scheduled && (e.initializing || e.life > 0) && e.distance <= 200 &&
          (!nearest || e.distance < nearest->distance)) nearest = &e;
    if (!nearest) {
      wake.phase = DevWake::Done;
      REXLOG_WARN("KK dev wake: no dormant enemy within 200 units");
      return;
    }
    if (p.level != 203 || Load32(b, game+12) != 5 || nearest->family != EnemyFamily::Scorpion ||
        ShotDistance(nearest->position, {51.110620f, 132.907700f, 19.930140f}) > .5f) {
      wake.phase = DevWake::Done;
      REXLOG_WARN("KK dev wake: no recovered trigger route for nearest dormant actor={:08X} level={} start={} pos=({:.3f},{:.3f},{:.3f}); supported=203/5-Scorpion_00",
          nearest->actor, p.level, Load32(b, game+12), nearest->position.x, nearest->position.y, nearest->position.z);
      return;
    }
    const auto m = Ptr(b, hero, 12, 64);
    if (!m) return;
    wake.fx = -LoadF(b, m+16); wake.fy = -LoadF(b, m+20);
    wake.home = origin; wake.target = *nearest;
    wake.game = game; wake.world = w; wake.hero = hero;
    const ShotVector interior{-52.4335f, 147.3785f, 12.083f};
    if (!kk::DevTeleportJack(ctx, b, hero, interior, wake.fx, wake.fy)) return;
    wake.phase = DevWake::Visiting; wake.next = t+2;
    REXLOG_INFO("KK dev wake: at {:.2f} s visit 01_EVT_Scorpions_Activator_BV interior=({:.3f},{:.3f},{:.3f}) via 82538B70 for 2 s; target={:08X} init={} life={:.2f} home=({:.3f},{:.3f},{:.3f})",
        t, interior.x, interior.y, interior.z, nearest->actor, nearest->initializing, nearest->life, origin.x, origin.y, origin.z);
    return;
  }
  if (game != wake.game || w != wake.world || hero != wake.hero || p.level != 203 || Load32(b, game+12) != 5) {
    wake.phase = DevWake::Done;
    REXLOG_WARN("KK dev wake: world/player changed; discarded saved placement");
    return;
  }
  if (t < wake.next) return;
  if (wake.phase == DevWake::Visiting) {
    if (!kk::DevTeleportJack(ctx, b, hero, wake.home, wake.fx, wake.fy)) return;
    wake.phase = DevWake::Observing; wake.finish = t+10;
    REXLOG_INFO("KK dev wake: at {:.2f} s returned Jack; activation remains native, observing target for 10 s", t);
  }
  wake.next = t+1;
  const auto all = DevEnemies(b, w, origin);
  const auto found = std::find_if(all.begin(), all.end(), [&](const auto& e) {
    return e.actor == wake.target.actor && e.ai == wake.target.ai && e.matrix == wake.target.matrix;
  });
  if (found == all.end()) {
    REXLOG_WARN("KK dev wake: target disappeared or changed components; result=unknown");
    wake.phase = DevWake::Done;
    return;
  }
  const auto& e = *found;
  const bool awake = e.scheduled && !e.initializing && e.life > 0;
  REXLOG_INFO("KK dev wake: after actor={:08X} status={} init={} life={:.2f} state={:08X} awake={} native-activity-calls={}",
      e.actor, e.scheduled ? "live" : "dormant", e.initializing, e.life, e.state, awake, wake.activity);
  if (t >= wake.finish) {
    wake.phase = DevWake::Done;
    if (!awake) REXLOG_WARN("KK dev wake: visit did not establish a living scheduled target; inspect KK_DEV_TRIG_TRACE");
  }
}
void DevEnemyNear(const uint8_t* b, uint32_t w) {
  static const bool enabled = [] {
    const char* value = std::getenv("KK_DEV_ENEMY_NEAR");
    return value && std::strcmp(value, "1") == 0;
  }();
  if (!enabled) return;
  static double next = 0;
  const auto now = Now();
  if (now < next) return;
  next = now+1;
  const auto jack = ReadJackPose([&](uint32_t p) { return Load32(b, p); },
      [&](uint32_t p) { return Load16(b, p); }, [&](uint32_t p) { return LoadF(b, p); }, now);
  if (!jack.pose || !DevWorld(b, w)) {
    REXLOG_INFO("KK dev enemy near: waiting reason={}", jack.pose ? "world-loading-or-photo" : jack.failure);
    return;
  }
  const auto& pose = *jack.pose;
  const auto all = DevEnemies(b, w, {pose.x, pose.y, pose.z});
  const DevEnemy* nearest = nullptr;
  size_t scheduled = 0, dormant = 0;
  for (const auto& e : all) {
    e.scheduled ? ++scheduled : ++dormant;
    if (!nearest || e.distance < nearest->distance) nearest = &e;
  }
  const auto game = Load32(b, 0x82CC97C0);
  const auto weapon = Load32(b, game+13128);
  constexpr uint32_t ammo_offsets[] = {13432, 13504, 13576, 13648};
  const auto ammo = weapon >= 1 && weapon <= 4 ? Load32(b, game+ammo_offsets[weapon-1]) : 0;
  if (!nearest) {
    REXLOG_INFO("KK dev enemy near: level={} start={} nearest=none scheduled=0 dormant=0 weapon={} ammo={} jack=({:.3f},{:.3f},{:.3f})",
        pose.level, Load32(b, game+12), weapon, ammo, pose.x, pose.y, pose.z);
    return;
  }
  const auto& e = *nearest;
  REXLOG_INFO("KK dev enemy near: level={} start={} family={} distance={:.3f} status={} init={} life={:.2f} actor={:08X} state={:08X} pos=({:.3f},{:.3f},{:.3f}) jack=({:.3f},{:.3f},{:.3f}) weapon={} ammo={} scheduled={} dormant={}",
      pose.level, Load32(b, game+12), DevFamilyName(e.family), e.distance, e.scheduled ? "live" : "dormant",
      e.initializing, e.life, e.actor, e.state, e.position.x, e.position.y, e.position.z,
      pose.x, pose.y, pose.z, weapon, ammo, scheduled, dormant);
}
#endif
struct Authored {
  uint64_t key; uint32_t ai, matrix; EnemyFamily family;
  float x, y, z; bool ambiguous = false;
};
struct Pending { EnemyId id; uint32_t ai, matrix; float x, y, z; };
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
  std::map<EnemyId, EnemyId> aliases; // host ID -> immutable local ID
  std::map<EnemyId, MatchRetry> retry;
  std::set<EnemyId> retry_diagnosed;
  ShotValidator shot_validator;
  ShotCounters shots;
  uint32_t next_shot = 0;
  bool shooting = false;
  ShotIntent shot;
  double census_until = 0, next_numbers = 0, next_shot_log = 0;
  double error_sum = 0, error_max = 0;
  uint64_t error_count = 0;
  uint32_t unbound = 0;
  Counters counts;
  double next_log = 0, prepared_at = 0;
  void ResetWorld() {
    world = game = 0; capture = prepared = false; identity = {};
    load_refused_logged = false;
    authored.clear(); spawners.clear(); bindings.clear(); pending.clear(); records.clear(); driving.clear(); unmatched_logged.clear(); counts = {};
    death_applied.clear(); death_refused.clear();
    orphan_logged.clear(); unbound = 0;
    aliases.clear(); retry.clear(); retry_diagnosed.clear(); shots = {}; shot_validator = {}; shooting = false; shot = {};
    census_until = next_numbers = next_log = next_shot_log = error_sum = error_max = 0; error_count = 0;
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
void Capture(const uint8_t* b, uint32_t w, bool initial = true) {
  auto& s = Get();
  // The full registry includes dormant actors before they enter the scheduled AI table.
  uint32_t begin, end; if (!Table(b, w, begin, end, true)) return;
  s.world = w; s.capture = false;
  if (initial) s.census_until = Now()+10;
  for (uint32_t p = begin; p < end; p += 8) {
    const auto actor = Load32(b, p);
    if (!PoseAddress(actor, 32) || (Load32(b, actor)&0x00400001)) continue;
    const auto ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
    bool spawner = false, initializing = false; uint32_t state = 0;
    const auto family = Tracks(b, ai, state, spawner, &initializing);
    if (!spawner && family == EnemyFamily::None) continue;
    // Late registration is safe only before the actor's init executes. Never hash
    // a newly noticed moving actor as an authored spawn point.
    if (!initial && !initializing) continue;
    const auto key = KeyAt(b, actor, spawner ? 0x53504157u : uint32_t(family));
    if (!key || s.authored.contains(actor) || s.authored.size() >= kMaxEnemies) continue;
    const auto matrix = Ptr(b, actor, 12, 64);
    s.authored[actor] = {key, ai, matrix, family, LoadF(b, matrix+48), LoadF(b, matrix+52), LoadF(b, matrix+56)};
#if defined(KK_DEV_TOOLS)
    const auto& a = s.authored[actor];
    REXLOG_INFO("ENEMIES census id={:016X}/0/{} spawn=({:.3f},{:.3f},{:.3f}) scheduled={} init={}",
        key, uint16_t(family), a.x, a.y, a.z, Member(b, w, actor), initializing);
#endif
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
  std::map<uint64_t, unsigned> counts;
  for (const auto& [actor, a] : s.authored) ++counts[a.key];
  for (auto& [actor, a] : s.authored) a.ambiguous = counts[a.key] != 1;
  std::erase_if(s.spawners, [&](const auto& item) { return counts[item.second.key] != 1; });
  if (initial) REXLOG_INFO("ENEMIES identity level={} start={} serial={} authored={} spawners={}",
      s.identity.level, s.identity.start, s.identity.serial, s.authored.size(), s.spawners.size());
}
void Scan(const uint8_t* b) {
  auto& s = Get();
  // A guest must also bind initialized dormant actors. Driving a transform does
  // not activate/schedule them. Host authority still samples scheduled actors.
  uint32_t begin, end; if (!Table(b, s.world, begin, end, s.link.role == Role::Guest)) return;
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
        e.spawn_x = spawned->second.x; e.spawn_y = spawned->second.y; e.spawn_z = spawned->second.z;
      } else {
        const auto it = s.authored.find(actor);
        if (it == s.authored.end() || it->second.ambiguous || it->second.ai != binding.ai || it->second.matrix != binding.matrix) {
          unbound(actor, e.id.family); continue;
        }
        binding.id = {it->second.key, 0, e.id.family};
        e.spawn_x = it->second.x; e.spawn_y = it->second.y; e.spawn_z = it->second.z;
      }
      if (s.records.contains(binding.id) || s.records.size() >= kMaxEnemies) { unbound(actor, e.id.family); continue; }
      s.bindings[actor] = binding;
      s.pending.erase(actor);
    }
    e.id = binding.id; present.insert(e.id);
    auto it = s.records.find(e.id);
    if (it != s.records.end() && it->second.kind == EnemyEventKind::Removed) continue;
    if (it != s.records.end()) {
      e.spawn_x = it->second.enemy.spawn_x; e.spawn_y = it->second.enemy.spawn_y; e.spawn_z = it->second.enemy.spawn_z;
    }
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
  return Member(b, Get().world, binding.actor, Get().link.role == Role::Guest) && Read(b, binding.actor, now, e) &&
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
  const double now = Now();
  const bool numbers = drive && now >= s.next_numbers;
  if (numbers) s.next_numbers = now+1;
  std::set<uint32_t> matched;
  std::set<EnemyId> remote_ids, claimed;
  for (const auto& r : s.link.remote.records) remote_ids.insert(r.enemy.id);
  for (const auto& [host, local] : s.aliases) claimed.insert(local);
  std::map<EnemyId, unsigned> neighbour_counts;
  for (const auto& r : s.link.remote.records) {
    const Binding* binding = nullptr;
    auto alias = s.aliases.find(r.enemy.id);
    if (alias != s.aliases.end()) binding = Match(alias->second, bindings);
    else {
      std::vector<MatchCandidate> candidates;
      for (const auto& local : bindings) {
        const auto it = s.records.find(local.id); if (it == s.records.end()) continue;
        bool available = it->second.kind != EnemyEventKind::Removed && !claimed.contains(local.id);
        // Do not let a fallback take another host ID's exact local actor.
        if (local.id != r.enemy.id && remote_ids.contains(local.id)) available = false;
        const auto& e = it->second.enemy;
        // Refuse competing host spawn signatures as well as competing local ones.
        auto [neighbours, inserted] = neighbour_counts.try_emplace(local.id, 0);
        if (inserted) for (const auto& other : s.link.remote.records) if (!other.enemy.id.sequence &&
              other.enemy.id.family == local.id.family &&
              std::hypot(other.enemy.spawn_x-e.spawn_x, other.enemy.spawn_y-e.spawn_y, other.enemy.spawn_z-e.spawn_z) <= .25f) ++neighbours->second;
        if (local.id != r.enemy.id && neighbours->second != 1) available = false;
        candidates.push_back({local, e.spawn_x, e.spawn_y, e.spawn_z, available});
      }
      // Include ambiguous and not-yet-initialized census actors as unavailable
      // candidates. They must still block a guessed nearest-position association.
      for (const auto& [actor, a] : s.authored) if (!s.bindings.contains(actor) && a.family != EnemyFamily::None)
        candidates.push_back({{{a.key, 0, a.family}, actor, a.ai, 0, a.matrix}, a.x, a.y, a.z, false});
      if (const auto* found = MatchAuthored(r.enemy, candidates, s.retry[r.enemy.id].ToleranceAllowed(now))) {
        s.aliases[r.enemy.id] = found->id;
        claimed.insert(found->id);
        binding = Match(found->id, bindings);
        REXLOG_INFO("ENEMIES matched host={:016X}/{} local={:016X}/{} mode={} spawn-error={:.4f} scheduled={}",
            r.enemy.id.source, r.enemy.id.sequence, found->id.source, found->id.sequence,
            found->id == r.enemy.id ? "exact" : "authored-tolerance",
            std::hypot(r.enemy.spawn_x-s.records[found->id].enemy.spawn_x,
                r.enemy.spawn_y-s.records[found->id].enemy.spawn_y, r.enemy.spawn_z-s.records[found->id].enemy.spawn_z),
            Member(b, s.world, found->actor));
      }
    }
    if (!binding || !ValidBinding(b, *binding)) {
      if (r.kind != EnemyEventKind::Spawned || r.enemy.life <= 0) continue; // already locally gone
      ++s.counts.unmatched;
      if (s.unmatched_logged.insert(r.enemy.id).second)
        REXLOG_INFO("ENEMIES unmatched host source={:016X} sequence={} family={} spawn=({:.3f},{:.3f},{:.3f}) retry=exact-always,tolerance-10s leave-alone=true",
            r.enemy.id.source, r.enemy.id.sequence, uint16_t(r.enemy.id.family), r.enemy.spawn_x, r.enemy.spawn_y, r.enemy.spawn_z);
#if defined(KK_DEV_TOOLS)
      if (now-s.retry[r.enemy.id].first > 10 && s.retry_diagnosed.insert(r.enemy.id).second) {
        const Authored* closest = nullptr; uint32_t actor = 0; float distance = 1e9f;
        for (const auto& [a, authored] : s.authored) if (authored.family == r.enemy.id.family) {
          const float d = std::hypot(authored.x-r.enemy.spawn_x, authored.y-r.enemy.spawn_y, authored.z-r.enemy.spawn_z);
          if (d < distance) { closest = &authored; distance = d; actor = a; }
        }
        if (closest) {
          bool spawner = false, initializing = false; uint32_t fn = 0;
          Tracks(b, closest->ai, fn, spawner, &initializing);
          REXLOG_INFO("ENEMIES retry-expired host={:016X} local={:016X} spawn-error={:.4f} scheduled={} init={} ambiguous={} flags={:08X} bound={} leave-alone=true",
              r.enemy.id.source, closest->key, distance, Member(b, s.world, actor), initializing,
              closest->ambiguous, Load32(b, actor), s.bindings.contains(actor));
        } else REXLOG_INFO("ENEMIES retry-expired host={:016X} reason=no-local-family leave-alone=true", r.enemy.id.source);
      }
#endif
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
      if (numbers) {
        Binding observed; EnemyState local;
        if (Read(b, binding->actor, observed, local)) {
          const auto& host = r.latest;
          const float error = std::hypot(host.x-local.x, host.y-local.y, host.z-local.z);
          s.error_sum += error; s.error_max = std::max(s.error_max, double(error)); ++s.error_count;
#if defined(KK_DEV_TOOLS)
          REXLOG_INFO("ENEMIES guest id={:016X}/{}/{} host=({:.3f},{:.3f},{:.3f}) local=({:.3f},{:.3f},{:.3f}) err={:.4f} life={:.2f}/{:.2f} action={}/{}",
              r.enemy.id.source, r.enemy.id.sequence, uint16_t(r.enemy.id.family), host.x, host.y, host.z,
              local.x, local.y, local.z, error, host.life, local.life, host.action, local.action);
#endif
        }
      }
    }
  }
  for (const auto& binding : bindings) if (!matched.contains(binding.actor) && ValidBinding(b, binding)) {
    ++s.counts.unmatched;
    if (s.unmatched_logged.insert(binding.id).second)
      REXLOG_INFO("ENEMIES unmatched guest source={:016X} sequence={} family={} leave-alone=true",
          binding.id.source, binding.id.sequence, uint16_t(binding.id.family));
  }
}
ShotVector Vector(const uint8_t* b, uint32_t p) { return {LoadF(b, p), LoadF(b, p+4), LoadF(b, p+8)}; }
ShotVector Direction(ShotVector a, ShotVector z) {
  const float d = ShotDistance(a, z);
  return d > .001f && std::isfinite(d) ? ShotVector{(z.x-a.x)/d, (z.y-a.y)/d, (z.z-a.z)/d} : ShotVector{};
}
bool JackReady(const uint8_t* b) {
  return ReadJackPose([&](uint32_t a) { return Load32(b, a); }, [&](uint32_t a) { return Load16(b, a); },
      [&](uint32_t a) { return LoadF(b, a); }, Now()).pose.has_value();
}
bool BeginShot(const uint8_t* b) {
  auto& s = Get();
  if (!Connected() || s.link.role != Role::Guest || !Environment(b, s.world) || !JackReady(b)) return false;
  const auto hero = Load32(b, 0x82CCBF7C), ai = Ptr(b, Ptr(b, hero, 24, 8), 4, 88);
  const auto vars = Ptr(b, ai, 64, 13652), matrix = Ptr(b, hero, 12, 64);
  if (!vars || !matrix || Load32(b, 0x82CCBF6C) != hero || Load32(b, 0x82CCBF64) != ai) return false;
  // H_callback_tir uses the same current weapon selector as cheats.cpp.
  const auto weapon = Load32(b, s.game+13128);
  if (weapon < 1 || weapon > 4 || s.next_shot == UINT32_MAX) return false;
  s.shot = {}; s.shot.world = s.identity; s.shot.number = ++s.next_shot;
  s.shot.weapon = uint8_t(weapon); s.shot.time = Now();
  s.shot.origin = Vector(b, matrix+48); s.shot.origin.z += 1.5f;
  const float x = -LoadF(b, matrix+16), y = -LoadF(b, matrix+20), length = std::hypot(x, y);
  if (!std::isfinite(length) || length < .01f) return false;
  s.shot.direction = {x/length, y/length, 0}; s.shooting = true; return true;
}
void ShotRay(const uint8_t* b, uint32_t origin, uint32_t point) {
  auto& s = Get();
  if (!s.shooting || !PoseAddress(origin, 12) || !PoseAddress(point, 12)) return;
  // Resolver's r3/r4 output vectors: muzzle/camera origin and actual impact.
  auto copy = s.shot; copy.origin = Vector(b, origin); copy.direction = Direction(copy.origin, Vector(b, point));
  if (ValidShotIntent(copy)) { s.shot.origin = copy.origin; s.shot.direction = copy.direction; }
}
bool GuestShotEvent(PPCContext& ctx, uint8_t* b) {
  auto& s = Get();
  if (!s.shooting || !Connected() || s.link.role != Role::Guest || !Environment(b, s.world) ||
      ctx.r6.u32 != Load32(b, 0x82CCBF7C)) return false;
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr != 0x82696E74 && lr != 0x82697494 && lr != 0x82697970 && lr != 0x82698C00) return false;
  const auto it = s.bindings.find(ctx.r8.u32);
  if (it == s.bindings.end() || !ValidBinding(b, it->second)) return false;
  auto alias = std::find_if(s.aliases.begin(), s.aliases.end(), [&](const auto& a) { return a.second == it->second.id; });
  if (alias == s.aliases.end()) return false;
  // Builder's caller stack+96 is the resolved impact, +80 its normal/vector.
  // See .47:26501-26539 and .39:23618-23652. No damage/bone value goes on wire.
  if (PoseAddress(ctx.r1.u32, 108) && s.shot.hits.size() < (s.shot.weapon == 3 ? kMaxShotHits : 1)) {
    const ShotHit hit{alias->first, Vector(b, ctx.r1.u32+96)};
    const bool duplicate = std::any_of(s.shot.hits.begin(), s.shot.hits.end(), [&](const auto& h) { return h.id == hit.id; });
    auto copy = s.shot;
    if (copy.hits.empty()) copy.direction = Direction(copy.origin, hit.point);
    if (!duplicate) copy.hits.push_back(hit);
    if (ValidShotIntent(copy)) s.shot = std::move(copy);
  }
  // Only the damage event is withheld. The normal callback keeps its muzzle,
  // sound, ammo, impact effects and animation paths. Invalid native handle.
  if (PoseAddress(ctx.r3.u32, 8)) { Store32(b, ctx.r3.u32, UINT32_MAX); Store32(b, ctx.r3.u32+4, 0); }
  return true;
}
void EndShot(bool fired) {
  auto& s = Get(); s.shooting = false;
  if (fired && ValidShotIntent(s.shot) && !SubmitShotIntent(s.shot)) s.shots.Reject(ShotReject::Queue);
}
bool NativeShot(PPCContext& ctx, uint8_t* b, const ShotIntent& shot, const ShotHit& hit, const Binding& target) {
  auto& s = Get();
  if (s.link.role != Role::Host || !Connected() || !Environment(b, s.world) ||
      !Scratch::CanUse(ctx) || !ValidBinding(b, target) || !PoseAddress(s.game, 18500) || !JackReady(b)) return false;
  const auto hero = Load32(b, 0x82CCBF7C), ai = Ptr(b, Ptr(b, hero, 24, 8), 4, 88);
  if (!ai || !Ptr(b, ai, 64, 13652) || !Ptr(b, hero, 12, 64)) return false;
  // Native type-4 pool layout verified in 8271E4E8 (.52:27465).
  constexpr uint32_t pool = 0x82C8EF30;
  const auto count = Load32(b, pool+12), used = Load32(b, pool+8);
  if (!count || count > 4096 || used > count || !Ptr(b, pool, 0, count*116)) return false;
  const float duration = LoadF(b, 0x820B0E48); // H_callback_tir's revolver event lifetime (.47:26214).
  if (!std::isfinite(duration) || duration <= 0 || duration > 10) return false;
  Scratch scratch(ctx, b);
  constexpr uint32_t globals[] = {0x82CCBF64, 0x82CCBF68, 0x82CCBF6C, 0x82CCBF70};
  uint32_t saved[4]; for (unsigned i = 0; i < 4; ++i) saved[i] = Load32(b, globals[i]);
  Store32(b, globals[0], ai); Store32(b, globals[1], ai);
  Store32(b, globals[2], hero); Store32(b, globals[3], hero);
  // Use the HOST's weapon tables/one-hit setting. No host life or ammo writes.
  const float distance = ShotDistance(shot.origin, hit.point);
  ctx.r3.u64 = shot.weapon; ctx.f1.f64 = double(distance)*distance;
  __imp__sub_82419C38(ctx, b);
  const auto damage = ctx.r3.s32;
  bool queued = false;
  if (damage > 0 && damage <= 1000000 && ValidBinding(b, target)) {
    const uint32_t result = scratch.Vec(0, 0, 0, 0x100);
    Store32(b, result, UINT32_MAX); Store32(b, result+4, 0);
    // Builder ABI: result, -1, firearm flags, shooter, vector bits, target,
    // bone=-1, damage; caller stack+80 normal, +96 hit point. f1=lifetime.
    // 82634428 builds the 108-byte record and calls 8271E4E8(type=4).
    const auto normal = Direction(hit.point, shot.origin);
    scratch.Vec(normal.x, normal.y, normal.z, 80);
    scratch.Vec(hit.point.x, hit.point.y, hit.point.z, 96);
    ctx.r3.u64 = result; ctx.r4.s64 = -1; ctx.r5.u64 = shot.weapon == 1 ? 68 : 64;
    ctx.r6.u64 = hero; ctx.r7.u64 = 0; ctx.r8.u64 = target.actor; ctx.r9.s64 = -1; ctx.r10.u64 = uint32_t(damage);
    ctx.f1.f64 = duration;
    __imp__sub_82634428(ctx, b);
    queued = Load32(b, result) != UINT32_MAX && Load32(b, result+4) != 0;
  }
  for (unsigned i = 0; i < 4; ++i) Store32(b, globals[i], saved[i]);
  return queued;
}
void HostShots(PPCContext& ctx, uint8_t* b) {
  auto& s = Get();
  for (const auto& received : TakeShotIntents()) {
    const auto& shot = received.intent;
    const auto weapon = PoseAddress(s.game, 13652) ? Load32(b, s.game+13128) : 0;
    auto reason = s.shot_validator.Validate(received, s.identity, weapon <= 4 ? uint8_t(weapon) : 0, s.link.partner, Now());
    std::vector<Binding> all;
    for (const auto& [actor, binding] : s.bindings) all.push_back(binding);
    std::vector<Binding> targets;
    if (reason == ShotReject::None) for (const auto& hit : shot.hits) {
      const auto* binding = Match(hit.id, all);
      Binding observed; EnemyState e;
      if (!binding || !ValidBinding(b, *binding) || !Read(b, binding->actor, observed, e)) { reason = ShotReject::Target; break; }
      e.id = binding->id;
      reason = ValidateShotHit(shot, hit, e);
      if (reason != ShotReject::None) break;
      targets.push_back(*binding);
    }
    bool applied = false;
    if (reason == ShotReject::None) for (size_t i = 0; i < targets.size(); ++i) {
      if (!NativeShot(ctx, b, shot, shot.hits[i], targets[i])) { reason = ShotReject::Engine; break; }
      applied = true;
    }
    if (applied) ++s.shots.applied; // queued normal native event; life outcome comes from Scan.
    if (reason != ShotReject::None) s.shots.Reject(reason);
#if defined(KK_DEV_TOOLS)
    if (Now() >= s.next_shot_log) {
      s.next_shot_log = Now()+1;
      REXLOG_INFO("ENEMIES shot number={} weapon={} hits={} applied={} reject={}",
          shot.number, shot.weapon, shot.hits.size(), applied, ShotRejectName(reason));
    }
#endif
  }
}
void BeforeAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
  auto& s = Get(); s.prepared = false; s.driving.clear();
  if (uint32_t(ctx.lr) != 0x82881CF0) return;
#if defined(KK_DEV_TOOLS)
  DevWakeTick(ctx, b, w);
  DevEnemyNear(b, w);
#endif
  if (!REXCVAR_GET(kk_coop_enabled)) return;
  // Capture before connection, at the first AI pass of this load. Waiting for
  // the peer could otherwise hash actors that already moved locally.
  if (s.capture) Capture(b, w);
  if (s.world == w && Now() <= s.census_until && Environment(b, w)) Capture(b, w, false);
  if (!Connected()) return;
  s.link = EnemyLinkSnapshot();
  if (!s.game && !s.load_refused_logged) {
    s.load_refused_logged = true;
    REXLOG_INFO("ENEMIES waiting reason=no-connected-load-identity restart-both-and-load-while-connected=true");
  }
  if (!Environment(b, w)) return;
  Scan(b); s.prepared = true; s.prepared_at = Now();
  if (s.link.role == Role::Guest) Guest(ctx, b, false);
  else HostShots(ctx, b);
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
#if defined(KK_DEV_TOOLS)
    if (Now() >= s.next_numbers) {
      s.next_numbers = Now()+1;
      for (const auto& [id, r] : s.records) if (r.kind != EnemyEventKind::Removed)
        REXLOG_INFO("ENEMIES host id={:016X}/{}/{} pos=({:.3f},{:.3f},{:.3f}) life={:.2f} action={}",
            id.source, id.sequence, uint16_t(id.family), r.enemy.x, r.enemy.y, r.enemy.z, r.enemy.life, r.enemy.action);
    }
#endif
  }
  s.counts.sent = s.link.sent; SetEnemyCounters(s.counts);
  SetShotCounters(s.shots);
#if defined(KK_DEV_TOOLS)
  if (Now() >= s.next_log) {
    s.next_log = Now()+5;
    REXLOG_INFO("ENEMIES status role={} sent={} matched={} unmatched={} driven={} tracked={}",
        s.link.role == Role::Host ? "host" : "guest", s.link.sent, s.counts.matched,
        s.counts.unmatched, s.counts.driven, s.records.size());
    REXLOG_INFO("ENEMIES error samples={} max={:.4f} avg={:.4f} shots-sent={} applied={} rejected={} reason={}",
        s.error_count, s.error_max, s.error_count ? s.error_sum/double(s.error_count) : 0,
        s.link.shots_sent, s.shots.applied, s.shots.rejected, ShotRejectName(s.shots.last));
    s.error_sum = s.error_max = 0; s.error_count = 0;
    for (size_t i = 1; i < size_t(ShotReject::Count); ++i) if (s.shots.reasons[i])
      REXLOG_INFO("ENEMIES shots-rejected reason={} total={}", ShotRejectName(ShotReject(i)), s.shots.reasons[i]);
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
  s.pending[actor] = {id, ai, matrix, LoadF(b, matrix+48), LoadF(b, matrix+52), LoadF(b, matrix+56)};
}
} // namespace
void EnemyBeforeLoad() {
#if defined(KK_DEV_TOOLS)
  DevWakeBeforeLoad();
#endif
  if (!REXCVAR_GET(kk_coop_enabled)) return;
  Get().ResetWorld(); PublishEnemyFrame({}); SetEnemyCounters({}); SetShotCounters({});
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
#if defined(KK_DEV_TOOLS)
std::optional<ShotVector> EnemyDormantTarget(const uint8_t* b, uint32_t w, ShotVector origin) {
  std::optional<ShotVector> best;
  float distance = 200;
  for (const auto& e : DevEnemies(b, w, origin)) {
    // Avoid large vertical warps, uninitialized templates, corpses, and active
    // combat. This is synthetic placement; collision/visibility are not proven.
    if (e.scheduled || e.initializing || e.life <= 0 || std::abs(e.position.z-origin.z) > 2 || e.distance >= distance) continue;
    distance = e.distance; best = e.position;
  }
  return best;
}
std::optional<ShotVector> EnemyAimTarget(const uint8_t* b, ShotVector origin) {
  auto& s = Get();
  if (!Connected() || s.link.role != Role::Guest || !Environment(b, s.world) || Now()-s.prepared_at > .5) return {};
  std::optional<ShotVector> best; float distance = 300;
  for (const auto& [actor, record] : s.driving) {
    const auto it = s.bindings.find(actor);
    if (it == s.bindings.end() || record.enemy.life <= 0 || !ValidBinding(b, it->second)) continue;
    const auto m = it->second.matrix;
    ShotVector point = Vector(b, m+48);
    point.z += it->second.id.family == EnemyFamily::Raptor ? 1.2f : .4f;
    const float d = ShotDistance(origin, point);
    if (d > .1f && d < distance) { distance = d; best = point; }
  }
  return best;
}
#endif
} // namespace kk::coop

#if defined(KK_DEV_TOOLS)
// Observe the real scripts' activation execution, without changing their
// conditions, arguments or result. See TrigExec_SetActivity (.115:18091) and
// TrigExec_SetActiveAndVisible (.25:19027): +4=actor, +84=off.
REX_HOOK_RAW(sub_8250EE50) {
  const auto record = ctx.r3.u32;
  const bool readable = kk::coop::PoseAddress(record, 88);
  const auto actor = readable ? kk::hfr::Load32(base, record+4) : 0;
  const auto off = readable ? kk::hfr::Load32(base, record+84) : 0;
  __imp__sub_8250EE50(ctx, base);
  if (readable) kk::coop::DevWakeActivity(actor, off, "SetActivity");
}
REX_HOOK_RAW(sub_8250EDE8) {
  const auto record = ctx.r3.u32;
  const bool readable = kk::coop::PoseAddress(record, 88);
  const auto actor = readable ? kk::hfr::Load32(base, record+4) : 0;
  const auto off = readable ? kk::hfr::Load32(base, record+84) : 0;
  __imp__sub_8250EDE8(ctx, base);
  if (readable) kk::coop::DevWakeActivity(actor, off, "SetActiveAndVisible");
}
#endif

// One owner for each shared hook in both dev and production builds.
REX_HOOK_RAW(sub_8271D498) {
  const uint32_t world = ctx.r3.u32;
#if defined(KK_DEV_TOOLS)
  kk::DevSampleTick(&ctx, base, world);
#endif
  kk::PartnerBodyBeforeAi(ctx, base, world);
  kk::coop::BeforeAi(ctx, base, world);
  __imp__sub_8271D498(ctx, base);
  kk::PartnerBodyAfterAi(ctx, base, world);
  kk::coop::AfterAi(ctx, base, world);
}
REX_HOOK_RAW(sub_8271CCB8) {
  if (kk::coop::Hold(ctx, base)) return;
  if (kk::PartnerBodyHold(ctx, base)) return;
  __imp__sub_8271CCB8(ctx, base);
}
REX_HOOK_RAW(sub_82715AC8) {
  const bool observe = uint32_t(ctx.lr) == 0x826F9240 && kk::coop::Connected();
  const auto source = ctx.r3.u32;
  const auto spawner = observe ? kk::hfr::Load32(base, 0x82CCBF6C) : 0;
  __imp__sub_82715AC8(ctx, base);
  if (observe && ctx.r3.u32) kk::coop::Spawned(base, spawner, source, ctx.r3.u32);
}
REX_HOOK_RAW(sub_82696A28) {
  const bool observe = kk::coop::BeginShot(base);
  const auto game = observe ? kk::hfr::Load32(base, 0x82CC97C0) : 0;
  const auto weapon = observe ? kk::hfr::Load32(base, game+13128) : 0;
  constexpr uint32_t ammo[] = {13432, 13504, 13576, 13648};
  const auto before = observe ? kk::hfr::Load32(base, game+ammo[weapon-1]) : 0;
  __imp__sub_82696A28(ctx, base);
  if (observe) {
    const bool same = game == kk::hfr::Load32(base, 0x82CC97C0);
    kk::coop::EndShot(same && kk::hfr::Load32(base, game+ammo[weapon-1]) < before);
  }
}
REX_HOOK_RAW(sub_8243A3C0) {
  const auto origin = ctx.r3.u32, point = ctx.r4.u32;
  __imp__sub_8243A3C0(ctx, base);
  kk::coop::ShotRay(base, origin, point);
}
REX_HOOK_RAW(sub_82634428) {
  if (!kk::coop::GuestShotEvent(ctx, base)) __imp__sub_82634428(ctx, base);
}
