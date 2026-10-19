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
#if defined(KK_DEV_TOOLS)
#include "coop_stimulus_debug.h"
#endif
#include <algorithm>
#include <bit>
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
REX_EXTERN(__imp__sub_8261DE68);
REX_EXTERN(__imp__sub_826A0920);
REX_EXTERN(__imp__sub_82548BC0);
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_8254A8F8);
REX_EXTERN(__imp__sub_82569A70);
REX_EXTERN(__imp__sub_8245A7B8);
REX_EXTERN(__imp__sub_8246DC58);
REX_EXTERN(__imp__sub_82428020);
REX_EXTERN(__imp__sub_82464680);
#endif
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
#if defined(KK_DEV_TOOLS)
struct DevRaptorLog {
  double select_next = 0, vision_next = 0, filter_next = 0, register_next = 0, ray_next = 0, gate_next = 0;
  unsigned select_count = 0, vision_count = 0, filter_count = 0, register_count = 0, ray_count = 0, gate_count = 0;
};
#endif
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
  uint64_t combat_epoch = 0, incarnation = 0;
  uint32_t next_damage = 0, next_status = 0;
  uint32_t combat_hero = 0, combat_ai = 0, combat_vars = 0;
  bool injecting_damage = false;
  combat::DamageReplay damage_replay;
  combat::StatusReplay status_replay;
  std::deque<ReceivedCombat> pending_damage;
  std::optional<PlayerStatus> sent_status;
  double next_status_time = 0, partner_status_time = -1;
  uint64_t hits_produced = 0, hits_queued = 0, hits_native = 0, hits_refused = 0;
  bool shooting = false, shot_resolved = false;
  ShotIntent shot;
  InventoryState shot_inventory;
#if defined(KK_DEV_TOOLS)
  uint32_t shot_hit_object = 0;
  bool shot_matched = false;
  const char* shot_hit_reason = "no-hit";
  unsigned shot_target_diagnostics = 0;
  std::map<uint32_t, DevRaptorLog> raptor_logs;
  unsigned raptor_candidate_lines = 0;
#endif
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
    aliases.clear(); retry.clear(); retry_diagnosed.clear(); shots = {}; shot_validator = {}; shooting = shot_resolved = false; shot = {};
    census_until = next_numbers = next_log = next_shot_log = error_sum = error_max = 0; error_count = 0;
    combat_epoch = incarnation = 0; combat_hero = combat_ai = combat_vars = 0;
    next_damage = next_status = 0; pending_damage.clear(); sent_status.reset();
    damage_replay = {}; status_replay = {}; injecting_damage = false;
    next_status_time = 0; partner_status_time = -1;
#if defined(KK_DEV_TOOLS)
    raptor_logs.clear(); raptor_candidate_lines = 0;
#endif
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
bool RetainedDamageSource(const uint8_t* b, const Binding& source) {
  // Preserve the last committed attack of a corpse still in the full registry.
  // Removal, component reuse and a new initializer invalidate it immediately.
  bool initializing = false, spawner = false; uint32_t state = 0;
  return Member(b, Get().world, source.actor, true) && PoseAddress(source.actor, 32) &&
      !(Load32(b, source.actor)&1) && Ptr(b, Ptr(b, source.actor, 24, 8), 4, 88) == source.ai &&
      Ptr(b, source.ai, 64, LifeOffset(source.id.family)+4) == source.vars &&
      Ptr(b, source.actor, 12, 64) == source.matrix &&
      Tracks(b, source.ai, state, spawner, &initializing) == source.id.family && !initializing;
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
struct HumanStatus {
  uint32_t hero = 0, ai = 0, vars = 0, cell = 0, bank = 0;
  float value = 0, hurt = 0, dead = 0, recovered = 0;
  PlayerCondition condition = PlayerCondition::Alive;
};
std::optional<HumanStatus> ReadHumanStatus(const uint8_t* b) {
  HumanStatus h;
  h.hero = Load32(b, 0x82CCBF7C);
  h.ai = Ptr(b, Ptr(b, h.hero, 24, 8), 4, 88);
  h.vars = Ptr(b, h.ai, 64, 15232);
  h.bank = Load32(b, 0x82CC8848);
  if (!h.vars || !PoseAddress(h.hero, 32) || !Ptr(b, h.hero, 12, 64)) return {};
  const auto id = Load32(b, h.vars+4);
  if (id > 12 || !PoseAddress(h.bank, 7096+4*id) || !Load32(b, h.vars)) return {};
  // Verify canonical H tagoff even during native death, without requiring the
  // ordinary live-pose reader (which can reject inactive/dead actors).
  const auto first = Load16(b, h.ai+80), last = Load16(b, h.ai+82);
  if (first > last || last > 4096 || !PoseAddress(h.ai, (first+7)*4)) return {};
  const auto tag = Ptr(b, h.ai, (first+6)*4, 24);
  if (!tag || Load32(b, tag+20) != 0x826718B8) return {};
  h.cell = h.bank+7092+4*id;
  h.value = LoadF(b, h.cell); h.dead = LoadF(b, 0x820B0C98);
  h.hurt = LoadF(b, 0x828D28E8); h.recovered = LoadF(b, 0x828E3AC0);
  if (!std::isfinite(h.value) || !std::isfinite(h.dead) || !std::isfinite(h.hurt) ||
      !std::isfinite(h.recovered) || h.dead == h.hurt || h.dead == h.recovered || h.hurt == h.recovered) return {};
  h.condition = h.value == h.dead ? PlayerCondition::Dead :
      h.value == h.hurt ? PlayerCondition::Hurt : PlayerCondition::Alive;
  return h;
}
void CombatContext(const uint8_t* b) {
  auto& s = Get();
  if (s.link.role == Role::Host) {
    // Host hit sequence belongs to the guest's life, not the host's Jack or a
    // transient one-way connection loss. The guest status changes its life.
    if (s.combat_epoch != s.link.epoch) {
      s.combat_epoch = s.link.epoch; s.status_replay.Reset(s.combat_epoch, s.identity);
      s.next_damage = 0; s.partner_status_time = -1; s.pending_damage.clear();
    }
    return;
  }
  const auto h = ReadHumanStatus(b);
  if (!h) {
#if defined(KK_DEV_TOOLS)
    static double next = 0; static unsigned count = 0;
    if (count < 8 && Now() >= next) {
      ++count; next = Now()+1;
      const auto hero = Load32(b, 0x82CCBF7C), ai = Ptr(b, Ptr(b, hero, 24, 8), 4, 88);
      REXLOG_INFO("BITE Jack-status unavailable hero={:08X} ai={:08X} vars={:08X} bank={:08X} dead={:.3f} hurt={:.3f} recovered={:.3f}",
          hero, ai, Ptr(b, ai, 64, 8), Load32(b, 0x82CC8848), LoadF(b, 0x820B0C98), LoadF(b, 0x828D28E8), LoadF(b, 0x828E3AC0));
    }
#endif
    return;
  }
  if (s.combat_epoch != s.link.epoch || s.combat_hero != h->hero ||
      s.combat_ai != h->ai || s.combat_vars != h->vars || !s.incarnation) {
    static uint64_t life = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    s.combat_epoch = s.link.epoch; s.incarnation = ++life;
    s.combat_hero = h->hero; s.combat_ai = h->ai; s.combat_vars = h->vars;
    s.damage_replay.Reset(s.combat_epoch, s.identity, s.incarnation);
    s.status_replay.Reset(s.combat_epoch, s.identity);
    s.sent_status.reset(); s.next_status = s.next_damage = 0;
    s.next_status_time = 0; s.partner_status_time = -1; s.pending_damage.clear();
  }
}
void PublishPlayerStatus(const uint8_t* b) {
  auto& s = Get();
  if (!s.prepared || s.link.role != Role::Guest || !Connected() || !Environment(b, s.world) ||
      !s.combat_epoch || s.combat_epoch != s.link.epoch || !s.incarnation) return;
  const auto h = ReadHumanStatus(b); if (!h || h->hero != s.combat_hero || h->ai != s.combat_ai || h->vars != s.combat_vars) return;
  PlayerStatus p{s.identity, s.incarnation, s.next_status+1, s.damage_replay.Last(), h->condition};
  const bool changed = !s.sent_status || p.condition != s.sent_status->condition || p.last_damage != s.sent_status->last_damage;
  if (!changed && Now() < s.next_status_time) return;
  if (!p.number) return; // Do not wrap a replay sequence.
  Message m; m.type = MessageType::PlayerStatus; m.epoch = s.combat_epoch; m.player_status = p;
  if (SubmitCombat(m, Now())) {
    ++s.next_status; s.sent_status = p; s.next_status_time = Now()+.5;
#if defined(KK_DEV_TOOLS)
    if (changed) REXLOG_INFO("BITE player-status incarnation={} condition={} last-processed={} cell={:08X} value={:.3f} dead={:.3f} hurt={:.3f} recovered={:.3f} bank={:08X} G={:08X}",
        p.incarnation, uint8_t(p.condition), p.last_damage, h->cell, h->value, h->dead, h->hurt, h->recovered, h->bank, Load32(b, 0x82CC97C0));
#endif
  }
}
bool NativeDamage(PPCContext& ctx, uint8_t* b, const DamageEvent& d) {
  auto& s = Get();
  if (!Scratch::CanUse(ctx) || !JackReady(b) || !Environment(b, s.world)) return false;
  auto alias = s.aliases.find(d.enemy);
  if (alias == s.aliases.end()) return false;
  std::vector<Binding> bindings;
  for (const auto& [actor, binding] : s.bindings) bindings.push_back(binding);
  const Binding* source = Match(alias->second, bindings); // also refuses mutable-component aliases
  const auto h = ReadHumanStatus(b);
  if (!source || !RetainedDamageSource(b, *source) || !h || h->hero != s.combat_hero || h->ai != s.combat_ai ||
      h->vars != s.combat_vars || h->condition == PlayerCondition::Dead) return false;
  constexpr uint32_t pool = 0x82C8EF30;
  const auto count = Load32(b, pool+12), used = Load32(b, pool+8);
  const float duration = LoadF(b, 0x820B0E48); // Raptor bite 8256D2F0 lifetime.
  if (!count || count > 4096 || used >= count || !Ptr(b, pool, 0, count*116) ||
      !std::isfinite(duration) || duration <= 0 || duration > 10) return false;
  Scratch scratch(ctx, b);
  constexpr uint32_t globals[] = {0x82CCBF64, 0x82CCBF68, 0x82CCBF6C, 0x82CCBF70};
  uint32_t saved[4]; for (unsigned i = 0; i < 4; ++i) saved[i] = Load32(b, globals[i]);
  Store32(b, globals[0], source->ai); Store32(b, globals[1], source->ai);
  Store32(b, globals[2], source->actor); Store32(b, globals[3], source->actor);
  const uint32_t result = scratch.Vec(0, 0, 0, 0x100);
  Store32(b, result, UINT32_MAX); Store32(b, result+4, 0);
  StoreF(b, ctx.r1.u32+88, d.direction.z);
  ctx.r3.u64 = result; ctx.r4.s64 = -1; ctx.r5.u64 = d.flags;
  ctx.r6.u64 = source->actor; ctx.r8.u64 = h->hero; ctx.r9.u64 = d.magnitude;
  ctx.r10.u64 = (uint64_t(std::bit_cast<uint32_t>(d.direction.x)) << 32) | std::bit_cast<uint32_t>(d.direction.y);
  ctx.f1.f64 = duration;
  s.injecting_damage = true;
  __imp__sub_8261DE68(ctx, b);
  s.injecting_damage = false;
  for (unsigned i = 0; i < 4; ++i) Store32(b, globals[i], saved[i]);
  return Load32(b, result) != UINT32_MAX && Load32(b, result+4) != 0;
}
void CombatBeforeAi(PPCContext& ctx, uint8_t* b) {
  auto& s = Get();
  CombatContext(b);
  for (auto& entry : TakeCombat()) {
    if (s.link.role == Role::Host) {
      const auto old = s.status_replay.Status();
      if (s.status_replay.Accept(entry.message, entry.received, Now())) {
        s.partner_status_time = Now();
        if (old.incarnation != s.status_replay.Status().incarnation) { s.next_damage = 0; s.pending_damage.clear(); }
#if defined(KK_DEV_TOOLS)
        const auto& p = s.status_replay.Status();
        static unsigned logged = 0;
        if (logged < 256 && (old.incarnation != p.incarnation || old.condition != p.condition || old.last_damage != p.last_damage)) {
          ++logged;
          REXLOG_INFO("BITE partner-status incarnation={} condition={} last-processed={}", p.incarnation, uint8_t(p.condition), p.last_damage);
        }
#endif
      }
    } else if (entry.message.type == MessageType::DamageEvent) {
      if (s.pending_damage.size() < combat::kQueueLimit) s.pending_damage.push_back(std::move(entry));
      else {
        ++s.hits_refused;
#if defined(KK_DEV_TOOLS)
        static unsigned logged = 0;
        if (logged++ < 8) REXLOG_WARN("BITE guest damage refused reason=pending-queue-full");
#endif
      }
    }
  }
  if (s.link.role == Role::Host) {
    while (!s.pending_damage.empty()) {
      const auto& pending = s.pending_damage.front();
      if (!combat::Fresh(pending.received, Now())) { ++s.hits_refused; s.pending_damage.pop_front(); continue; }
      if (!SubmitCombat(pending.message, pending.received)) break;
      ++s.hits_queued; s.pending_damage.pop_front();
    }
  } else {
    while (!s.pending_damage.empty()) {
      const auto& entry = s.pending_damage.front();
      // Wait briefly for the normal enemy matcher, without advancing replay.
      if ((!s.aliases.contains(entry.message.damage.enemy) || !s.link.partner) && combat::Fresh(entry.received, Now())) break;
      const bool accepted = s.link.partner && s.link.partner->level == s.identity.level &&
          combat::DamageClock(entry.message.damage.time, s.link.partner->time) &&
          s.damage_replay.Accept(entry.message, entry.received, Now());
      const bool queued = accepted && NativeDamage(ctx, b, entry.message.damage);
      if (queued) ++s.hits_native; else ++s.hits_refused;
#if defined(KK_DEV_TOOLS)
      REXLOG_INFO("BITE guest number={} incarnation={} replay-accepted={} native-queued={} processed={} injured=observe-native-consumer",
          entry.message.damage.number, entry.message.damage.incarnation, accepted, queued, s.damage_replay.Last());
#endif
      s.pending_damage.pop_front();
    }
    PublishPlayerStatus(b);
  }
}
#if defined(KK_DEV_TOOLS)
void DevBait(PPCContext& ctx, uint8_t* b) {
  struct Bait {
    double due = -1;
    bool done = false;
    Bait() {
      const char* value = std::getenv("KK_DEV_BAIT");
      if (!value || !*value) return;
      char* end = nullptr; const auto parsed = std::strtod(value, &end);
      if (end != value && !*end && std::isfinite(parsed) && parsed >= 0 && parsed <= 3600) due = parsed;
      else REXLOG_WARN("KK dev bait: invalid KK_DEV_BAIT, expected seconds in 0..3600");
    }
  };
  static Bait bait;
  if (bait.done || bait.due < 0) return;
  const double t = kk::DevSecondsSinceMenu();
  if (t < bait.due) return;
  if (t > bait.due+10) { bait.done = true; REXLOG_WARN("KK dev bait: expired waiting for matched live raptor on guest"); return; }
  auto& s = Get();
  if (!s.prepared || s.link.role != Role::Guest || !Connected() || !Environment(b, s.world)) return;
  const auto jack = ReadJackPose([&](uint32_t p) { return Load32(b, p); },
      [&](uint32_t p) { return Load16(b, p); }, [&](uint32_t p) { return LoadF(b, p); }, Now());
  if (!jack.pose) return;
  const ShotVector origin{jack.pose->x, jack.pose->y, jack.pose->z};
  const Record* nearest = nullptr; float distance = 300;
  for (const auto& [actor, record] : s.driving) {
    if (record.kind != EnemyEventKind::Spawned || record.enemy.life <= 0 || record.enemy.id.family != EnemyFamily::Raptor) continue;
    const auto it = s.bindings.find(actor); const auto alias = s.aliases.find(record.enemy.id);
    if (it == s.bindings.end() || alias == s.aliases.end() || alias->second != it->second.id || !ValidBinding(b, it->second)) continue;
    const float d = ShotDistance(origin, {record.enemy.x, record.enemy.y, record.enemy.z});
    if (d < distance) { distance = d; nearest = &record; }
  }
  if (!nearest) return;
  const auto& e = nearest->enemy;
  const float dx = e.x-origin.x, dy = e.y-origin.y, length = std::hypot(dx, dy);
  const float fx = length > .001f ? dx/length : std::cos(e.facing);
  const float fy = length > .001f ? dy/length : std::sin(e.facing);
  const ShotVector target{e.x-3*fx, e.y-3*fy, e.z};
  if (!kk::DevTeleportJack(ctx, b, Load32(b, 0x82CCBF7C), target, fx, fy)) return;
  bait.done = true;
  REXLOG_INFO("KK dev bait: at {:.2f} guest teleported by native 82538B70 raptor={:016X}/{} from=({:.3f},{:.3f},{:.3f}) to=({:.3f},{:.3f},{:.3f}) stand-off=3 collision-unverified=true",
      t, e.id.source, e.id.sequence, origin.x, origin.y, origin.z, target.x, target.y, target.z);
}
// Diagnostics are per validated host raptor, bounded and reset on load. The old
// process-wide timer hid other raptors; a handle alone did not identify a victim.
uint32_t DevRaptorVars(const uint8_t* b, uint32_t actor, uint32_t ai) {
  const auto& s = Get();
  if (!REXCVAR_GET(kk_coop_enabled) || !Connected() || s.link.role != Role::Host || !Environment(b, s.world)) return 0;
  const auto it = s.bindings.find(actor);
  if (it == s.bindings.end() || it->second.id.family != EnemyFamily::Raptor ||
      it->second.ai != ai || !ValidBinding(b, it->second)) return 0;
  const auto vars = Ptr(b, ai, 64, 14200);
  return vars == it->second.vars ? vars : 0;
}
DevRaptorLog* DevRaptorBudget(uint32_t actor) {
  auto& logs = Get().raptor_logs;
  if (!logs.contains(actor) && logs.size() >= 16) return nullptr;
  return &logs[actor];
}
bool DevSample(double& next, unsigned& count, bool changed = false) {
  const auto now = Now();
  if (count >= 64 || (!changed && now < next)) return false;
  ++count; next = now+1; return true;
}
int32_t DevBodySlot(const uint8_t* b, uint32_t vars, uint32_t body) {
  const auto count = Load32(b, vars+5236);
  if (!body || count > 20) return -1;
  for (uint32_t i = 0; i < count; ++i) if (Load32(b, vars+6244+4*i) == body) return int32_t(i);
  return -1;
}
void DevRaptorCandidates(const uint8_t* b, uint32_t actor, uint32_t vars, uint32_t body) {
  const auto count = Load32(b, vars+5236);
  if (count > 20) { REXLOG_WARN("BITE raptor-candidates actor={:08X} invalid-count={} limit=20", actor, count); return; }
  for (uint32_t i = 0; i < count; ++i) {
    const auto target = Load32(b, vars+6244+4*i), off = 4*i, pos = vars+6000+12*i;
    // Reserve evidence for a late body registration even after the other-row
    // budget is spent. Body rows remain bounded by each raptor's 64 samples.
    if (!(body && target == body)) {
      if (Get().raptor_candidate_lines >= 1024) continue;
      ++Get().raptor_candidate_lines;
    }
    // Best-interest has several priority passes, not one scalar score. +5748 is
    // native distance; +5832 is compared as a score in 8254AABC. Log both plus
    // eligibility/priority fields rather than infer why a lower score won.
    REXLOG_INFO("BITE raptor-candidate actor={:08X} slot={} candidate={:08X} body={} selected={} visited={} class+5664={} flags+5496={:08X} seen+5244={} area+5328={} area-id+5412={} group+5580={} distance+5748={:.3f} score+5832={:.3f} life+5916={:.3f} pos=({:.3f},{:.3f},{:.3f})",
        actor, i, target, body && target == body, Load32(b, vars+5228) == i,
        (Load32(b, vars+5232) & (1u << i)) != 0, Load32(b, vars+5664+off), Load32(b, vars+5496+off),
        Load32(b, vars+5244+off), Load32(b, vars+5328+off), Load32(b, vars+5412+off), Load32(b, vars+5580+off),
        LoadF(b, vars+5748+off), LoadF(b, vars+5832+off), LoadF(b, vars+5916+off),
        LoadF(b, pos), LoadF(b, pos+4), LoadF(b, pos+8));
  }
}
void DevRaptorVision(PPCContext& ctx, uint8_t* b) {
  const auto ai = Load32(b, 0x82CCBF64), actor = Load32(b, 0x82CCBF6C);
  const auto vars = DevRaptorVars(b, actor, ai), body = vars ? kk::PartnerBodyCombatActor(b) : 0;
  auto* budget = vars ? DevRaptorBudget(actor) : nullptr;
  const bool sample = budget && body && DevSample(budget->vision_next, budget->vision_count);
  if (sample) {
    const auto u32 = [&](uint32_t p) { return Load32(b, p); };
    const auto pool = stimulus_debug::ReadPool(u32, 1);
    unsigned found = 0, enumerable = 0;
    if (pool.valid && pool.count <= 4096) for (uint32_t i = 0; i < pool.count; ++i) {
      const auto r = stimulus_debug::ReadSlot(u32, pool, i);
      if (!r.valid || r.source != body) continue;
      ++found; enumerable += r.enumerable;
      if (found <= 4) REXLOG_INFO("BITE vision-stimulus actor={:08X} body={:08X} id={:08X}/{} pool-flags={:08X} enumerable={} lifetime={:.6f} class={} status={} life={:.3f} pos=({:.3f},{:.3f},{:.3f})",
          actor, body, r.id, r.generation, r.flags, r.enumerable, LoadF(b, r.address+4),
          Load32(b, r.address+8+96), Load32(b, r.address+8+92), LoadF(b, r.address+8+48),
          LoadF(b, r.address+8+24), LoadF(b, r.address+8+28), LoadF(b, r.address+8+32));
    }
    REXLOG_INFO("BITE vision-input actor={:08X} body={:08X} pool-valid={} scan-complete={} pool={}/{} body-records={} enumerable={} cached+5144={} candidates={} body-slot={}",
        actor, body, pool.valid, pool.valid && pool.count <= 4096, pool.count, pool.capacity,
        found, enumerable, Load32(b, vars+5144), Load32(b, vars+5236), DevBodySlot(b, vars, body));
  }
  __imp__sub_82569A70(ctx, b);
  if (sample && DevRaptorVars(b, actor, ai) == vars)
    REXLOG_INFO("BITE vision-output actor={:08X} body={:08X} candidates={} body-slot={} visited-mask={:08X}",
        actor, body, Load32(b, vars+5236), DevBodySlot(b, vars, body), Load32(b, vars+5232));
}
void DevRaptorFilter(PPCContext& ctx, uint8_t* b) {
  const auto ai = Load32(b, 0x82CCBF64), actor = Load32(b, 0x82CCBF6C);
  const auto vars = uint32_t(ctx.lr) == 0x82569BD8 ? DevRaptorVars(b, actor, ai) : 0;
  const auto body = vars ? kk::PartnerBodyCombatActor(b) : 0;
  const auto r = body ? stimulus_debug::ReadHandle([&](uint32_t p) { return Load32(b, p); }, ctx.r3.u32) : stimulus_debug::Record{};
  const auto classification = r.valid ? Load32(b, r.address+8+96) : 0;
  __imp__sub_8245A7B8(ctx, b);
  auto* budget = r.valid && r.source == body ? DevRaptorBudget(actor) : nullptr;
  if (budget && DevSample(budget->filter_next, budget->filter_count))
    REXLOG_INFO("BITE body-filter actor={:08X} body={:08X} id={:08X}/{} class={} rejected={}",
        actor, body, r.id, r.generation, classification, ctx.r3.u32 != 0);
}
void DevRaptorRegister(PPCContext& ctx, uint8_t* b) {
  const auto ai = Load32(b, 0x82CCBF64), actor = Load32(b, 0x82CCBF6C);
  const auto vars = uint32_t(ctx.lr) == 0x82569BF8 ? DevRaptorVars(b, actor, ai) : 0;
  const auto body = vars ? kk::PartnerBodyCombatActor(b) : 0;
  const bool observe = body && ctx.r3.u32 == body;
  const auto before = observe ? Load32(b, vars+5236) : 0;
  __imp__sub_8246DC58(ctx, b);
  auto* budget = observe && DevRaptorVars(b, actor, ai) == vars ? DevRaptorBudget(actor) : nullptr;
  if (budget && DevSample(budget->register_next, budget->register_count))
    REXLOG_INFO("BITE body-register actor={:08X} body={:08X} returned-slot={} body-slot={} candidates={}->{} full-before={}",
        actor, body, ctx.r3.s32, DevBodySlot(b, vars, body), before, Load32(b, vars+5236), before == 20);
}
void DevRaptorVisibility(PPCContext& ctx, uint8_t* b, bool ray) {
  const auto ai = Load32(b, 0x82CCBF64), actor = Load32(b, 0x82CCBF6C), caller = uint32_t(ctx.lr);
  const auto vars = caller == (ray ? 0x82569FE8u : 0x8256A0DCu) ? DevRaptorVars(b, actor, ai) : 0;
  const auto body = vars ? kk::PartnerBodyCombatActor(b) : 0;
  const auto index = ctx.r3.u32;
  const bool observe = body && (ray ? index == body : index < 20 && index < Load32(b, vars+5236) && Load32(b, vars+6244+4*index) == body);
  if (ray) __imp__sub_82428020(ctx, b); else __imp__sub_82464680(ctx, b);
  auto* budget = observe ? DevRaptorBudget(actor) : nullptr;
  if (budget && DevSample(ray ? budget->ray_next : budget->gate_next, ray ? budget->ray_count : budget->gate_count))
    REXLOG_INFO("BITE body-visibility actor={:08X} body={:08X} helper={} caller={:08X} result={} (native-vision-ray-nonzero-passes; gate-zero-passes)",
        actor, body, ray ? "82428020" : "82464680", caller, ctx.r3.u32);
}
void DevRaptorSelection(PPCContext& ctx, uint8_t* b) {
  const auto ai = Load32(b, 0x82CCBF64), actor = Load32(b, 0x82CCBF6C);
  const auto vars = DevRaptorVars(b, actor, ai);
  const auto before = vars ? Load32(b, vars+5216) : 0;
  const auto cached = vars ? Load32(b, vars+5168) : 0;
  __imp__sub_8254A8F8(ctx, b);
  if (!vars || DevRaptorVars(b, actor, ai) != vars) return;
  auto* budget = DevRaptorBudget(actor);
  const auto after = Load32(b, vars+5216), body = kk::PartnerBodyCombatActor(b);
  if (!budget || !DevSample(budget->select_next, budget->select_count, before != after)) return;
  const auto u32 = [&](uint32_t p) { return Load32(b, p); };
  const auto target = stimulus_debug::ReadHandle(u32, vars+5216);
  const auto force = stimulus_debug::ReadHandle(u32, vars+8436);
  uint32_t state = 0; bool spawner = false; Tracks(b, ai, state, spawner);
  REXLOG_INFO("BITE raptor-select actor={:08X} hero-cache+5100={:08X} target-handle+5216={:08X}->{:08X}/{} valid={} source={:08X} victim={:08X} body={:08X} victim-is-body={} slot={} body-slot={} candidates={} state={:08X} cached-before+5168={} mode+5108={} kind+32={} script+13932={} script-actor+10808={:08X} script-op+10904={} force+8436={:08X}/{} force-owner={:08X} force-victim={:08X} force-active+8432={} human-policy+8316={:.3f} interest-value+36={:.3f}",
      actor, Load32(b, vars+5100), before, after, target.generation, target.valid, target.source, target.target,
      body, body && target.valid && target.target == body, int32_t(Load32(b, vars+5228)), DevBodySlot(b, vars, body),
      Load32(b, vars+5236), state, cached, Load32(b, vars+5108), Load32(b, vars+32), Load32(b, vars+13932),
      Load32(b, vars+10808), Load32(b, vars+10904), force.id, force.generation, force.target,
      force.valid ? Load32(b, force.address+16) : 0,
      Load32(b, vars+8432), LoadF(b, vars+8316), target.valid ? LoadF(b, target.address+44) : 0.0f);
  DevRaptorCandidates(b, actor, vars, body);
}
#endif
void ObserveHumanConsumption(PPCContext& ctx, uint8_t* b) {
#if defined(KK_DEV_TOOLS)
  const bool observe = REXCVAR_GET(kk_coop_enabled) && Connected() && Load32(b, 0x82CCBF6C) == Load32(b, 0x82CCBF7C);
  const auto before = observe ? ReadHumanStatus(b) : std::nullopt;
#endif
  __imp__sub_826A0920(ctx, b);
#if defined(KK_DEV_TOOLS)
  const auto after = before ? ReadHumanStatus(b) : std::nullopt;
  static unsigned count = 0; static double next = 0;
  if (before && after && before->hero == after->hero && before->vars == after->vars &&
      count < 256 && (before->value != after->value || Now() >= next)) {
    ++count; next = Now()+1;
    REXLOG_INFO("BITE Jack-consume cell={:08X} before={:.3f} after={:.3f} condition={} flags={:08X} magnitude={:.3f} immunity={} hit-time={:.3f} recovery-delay={:.3f} native-queued={} processed={}",
        after->cell, before->value, after->value, uint8_t(after->condition), Load32(b, after->vars+3204),
        LoadF(b, after->vars+3228), Load32(b, after->vars+3232), LoadF(b, after->vars+11852),
        LoadF(b, after->vars+13288), Get().hits_native, Get().damage_replay.Last());
  }
#endif
  PublishPlayerStatus(b);
}
// A null reason means observation began. Keep every refusal distinguishable in
// dev diagnostics instead of treating an unobserved callback as a missed shot.
const char* BeginShot(const uint8_t* b) {
  auto& s = Get();
  if (!Connected()) return "disconnected";
  if (s.link.role != Role::Guest) return "not-guest";
  if (s.shooting) return "nested-callback";
  if (!Environment(b, s.world) || !PoseAddress(s.game, 13652)) return "world-not-ready";
  if (!JackReady(b)) return "jack-not-ready";
  const auto hero = Load32(b, 0x82CCBF7C), ai = Ptr(b, Ptr(b, hero, 24, 8), 4, 88);
  const auto vars = Ptr(b, ai, 64, 13652), matrix = Ptr(b, hero, 12, 64);
  if (!vars || !matrix) return "jack-components";
  if (Load32(b, 0x82CCBF6C) != hero || Load32(b, 0x82CCBF64) != ai) return "callback-not-jack";
  // H_callback_tir uses the same current weapon selector as cheats.cpp.
  const auto weapon = Load32(b, s.game+13128);
  if (weapon < 1 || weapon > 4) return "no-gun";
  if (s.next_shot == UINT32_MAX) return "sequence-exhausted";
  s.shot = {}; s.shot.world = s.identity; s.shot.number = ++s.next_shot;
  s.shot.weapon = uint8_t(weapon); s.shot.time = Now();
  s.shot_inventory = {s.identity, s.shot.time, uint8_t(weapon)};
  for (unsigned i = 0; i < 4; ++i) s.shot_inventory.ammo[i] = Load32(b, s.game+13432+i*72);
  s.shot.origin = Vector(b, matrix+48); s.shot.origin.z += 1.5f;
  const float x = -LoadF(b, matrix+16), y = -LoadF(b, matrix+20), length = std::hypot(x, y);
  if (!std::isfinite(length) || length < .01f) return "invalid-facing";
  s.shot_resolved = false;
#if defined(KK_DEV_TOOLS)
  s.shot_hit_object = 0; s.shot_matched = false; s.shot_hit_reason = "no-hit";
#endif
  s.shot.direction = {x/length, y/length, 0}; s.shooting = true; return nullptr;
}
#if defined(KK_DEV_TOOLS)
void DevShotHit(const uint8_t* b, uint32_t actor) {
  auto& s = Get(); s.shot_hit_object = actor; s.shot_matched = false;
  s.shot_hit_reason = actor ? "unmatched-object" : "miss";
  const auto it = s.bindings.find(actor);
  if (it == s.bindings.end() || !ValidBinding(b, it->second)) return;
  s.shot_matched = std::any_of(s.aliases.begin(), s.aliases.end(), [&](const auto& a) { return a.second == it->second.id; });
  if (s.shot_matched) s.shot_hit_reason = "matched-no-damage-event";
}
#endif
void ShotRay(const uint8_t* b, uint32_t origin, uint32_t point, uint32_t object, uint32_t caller) {
  auto& s = Get();
  // Only the three gun ray resolutions inside H_callback_tir establish a shot.
  // They run after native firing checks/ammo consumption, even on a miss (.47).
  if (caller != 0x82696D10 && caller != 0x8269727C && caller != 0x826977E8) return;
  if (!s.shooting || !PoseAddress(origin, 12) || !PoseAddress(point, 12)) return;
  // Resolver's r3/r4 output vectors: muzzle/camera origin and actual impact.
  auto copy = s.shot; copy.origin = Vector(b, origin); copy.direction = Direction(copy.origin, Vector(b, point));
  if (ValidShotIntent(copy)) {
    s.shot.origin = copy.origin; s.shot.direction = copy.direction; s.shot_resolved = true;
  }
#if defined(KK_DEV_TOOLS)
  if (PoseAddress(object)) DevShotHit(b, Load32(b, object));
#else
  (void)object;
#endif
}
bool GuestShotEvent(PPCContext& ctx, uint8_t* b) {
  auto& s = Get();
  if (!s.shooting || !Connected() || s.link.role != Role::Guest || !Environment(b, s.world) ||
      ctx.r6.u32 != Load32(b, 0x82CCBF7C)) return false;
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr != 0x82696E74 && lr != 0x82697494 && lr != 0x82697970 && lr != 0x82698C00) return false;
#if defined(KK_DEV_TOOLS)
  DevShotHit(b, ctx.r8.u32);
#endif
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
    if (ValidShotIntent(copy)) {
      s.shot = std::move(copy);
#if defined(KK_DEV_TOOLS)
      s.shot_hit_reason = duplicate ? "matched-duplicate" : "matched-hit-added";
#endif
    }
#if defined(KK_DEV_TOOLS)
    else s.shot_hit_reason = "matched-invalid-impact";
#endif
  }
#if defined(KK_DEV_TOOLS)
  else s.shot_hit_reason = "matched-stack-or-hit-limit";
#endif
  // Only the damage event is withheld. The normal callback keeps its muzzle,
  // sound, ammo, impact effects and animation paths. Invalid native handle.
  if (PoseAddress(ctx.r3.u32, 8)) { Store32(b, ctx.r3.u32, UINT32_MAX); Store32(b, ctx.r3.u32+4, 0); }
  return true;
}
const char* EndShot(const uint8_t* b, bool fired) {
  auto& s = Get(); s.shooting = false;
  if (!Connected() || !Environment(b, s.world)) return "world-changed";
  // A confirmed native gun ray is also evidence of firing. Do not require an
  // ammo delta to forward misses or a real shot whose ammo was replenished.
  if (!fired && !s.shot_resolved) return "no-native-shot";
  if (!ValidShotIntent(s.shot)) return "invalid-intent";
  if (!SubmitShotIntent(s.shot, s.shot_inventory)) { s.shots.Reject(ShotReject::Queue); return "queue-refused"; }
  return nullptr;
}
#if defined(KK_DEV_TOOLS)
void DevShotLog(const uint8_t* b, bool guest, bool observed, uint32_t weapon,
    uint32_t before, uint32_t after, uint32_t caller, uint32_t result, const char* reason) {
  static unsigned lines = 0;
  if (!guest || lines >= 128) return;
  ++lines;
  const auto& s = Get();
  REXLOG_INFO("KK dev guest fire callback: fn=82696A28 call={} t={:.3f} lr={:08X} weapon={} ammo={}/{} result={} observed={} ray={} hit-object={:08X} matched={} hits={} queued={} reason={} hit-reason={} hero={:08X} current={:08X} ai={:08X} cap=128",
      lines, kk::DevSecondsSinceMenu(), caller, weapon, before, after, result, observed,
      observed && s.shot_resolved, observed ? s.shot_hit_object : 0, observed && s.shot_matched,
      observed ? s.shot.hits.size() : size_t(0), reason == nullptr,
      reason ? reason : (s.shot.hits.empty() ? "queued-miss" : "queued-hit"), observed ? s.shot_hit_reason : "not-observed",
      Load32(b, 0x82CCBF7C), Load32(b, 0x82CCBF6C), Load32(b, 0x82CCBF64));
}
#endif
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
    auto reason = s.shot_validator.Validate(received, s.identity, s.link.partner, Now());
    std::vector<Binding> all;
    for (const auto& [actor, binding] : s.bindings) all.push_back(binding);
    std::vector<std::pair<Binding, ShotHit>> targets;
    bool late = false;
    if (reason == ShotReject::None) for (const auto& hit : shot.hits) {
      const auto* binding = Match(hit.id, all);
      Binding observed; EnemyState e;
      const bool valid = binding && ValidBinding(b, *binding);
      if (!valid || !Read(b, binding->actor, observed, e)) {
        // Only a confirmed death tombstone can turn a missing binding into late.
        const auto known = s.records.find(hit.id);
        if (known != s.records.end() && known->second.enemy.life <= 0 &&
            ValidateShotHit(shot, hit, known->second.enemy) == ShotReject::Late) { late = true; continue; }
#if defined(KK_DEV_TOOLS)
        if (s.shot_target_diagnostics++ < 64)
          REXLOG_INFO("ENEMIES shot target rejected number={} id={:016X}/{}/{} detail={} known={} life={:.2f} cap=64",
              shot.number, hit.id.source, hit.id.sequence, uint16_t(hit.id.family),
              !binding ? "missing-binding" : !valid ? "invalid-binding" : "unreadable-target",
              known != s.records.end(), known == s.records.end() ? -1.f : known->second.enemy.life);
#endif
        reason = ShotReject::Target; break;
      }
      e.id = binding->id;
      reason = ValidateShotHit(shot, hit, e);
      if (reason == ShotReject::Late) { late = true; reason = ShotReject::None; continue; }
      if (reason != ShotReject::None) break;
      targets.push_back({*binding, hit});
    }
    bool applied = false;
    if (reason == ShotReject::None) for (size_t i = 0; i < targets.size(); ++i) {
      if (!NativeShot(ctx, b, shot, targets[i].second, targets[i].first)) { reason = ShotReject::Engine; break; }
      applied = true;
    }
    if (applied) ++s.shots.applied; // queued normal native event; life outcome comes from Scan.
    if (reason != ShotReject::None) s.shots.Reject(reason);
    else if (late) s.shots.Reject(ShotReject::Late);
#if defined(KK_DEV_TOOLS)
    if (Now() >= s.next_shot_log) {
      s.next_shot_log = Now()+1;
      REXLOG_INFO("ENEMIES shot number={} weapon={} hits={} applied={} late={} reject={}",
          shot.number, shot.weapon, shot.hits.size(), applied, late, ShotRejectName(reason));
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
  if (s.link.role == Role::Guest) {
    Guest(ctx, b, false);
    if (PoseAddress(s.game, 13652) && JackReady(b)) {
      const auto weapon = Load32(b, s.game+13128);
      if (weapon <= 4) {
        InventoryState inventory{s.identity, Now(), uint8_t(weapon)};
        for (unsigned i = 0; i < 4; ++i) inventory.ammo[i] = Load32(b, s.game+13432+i*72);
        PublishInventoryState(inventory);
      }
    }
  }
  else HostShots(ctx, b);
  CombatBeforeAi(ctx, b);
#if defined(KK_DEV_TOOLS)
  DevBait(ctx, b);
#endif
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
  PublishPlayerStatus(b);
  s.counts.sent = s.link.sent; SetEnemyCounters(s.counts);
  SetShotCounters(s.shots);
#if defined(KK_DEV_TOOLS)
  if (Now() >= s.next_log) {
    s.next_log = Now()+5;
    REXLOG_INFO("ENEMIES status role={} sent={} matched={} unmatched={} driven={} tracked={}",
        s.link.role == Role::Host ? "host" : "guest", s.link.sent, s.counts.matched,
        s.counts.unmatched, s.counts.driven, s.records.size());
    REXLOG_INFO("ENEMIES error samples={} max={:.4f} avg={:.4f} shots-sent={} applied={} late={} rejected={} reason={}",
        s.error_count, s.error_max, s.error_count ? s.error_sum/double(s.error_count) : 0,
        s.link.shots_sent, s.shots.applied, s.shots.late, s.shots.rejected, ShotRejectName(s.shots.last));
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
  ClearCombat();
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
    s.pending_damage.clear(); s.sent_status.reset(); s.partner_status_time = -1;
    if (s.link.role == Role::Guest) {
      s.combat_epoch = s.incarnation = 0; s.damage_replay = {};
    }
    ClearCombat();
  }
}
bool EnemyCombatReference(const uint8_t* b, uint32_t owner, uint32_t ai, uint32_t vars, uint32_t offset) {
  auto& s = Get();
  if (!s.prepared || s.link.role != Role::Host || !Connected() || !Environment(b, s.world) ||
      Now()-s.prepared_at > .5 || !PoseAddress(vars, 6324)) return false;
  const auto binding = s.bindings.find(owner);
  return binding != s.bindings.end() && binding->second.id.family == EnemyFamily::Raptor &&
      binding->second.ai == ai && binding->second.vars == vars && ValidBinding(b, binding->second) &&
      combat::CandidateReference(offset, Load32(b, vars+5236));
}
bool EnemyPartnerCombatReady() {
  const auto& s = Get(); const auto& p = s.status_replay.Status();
  return s.link.role == Role::Host && s.prepared && Connected() && s.combat_epoch == s.link.epoch &&
      p.world == s.identity && p.incarnation && p.condition != PlayerCondition::Dead &&
      combat::Fresh(s.partner_status_time, Now()) && s.link.partner && s.link.partner->level == s.identity.level;
}
bool EnemyDamageEnqueue(PPCContext& ctx, uint8_t* b, uint32_t body) {
  auto& s = Get();
  if (!REXCVAR_GET(kk_coop_enabled) || ctx.r4.u32 != 4 || !PoseAddress(ctx.r6.u32, 108)) return false;
  const auto payload = ctx.r6.u32, source = ctx.r3.u32, victim = Load32(b, payload+4);
  bool matched_source = false;
  if (s.prepared && s.link.role == Role::Guest && Connected() && Environment(b, s.world) && Now()-s.prepared_at <= .5) {
    const auto it = s.bindings.find(source);
    if (it != s.bindings.end() && RetainedDamageSource(b, it->second))
      for (const auto& [host, local] : s.aliases) matched_source |= local == it->second.id;
  }
  const auto action = combat::FilterVictim(4, victim, body, Load32(b, 0x82CCBF7C),
      s.link.role == Role::Guest, matched_source, s.injecting_damage);
  if (action == combat::VictimAction::Pass) return false;
  if (action == combat::VictimAction::ProtectBody) {
    ++s.hits_produced;
    const auto binding = s.bindings.find(source);
    DamageEvent damage{s.identity, s.status_replay.Status().incarnation, s.next_damage+1, {},
        Load32(b, payload+88), Load32(b, payload+92),
        {LoadF(b, payload+24), LoadF(b, payload+28), LoadF(b, payload+32)}};
    damage.time = Now();
    if (binding != s.bindings.end()) damage.enemy = binding->second.id;
    const bool audited = binding != s.bindings.end() &&
        binding->second.id.family == EnemyFamily::Raptor && ValidBinding(b, binding->second) &&
        uint32_t(ctx.lr) == 0x8261DEEC && source == Load32(b, 0x82CCBF6C) &&
        combat::RaptorHit(damage.flags, damage.magnitude);
    const bool qualified = audited && EnemyPartnerCombatReady() && ValidDamageEvent(damage);
    bool queued = false;
    if (qualified) {
      Message m; m.type = MessageType::DamageEvent; m.epoch = s.combat_epoch; m.damage = damage;
      if (s.pending_damage.empty() && SubmitCombat(m, Now())) { queued = true; ++s.hits_queued; }
      else if (s.pending_damage.size() < combat::kQueueLimit) { s.pending_damage.push_back({m, Now()}); queued = true; }
      if (queued) ++s.next_damage;
    }
    if (!queued) ++s.hits_refused;
    if (!audited) kk::PartnerBodyTriggerReference(b, payload); // unknown reference still releases auto leases
#if defined(KK_DEV_TOOLS)
    static unsigned logged = 0;
    if (logged++ < 256) REXLOG_INFO("BITE host melee-enqueue source={:08X} victim={:08X} body={:08X} flags={:08X} magnitude={} qualified={} queued={} number={} body-protected=true produced={} sent={} refused={}",
        source, victim, body, damage.flags, damage.magnitude, qualified, queued, damage.number,
        s.hits_produced, s.hits_queued, s.hits_refused);
#endif
  }
#if defined(KK_DEV_TOOLS)
  else { static unsigned logged = 0; if (logged++ < 32) REXLOG_INFO("BITE guest local-duplicate suppressed source={:08X} victim={:08X}", source, victim); }
#endif
  // Mirror pool-full semantics: no new handle. Initialize the caller output to
  // invalid explicitly, rather than leaving a previous successful event there.
  if (PoseAddress(ctx.r8.u32, 8)) { Store32(b, ctx.r8.u32, UINT32_MAX); Store32(b, ctx.r8.u32+4, 0); }
  return true;
}
bool PartnerBodyGrabRefusal(const uint8_t* b, uint32_t victim) {
  // 82548BC0 explicitly returns zero without side effects for a null victim
  // (.28:19664 -> 82548E7C). Protect only the leased body with that native path.
  // Raptor LR8256D900 falls through to bites; Worm LR82533BA4 returns before
  // damage, and Scorpion LR825FBEAC skips attachment. No other shared combat
  // support is implied by refusing their attachment to an authored human.
  return victim && kk::PartnerBodyCombatActor(b) == victim;
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
  EnemyId best_id{}; uint32_t best_actor = 0;
  for (const auto& [actor, record] : s.driving) {
    const auto it = s.bindings.find(actor);
    const auto alias = s.aliases.find(record.enemy.id);
    if (it == s.bindings.end() || record.kind != EnemyEventKind::Spawned || record.enemy.life <= 0 ||
        alias == s.aliases.end() || alias->second != it->second.id || !ValidBinding(b, it->second)) continue;
    const auto m = it->second.matrix;
    ShotVector point = Vector(b, m+48);
    point.z += it->second.id.family == EnemyFamily::Raptor ? 1.2f : .4f;
    const float d = ShotDistance(origin, point);
    if (d > .1f && d < distance) { distance = d; best = point; best_id = record.enemy.id; best_actor = actor; }
  }
  static double next_log = 0;
  static unsigned lines = 0;
  if (best && Now() >= next_log && lines < 128) {
    next_log = Now()+1; ++lines;
    REXLOG_INFO("KK dev: face enemy target id={:016X}/{}/{} actor={:08X} matched=true distance={:.3f} pos=({:.3f},{:.3f},{:.3f})",
        best_id.source, best_id.sequence, uint16_t(best_id.family), best_actor, distance, best->x, best->y, best->z);
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
REX_HOOK_RAW(sub_826A0920) {
  kk::coop::ObserveHumanConsumption(ctx, base);
}
REX_HOOK_RAW(sub_82548BC0) {
  if (kk::coop::PartnerBodyGrabRefusal(base, ctx.r3.u32)) {
#if defined(KK_DEV_TOOLS)
    static unsigned count = 0;
    if (count++ < 32) REXLOG_INFO("BITE body grab refused victim={:08X} caller={:08X} raptor-bite-fallback={}",
        ctx.r3.u32, uint32_t(ctx.lr), uint32_t(ctx.lr) == 0x8256D900);
#endif
    ctx.r3.u64 = 0;
    __imp__sub_82548BC0(ctx, base); // native null-victim return, preserving native call semantics
    return;
  }
  __imp__sub_82548BC0(ctx, base);
}
#if defined(KK_DEV_TOOLS)
REX_HOOK_RAW(sub_82569A70) {
  kk::coop::DevRaptorVision(ctx, base);
}
REX_HOOK_RAW(sub_8245A7B8) {
  kk::coop::DevRaptorFilter(ctx, base);
}
REX_HOOK_RAW(sub_8246DC58) {
  kk::coop::DevRaptorRegister(ctx, base);
}
REX_HOOK_RAW(sub_82428020) {
  kk::coop::DevRaptorVisibility(ctx, base, true);
}
REX_HOOK_RAW(sub_82464680) {
  kk::coop::DevRaptorVisibility(ctx, base, false);
}
REX_HOOK_RAW(sub_8254A8F8) {
  kk::coop::DevRaptorSelection(ctx, base);
}
REX_HOOK_RAW(sub_8261DE68) {
  if (REXCVAR_GET(kk_coop_enabled) && kk::coop::EnemyConnectionReady()) {
    static unsigned count = 0;
    if (count++ < 256) REXLOG_INFO("BITE melee-builder lr={:08X} source={:08X} victim={:08X} flags={:08X} magnitude={}",
        uint32_t(ctx.lr), ctx.r6.u32, ctx.r8.u32, ctx.r5.u32, ctx.r9.u32);
  }
  __imp__sub_8261DE68(ctx, base);
}
#endif
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
  const char* reason = kk::coop::BeginShot(base);
  const bool observe = reason == nullptr;
  const auto game = kk::hfr::Load32(base, 0x82CC97C0);
  const auto weapon = kk::coop::PoseAddress(game, 13652) ? kk::hfr::Load32(base, game+13128) : 0;
  constexpr uint32_t ammo[] = {13432, 13504, 13576, 13648};
  const bool armed = weapon >= 1 && weapon <= 4;
  const auto before = armed ? kk::hfr::Load32(base, game+ammo[weapon-1]) : 0;
#if defined(KK_DEV_TOOLS)
  const bool guest = kk::coop::EnemyLinkSnapshot().role == kk::coop::Role::Guest;
  const auto caller = uint32_t(ctx.lr);
#endif
  __imp__sub_82696A28(ctx, base);
  const bool same = game == kk::hfr::Load32(base, 0x82CC97C0);
  const auto after = same && armed ? kk::hfr::Load32(base, game+ammo[weapon-1]) : before;
  if (observe) {
    reason = kk::coop::EndShot(base, same && after < before);
  }
#if defined(KK_DEV_TOOLS)
  kk::coop::DevShotLog(base, guest, observe, weapon, before, after, caller, ctx.r3.u32, reason);
#endif
}
REX_HOOK_RAW(sub_8243A3C0) {
  const auto origin = ctx.r3.u32, point = ctx.r4.u32, object = ctx.r6.u32, caller = uint32_t(ctx.lr);
  __imp__sub_8243A3C0(ctx, base);
  kk::coop::ShotRay(base, origin, point, object, caller);
}
REX_HOOK_RAW(sub_82634428) {
  if (!kk::coop::GuestShotEvent(ctx, base)) __imp__sub_82634428(ctx, base);
}
