// Per-enemy authority. Native memory is accessed on the game thread only.
#include "coop_enemies_game.h"
#include "coop_objects_game.h"
#include "coop_objects.h"
#include "coop_enemies.h"
#include "coop_ownership.h"
#include "coop_runtime.h"
#include "coop_pose.h"
#include "coop_animation_fire.h"
#include "hfr.h"
#include "settings.h"
#include "play_internal.h"
#include "photo_mode.h"
#include "coop_partner_body.h"
#include "coop_world_snapshot.h"
#include "coop_stimulus_debug.h"
#include "coop_event_budget.h"
#include "coop_rebirth.h"
#include <algorithm>
#include <bit>
#include <rex/logging.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

REX_EXTERN(__imp__sub_8271D498);
REX_EXTERN(__imp__sub_8271CCB8);
REX_EXTERN(__imp__sub_82715AC8);
REX_EXTERN(__imp__sub_82714E40);
REX_EXTERN(__imp__sub_826E1EE8);
REX_EXTERN(__imp__sub_826F8BC8);
REX_EXTERN(__imp__sub_827117E0);
REX_EXTERN(__imp__sub_82711278);
REX_EXTERN(__imp__sub_8270FCE0);
REX_EXTERN(__imp__sub_82401AA8);
REX_EXTERN(__imp__sub_82850100);
REX_EXTERN(__imp__sub_826FA3B0);
REX_EXTERN(__imp__sub_82696A28);
REX_EXTERN(__imp__sub_8243A3C0);
REX_EXTERN(__imp__sub_82634428);
REX_EXTERN(__imp__sub_82419C38);
REX_EXTERN(__imp__sub_8261DE68);
REX_EXTERN(__imp__sub_826A0920);
REX_EXTERN(__imp__sub_826C12C8);
REX_EXTERN(__imp__sub_826A1178);
REX_EXTERN(__imp__sub_820D2298);
REX_EXTERN(__imp__sub_82403E38);
REX_EXTERN(__imp__sub_82404490);
REX_EXTERN(__imp__sub_82425038);
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_826C4E98);
REX_EXTERN(__imp__sub_82451DB8);
REX_EXTERN(__imp__sub_8286A660);
REX_EXTERN(__imp__sub_82459498);
REX_EXTERN(__imp__sub_8270FA58);
#endif
REX_EXTERN(__imp__sub_82548BC0);
REX_EXTERN(__imp__sub_82538B70);
REX_EXTERN(__imp__sub_82634978);
REX_EXTERN(__imp__sub_8265BF60);
REX_EXTERN(__imp__sub_82463A58);
REX_EXTERN(__imp__sub_8266F740);
REX_EXTERN(__imp__sub_8266ACC8);
REX_EXTERN(__imp__sub_8260EB68);
REX_EXTERN(__imp__sub_8242DB60);
REX_EXTERN(__imp__sub_824509E0);
REX_EXTERN(__imp__sub_8244B9A0);
REX_EXTERN(__imp__sub_82443F10);
REX_EXTERN(__imp__sub_8243EB40);
REX_EXTERN(__imp__sub_8243ED78);
REX_EXTERN(__imp__sub_826131D8);
REX_EXTERN(__imp__sub_82613428);
REX_EXTERN(__imp__sub_825F2890);
REX_EXTERN(__imp__sub_82617858);
REX_EXTERN(__imp__sub_82456778);
REX_EXTERN(__imp__sub_8241A648);
REX_EXTERN(__imp__sub_824EF8A0);
REX_EXTERN(__imp__sub_824EF5B8);
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_824E4838);
REX_EXTERN(__imp__sub_824EDBC0);
#endif
REX_EXTERN(__imp__sub_82658ED8);
REX_EXTERN(__imp__sub_82652FF8);
REX_EXTERN(__imp__sub_824ECBC0);
REX_EXTERN(__imp__sub_8254A8F8);
REX_EXTERN(__imp__sub_825ED648);
REX_EXTERN(__imp__sub_825EDC70);
REX_EXTERN(__imp__sub_825043E0);
REX_EXTERN(__imp__sub_82572E40);
REX_EXTERN(__imp__sub_8255C9D8);
REX_EXTERN(__imp__sub_825CE478);
REX_EXTERN(__imp__sub_826627D0);
REX_EXTERN(__imp__sub_82626618);
REX_EXTERN(__imp__sub_8263F790);
REX_EXTERN(__imp__sub_82614DC0);
REX_EXTERN(__imp__sub_8271E760);
REX_EXTERN(__imp__sub_8271E968);
REX_EXTERN(__imp__sub_8271EBE8);
REX_EXTERN(__imp__sub_8271E4E8);
REX_EXTERN(__imp__sub_8262FE20);
REX_EXTERN(__imp__sub_825491B0);
REX_EXTERN(__imp__sub_8271ECF8);
REX_EXTERN(__imp__sub_826FB0A0);
REX_EXTERN(__imp__sub_826F9C60);
REX_EXTERN(__imp__sub_8271E450);
REX_EXTERN(__imp__sub_8271E290);
REX_EXTERN(__imp__sub_823FCF58);
REX_EXTERN(__imp__sub_823FCEB0);
REX_EXTERN(__imp__sub_8284D638);
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_82663550);
REX_EXTERN(__imp__sub_8241F0A8);
REX_EXTERN(__imp__sub_8266D880);
REX_EXTERN(__imp__sub_82670980);
REX_EXTERN(__imp__sub_826FB0A0);
REX_EXTERN(__imp__sub_826F9C60);
#endif
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_82451A60);
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
  uint32_t track_one = 0;
  for (uint32_t t = first; t <= last; ++t) {
    const auto block = Ptr(b, ai, (t+6)*4, 24);
    if (!block) continue;
    const auto fn = Load32(b, block+20);
    if (initializing && Initializer(fn)) *initializing = true;
    spawner |= fn == 0x826EB8C0 || fn == 0x826E1EE8 || fn == 0x826F8BC8;
    const auto f = Family(fn);
    if (f != EnemyFamily::None) {
      if (family != EnemyFamily::None && family != f) return EnemyFamily::None;
      family = f;
    }
    if (t == 1) track_one = fn;
    if (t == first+2) state = fn;
  }
  if (family == EnemyFamily::Pny) state = track_one;
  return family;
}
uint32_t LifeCell(const uint8_t* b, const Binding& binding) {
  const auto offset = LifeOffset(binding.id.family);
  if (!offset) return 0;
  if (binding.id.family == EnemyFamily::VRex)
    return StimulusLifeCell(binding.vars+offset,
        [&](uint32_t p) { return Load32(b,p); },
        [&](uint32_t p, uint32_t bytes) { return PoseAddress(p,bytes); });
  return PoseAddress(binding.vars,offset+4) ? binding.vars+offset : 0;
}
bool Read(const uint8_t* b, uint32_t actor, Binding& bind, EnemyState& e) {
  if (!PoseAddress(actor, 32) || actor == Load32(b, 0x82CCBF7C) || (Load32(b, actor)&0x00400001)) return false;
  bind.actor = actor;
  bind.ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
  bool spawner = false, initializing = false;
  const auto family = Tracks(b, bind.ai, e.state, spawner, &initializing);
  if (family == EnemyFamily::None || initializing) return false;
  bind.vars = Ptr(b, bind.ai, 64, LifeBytes(family)); bind.matrix = Ptr(b, actor, 12, 64);
  if (!bind.vars || !bind.matrix) return false;
  const auto hero = Load32(b, 0x82CCBF7C);
  const auto hero_ai = Ptr(b, Ptr(b, hero, 24, 8), 4, 88);
  if (bind.ai == hero_ai || bind.vars == Ptr(b, hero_ai, 64, 4) || bind.matrix == Ptr(b, hero, 12, 64)) return false;
  bind.id.family = e.id.family = family;
  e.x = LoadF(b, bind.matrix+48); e.y = LoadF(b, bind.matrix+52); e.z = LoadF(b, bind.matrix+56);
  const auto facing=NativeFacing(family,e.state,
      {LoadF(b,bind.matrix+16),LoadF(b,bind.matrix+20),LoadF(b,bind.matrix+24)},
      {LoadF(b,bind.matrix),LoadF(b,bind.matrix+4),LoadF(b,bind.matrix+8)});
  if (!facing) return false;
  e.facing=*facing;
  if (StateLife(family)) e.life = ReplicatedLife(family,e.state,0);
  else {
    const auto cell = LifeCell(b,bind); if (!cell) return false;
    e.life = ReplicatedLife(family,e.state,LoadF(b,cell));
  }
  if (const auto secondary = SecondaryLifeOffset(family)) {
    e.secondary_life = LoadF(b, bind.vars+secondary);
    // Either native pool can exhaust the actor. Keep the existing terminal
    // lifecycle contract while retaining both values for live handovers.
    if (e.secondary_life <= 0) e.life = 0;
  }
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
    case EnemyFamily::Crab: return "crab";
    case EnemyFamily::Bat: return "bat";
    case EnemyFamily::Centipede: return "centipede";
    case EnemyFamily::Native: return "native";
    case EnemyFamily::NativeKna: return "native-kna";
    case EnemyFamily::Marine: return "marine";
    case EnemyFamily::VRex: return "v-rex";
    case EnemyFamily::SwampCrawler: return "swamp-crawler";
    case EnemyFamily::SwampCrawlerSmall: return "small-swamp-crawler";
    case EnemyFamily::Spider: return "spider";
    case EnemyFamily::Pny: return "pny";
    case EnemyFamily::Prey: return "prey";
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
void DevWakeActivity(uint32_t actor, uint32_t active, const char* name) {
  auto& wake = Wake();
  if (!wake.enabled || (wake.phase != DevWake::Visiting && wake.phase != DevWake::Observing) ||
      wake.activity >= 32) return;
  ++wake.activity;
  REXLOG_INFO("KK dev wake: native {} actor={:08X} active={} selected-target={}",
      name, actor, active, actor == wake.target.actor);
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
    // The fixture names this authored scorpion, rather than whichever other
    // dormant family happens to be closest at the checkpoint.
    if (p.level==203 && Load32(b,game+12)==5) for (const auto& e:all)
      if (!e.scheduled && e.family==EnemyFamily::Scorpion && (e.initializing || e.life>0) &&
          ShotDistance(e.position,{51.110620f,132.907700f,19.930140f})<=.5f) nearest=&e;
    const bool named=nearest!=nullptr;
    for (const auto& e : all)
      if (!named && !e.scheduled && (e.initializing || e.life > 0) && e.distance <= 200 &&
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
  static const uint32_t family=[] {
    const auto* value=std::getenv("KK_DEV_ENEMY_NEAR_FAMILY");
    char* end=nullptr;
    const auto parsed=value ? std::strtoul(value,&end,10):0;
    return value && end!=value && !*end && parsed>=1 && parsed<=unsigned(EnemyFamily::NativeKna) ? uint32_t(parsed):0u;
  }();
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
    if (family && uint32_t(e.family)!=family) continue;
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
  uint32_t file_key = 0;
};
struct Pending {
  EnemyId id; uint32_t ai, matrix; float x, y, z;
  uint64_t birth_spawner=0; uint8_t birth_template=0,birth_point=0;
};
struct Spawner { uint64_t key; uint32_t ai, vars, sequence = 0; bool shared_births=false; };
const EnemyState* replay_birth=nullptr;
bool SharedSpawner(const uint8_t* b,uint32_t vars) {
  if (!PoseAddress(vars,0x2094)) return false;
  bool any=false,centipede=false;
  for (unsigned slot=0;slot<19;++slot) {
    const auto source=Load32(b,vars+(slot<2 ? 1592+184*slot : 1972+4*(slot-2)));
    if (!source) { if (slot>=2) break; else continue; }
    bool nested=false; uint32_t fn=0;
    const auto family=Tracks(b,Ptr(b,Ptr(b,source,24,8),4,88),fn,nested);
    if (nested || family==EnemyFamily::None) {
#if defined(KK_DEV_TOOLS)
      REXLOG_INFO("ENEMIES birth admission refused vars={:08X} slot={} template={:08X} state={:08X} nested={}",vars,slot,source,fn,nested);
#endif
      return false;
    }
    any=true; centipede|=family==EnemyFamily::Centipede;
  }
  return any && centipede;
}
struct RaptorPlayerCache { Binding binding; uint32_t hero = 0, reference = 0; };
#if defined(KK_DEV_TOOLS)
struct DevRaptorLog {
  double select_next = 0, vision_next = 0, filter_next = 0, register_next = 0, ray_next = 0, gate_next = 0;
  unsigned select_count = 0, vision_count = 0, filter_count = 0, register_count = 0, ray_count = 0, gate_count = 0;
};
#endif
void ShotSummary(const char* boundary, bool end = true);
struct State {
  uint32_t world = 0, game = 0;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> visits;
  bool capture = false, prepared = false, load_refused_logged = false;
  EnemyWorld identity;
  EnemyLink link;
  std::map<uint32_t, Authored> authored;
  std::map<uint32_t, Spawner> spawners;
  std::map<uint32_t, Binding> bindings;
  std::map<uint32_t, RaptorPlayerCache> raptor_players;
  struct NativePlayerChoice {
    uint32_t player=0; double until=0;
    uint32_t aim_player=0; double aim_time=0;
  };
  std::map<uint32_t,NativePlayerChoice> native_player_switch;
  std::map<uint32_t, Pending> pending;
  std::map<EnemyId, Record> records;
  std::map<EnemyId, Authority> authority; // Always indexed by canonical host ID.
  std::set<EnemyId> private_reseed;
  std::map<EnemyId, OwnershipChange> ownership_out, committed;
  std::map<EnemyId, double> matched_at;
  std::deque<Message> ownership_in;
  uint64_t ownership_epoch = 0;
  uint32_t rejoin_revision = 0;
  double next_ownership = 0;
  std::map<uint32_t, Record> driving;
  std::set<EnemyId> unmatched_logged;
  std::set<EnemyId> death_applied, death_refused;
  std::set<EnemyId> retirement_applied;
  std::set<uint32_t> orphan_logged;
  std::map<EnemyId, EnemyId> aliases; // host ID -> immutable local ID
  std::map<EnemyId, MatchRetry> retry;
  std::set<EnemyId> retry_diagnosed;
  ShotValidator shot_validator;
  ShotTargetHistory shot_targets;
  ShotCounters shots;
  uint64_t shot_submitted = 0, shot_accepted = 0, shot_blocked = 0;
  uint64_t shot_confirmed_deaths = 0;
  std::map<std::string, uint64_t> shot_send_reasons;
  bool shot_audit_active = false;
  double next_send_log = 0;
  double next_result_log = 0;
  double next_audit_log = 0;
  uint32_t next_shot = 0;
  uint64_t combat_epoch = 0, incarnation = 0;
  uint32_t next_damage = 0, next_status = 0;
  uint32_t combat_hero = 0, combat_ai = 0, combat_vars = 0;
  bool injecting_damage = false;
  combat::DamageReplay damage_replay;
  combat::StatusReplay status_replay;
  std::deque<ReceivedCombat> pending_damage;
  std::deque<ReceivedCombat> outgoing_damage;
  std::optional<PlayerStatus> sent_status;
  double next_status_time = 0, partner_status_time = -1;
  uint64_t hits_produced = 0, hits_queued = 0, hits_native = 0, hits_refused = 0;
  // Protected partner body hits that were not relayed, by combat::BodyHit.
  uint64_t body_enemy_here = 0, body_enemy_remote = 0, body_world = 0;
  uint64_t events_deferred = 0; bool events_paused = false; // C101
  bool shooting = false, shot_resolved = false;
  ShotIntent shot;
  InventoryState shot_inventory;
  struct PeerGrab {
    uint32_t actor=0,ai=0,vars=0,id=UINT32_MAX,generation=0;
    uint32_t client=0,enabled=0,proposal=0;
    Binding source{};
    uint64_t incarnation=0;
    uint32_t first_damage=0,ownership_generation=0;
    double started=0;
    bool experimental=false,client_ready=false;
  } peer_grab;
  struct LocalNativeGrab {
    Binding source{};
    uint32_t hero=0,ai=0,vars=0,id=UINT32_MAX,generation=0,ownership_generation=0,first_damage=0;
    uint64_t incarnation=0;
    bool ended=false;
    uint32_t observed_state=UINT32_MAX,observed_holder=UINT32_MAX,observed_attachment=UINT32_MAX;
  } local_native_grab;
  struct SentGrab {
    EnemyId enemy{}; uint64_t incarnation=0;
    uint32_t number=0,generation=0; bool active=false;
  } sent_grab;
  struct StingSound { uint32_t generation=0; double time=0; bool consumed=false; };
  std::map<EnemyId,StingSound> sting_sounds;
#if defined(KK_DEV_TOOLS)
  uint32_t shot_hit_object = 0;
  bool shot_matched = false;
  const char* shot_hit_reason = "no-hit";
  unsigned shot_target_diagnostics = 0;
  std::map<uint32_t, DevRaptorLog> raptor_logs;
  std::map<uint32_t,uint32_t> combat_targets;
  uint64_t targets_host = 0, targets_guest = 0, hits_host = 0;
  double next_targets = 0;
  std::map<uint32_t,uint32_t> diagnostic_keys;
  std::set<uint32_t> ambiguous_logged;
  unsigned raptor_candidate_lines = 0;
#endif
  double census_until = 0, next_numbers = 0, next_shot_log = 0;
  double error_sum = 0, error_max = 0;
  uint64_t error_count = 0;
  uint32_t unbound = 0;
  Counters counts;
  double next_log = 0, prepared_at = 0;
  void ResetWorld() {
    ShotSummary("world-end");
    world = game = 0; capture = prepared = false; identity = {};
    load_refused_logged = false;
    authored.clear(); spawners.clear(); bindings.clear(); raptor_players.clear(); native_player_switch.clear(); pending.clear(); records.clear(); driving.clear(); unmatched_logged.clear(); counts = {};
    death_applied.clear(); death_refused.clear(); retirement_applied.clear();
    orphan_logged.clear(); unbound = 0;
    aliases.clear(); retry.clear(); retry_diagnosed.clear(); shots = {}; shot_validator = {}; shot_targets = {}; shooting = shot_resolved = false; shot = {};
    shot_submitted = shot_accepted = shot_blocked = shot_confirmed_deaths = 0;
    shot_send_reasons.clear(); shot_audit_active = false; next_send_log = next_result_log = next_audit_log = 0;
    authority.clear(); ownership_out.clear(); committed.clear(); ownership_in.clear(); ownership_epoch = 0; next_ownership = 0;
    matched_at.clear();
    private_reseed.clear();
    census_until = next_numbers = next_log = next_shot_log = error_sum = error_max = 0; error_count = 0;
    combat_epoch = incarnation = 0; combat_hero = combat_ai = combat_vars = 0;
    next_damage = next_status = 0; pending_damage.clear(); outgoing_damage.clear(); sent_status.reset();
    damage_replay = {}; status_replay = {}; injecting_damage = false;
    next_status_time = 0; partner_status_time = -1;
    peer_grab={};
    local_native_grab={};
    sent_grab={};
    sting_sounds.clear();
#if defined(KK_DEV_TOOLS)
    raptor_logs.clear(); raptor_candidate_lines = 0;
    combat_targets.clear(); targets_host = targets_guest = hits_host = 0; next_targets = 0;
    diagnostic_keys.clear(); ambiguous_logged.clear();
#endif
  }
};
State& Get() { static State s; return s; }
void ShotSummary(const char* boundary, bool end) {
  auto& s = Get();
  if (!s.shot_audit_active) return;
  REXLOG_INFO("COOP SHOTS summary boundary={} role={} world={}/{}/{} submitted={} accepted={} native-queued={} blocked-local={} rejected={} late={} confirmed-deaths={}",
      boundary, s.link.role == Role::Host ? "host" : "guest", s.identity.level, s.identity.start, s.identity.serial,
      s.shot_submitted, s.shot_accepted, s.shots.applied, s.shot_blocked, s.shots.rejected, s.shots.late, s.shot_confirmed_deaths);
  for (const auto& [reason, total] : s.shot_send_reasons)
    REXLOG_INFO("COOP SHOTS send-reason={} total={}", reason, total);
  for (size_t i = 1; i < size_t(ShotReject::Count); ++i) if (s.shots.reasons[i])
    REXLOG_INFO("COOP SHOTS reject-reason={} total={}", ShotRejectName(ShotReject(i)), s.shots.reasons[i]);
  if (end) s.shot_audit_active = false;
}
void ShotSendLog(const char* reason, bool submitted = false) {
  auto& s = Get(); s.shot_audit_active = true;
  ++s.shot_send_reasons[reason];
  if (submitted) ++s.shot_submitted;
  const double now = Now();
  if (now < s.next_send_log) return;
  s.next_send_log = now+1;
  REXLOG_INFO("COOP SHOTS send role={} number={} weapon={} hits={} queued={} reason={} matched={} tracked={} remote-world={}/{}/{}",
      s.link.role == Role::Host ? "host" : "guest", s.shot.number, s.shot.weapon, s.shot.hits.size(), submitted,
      reason, s.aliases.size(), s.bindings.size(), s.link.remote.world.level, s.link.remote.world.start, s.link.remote.world.serial);
}
void ShotResultLog(const EnemyState& enemy, float before, bool replica) {
  auto& s = Get();
  if (before <= 0 || enemy.life == before) return;
  if (enemy.life <= 0) ++s.shot_confirmed_deaths;
  s.shot_audit_active = true;
  if (Now() < s.next_result_log && enemy.life > 0) return;
  s.next_result_log = Now()+1;
  REXLOG_INFO("COOP SHOTS result role={} source={} id={:016X}/{}/{} generation={} life={:.2f}->{:.2f} confirmed-death={}",
      replica ? "guest" : "host", replica ? "host-snapshot" : "native", enemy.id.source, enemy.id.sequence,
      uint16_t(enemy.id.family), enemy.generation, before, enemy.life, enemy.life <= 0);
}
EnemyOwner Side() { return Get().link.role == Role::Host ? EnemyOwner::Host : EnemyOwner::Guest; }
EnemyId Canonical(EnemyId local) {
  const auto& s = Get();
  if (s.link.role == Role::Host) return local;
  for (const auto& [host, id] : s.aliases) if (id == local) return host;
  return {};
}
Authority* AuthorityFor(EnemyId local) {
  auto& all = Get().authority; const auto it = all.find(Canonical(local));
  return it == all.end() ? nullptr : &it->second;
}
std::optional<EnemyWorld> private_identity;
struct GuardWorld { EnemyWorld world; uint64_t epoch=0; uint32_t generation=0; };
std::optional<GuardWorld> guard_world;
std::map<uint64_t,uint32_t> private_spawner_sequences;
// C107: living spawned identities of the world before a private reload, for its native rebirths.
RebirthAdoption private_rebirths;
unsigned private_rebirths_adopted=0;
struct PrivateSequences {
  uint32_t shot=0, damage=0;
  uint64_t epoch=0;
  ShotValidator shots;
  combat::StatusReplay status;
} private_sequences;
// C36 authority survives C37's private reload of the same logical world.
// Retain value identities and transactions only, never native actor bindings.
struct PrivateOwnership {
  uint64_t epoch = 0;
  std::map<EnemyId, EnemyId> aliases;
  std::map<EnemyId, Authority> authority;
  std::map<EnemyId, OwnershipChange> outgoing, committed;
  std::deque<Message> incoming;
} private_ownership;
bool Connected() { return EnemyConnectionReady(); }
bool Environment(const uint8_t* b, uint32_t w) {
  const auto& s = Get();
  return s.world == w && PoseAddress(s.game, 28) && Load32(b, 0x82CC97C0) == s.game &&
      !Load32(b, s.game+4) && Load32(b, s.game+0x18) == s.identity.level && !play::StartLevelLoading() && !photo::Active();
}
uint64_t KeyAt(const uint8_t* b, uint32_t actor, uint32_t family) {
  const auto m = Ptr(b, actor, 12, 64);
  if (!m) return 0;
  if (PoseAddress(actor,36)) if (const auto key=FileAuthoredKey(family,Load32(b,actor+32))) return key;
  return AuthoredKey(family, LoadF(b, m+48), LoadF(b, m+52), LoadF(b, m+56));
}
uint64_t TemplateIdentity(const uint8_t* b,uint32_t actor) {
  bool spawner=false; uint32_t state=0;
  const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88);
  const auto family=Tracks(b,ai,state,spawner);
  if (spawner || family==EnemyFamily::None) return 0;
  const auto key=KeyAt(b,actor,uint32_t(family));
  if (family!=EnemyFamily::Centipede) return key;
  const auto vars=Ptr(b,ai,64,4);
  return vars ? CentipedeAuthoredKey(key,Load32(b,vars)):0;
}
void Capture(const uint8_t* b, uint32_t w, bool initial = true) {
  auto& s = Get();
  // The full registry includes dormant actors before they enter the scheduled AI table.
  uint32_t begin, end; if (!Table(b, w, begin, end, true)) return;
  s.world = w; s.capture = false;
#if defined(KK_DEV_TOOLS)
  if (initial) s.diagnostic_keys=objects::FileKeys([&](uint32_t p){ return Load32(b,p); });
#endif
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
    auto key = KeyAt(b, actor, spawner ? 0x53504157u : uint32_t(family));
    if (spawner) {
      const auto vars=Ptr(b,ai,64,2040);
      uint64_t templates[19]{};
      if (vars) {
        templates[0]=TemplateIdentity(b,Load32(b,vars+1592));
        templates[1]=TemplateIdentity(b,Load32(b,vars+1776));
        for (unsigned i=0;i<17;++i) templates[i+2]=TemplateIdentity(b,Load32(b,vars+1972+4*i));
      }
      key=SpawnerAuthoredKey(key,templates);
#if defined(KK_DEV_TOOLS)
      if (initial) REXLOG_INFO("ENEMIES spawner definition actor={:08X} id={:016X} file-key={:08X} primary={:016X} secondary={:016X} first-wave={:016X}",
          actor,key,PoseAddress(actor,36) ? Load32(b,actor+32):0,templates[0],templates[1],templates[2]);
#endif
    }
    if (!spawner && family==EnemyFamily::Centipede) {
      const auto vars=Ptr(b,ai,64,4);
      key=vars ? CentipedeAuthoredKey(key,Load32(b,vars)):0;
    }
    if (!key || s.authored.contains(actor) || s.authored.size() >= kMaxEnemies) continue;
    const auto matrix = Ptr(b, actor, 12, 64);
    s.authored[actor] = {key, ai, matrix, family, LoadF(b, matrix+48), LoadF(b, matrix+52), LoadF(b, matrix+56)};
    s.authored[actor].file_key=PoseAddress(actor,36) ? ObjectFileKey(Load32(b,actor+32)):0;
#if defined(KK_DEV_TOOLS)
    const auto& a = s.authored[actor];
    REXLOG_INFO("ENEMIES census id={:016X}/0/{} spawn=({:.3f},{:.3f},{:.3f}) scheduled={} init={}",
        key, uint16_t(family), a.x, a.y, a.z, Member(b, w, actor), initializing);
#endif
    const auto vars = Ptr(b, ai, 64, 7808);
    if (!spawner || !vars) continue;
    Spawner sp{key, ai, vars, 0};
    sp.shared_births=SharedSpawner(b,vars);
#if defined(KK_DEV_TOOLS)
    if (initial) REXLOG_INFO("ENEMIES birth admission spawner={:016X} shared={}",key,sp.shared_births);
#endif
    if (const auto old=private_spawner_sequences.find(key); old!=private_spawner_sequences.end()) sp.sequence=old->second;
    if (sp.shared_births && !SessionFlowSnapshot().host) sp.sequence=0;
    s.spawners[actor] = std::move(sp);
  }
  // Co-located same-family actors or hash collisions are ambiguous. Refuse all
  // participants rather than use a heap address or a mutable table ordinal.
  std::map<uint64_t, unsigned> counts;
  for (const auto& [actor, a] : s.authored) ++counts[a.key];
  for (auto& [actor, a] : s.authored) a.ambiguous = counts[a.key] != 1;
#if defined(KK_DEV_TOOLS)
  for (const auto& [actor,a]:s.authored) if (a.ambiguous && a.family!=EnemyFamily::None && s.ambiguous_logged.insert(actor).second) {
    const auto v=Ptr(b,a.ai,64,64);
    const auto key=[&](uint32_t p){ const auto it=s.diagnostic_keys.find(p); return it==s.diagnostic_keys.end() ? 0:it->second; };
    REXLOG_INFO("ENEMIES ambiguous id={:016X} family={} actor={:08X} asset={:08X} model-key={:08X} type={:08X} vars0-16={:08X},{:08X},{:08X},{:08X},{:08X} vars48-56={:08X},{:08X},{:08X}",
        a.key,uint16_t(a.family),actor,key(actor),key(Load32(b,a.ai)),Load32(b,actor),
        v ? Load32(b,v):0,v ? Load32(b,v+4):0,v ? Load32(b,v+8):0,v ? Load32(b,v+12):0,v ? Load32(b,v+16):0,
        v ? Load32(b,v+48):0,v ? Load32(b,v+52):0,v ? Load32(b,v+56):0);
  }
#endif
  std::erase_if(s.spawners, [&](const auto& item) { return counts[item.second.key] != 1; });
  if (initial) REXLOG_INFO("ENEMIES identity level={} start={} serial={} authored={} spawners={}",
      s.identity.level, s.identity.start, s.identity.serial, s.authored.size(), s.spawners.size());
}
void Scan(const uint8_t* b) {
  auto& s = Get();
  // Retain known host actors through sleep/handover. Guest discovery still
  // includes dormant candidates; host discovery starts only when scheduled.
  uint32_t begin, end; if (!Table(b, s.world, begin, end, true)) return;
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
    if (!Read(b, actor, binding, e)) {
#if defined(KK_DEV_TOOLS)
      const auto authored=s.authored.find(actor);
      if (authored!=s.authored.end() && authored->second.family!=EnemyFamily::None) {
        bool spawner=false,initializing=false; uint32_t state=0;
        Tracks(b,authored->second.ai,state,spawner,&initializing);
        if (!initializing && s.orphan_logged.insert(actor).second) {
          const auto m=authored->second.matrix;
          REXLOG_INFO("ENEMIES native-read-refused id={:016X} family={} actor={:08X} state={:08X} forward=({},{},{}) right=({},{},{}) flags={:08X}",
              authored->second.key,uint16_t(authored->second.family),actor,state,
              LoadF(b,m+16),LoadF(b,m+20),LoadF(b,m+24),LoadF(b,m),LoadF(b,m+4),LoadF(b,m+8),Load32(b,actor));
        }
      }
#endif
      continue;
    }
    auto existing = s.bindings.find(actor);
    if (s.link.role == Role::Host && existing == s.bindings.end() && !Member(b, s.world, actor)) continue;
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
        e.birth_spawner=spawned->second.birth_spawner;
        e.birth_template=spawned->second.birth_template; e.birth_point=spawned->second.birth_point;
      } else {
        const auto it = s.authored.find(actor);
        if (it == s.authored.end() || it->second.ambiguous || it->second.ai != binding.ai || it->second.matrix != binding.matrix) {
          unbound(actor, e.id.family); continue;
        }
        binding.id = {it->second.key, 0, e.id.family};
        e.spawn_x = it->second.x; e.spawn_y = it->second.y; e.spawn_z = it->second.z;
      }
      if (s.records.contains(binding.id) || s.records.size() >= kMaxEnemies) { unbound(actor, e.id.family); continue; }
      // A recycled actor address is a new aiming history and aggro lease.
      s.native_player_switch.erase(actor);
      s.bindings[actor] = binding;
      s.pending.erase(actor);
    }
    e.id = binding.id; present.insert(e.id);
    auto it = s.records.find(e.id);
    if (it != s.records.end() && it->second.kind == EnemyEventKind::Removed) continue;
    if (it != s.records.end()) {
      e.spawn_x = it->second.enemy.spawn_x; e.spawn_y = it->second.enemy.spawn_y; e.spawn_z = it->second.enemy.spawn_z;
      e.birth_spawner=it->second.enemy.birth_spawner;
      e.birth_template=it->second.enemy.birth_template; e.birth_point=it->second.enemy.birth_point;
    }
    if (s.link.role == Role::Host) {
      s.aliases[e.id] = e.id;
      auto [a, inserted] = s.authority.try_emplace(e.id);
      if (inserted) a->second.last = e;
    }
    if (auto* a = AuthorityFor(binding.id)) {
      e.owner = a->owner; e.generation = a->generation;
      if (a->dead) e.life = 0;
    }
    if (s.link.role == Role::Host && it != s.records.end()) ShotResultLog(e, it->second.enemy.life, false);
    if (s.link.role == Role::Host) s.shot_targets.Observe(e, Now());
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
    const bool same = registered.contains(actor) &&
        Ptr(b, Ptr(b, actor, 24, 8), 4, 88) == binding.ai &&
        Ptr(b, binding.ai, 64, LifeBytes(binding.id.family)) == binding.vars &&
        Ptr(b, actor, 12, 64) == binding.matrix;
    if (present.contains(binding.id)) { ++binding_it; continue; }
    if (!same) {
      if (auto it = s.records.find(binding.id); it != s.records.end()) it->second.kind = EnemyEventKind::Removed;
      s.native_player_switch.erase(binding.actor);
      binding_it = s.bindings.erase(binding_it);
    } else ++binding_it;
  }
}
bool ValidBinding(const uint8_t* b, const Binding& binding) {
  Binding now; EnemyState e;
  return Member(b, Get().world, binding.actor, true) && Read(b, binding.actor, now, e) &&
      now.ai == binding.ai && now.vars == binding.vars && now.matrix == binding.matrix && e.id.family == binding.id.family;
}
bool RetainedDamageSource(const uint8_t* b, const Binding& source) {
  // Preserve the last committed attack of a corpse still in the full registry.
  // Removal, component reuse and a new initializer invalidate it immediately.
  bool initializing = false, spawner = false; uint32_t state = 0;
  return Member(b, Get().world, source.actor, true) && PoseAddress(source.actor, 32) &&
      !(Load32(b, source.actor)&1) && Ptr(b, Ptr(b, source.actor, 24, 8), 4, 88) == source.ai &&
      Ptr(b, source.ai, 64, LifeBytes(source.id.family)) == source.vars &&
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
const Binding* CombatOwner(const uint8_t* b, EnemyFamily family) {
  auto& s = Get();
  if (!EnemyPartnerCombatReady() || Now()-s.prepared_at > .5 || !Environment(b,s.world)) return nullptr;
  const auto actor = Load32(b,0x82CCBF6C), ai = Load32(b,0x82CCBF64);
  const auto it = s.bindings.find(actor);
  if (it == s.bindings.end() || it->second.ai != ai || it->second.id.family != family ||
      !ValidBinding(b,it->second)) return nullptr;
  const auto* a = AuthorityFor(it->second.id);
  return a && a->Runs(Side()) ? &it->second : nullptr;
}
float PlayerDistance(const uint8_t* b, uint32_t actor, uint32_t player) {
  const auto a = Ptr(b,actor,12,64), p = Ptr(b,player,12,64);
  if (!a || !p) return std::numeric_limits<float>::infinity();
  const float distance = std::hypot(LoadF(b,a+48)-LoadF(b,p+48), LoadF(b,a+52)-LoadF(b,p+52), LoadF(b,a+56)-LoadF(b,p+56));
  return std::isfinite(distance) ? distance : std::numeric_limits<float>::infinity();
}
uint32_t NearestPlayer(PPCContext& ctx, uint8_t* b, const Binding& owner, uint32_t original, bool crab) {
  const auto hero = Load32(b,0x82CCBF7C), body = kk::PartnerBodyCombatActor(b);
  if (!body || (original != hero && original != body) || !Scratch::CanUse(ctx)) return original;
  const auto other = original == hero ? body : hero;
  if (!(PlayerDistance(b,owner.actor,other) < PlayerDistance(b,owner.actor,original))) return original;
  Scratch scratch(ctx,b);
  ctx.r3.u64 = other;
  if (crab) __imp__sub_82463A58(ctx,b);
  else if (owner.id.family==EnemyFamily::NativeKna) __imp__sub_8244B9A0(ctx,b);
  else __imp__sub_8266ACC8(ctx,b);
#if defined(KK_DEV_TOOLS)
  static const bool trace=std::getenv("KK_DEV_PROJECTILE_TRACE")!=nullptr;
  static double next=0; static unsigned lines=0;
  if (trace && !crab && (owner.id.family==EnemyFamily::Native || owner.id.family==EnemyFamily::NativeKna) &&
      Now()>=next && lines++<64) {
    next=Now()+1;
    const auto matrix=Ptr(b,other,12,64);
    REXLOG_INFO("COOP PROJECTILE native-player-candidate actor={:08X} family={} candidate={:08X} partner={} accepted={} excluded={:08X} required={:08X} first-rejected={:08X} object-flags={:08X} pos=({:.3f},{:.3f},{:.3f})",
        owner.actor,uint16_t(owner.id.family),other,other==body,ctx.r3.u32,Load32(b,owner.vars+200),
        Load32(b,owner.vars+204),Load32(b,owner.vars+212),Load32(b,other+4),
        matrix ? LoadF(b,matrix+48):0,matrix ? LoadF(b,matrix+52):0,matrix ? LoadF(b,matrix+56):0);
  }
#endif
  return ctx.r3.u32 ? other : original;
}
// Refresh only the singular preferred-player index in the native selector.
// No candidate flag, visibility result, claim, forced target or attack is created.
void PlayerPriority(uint8_t* b, EnemyFamily family) {
  const auto* owner = CombatOwner(b,family); if (!owner) return;
  const auto hero = Load32(b,0x82CCBF7C), body = kk::PartnerBodyCombatActor(b);
  if (!body) return;
  const auto table = combat::Candidates(family);
  const auto vars = owner->vars;
  const auto count = Load32(b,vars+table.count);
  if (!table.limit || count > table.limit || !PoseAddress(vars,table.actors+4*table.limit)) return;
  uint32_t index = 0, kinds = 0, distance = 0, flags = 0;
  switch (family) {
    case EnemyFamily::Raptor: index=5224; kinds=5664; distance=5748; flags=5496; break;
    case EnemyFamily::Scorpion: index=3000; kinds=3440; distance=3608; flags=3272; break;
    case EnemyFamily::Spider: index=544; kinds=644; distance=728; flags=560; break;
    case EnemyFamily::Worm: index=2956; kinds=3392; distance=3644; flags=3224; break;
    default: return;
  }
  uint32_t selected = UINT32_MAX; float nearest = std::numeric_limits<float>::infinity();
  for (uint32_t i=0;i<count;++i) {
    const auto actor = Load32(b,vars+table.actors+4*i), bits = Load32(b,vars+flags+4*i);
    if ((actor != hero && actor != body) || Load32(b,vars+kinds+4*i) != 1) continue;
    if (family == EnemyFamily::Raptor && (!(bits&1) || (bits&0x4600) || !Load32(b,vars+5244+4*i) || !Load32(b,vars+5328+4*i))) continue;
    if (family == EnemyFamily::Scorpion && ((bits&0x06000402) || !Load32(b,vars+3104+4*i) || !Load32(b,vars+3020+4*i))) continue;
    if (family == EnemyFamily::Spider && (!(bits&0x10) || (bits&0x300))) continue;
    if (family == EnemyFamily::Worm && ((bits&0x140) || (!Load32(b,vars+2972+4*i) && !(bits&0x90)))) continue;
    const auto score = LoadF(b,vars+distance+4*i);
    if (std::isfinite(score) && score >= 0 && score < nearest) { nearest=score; selected=i; }
  }
  if (selected != UINT32_MAX) Store32(b,vars+index,selected);
}
thread_local uint32_t crab_player_scope = 0;
struct CrabFightGuard {
  uint8_t* base; Binding binding; uint32_t hero, previous;
  ~CrabFightGuard() {
    if (Ptr(base,binding.ai,64,2050)==binding.vars && RetainedDamageSource(base,binding))
      Store32(base,binding.vars+2040,hero);
    crab_player_scope=previous;
  }
};
uint32_t CrabSelectedPlayer(const uint8_t* b, const Binding& binding, uint32_t hero) {
  const auto body=kk::PartnerBodyCombatActor(b);
  const auto claim=stimulus_debug::ReadHandle([&](uint32_t p) { return Load32(b,p); },binding.vars+1048);
  return body && claim.valid && (claim.id>>24)==16 && claim.source==binding.actor && claim.target==body ? body : hero;
}
#if defined(KK_DEV_TOOLS)
// C111 evidence: why CR_ETAT_FIGHT waits (8265C5xx wait branch). Reads the
// same inputs the native strike gate reads, plus the native attacker count and
// the native strike ray, once a second per crab. Changes no crab state.
void DevCrabFightProbe(PPCContext& ctx, uint8_t* b) {
  static const bool trace=std::getenv("KK_DEV_CRAB_FIGHT_TRACE")!=nullptr;
  if (!trace || !Scratch::CanUse(ctx)) return;
  static std::map<uint32_t,double> next; static unsigned lines=0;
  const auto actor=Load32(b,0x82CCBF6C), ai=Load32(b,0x82CCBF64);
  const auto vars=Ptr(b,ai,64,4600);
  if (!vars || !PoseAddress(actor,32) || lines>=1500) return;
  if (next.size()>=64 && !next.contains(actor)) return;
  auto& due=next[actor]; if (Now()<due) return;
  due=Now()+1; ++lines;
  const auto hero=Load32(b,0x82CCBF7C), body=kk::PartnerBodyCombatActor(b);
  const auto target=Load32(b,vars+1056), cached=Load32(b,vars+2040), mouth=Load32(b,vars+2484);
  const auto own=Ptr(b,actor,12,64), tm=target ? Ptr(b,target,12,64) : 0;
  float dx=0,dy=0,dz=0;
  if (own && tm) { dx=LoadF(b,tm+48)-LoadF(b,own+48); dy=LoadF(b,tm+52)-LoadF(b,own+52); dz=LoadF(b,tm+56)-LoadF(b,own+56); }
  const auto anim=Ptr(b,Ptr(b,actor,20,4),0,164);
  uint32_t attackers=0, ray=0, child=0;
  if (target && PoseAddress(target,32)) {
    Scratch scratch(ctx,b);
    ctx.r3.u64=target; ctx.r4.u64=0; ctx.r5.u64=5; ctx.r6.u64=1; ctx.r7.u64=0;
    __imp__sub_82451DB8(ctx,b); attackers=ctx.r3.u32;
    ctx.r3.u64=target; ctx.r4.u64=0; __imp__sub_8286A660(ctx,b); child=ctx.r3.u32;
    const auto from=mouth ? Ptr(b,mouth,12,64) : 0, to=Ptr(b,child ? child:target,12,64);
    if (from && to) {
      const auto p=scratch.Vec(LoadF(b,from+48),LoadF(b,from+52),LoadF(b,from+56),0x100);
      const auto d=scratch.Vec(LoadF(b,to+48)-LoadF(b,from+48),LoadF(b,to+52)-LoadF(b,from+52),LoadF(b,to+56)-LoadF(b,from+56),0x110);
      ctx.r3.u64=actor; ctx.r4.u64=p; ctx.r5.u64=d; ctx.r6.u64=UINT32_MAX; ctx.r7.u64=2; ctx.r8.u64=0; ctx.r9.u64=1;
      __imp__sub_82403E38(ctx,b); ray=ctx.r3.u32;
    }
  }
  REXLOG_INFO("COOP CRAB fight actor={:08X} sub={} kind={} anim={} target={:08X} cached={:08X} hero={:08X} body={:08X} dist={:.2f} dz={:.2f} r1408={:.2f} r1412={:.2f} r1416={:.2f} f1460={:.2f} f1432={:.2f} i1092={} f1024={:.2f} f1124={:.2f} f3528={:.2f} i1068={} i276={} f280={:.2f} i1404={} i1044={} i1464={} attackers={} ray={:08X}{} child={:08X} mouth={:08X}",
      actor,Load32(b,vars+3496),Load32(b,vars+4),anim ? Load16(b,anim+156):0xFFFF,target,cached,hero,body,
      std::hypot(dx,dy),dz,LoadF(b,vars+1408),LoadF(b,vars+1412),LoadF(b,vars+1416),LoadF(b,vars+1460),LoadF(b,vars+1432),
      Load32(b,vars+1092),LoadF(b,vars+1024),LoadF(b,vars+1124),LoadF(b,vars+3528),int32_t(Load32(b,vars+1068)),
      Load32(b,vars+276),LoadF(b,vars+280),Load32(b,vars+1404),Load32(b,vars+1044),Load32(b,vars+1464),attackers,ray,
      ray==0 ? "" : ray==hero ? "(hero)" : body && ray==body ? "(body)" : ray==target ? "(target)" : ray==actor ? "(self)" : "",
      child,mouth);
  // fn_82463A58 candidate predicate for the local Jack, and its parts.
  uint32_t hero_ok=0, hero_tokens=0, hero_zone=0;
  if (hero && PoseAddress(hero,32)) {
    Scratch scratch(ctx,b);
    ctx.r3.u64=hero; __imp__sub_82463A58(ctx,b); hero_ok=ctx.r3.u32;
    ctx.r3.u64=hero; ctx.r4.u64=0; ctx.r5.u64=5; ctx.r6.u64=0; ctx.r7.u64=0;
    __imp__sub_82451DB8(ctx,b); hero_tokens=ctx.r3.u32;
    ctx.r3.u64=hero; ctx.r4.u64=Load32(b,vars+(Load32(b,vars+3532) ? 16:12)); __imp__sub_82459498(ctx,b); hero_zone=ctx.r3.u32;
  }
  REXLOG_INFO("COOP CRAB fight hero actor={:08X} hero={:08X} accepted={} kind5-tokens={} zone={} excluded={:08X} f216={:.2f} v224={:08X} v232={:08X} i3532={} v3512={:08X}",
      actor,hero,hero_ok,hero_tokens,hero_zone,Load32(b,vars+4424),LoadF(b,vars+216),Load32(b,vars+224),Load32(b,vars+232),
      Load32(b,vars+3532),Load32(b,vars+3512));
  if (hero_tokens) {
    const auto u32=[&](uint32_t p) { return Load32(b,p); };
    const auto pool=stimulus_debug::ReadPool(u32,16);
    for (uint32_t i=0;i<pool.count && i<512;++i) {
      const auto r=stimulus_debug::ReadSlot(u32,pool,i);
      bool names=false; for (uint32_t o=8;o<=40;o+=4) names|=Load32(b,r.address+o)==hero;
      if (!r.valid || !names) continue;
      REXLOG_INFO("COOP CRAB fight hero-record actor={:08X} slot={} gen={} flags={:08X} w8={:08X} w12={:08X} w16={:08X} w20={:08X} w24={:08X} w28={:08X} w32={:08X} w36={:08X} w40={:08X}",
          actor,i,r.generation,r.flags,Load32(b,r.address+8),Load32(b,r.address+12),Load32(b,r.address+16),Load32(b,r.address+20),
          Load32(b,r.address+24),Load32(b,r.address+28),Load32(b,r.address+32),Load32(b,r.address+36),Load32(b,r.address+40));
    }
  }
  // Facing gate (front_dot against F1432) and the turn rate the finish block uses.
  if (own && tm) {
    const float len=std::hypot(dx,dy);
    const float ux=len>0 ? dx/len:0, uy=len>0 ? dy/len:0;
    auto row=[&](uint32_t r) { const float x=LoadF(b,own+r), y=LoadF(b,own+r+4), l=std::hypot(x,y); return l>0 ? (x*ux+y*uy)/l : 0.f; };
    // Native forward (8270FA58) of the crab and of its target, as CR_ETAT_FIGHT reads them.
    float sf[3]{}, tf[3]{};
    {
      Scratch scratch(ctx,b);
      const auto out=scratch.Vec(0,0,0,0x160);
      ctx.r3.u64=actor; ctx.r4.u64=out; __imp__sub_8270FA58(ctx,b);
      for (int i=0;i<3;++i) sf[i]=LoadF(b,out+4*i);
      ctx.r3.u64=target; ctx.r4.u64=out; __imp__sub_8270FA58(ctx,b);
      for (int i=0;i<3;++i) tf[i]=LoadF(b,out+4*i);
    }
    auto hn=[](const float* v,float& x,float& y) { const float l=std::hypot(v[0],v[1]); x=l>0 ? v[0]/l:0; y=l>0 ? v[1]/l:0; };
    float sx,sy,tx,ty; hn(sf,sx,sy); hn(tf,tx,ty);
    REXLOG_INFO("COOP CRAB fight facing actor={:08X} front={:.3f} special-dot={:.3f} target-forward=({:.3f},{:.3f}) dot-row1={:.3f} f1260={:.3f} f1264={:.3f} f1316={:.3f} f1320={:.3f} f2564={:.3f} i1100={} i3488={}",
        actor,sx*ux+sy*uy,tx*ux+ty*uy,tx,ty,row(16),LoadF(b,vars+1260),LoadF(b,vars+1264),LoadF(b,vars+1316),LoadF(b,vars+1320),
        LoadF(b,vars+2564),Load32(b,vars+1100),Load32(b,vars+3488));
  }
  // fn_8244CC10 strike gate: the shared next strike time and its overrides.
  const auto w9998=Load32(b,0x82CC9998), w8a90=Load32(b,0x82CC8A90);
  REXLOG_INFO("COOP CRAB fight gate actor={:08X} now={:.3f} next9998={:.3f} next8a90={:.3f} same={} i3536={} i1152={} f1500={:.2f} f1492={:.2f} f1528={:.2f} f1248={:.2f} f1504={:.2f} i3524={} i1112={}",
      actor,LoadF(b,0x82CCAE1C)-LoadF(b,0x82CCC02C),PoseAddress(w9998,92308) ? LoadF(b,w9998+92304):-1.f,
      PoseAddress(w8a90,92308) ? LoadF(b,w8a90+92304):-1.f,w9998==w8a90,Load32(b,vars+3536),Load32(b,vars+1152),
      LoadF(b,vars+1500),LoadF(b,vars+1492),LoadF(b,vars+1528),LoadF(b,vars+1248),LoadF(b,vars+1504),
      Load32(b,vars+3524),Load32(b,vars+1112));
  if (attackers) {
    // List the type 16 records that name this target, to see which crab holds the strike.
    const auto u32=[&](uint32_t p) { return Load32(b,p); };
    const auto pool=stimulus_debug::ReadPool(u32,16);
    for (uint32_t i=0;i<pool.count && i<512;++i) {
      const auto r=stimulus_debug::ReadSlot(u32,pool,i);
      if (!r.valid || (r.source!=target && r.target!=target && Load32(b,r.address+16)!=target)) continue;
      REXLOG_INFO("COOP CRAB fight record actor={:08X} slot={} flags={:08X} w8={:08X} w12={:08X} w16={:08X} w20={:08X} w24={:08X} w28={:08X} w32={:08X} w36={:08X}",
          actor,i,r.flags,Load32(b,r.address+8),Load32(b,r.address+12),Load32(b,r.address+16),Load32(b,r.address+20),
          Load32(b,r.address+24),Load32(b,r.address+28),Load32(b,r.address+32),Load32(b,r.address+36));
    }
  }
}
#endif
void CrabFight(PPCContext& ctx, uint8_t* b) {
#if defined(KK_DEV_TOOLS)
  DevCrabFightProbe(ctx,b);
#endif
  const auto* owner=CombatOwner(b,EnemyFamily::Crab);
  const auto hero=Load32(b,0x82CCBF7C);
  if (!owner || Load32(b,owner->vars+2040)!=hero) { __imp__sub_8265BF60(ctx,b); return; }
  const auto binding=*owner;
  CrabFightGuard guard{b,binding,hero,crab_player_scope};
  crab_player_scope=binding.actor;
  // CR kind1 uses its cached Jack to choose the native melee branch rather
  // than an NPC grab. Only an already selected native partner claim aliases it.
  Store32(b,binding.vars+2040,CrabSelectedPlayer(b,binding,hero));
  __imp__sub_8265BF60(ctx,b);
}
void CrabPlayerTarget(PPCContext& ctx, uint8_t* b) {
  const auto* owner = CombatOwner(b,EnemyFamily::Crab);
  if (!owner) { __imp__sub_82634978(ctx,b); return; }
  const auto binding = *owner;
  const auto original = Load32(b,binding.vars+2040);
  const auto nearest = NearestPlayer(ctx,b,binding,original,true);
  Store32(b,binding.vars+2040,nearest);
  __imp__sub_82634978(ctx,b);
#if defined(KK_DEV_TOOLS)
  static std::map<uint32_t,std::pair<double,unsigned>> samples;
  if (samples.size()<32 || samples.contains(binding.actor)) {
    auto& sample=samples[binding.actor];
    if (sample.second<32 && Now()>=sample.first) {
      sample.first=Now()+2; ++sample.second;
      const auto body=kk::PartnerBodyCombatActor(b),hero=Load32(b,0x82CCBF7C);
      uint32_t body_live=0,hero_live=0;
      if (Scratch::CanUse(ctx)) {
        Scratch scratch(ctx,b);
        ctx.r3.u64=body; __imp__sub_82451A60(ctx,b); body_live=ctx.r3.u32;
        ctx.r3.u64=hero; __imp__sub_82451A60(ctx,b); hero_live=ctx.r3.u32;
      }
      REXLOG_INFO("COOP CRAB selector actor={:08X} original={:08X} preferred={:08X} target={:08X} candidates={} enabled={} default-enabled={} retained={} forbidden={:08X} cooldown={:.2f} body={:08X} distance={:.2f}",
          binding.actor,original,nearest,Load32(b,binding.vars+1056),Load32(b,binding.vars+1536),
          Load32(b,binding.vars+288),Load32(b,binding.vars+772),Load32(b,binding.vars+3496),
          Load32(b,binding.vars+4424),LoadF(b,binding.vars+1148),kk::PartnerBodyCombatActor(b),
          PlayerDistance(b,binding.actor,kk::PartnerBodyCombatActor(b)));
      REXLOG_INFO("COOP CRAB eligibility actor={:08X} body-live={} hero-live={} body-flags={:08X} hero-flags={:08X} required={:08X} delay={:.2f}",
          binding.actor,body_live,hero_live,body ? Load32(b,body+4) : 0,Load32(b,hero+4),
          Load32(b,binding.vars+(Load32(b,binding.vars+3532) ? 16 : 12)),LoadF(b,binding.vars+216));
    }
  }
#endif
  if (Ptr(b,binding.ai,64,2050) == binding.vars)
    Store32(b,binding.vars+2040,crab_player_scope==binding.actor ?
        CrabSelectedPlayer(b,binding,Load32(b,0x82CCBF7C)) : original);
}
void NativePlayerTarget(PPCContext& ctx, uint8_t* b, bool kna=false) {
  if (kna) __imp__sub_824509E0(ctx,b); else __imp__sub_8266F740(ctx,b);
  const auto* owner = CombatOwner(b,kna ? EnemyFamily::NativeKna:EnemyFamily::Native);
  if (owner) ctx.r3.u64 = NearestPlayer(ctx,b,*owner,ctx.r3.u32,false);
}
void BatPlayerTarget(PPCContext& ctx, uint8_t* b) {
  __imp__sub_824EF8A0(ctx,b);
  const auto* owner=CombatOwner(b,EnemyFamily::Bat);
  const auto hero=Load32(b,0x82CCBF7C),body=kk::PartnerBodyCombatActor(b);
  if (!ctx.r3.u32 || !owner || !body || !Scratch::CanUse(ctx)) return;
  const auto selected=Load32(b,owner->vars+848);
  if (selected!=hero && selected!=body) return;
  const auto other=selected==hero ? body:hero;
  const auto current_distance=PlayerDistance(b,owner->actor,selected),other_distance=PlayerDistance(b,owner->actor,other);
#if defined(KK_DEV_TOOLS)
  static const bool trace=std::getenv("KK_DEV_BAT_CHOICES")!=nullptr;
  static unsigned lines=0;
  if (!(other_distance<current_distance) && trace && lines++<64)
    REXLOG_INFO("COOP BAT choice actor={:08X} selected={:08X} other={:08X} body={:08X} distance-current={:.3f} distance-other={:.3f} closer=false",
        owner->actor,selected,other,body,current_distance,other_distance);
#endif
  if (!(other_distance<current_distance)) return;
  // Preserve the selector's activation, authored conditions and cooldowns.
  // Only replace an accepted player with the nearer player if the same native
  // life, exclusion and territory predicate accepts that player too.
  Scratch scratch(ctx,b);
  ctx.r3.u64=other; __imp__sub_824EF5B8(ctx,b);
  const auto accepted=ctx.r3.u32;
#if defined(KK_DEV_TOOLS)
  if (trace && lines++<64) {
    ctx.r3.u64=other; __imp__sub_824E4838(ctx,b); const auto live=ctx.r3.u32;
    ctx.r3.u64=other; ctx.r4.u64=0; __imp__sub_824EDBC0(ctx,b); const auto territory=ctx.r3.u32;
    REXLOG_INFO("COOP BAT choice actor={:08X} selected={:08X} other={:08X} body={:08X} distance-current={:.3f} distance-other={:.3f} closer=true accepted={} live={} territory={} flags6={:04X} excluded={:08X} included-bv={:08X} excluded-bv={:08X}",
        owner->actor,selected,other,body,current_distance,other_distance,accepted,live,territory,
        Load16(b,other+6),Load32(b,owner->vars+284),Load32(b,owner->vars+24),Load32(b,owner->vars+28));
  }
#endif
  if (accepted) Store32(b,owner->vars+848,other);
}
thread_local uint32_t bat_contact_actor=0;
void BatPlayerFight(PPCContext& ctx, uint8_t* b, void (*native)(PPCContext&,uint8_t*)) {
  const auto* owner=CombatOwner(b,EnemyFamily::Bat);
  const auto hero=Load32(b,0x82CCBF7C),body=kk::PartnerBodyCombatActor(b);
  if (!owner || !body || Load32(b,owner->vars+848)!=body || Load32(b,owner->vars+3108)!=hero) {
    native(ctx,b); return;
  }
  const auto binding=*owner;
  // Approach chooses strike880=1 for its cached player. Contact's aim helper
  // uses the same equality. Scope the cache to these calls for a selected peer. Native
  // contact, animation, flags and damage stay unchanged.
  struct Restore {
    uint8_t* base; Binding binding; uint32_t hero;
    ~Restore() {
      if (Ptr(base,binding.ai,64,3112)==binding.vars && RetainedDamageSource(base,binding))
        Store32(base,binding.vars+3108,hero);
    }
  } restore{b,binding,hero};
  struct ContactScope {
    uint32_t previous;
    ~ContactScope() { bat_contact_actor=previous; }
  } contact_scope{bat_contact_actor};
  if (native==__imp__sub_82652FF8) bat_contact_actor=binding.actor;
  Store32(b,binding.vars+3108,body);
#if defined(KK_DEV_TOOLS)
  const bool entry=native==__imp__sub_82658ED8 && Load32(b,binding.vars+3084)!=17;
#endif
  native(ctx,b);
#if defined(KK_DEV_TOOLS)
  static unsigned lines=0;
  if (entry && lines++<64 && RetainedDamageSource(b,binding))
    REXLOG_INFO("COOP BAT player-approach actor={:08X} target={:08X} attack={} flags={:08X}",
        binding.actor,Load32(b,binding.vars+848),Load32(b,binding.vars+880),Load32(b,binding.vars+928));
#endif
}
void BatNpcLink(PPCContext& ctx,uint8_t* b) {
  // TAG_On deliberately excludes Jack from its NPC bookkeeping. Apply that
  // same exclusion to the validated peer at its held/selected-target calls,
  // leaving the separate carried-prey call and all other users unchanged.
  const auto caller=uint32_t(ctx.lr);
  if (caller==0x8263D6C0 || caller==0x8263D708) {
    const auto* owner=CombatOwner(b,EnemyFamily::Bat);
    const auto body=kk::PartnerBodyCombatActor(b);
    if (owner && body && ctx.r3.u32==body) return;
  }
  __imp__sub_824ECBC0(ctx,b);
}
void NativePlayerReflex(PPCContext& ctx, uint8_t* b, bool kna=false) {
  if (kna) __imp__sub_8242DB60(ctx,b); else __imp__sub_8260EB68(ctx,b);
  const auto* owner=CombatOwner(b,kna ? EnemyFamily::NativeKna:EnemyFamily::Native);
  const uint32_t target=kna ? 4084:4088,scripted_target=kna ? 304:308;
#if defined(KK_DEV_TOOLS)
  if (owner) {
    static std::map<uint32_t,std::pair<double,unsigned>> logs;
    if (logs.size()<32 || logs.contains(owner->actor)) {
      auto& log=logs[owner->actor];
      if (Now()>=log.first && log.second++<40) {
        log.first=Now()+2;
        REXLOG_INFO("COOP PROJECTILE native-reflex actor={:08X} target={:08X} body={:08X} scripted={:08X} required={:08X} excluded={:08X} state={} phase={} script-step={} launcher={:08X} distance-host={:.3f} distance-guest={:.3f} variant={}",
            owner->actor,Load32(b,owner->vars+target),kk::PartnerBodyCombatActor(b),Load32(b,owner->vars+scripted_target),
            Load32(b,owner->vars+204),Load32(b,owner->vars+212),Load32(b,owner->vars+252),Load32(b,owner->vars+264),
            Load32(b,owner->vars+(kna ? 400:404)),Load32(b,owner->vars+(kna ? 4092:4096)),
            PlayerDistance(b,owner->actor,Load32(b,0x82CCBF7C)),PlayerDistance(b,owner->actor,kk::PartnerBodyCombatActor(b)),kna ? "KNa":"Na");
      }
    }
  }
#endif
  if (!owner) return;
  const auto hero=Load32(b,0x82CCBF7C),body=kk::PartnerBodyCombatActor(b),scripted=Load32(b,owner->vars+scripted_target);
  if (!body || (scripted && scripted!=hero && scripted!=body)) return;
  auto& choice=Get().native_player_switch[owner->actor];
  const auto original=Load32(b,owner->vars+target);
  if (original!=hero && original!=body) return;
  uint32_t selected=original;
  if (Now()<choice.until && (choice.player==hero || choice.player==body) && Scratch::CanUse(ctx)) {
    Scratch scratch(ctx,b); ctx.r3.u64=choice.player;
    if (kna) __imp__sub_8244B9A0(ctx,b); else __imp__sub_8266ACC8(ctx,b);
    if (ctx.r3.u32) selected=choice.player;
  } else selected=NearestPlayer(ctx,b,*owner,original,false);
  if (selected!=original) {
    Store32(b,owner->vars+target,selected);
#if defined(KK_DEV_TOOLS)
    static unsigned switches=0;
    if (switches++<128) REXLOG_INFO("COOP PROJECTILE player-switch actor={:08X} from={:08X} to={:08X} partner={}",
        owner->actor,original,selected,selected==body);
#endif
    // A retained valid target otherwise prevents the native selector from
    // running again. Keep four seconds of aggro and the native volume test.
    if (choice.player!=selected) { choice.player=selected; choice.until=Now()+4; }
  }
}
void NativePlayerAuthoredState(PPCContext& ctx,uint8_t* b,bool kna,void (*native)(PPCContext&,uint8_t*)) {
  const auto* owner=CombatOwner(b,kna ? EnemyFamily::NativeKna:EnemyFamily::Native);
  const auto hero=Load32(b,0x82CCBF7C),body=kk::PartnerBodyCombatActor(b);
  const uint32_t offset=kna ? 304:308;
  const auto scripted=owner ? Load32(b,owner->vars+offset):0;
  const auto selected=owner ? Load32(b,owner->vars+(kna ? 4084:4088)):0;
  if (!owner || !combat::NativePlayerScriptTarget(scripted,selected,hero,body)) {
    native(ctx,b); return;
  }
  const auto binding=*owner;
  // Wait, move and launch reassert their authored player every frame. Keep
  // their already selected player through the native call without changing
  // waypoints, state transitions, timing, eligibility or the story record.
  struct Restore {
    uint8_t* base; Binding binding; uint32_t offset,bytes,original,alias;
    ~Restore() {
      if (Ptr(base,binding.ai,64,bytes)==binding.vars && RetainedDamageSource(base,binding) &&
          Load32(base,binding.vars+offset)==alias)
        Store32(base,binding.vars+offset,original);
    }
  } restore{b,binding,offset,kna ? 4532u:4492u,scripted,selected};
  Store32(b,binding.vars+offset,selected);
  native(ctx,b);
#if defined(KK_DEV_TOOLS)
  static unsigned lines=0;
  if (scripted!=selected && lines++<64 && RetainedDamageSource(b,binding))
    REXLOG_INFO("COOP PROJECTILE authored-player actor={:08X} family={} authored={:08X} selected={:08X} retained={:08X} native-state-target=true",
        binding.actor,uint16_t(binding.id.family),scripted,selected,Load32(b,binding.vars+(kna ? 4084:4088)));
#endif
}
void NativePlayerLaunch(PPCContext& ctx,uint8_t* b) {
  const auto* owner=CombatOwner(b,EnemyFamily::Native);
  const auto hero=Load32(b,0x82CCBF7C),body=kk::PartnerBodyCombatActor(b);
  const auto scripted=owner ? Load32(b,owner->vars+308):0;
  const auto selected=owner ? Load32(b,owner->vars+4088):0;
  if (owner && body && (!scripted || scripted==hero || scripted==body) && (selected==hero || selected==body)) {
    auto& choice=Get().native_player_switch[owner->actor];
    // Na_Exec_Lance leads from the preceding launch frame's sample at4472.
    // The authored target can be reasserted before every reflex, so compare
    // actual consecutive launch targets rather than counting those rewrites
    // as movement. Drop the sample after a readiness or state gap too.
    if (choice.aim_player!=selected || Now()-choice.aim_time>.5) {
      if (const auto matrix=Ptr(b,selected,12,64)) {
        StoreF(b,owner->vars+4472,LoadF(b,matrix+48));
        StoreF(b,owner->vars+4476,LoadF(b,matrix+52));
        StoreF(b,owner->vars+4480,LoadF(b,matrix+56));
      }
    }
    choice.aim_player=selected; choice.aim_time=Now();
  }
  NativePlayerAuthoredState(ctx,b,false,__imp__sub_82617858);
}
void VRexPlayerTarget(PPCContext& ctx,uint8_t* b) {
  __imp__sub_82456778(ctx,b);
  const auto* owner=CombatOwner(b,EnemyFamily::VRex);
  const auto hero=Load32(b,0x82CCBF7C),body=kk::PartnerBodyCombatActor(b);
  if (!owner || !body || !Scratch::CanUse(ctx)) return;
  const auto original=Load32(b,owner->vars+516);
  // The native selector must first name a player. Kong/Ann, finishers and
  // other authored actors keep their interaction protocols and cached refs.
  if (original!=hero && original!=body) return;
  const auto other=original==hero ? body:hero;
  if (!(PlayerDistance(b,owner->actor,other)<PlayerDistance(b,owner->actor,original))) return;
  const auto matrix=Ptr(b,other,12,64); if (!matrix) return;
  const ShotVector position{LoadF(b,matrix+48),LoadF(b,matrix+52),LoadF(b,matrix+56)};
  Scratch scratch(ctx,b);
  ctx.r3.u64=(uint64_t(std::bit_cast<uint32_t>(position.x))<<32)|std::bit_cast<uint32_t>(position.y);
  ctx.r4.u64=uint64_t(std::bit_cast<uint32_t>(position.z))<<32;
  __imp__sub_8241A648(ctx,b); // native three exclusions and required attack volume
  if (ctx.r3.u32) Store32(b,owner->vars+516,other);
}
void RestoreRaptorPlayers(uint8_t* b) {
  // Reflex has early reads before it refreshes the single-player caches. Start
  // every native AI pass with its own Jack references, including after loss.
  auto& s=Get();
  for (const auto& [actor,cache] : s.raptor_players) {
    const auto current=s.bindings.find(actor);
    if (current==s.bindings.end() || current->second.id!=cache.binding.id ||
        cache.hero!=Load32(b,0x82CCBF7C) || !Environment(b,s.world) || !RetainedDamageSource(b,cache.binding)) continue;
    Store32(b,cache.binding.vars+5100,cache.hero);
    Store32(b,cache.binding.vars+14192,cache.reference);
    const auto distance=PlayerDistance(b,actor,cache.hero);
    if (std::isfinite(distance)) StoreF(b,cache.binding.vars+5104,distance);
  }
}
void RaptorPlayer(uint8_t* b, bool capture) {
  const auto* owner=CombatOwner(b,EnemyFamily::Raptor); if (!owner || !PoseAddress(owner->vars,14200)) return;
  auto& s=Get(); const auto hero=Load32(b,0x82CCBF7C);
  if (capture && Load32(b,owner->vars+5100)==hero && PoseAddress(Load32(b,owner->vars+14192),32))
    s.raptor_players[owner->actor]={*owner,hero,Load32(b,owner->vars+14192)};
  const auto it=s.raptor_players.find(owner->actor);
  if (it==s.raptor_players.end() || it->second.hero!=hero || it->second.binding.ai!=owner->ai ||
      it->second.binding.vars!=owner->vars || it->second.binding.matrix!=owner->matrix) return;
  const auto body=kk::PartnerBodyCombatActor(b);
  const auto claim=stimulus_debug::ReadHandle([&](uint32_t p) { return Load32(b,p); },owner->vars+5216);
  const bool partner=body && claim.valid && (claim.id>>24)==16 && claim.source==owner->actor && claim.target==body;
  const auto player=partner ? body : hero;
  // FIGHT compares against5100. Test-bite and neck alignment aim at14192 for
  // classification1. Both must follow the actual selected player, otherwise
  // the partner is selected but the native bite still aims at local Jack.
  Store32(b,owner->vars+5100,player);
  Store32(b,owner->vars+14192,partner ? body : it->second.reference);
  const auto distance=PlayerDistance(b,owner->actor,player);
  if (std::isfinite(distance)) StoreF(b,owner->vars+5104,distance);
}
void RaptorViewPosition(PPCContext& ctx,uint8_t* b,uint32_t actor,uint32_t out,uint32_t caller) {
  const auto* owner=CombatOwner(b,EnemyFamily::Raptor);
  const auto body=kk::PartnerBodyCombatActor(b);
  auto& s=Get();
  if (!owner || !body || actor!=body || !PoseAddress(out,12) || !s.link.partner ||
      Load32(b,owner->vars+32)!=16 || Load32(b,owner->vars+14192)!=body || !Scratch::CanUse(ctx)) return;
  const auto cached=s.raptor_players.find(owner->actor);
  const auto hero=Load32(b,0x82CCBF7C);
  if (cached==s.raptor_players.end() || cached->second.hero!=hero || cached->second.binding.id!=owner->id ||
      cached->second.binding.ai!=owner->ai || cached->second.binding.vars!=owner->vars ||
      cached->second.binding.matrix!=owner->matrix || !PoseAddress(cached->second.reference,32) ||
      !Ptr(b,cached->second.reference,12,64)) return;
  const auto claim=stimulus_debug::ReadHandle([&](uint32_t p){return Load32(b,p);},owner->vars+5216);
  const auto slot=Load32(b,owner->vars+5228),count=Load32(b,owner->vars+5236);
  if (!claim.valid || (claim.id>>24)!=16 || claim.source!=owner->actor || claim.target!=body || count>20 ||
      slot>=count || Load32(b,owner->vars+6244+4*slot)!=body || Load32(b,owner->vars+5664+4*slot)!=1) return;
  const auto local=ReadJackPose([&](uint32_t p){return Load32(b,p);},[&](uint32_t p){return Load16(b,p);},
      [&](uint32_t p){return LoadF(b,p);},Now());
  if (!local.pose) return;
  ShotVector view;
  {
    Scratch scratch(ctx,b); const auto point=scratch.Vec(0,0,0,0x100);
    ctx.r3.u64=cached->second.reference; ctx.r4.u64=point; __imp__sub_8270FCE0(ctx,b);
    view={LoadF(b,point),LoadF(b,point+4),LoadF(b,point+8)};
  }
  const auto height=combat::RaptorViewHeight(owner->id.family,Load32(b,owner->vars+32),caller,
      {local.pose->x,local.pose->y,local.pose->z},view,local.pose->crouched,s.link.partner->crouched);
  if (!height || !std::isfinite(LoadF(b,out+8))) return;
  StoreF(b,out+8,LoadF(b,out+8)+*height);
#if defined(KK_DEV_TOOLS)
  static unsigned lines=0;
  if (lines++<64) REXLOG_INFO("COOP TARGET native-view actor={:08X} body={:08X} reference={:08X} caller={:08X} height={:.3f} native-camera-offset=true physical-target-unchanged=true",
      owner->actor,body,cached->second.reference,caller,*height);
#endif
}
void ContactVictim(PPCContext& ctx, const uint8_t* b) {
  // 82638B30 has already tested contact with *r29, but its kind-1 branch
  // passes the singular Jack cache at vars+6112. Keep the actual contact
  // victim when a second class-1 player is present. Never redirect by proximity.
  if (uint32_t(ctx.lr) != 0x82638CE0 || !REXCVAR_GET(kk_coop_enabled)) return;
  const auto& s = Get();
  if (!s.prepared || !Connected() || !Environment(b,s.world) || Now()-s.prepared_at>.5) return;
  const auto it=s.bindings.find(ctx.r6.u32);
  if (it==s.bindings.end() || it->second.id.family!=EnemyFamily::Centipede ||
      it->second.vars!=ctx.r30.u32 || !ValidBinding(b,it->second)) return;
  const auto* a=AuthorityFor(it->second.id);
  if (!a || !a->Runs(Side())) return;
  const auto vars=it->second.vars, slot=ctx.r29.u32-vars;
  if (!PoseAddress(vars,7440) || !combat::CandidateReference(combat::Candidates(EnemyFamily::Centipede),slot,Load32(b,vars+1400))) return;
  const auto index=(slot-3236)/4, victim=Load32(b,vars+slot);
  if (victim && PoseAddress(victim,32) && Load32(b,vars+2020+4*index)==1 && ctx.r8.u32==Load32(b,vars+6112)) ctx.r8.u64=victim;
}
uint32_t PlayerDamageCache(EnemyFamily family) {
  switch (family) {
    case EnemyFamily::Raptor: return 5100;
    case EnemyFamily::Crab: return 2040;
    case EnemyFamily::Bat: return 3108;
    case EnemyFamily::Scorpion: return 2972;
    default: return 0;
  }
}
struct PlayerDamageFrame { Binding binding; uint32_t offset=0, hero=0, body=0, pool=4; };
thread_local std::optional<PlayerDamageFrame> player_damage_frame;
struct PlayerDamageGuard {
  uint8_t* base; PlayerDamageFrame frame; uint32_t original;
  std::optional<PlayerDamageFrame> previous;
  ~PlayerDamageGuard() {
    if (Ptr(base,frame.binding.ai,64,frame.offset+4)==frame.binding.vars && RetainedDamageSource(base,frame.binding))
      Store32(base,frame.binding.vars+frame.offset,original);
    player_damage_frame=previous;
  }
};
void PlayerDamage(PPCContext& ctx, uint8_t* b, EnemyFamily family, void (*native)(PPCContext&,uint8_t*), uint32_t pool=4) {
  const auto* owner=CombatOwner(b,family);
  const auto hero=Load32(b,0x82CCBF7C),body=kk::PartnerBodyCombatActor(b),offset=PlayerDamageCache(family);
#if defined(KK_DEV_TOOLS)
  static unsigned frame_logs=0;
  if (body && frame_logs++<16)
    REXLOG_INFO("COOP THREAT consumer-context family={} owner={} actor={:08X} ai={:08X}/{:08X} body={:08X} cached={:08X}",
        uint16_t(family),bool(owner),Load32(b,0x82CCBF6C),Load32(b,0x82CCBF64),Load32(b,0x82CCBF68),body,
        owner && offset ? Load32(b,owner->vars+offset) : 0);
#endif
  if (!owner || !body || !offset) { native(ctx,b); return; }
  const auto binding=*owner;
  const auto original=Load32(b,binding.vars+offset);
  if (original!=hero && original!=body) { native(ctx,b); return; }
  PlayerDamageGuard guard{b,{binding,offset,hero,body,pool},original,player_damage_frame};
  player_damage_frame=guard.frame;
  // Each native hit follows its real player source, including mixed volleys.
  // BC sound scan82626618 likewise assigns its cached player to848 only after
  // the actual sound passes its native flag/radius checks. Scope that cache per
  // sound owner too, rather than attributing a peer's shot noise to Jack.
  Store32(b,binding.vars+offset,hero);
#if defined(KK_DEV_TOOLS)
  const auto target_before=pool==2 ? Load32(b,binding.vars+848):0;
#endif
  native(ctx,b);
#if defined(KK_DEV_TOOLS)
  static unsigned sound_targets=0;
  if (pool==2 && target_before!=body && RetainedDamageSource(b,binding) &&
      Load32(b,binding.vars+848)==body && sound_targets++<64)
    REXLOG_INFO("COOP BAT peer-sound-target actor={:08X} from={:08X} target={:08X}",
        binding.actor,target_before,body);
#endif
}
void PlayerDamageRecord(uint8_t* b, uint32_t packed, uint32_t returned) {
  if (!player_damage_frame || (packed>>24)!=player_damage_frame->pool) return;
  const auto& frame=*player_damage_frame;
  const auto pool=stimulus_debug::ReadPool([&](uint32_t p) { return Load32(b,p); },frame.pool);
  if (!pool.valid || pool.capacity>4096) return;
  const auto hit=stimulus_debug::ReadSlot([&](uint32_t p) { return Load32(b,p); },pool,packed&0xFFFFFF);
  if (!hit.valid || hit.address!=returned || (frame.pool==4 && hit.target!=frame.binding.actor) ||
      Ptr(b,frame.binding.ai,64,frame.offset+4)!=frame.binding.vars) return;
  const auto player=hit.source==frame.body && kk::PartnerBodyCombatActor(b)==frame.body ? frame.body : frame.hero;
#if defined(KK_DEV_TOOLS)
  static unsigned logged=0;
  if (player==frame.body && Load32(b,frame.binding.vars+frame.offset)!=player && logged++<64)
    REXLOG_INFO("COOP THREAT {} family={} actor={:08X} source={:08X} cached-player={:08X}",
        frame.pool==4 ? "player-hit-class":"player-sound-class",
        uint16_t(frame.binding.id.family),frame.binding.actor,hit.source,player);
#endif
  Store32(b,frame.binding.vars+frame.offset,player);
}
void PlayerDamageHandle(uint8_t* b, uint32_t handle) {
  if (!player_damage_frame || !PoseAddress(handle,8) || (Load32(b,handle)>>24)!=player_damage_frame->pool) return;
  const auto record=stimulus_debug::ReadHandle([&](uint32_t p) { return Load32(b,p); },handle);
  if (record.valid) PlayerDamageRecord(b,record.id,record.address);
}
void WriteLife(uint8_t* b, const Binding& binding, const EnemyState& e) {
  if (const auto cell = LifeCell(b,binding)) StoreF(b,cell,NativeLife(binding.id.family,e.life));
  if (const auto offset = SecondaryLifeOffset(binding.id.family)) StoreF(b, binding.vars+offset, e.secondary_life);
}
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
  // Resolve only a recovered local death block and verify its compiled
  // function. Families without one retain their native zero-life dispatch.
  const auto death = NativeDeath(binding.id.family);
  if (!death.slot) return;
  const auto block = Ptr(b, death.slot, 0, 24);
  if (!block || Load32(b, block+20) != death.function || !Scratch::CanUse(ctx)) {
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
  ctx.r3.u64 = death.track; ctx.r4.u64 = block;
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
  // A yaw-only snapshot cannot represent the native vertical perch basis.
  // Keep that local authored orientation while still applying host life/XYZ.
  if (!PerchedBat(binding.id.family,e.state)) call(__imp__sub_82711278, forward);
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
void Replicas(PPCContext& ctx, uint8_t* b, bool drive) {
  auto& s = Get(); s.driving.clear(); s.counts = {};
  s.counts.unmatched = s.unbound;
  if (s.link.remote.world != s.identity) {
    if (s.link.role == Role::Guest) s.counts.unmatched += uint32_t(s.bindings.size());
    return;
  }
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
    if (s.link.role == Role::Host && alias == s.aliases.end()) continue;
    if (alias != s.aliases.end()) binding = Match(alias->second, bindings);
    else {
      std::vector<MatchCandidate> candidates;
      for (const auto& local : bindings) {
        const auto it = s.records.find(local.id); if (it == s.records.end()) continue;
        bool available = it->second.kind != EnemyEventKind::Removed && !claimed.contains(local.id);
        // A distinct decoded BIG resource is never a positional substitute.
        const auto authored=s.authored.find(local.actor);
        if (local.id!=r.enemy.id && authored!=s.authored.end() && authored->second.file_key) available=false;
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
    auto a = s.authority.find(r.enemy.id);
    if (a == s.authority.end()) {
      if (s.link.role != Role::Guest || r.enemy.owner != EnemyOwner::Host || r.enemy.generation) continue;
      a = s.authority.try_emplace(r.enemy.id).first; a->second.last = r.enemy;
    }
    auto& authority = a->second;
    if (!authority.Accepts(r.enemy) || authority.owner == Side() || authority.Frozen()) continue;
    authority.Observe(r.latest.id.source ? r.latest : r.enemy);
    if (r.kind == EnemyEventKind::Removed) {
      // Death and retirement are separate reliable boundaries. A zero-life
      // replica still runs its corpse script until its owner removes the root.
      // Use native destruction so collision, relations and event 4 are retired
      // together. Never infer removal from age, distance or a missing packet.
      if (!Scratch::CanUse(ctx) || !ControllerSafe(b,binding->actor) ||
          s.retirement_applied.contains(binding->id)) continue;
      const auto actor=binding->actor;
      Scratch scratch(ctx,b);
      constexpr uint32_t globals[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
      uint32_t saved[7];
      for (unsigned i=0;i<7;++i) saved[i]=Load32(b,globals[i]);
      ctx.r3.u64=actor; __imp__sub_82714E40(ctx,b);
      for (unsigned i=0;i<7;++i) Store32(b,globals[i],saved[i]);
      if (auto local=s.records.find(binding->id);local!=s.records.end()) local->second.kind=EnemyEventKind::Removed;
      s.retirement_applied.insert(binding->id);
      REXLOG_INFO("ENEMIES retirement owner={} id={:016X}/{}/{} native=true",
          authority.owner==EnemyOwner::Host ? "host":"guest",r.enemy.id.source,r.enemy.id.sequence,uint16_t(r.enemy.id.family));
      continue;
    }
    if (authority.dead && r.enemy.life > 0) continue;
    const bool dead = r.kind != EnemyEventKind::Spawned || r.enemy.life <= 0;
    if (!dead && (!ControllerSafe(b, binding->actor) || !Scratch::CanUse(ctx))) continue;
    // Only the nonowner receives raw life. Corpses run native death dispatches.
    auto sample = r.enemy; if (dead) sample.life = 0;
    Binding previous_binding; EnemyState previous;
    const float before = Read(b,binding->actor,previous_binding,previous) ? previous.life : sample.life;
    ShotResultLog(sample, before, true);
    WriteLife(b, *binding, sample);
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
          REXLOG_INFO("ENEMIES replica role={} id={:016X}/{}/{} owner=({:.3f},{:.3f},{:.3f}) local=({:.3f},{:.3f},{:.3f}) err={:.4f} life={:.2f}/{:.2f} action={}/{} secondary={:.2f}/{:.2f}",
              s.link.role == Role::Host ? "host" : "guest", r.enemy.id.source, r.enemy.id.sequence, uint16_t(r.enemy.id.family), host.x, host.y, host.z,
              local.x, local.y, local.z, error, host.life, local.life, host.action, local.action, host.secondary_life, local.secondary_life);
#endif
        }
      }
    }
  }
  for (const auto& binding : bindings) if (!matched.contains(binding.actor) && ValidBinding(b, binding)) {
    if (const auto* a = AuthorityFor(binding.id); a && a->owner == Side()) continue;
    ++s.counts.unmatched;
    if (s.unmatched_logged.insert(binding.id).second)
      REXLOG_INFO("ENEMIES unmatched guest source={:016X} sequence={} family={} leave-alone=true",
          binding.id.source, binding.id.sequence, uint16_t(binding.id.family));
  }
}
const Binding* CanonicalBinding(const uint8_t* b, EnemyId id) {
  auto& s = Get();
  const auto alias = s.aliases.find(id); if (alias == s.aliases.end()) return nullptr;
  const Binding* found = nullptr;
  for (const auto& [actor, binding] : s.bindings) if (binding.id == alias->second) {
    if (found || !ValidBinding(b, binding)) return nullptr;
    found = &binding;
  }
  if (found) for (const auto& [actor, binding] : s.bindings) if (&binding != found &&
      (binding.ai == found->ai || binding.vars == found->vars || binding.matrix == found->matrix)) return nullptr;
  return found;
}
bool OwnedSample(const uint8_t* b, const Binding& binding, EnemyState& e) {
  auto* a = AuthorityFor(binding.id); Binding observed;
  if (!a || !Read(b, binding.actor, observed, e) || !ValidBinding(b, binding)) return false;
  e.id = Canonical(binding.id); e.owner = a->owner; e.generation = a->generation;
  e.spawn_x = a->last.spawn_x; e.spawn_y = a->last.spawn_y; e.spawn_z = a->last.spawn_z;
  e.birth_spawner=a->last.birth_spawner; e.birth_template=a->last.birth_template; e.birth_point=a->last.birth_point;
  if (a->dead) e.life = 0;
  return ValidEnemy(e);
}
bool SeedAuthority(PPCContext& ctx, uint8_t* b, const Binding& binding, const EnemyState& e) {
  if (e.life > 0 && !Drive(ctx, b, binding, e)) return false;
  WriteLife(b, binding, e);
  if (e.life <= 0) DeathTransition(ctx, b, binding);
  return true;
}
bool SendOwnership(const OwnershipChange& change) {
  Message m; m.type = MessageType::OwnershipChange; m.epoch = Get().link.epoch; m.ownership = change;
  return SubmitOwnership(std::move(m));
}
void OwnershipBeforeAi(PPCContext& ctx, uint8_t* b) {
  auto& s = Get(); const double now = Now();
  for (auto it = s.private_reseed.begin(); it != s.private_reseed.end();) {
    const auto a = s.authority.find(*it);
    const auto* binding = CanonicalBinding(b, *it);
    if (a == s.authority.end() || (binding && SeedAuthority(ctx, b, *binding, a->second.last)))
      it = s.private_reseed.erase(it);
    else ++it;
  }
  for (auto& m : TakeOwnership()) {
    if (m.epoch == s.link.epoch && m.ownership.world == s.identity && s.ownership_in.size() < kMaxEnemies*2)
      s.ownership_in.push_back(std::move(m));
  }
  for (auto it = s.ownership_in.begin(); it != s.ownership_in.end();) {
    const auto& c = it->ownership;
    auto a = s.authority.find(c.enemy.id);
    const auto* binding = CanonicalBinding(b, c.enemy.id);
    if (a == s.authority.end() || !binding) { it = s.ownership_in.erase(it); continue; }
    auto& authority = a->second;
    if (s.private_reseed.contains(c.enemy.id)) { ++it; continue; }
    if (c.phase == OwnershipPhase::Matched && s.link.role == Role::Host) {
      if (authority.Accepts(c.enemy)) { authority.matched = true; s.matched_at[c.enemy.id] = now; }
      // A lost worker/game handoff can be recovered without inventing an owner.
      if (c.generation < authority.generation) {
        if (const auto prior = s.committed.find(c.enemy.id); prior != s.committed.end()) SendOwnership(prior->second);
      }
    } else if (c.phase == OwnershipPhase::Prepare && s.link.role == Role::Guest) {
      EnemyState local; auto trial = authority;
      if (OwnedSample(b, *binding, local)) if (auto ready = trial.Prepare(c, local)) {
        if (!SeedAuthority(ctx, b, *binding, ready->enemy)) { ++it; continue; }
        authority = trial; s.ownership_out[c.enemy.id] = *ready; SendOwnership(*ready);
      }
    } else if (c.phase == OwnershipPhase::Ready && s.link.role == Role::Host) {
      if (auto commit = authority.Ready(c)) {
        if (!SeedAuthority(ctx, b, *binding, commit->enemy)) { ++it; continue; }
        auto final = commit->enemy; final.owner = authority.owner; final.generation = authority.generation;
        authority.pending->enemy = final; authority.last = final;
        s.ownership_out[c.enemy.id] = *commit; SendOwnership(*commit);
      }
    } else if (c.phase == OwnershipPhase::Commit) {
      auto trial = authority;
      if (trial.Commit(c)) {
        if (!SeedAuthority(ctx, b, *binding, c.enemy)) { ++it; continue; }
        const auto old = authority.owner;
        authority = trial; s.ownership_out.erase(c.enemy.id); s.committed[c.enemy.id] = c;
        // Do not let a delayed old live sample drive this new generation.
        s.driving.erase(binding->actor);
        REXLOG_INFO("OWNERSHIP id={:016X}/{} generation={} from={} to={} life={:.2f} pos=({:.3f},{:.3f},{:.3f})",
            c.enemy.id.source, c.enemy.id.sequence, c.generation,
            old == EnemyOwner::Host ? "host" : "guest", c.owner == EnemyOwner::Host ? "host" : "guest",
            c.enemy.life, c.enemy.x, c.enemy.y, c.enemy.z);
      }
    }
    it = s.ownership_in.erase(it);
  }
  // Observe native results only on the owner. A replica's old local life can
  // never veto the owner's outcome or resurrect an authoritative corpse.
  for (auto& [id, a] : s.authority) {
    if (s.private_reseed.contains(id)) continue;
    const auto* binding = CanonicalBinding(b, id); if (!binding) continue;
    EnemyState local;
    if (a.Runs(Side()) && OwnedSample(b, *binding, local)) a.Observe(local);
    // C68: retain host authority. Guest-owned native damage allowed a local
    // death before the host confirmed it, and moved hit consumption off host.
    // Keep the transaction receiver for private-world restoration, but do not
    // initiate proximity transfers in this protocol/build.
#if defined(KK_DEV_TOOLS)
    // Exercise the existing real transaction only in an explicit combat fixture.
    // Shipping retains C68's host authority policy, including firearm authority.
    static const int dev_proximity=[] {
      const char* value=std::getenv("KK_DEV_COMBAT_OWNERSHIP");
      return value && std::strcmp(value,"1")==0 ? 1 : value && std::strcmp(value,"guest")==0 ? 2 : 0;
    }();
    const auto matched=s.matched_at.find(id);
    if (dev_proximity && s.link.role==Role::Host && s.link.partner &&
        now-s.link.remote.time<=.5 && matched!=s.matched_at.end() && now-matched->second<=1.5 &&
        EnemyPartnerCombatReady() && ValidBinding(b,*binding) && ControllerSafe(b,binding->actor)) {
      const float host=PlayerDistance(b,binding->actor,Load32(b,0x82CCBF7C));
      const auto& peer=*s.link.partner;
      const float guest=std::hypot(a.last.x-peer.x,a.last.y-peer.y,a.last.z-peer.z);
      // Mode guest keeps a completed grant for a reverse-hit fixture. It still
      // requires the normal proximity grant and full reliable transaction.
      if (const auto target=a.proximity.Decide(a.owner,host,guest,now,
          !a.Frozen() && !a.dead && a.matched && (dev_proximity!=2 || a.owner==EnemyOwner::Host))) {
        auto trial=a;
        if (const auto prepare=trial.Begin(s.identity,*target,a.last))
          if (SeedAuthority(ctx,b,*binding,prepare->enemy)) {
            a=trial; s.ownership_out[id]=*prepare; SendOwnership(*prepare);
            REXLOG_INFO("COOP combat ownership fixture prepare id={:016X}/{} generation={} host-distance={:.2f} guest-distance={:.2f}",
                id.source,id.sequence,prepare->generation,host,guest);
          }
      }
    }
#endif
  }
  if (now < s.next_ownership) return;
  s.next_ownership = now+.5;
  for (const auto& [id, c] : s.ownership_out) SendOwnership(c);
  if (s.link.role == Role::Guest) for (auto& [id, a] : s.authority) {
    if (!CanonicalBinding(b, id)) continue;
    // A heartbeat confirms both binding availability and generation reception.
    SendOwnership({s.identity, OwnershipPhase::Matched, a.generation, a.owner, a.last});
  }
}
void HoldReplicas(PPCContext& ctx, uint8_t* b, bool drive) {
  auto& s = Get();
  for (auto& [id, a] : s.authority) {
    if (!a.Frozen() && a.owner == Side() && !s.private_reseed.contains(id)) continue;
    const auto* binding = CanonicalBinding(b, id); if (!binding) continue;
    if (s.retirement_applied.contains(binding->id)) continue;
    const auto& state = a.Frozen() ? a.pending->enemy : a.last;
    if (state.life <= 0) {
      WriteLife(b, *binding, state);
      DeathTransition(ctx, b, *binding); continue;
    }
    // Keep known replicas held even after live snapshots expire. Native AI
    // resumes only on a Commit, or when the connection/load guard releases it.
    if (a.Frozen() || !s.driving.contains(binding->actor)) {
      if (drive && !Drive(ctx, b, *binding, state)) continue;
      WriteLife(b, *binding, state);
      s.driving[binding->actor] = {state, EnemyEventKind::Spawned, state};
    }
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
  if (s.combat_epoch != s.link.epoch) {
    s.combat_epoch = s.link.epoch; s.status_replay.Reset(s.combat_epoch, s.identity);
    s.incarnation = 0; s.next_damage = 0; s.partner_status_time = -1;
    s.pending_damage.clear(); s.outgoing_damage.clear();
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
    // Local revival changes incoming replay only. Outgoing hits and peer
    // status belong to the other player's independent life.
    s.sent_status.reset(); s.next_status = 0;
    s.next_status_time = 0; s.pending_damage.clear();
  }
}
void CaptureNativeGrabStatus(const uint8_t*,uint32_t,uint32_t,uint32_t,PlayerStatus&);
void PeerGrabTick(PPCContext&,uint8_t*);
void PublishPlayerStatus(const uint8_t* b) {
  auto& s = Get();
  if (!s.prepared || !Connected() || !Environment(b, s.world) ||
      !s.combat_epoch || s.combat_epoch != s.link.epoch || !s.incarnation) return;
  const auto h = ReadHumanStatus(b); if (!h || h->hero != s.combat_hero || h->ai != s.combat_ai || h->vars != s.combat_vars) return;
  PlayerStatus p{s.identity, s.incarnation, s.next_status+1, s.damage_replay.Last(), h->condition};
  const auto extension=Ptr(b,h->hero,24,42);
  if (!extension) return;
  p.capabilities=Load16(b,extension+40);
  CaptureNativeGrabStatus(b,h->hero,h->ai,h->vars,p);
  if (s.sent_grab.number) {
    p.peer_grab_incarnation=s.sent_grab.incarnation; p.peer_grab_number=s.sent_grab.number;
    if (s.sent_grab.active) { p.peer_grab_enemy=s.sent_grab.enemy; p.peer_grab_generation=s.sent_grab.generation; }
  }
  const bool changed = !s.sent_status || p.condition != s.sent_status->condition || p.last_damage != s.sent_status->last_damage ||
      p.grab_ready!=s.sent_status->grab_ready || p.grab_enemy!=s.sent_status->grab_enemy ||
      p.grab_generation!=s.sent_status->grab_generation || p.grab_kind!=s.sent_status->grab_kind ||
      p.peer_grab_incarnation!=s.sent_status->peer_grab_incarnation || p.peer_grab_number!=s.sent_status->peer_grab_number ||
      p.peer_grab_enemy!=s.sent_status->peer_grab_enemy || p.peer_grab_generation!=s.sent_status->peer_grab_generation ||
      p.capabilities!=s.sent_status->capabilities;
  if (!changed && Now() < s.next_status_time) return;
  if (!p.number) return; // Do not wrap a replay sequence.
  Message m; m.type = MessageType::PlayerStatus; m.epoch = s.combat_epoch; m.player_status = p;
  if (SubmitCombat(m, Now())) {
    ++s.next_status; s.sent_status = p; s.next_status_time = Now()+.5;
#if defined(KK_DEV_TOOLS)
    if (changed) REXLOG_INFO("BITE player-status incarnation={} condition={} last-processed={} cell={:08X} value={:.3f} dead={:.3f} hurt={:.3f} recovered={:.3f} bank={:08X} G={:08X}",
        p.incarnation, uint8_t(p.condition), p.last_damage, h->cell, h->value, h->dead, h->hurt, h->recovered, h->bank, Load32(b, 0x82CC97C0));
    if (changed) REXLOG_INFO("COOP GRAB status local-life={} processed={} ready={} holder={:016X}/{} generation={} kind={} outgoing-life={} outgoing-first={} outgoing-source={:016X}/{} outgoing-generation={}",
        p.incarnation,p.last_damage,p.grab_ready,p.grab_enemy.source,p.grab_enemy.sequence,p.grab_generation,p.grab_kind,
        p.peer_grab_incarnation,p.peer_grab_number,p.peer_grab_enemy.source,p.peer_grab_enemy.sequence,p.peer_grab_generation);
#endif
  }
}
bool LocalNativeGrabEnabled() {
#if defined(KK_DEV_TOOLS)
  static const int override=[] { const auto* v=std::getenv("KK_DEV_NATIVE_GRAB_RECEIVE"); return v ? (std::strcmp(v,"1")==0 ? 1:0):-1; }();
  if (override>=0) return override!=0;
#endif
  return true;
}
stimulus_debug::Record LocalNativeGrabRecord(const uint8_t* b) {
  const auto& g=Get().local_native_grab;
  const auto read=[&](uint32_t p){return Load32(b,p);};
  const auto pool=stimulus_debug::ReadPool(read,65);
  if (!pool.valid || pool.capacity>4096 || (g.id>>24)!=65) return {};
  const auto record=stimulus_debug::ReadSlot(read,pool,g.id&0xFFFFFF);
  if (!record.valid || record.generation!=g.generation || record.source!=g.hero || record.target!=g.source.actor) return {};
  return record;
}
void LocalNativeGrabRelease(PPCContext& ctx,uint8_t* b,const char* reason) {
  auto& g=Get().local_native_grab;
  if (LocalNativeGrabRecord(b).valid && Scratch::CanUse(ctx)) {
    Scratch scratch(ctx,b); const auto handle=scratch.Vec(0,0,0,0x100);
    Store32(b,handle,g.id); Store32(b,handle+4,g.generation);
    ctx.r3.u64=handle; ctx.r4.u64=0; __imp__sub_8271ECF8(ctx,b);
  }
  g.ended=true;
  REXLOG_INFO("COOP GRAB guest release reason={} source={:08X} hero={:08X} native-controller-cleanup=true bridge=scorpion",reason,g.source.actor,g.hero);
}
bool LocalNativeGrabDamage(PPCContext& ctx,uint8_t* b,const DamageEvent& d,const Binding& source,
                          uint32_t hero,uint32_t ai,uint32_t vars) {
  if (!LocalNativeGrabEnabled() || source.id.family!=EnemyFamily::Scorpion || d.projectile) return true;
  auto& s=Get(); auto& g=s.local_native_grab;
  const bool start=combat::NativeScorpionGrabStart(d);
  const bool follow=(d.flags==5 && d.magnitude==10) || (d.flags==512 && !d.magnitude);
  if (!start && !follow) return true;
  if (start) {
    const auto* authority=AuthorityFor(source.id);
    if (!authority || authority->dead || authority->last.life<=0) return false;
    if (g.hero==hero && g.ai==ai && g.vars==vars && g.source.actor==source.actor &&
        g.source.id==source.id && g.ownership_generation==d.generation &&
        g.incarnation==s.incarnation && !g.ended && LocalNativeGrabRecord(b).valid) return true;
    if (LocalNativeGrabRecord(b).valid) return false; // Preserve an existing native claim.
    const auto init=Ptr(b,0x82CC8C58,0,24);
    if (!PoseAddress(vars,15688) || !init || Load32(b,init+20)!=0x825ED5F8) return false;
    const auto result=ctx.r1.u32+0x180;
    Store32(b,result,UINT32_MAX); Store32(b,result+4,0);
    ctx.r3.u64=hero; ctx.r4.u64=1; ctx.r5.u64=result; ctx.r6.u64=1;
    ctx.r7.u64=init; ctx.r8.u64=0; ctx.r9.u64=init;
    __imp__sub_82548BC0(ctx,b);
    const auto record=stimulus_debug::ReadHandle([&](uint32_t p){return Load32(b,p);},result);
    const bool accepted=ctx.r3.u32==hero && record.valid && record.source==hero &&
        record.target==source.actor && (record.id>>24)==65;
    g={}; g.source=source; g.hero=hero; g.ai=ai; g.vars=vars; g.incarnation=s.incarnation;
    g.id=record.id; g.generation=record.generation; g.ownership_generation=d.generation; g.first_damage=d.number; g.ended=!accepted;
    REXLOG_INFO("COOP GRAB guest claim source={:08X} hero={:08X} accepted={} native={:08X}/{} mode={} holder={:08X} state={} bridge=scorpion",
        source.actor,hero,accepted,g.id,g.generation,Load32(b,vars+4948),Load32(b,vars+4604),Load32(b,vars+2712));
    return accepted;
  }
  const bool active=g.hero==hero && g.ai==ai && g.vars==vars && g.source.actor==source.actor &&
      g.source.id==source.id && g.ownership_generation==d.generation &&
      g.incarnation==s.incarnation && !g.ended && LocalNativeGrabRecord(b).valid;
  if (!active) {
    static unsigned lines=0;
    if (lines++<64) REXLOG_INFO("COOP GRAB guest follow refused source={:08X} flags={} magnitude={} reason=no-live-native-claim bridge=scorpion",source.actor,d.flags,d.magnitude);
    return false;
  }
  if (d.flags==512) LocalNativeGrabRelease(ctx,b,"authoritative-timeout");
  return true;
}
void LocalNativeGrabTick(PPCContext& ctx,uint8_t* b) {
  auto& s=Get(); auto& g=s.local_native_grab;
  if (!g.hero) return;
  if (Load32(b,0x82CCBF7C)!=g.hero || Ptr(b,Ptr(b,g.hero,24,8),4,88)!=g.ai ||
      Ptr(b,g.ai,64,15688)!=g.vars || s.incarnation!=g.incarnation) { g={}; return; }
  const auto* authority=AuthorityFor(g.source.id);
  const char* release=nullptr;
  if (!LocalNativeGrabEnabled() || !Connected()) release="session-unavailable";
  else if (!combat::Fresh(s.partner_status_time,Now())) release="owner-status-stale";
  else if (combat::NativeGrabOwnerReleased(s.status_replay.Status(),s.identity,g.incarnation,Canonical(g.source.id),g.ownership_generation,g.first_damage)) release="owner-ended";
  else if (!RetainedDamageSource(b,g.source)) release="source-changed";
  else if (!authority || authority->dead || authority->last.life<=0) release="source-dead";
  else if (!combat::RemoteOwner(authority->owner,Side(),authority->generation,g.ownership_generation,authority->Frozen())) release="authority-changed";
  if (!g.ended && release) LocalNativeGrabRelease(ctx,b,release);
  if (!g.ended && !LocalNativeGrabRecord(b).valid) {
    g.ended=true;
    REXLOG_INFO("COOP GRAB guest ended-by-native-controller source={:08X} hero={:08X} bridge=scorpion",g.source.actor,g.hero);
  }
  const auto state=Load32(b,g.vars+2712),holder=Load32(b,g.vars+4604),attachment=Load32(b,g.vars+4944);
  if (state!=g.observed_state || holder!=g.observed_holder || attachment!=g.observed_attachment) {
    g.observed_state=state; g.observed_holder=holder; g.observed_attachment=attachment;
    static unsigned lines=0;
    if (lines++<128) REXLOG_INFO("COOP GRAB guest controller source={:08X} hero={:08X} state={} holder={:08X} kind={} attachment={:08X} live-claim={} ended={} bridge=scorpion",
        g.source.actor,g.hero,state,holder,Load32(b,g.vars+4948),attachment,LocalNativeGrabRecord(b).valid,g.ended);
  }
}
bool HumanGrabLayout(const uint8_t* b,uint32_t ai,uint32_t vars) {
  const auto model=Ptr(b,ai,0,4),definition=Ptr(b,model,0,8);
  const auto bytes=definition ? Load32(b,definition+4):0;
  return bytes>=15688 && bytes<=1048576 && Ptr(b,ai,64,bytes)==vars;
}
bool HumanGrabClient(const uint8_t* b,uint32_t hero,uint32_t ai) {
  const auto count=Load32(b,ai+68),events=count<=64 ? Ptr(b,ai,72,12*count):0;
  if (!events) return false;
  for (uint32_t i=0;i<count;++i) {
    const auto entry=events+12*i,block=Ptr(b,entry,0,24);
    if (block && Load32(b,entry+4)==20 && Load32(b,entry+8)==hero && Load32(b,block+20)==0x82670980) return true;
  }
  return false;
}
void CaptureNativeGrabStatus(const uint8_t* b,uint32_t hero,uint32_t ai,uint32_t vars,PlayerStatus& p) {
  if (p.condition==PlayerCondition::Dead || !LocalNativeGrabEnabled() || !HumanGrabLayout(b,ai,vars) ||
      !HumanGrabClient(b,hero,ai)) return;
  const auto init=Ptr(b,0x82CC8C58,0,24);
  if (!init || Load32(b,init+20)!=0x825ED5F8) return;
  const auto record=stimulus_debug::ReadHandle([&](uint32_t a){return Load32(b,a);},vars+4432);
  if (!record.valid || record.source!=hero || (record.id>>24)!=65) return;
  p.grab_ready=Load32(b,vars+4544)!=0;
  const auto it=Get().bindings.find(record.target);
  if (it==Get().bindings.end() || it->second.id.family!=EnemyFamily::Scorpion ||
      Load32(b,vars+4604)!=it->second.actor || !RetainedDamageSource(b,it->second)) return;
  const auto* authority=AuthorityFor(it->second.id);
  if (!authority || authority->dead || authority->last.life<=0 || authority->Frozen()) return;
  p.grab_enemy=Canonical(it->second.id); p.grab_generation=authority->generation; p.grab_kind=25;
  p.grab_ready=true;
}
bool NativeDamage(PPCContext& ctx, uint8_t* b, const DamageEvent& d) {
  auto& s = Get();
  if (!Scratch::CanUse(ctx) || !JackReady(b) || !Environment(b, s.world)) return false;
  auto alias = s.aliases.find(d.enemy);
  if (alias == s.aliases.end()) return false;
  std::vector<Binding> bindings;
  for (const auto& [actor, binding] : s.bindings) bindings.push_back(binding);
  const Binding* source = Match(alias->second, bindings); // also refuses mutable-component aliases
  const auto* authority = AuthorityFor(d.enemy);
  const auto h = ReadHumanStatus(b);
  if (!source || !authority || !combat::RemoteOwner(authority->owner, Side(), authority->generation, d.generation, authority->Frozen()) ||
      !RetainedDamageSource(b, *source) || !h || h->hero != s.combat_hero || h->ai != s.combat_ai ||
      h->vars != s.combat_vars || h->condition == PlayerCondition::Dead) return false;
  constexpr uint32_t pool = 0x82C8EF30;
  const auto count = Load32(b, pool+12), used = Load32(b, pool+8);
  const float duration = d.duration;
  if (!count || count > 4096 || used >= count || !Ptr(b, pool, 0, count*116) ||
      !std::isfinite(duration) || duration <= 0 || duration > 10) return false;
  Scratch scratch(ctx, b);
  constexpr uint32_t globals[] = {0x82CCBF64, 0x82CCBF68, 0x82CCBF6C, 0x82CCBF70};
  uint32_t saved[4]; for (unsigned i = 0; i < 4; ++i) saved[i] = Load32(b, globals[i]);
  Store32(b, globals[0], source->ai); Store32(b, globals[1], source->ai);
  Store32(b, globals[2], source->actor); Store32(b, globals[3], source->actor);
  if (!LocalNativeGrabDamage(ctx,b,d,*source,h->hero,h->ai,h->vars)) {
    for (unsigned i=0;i<4;++i) Store32(b,globals[i],saved[i]);
    return false;
  }
  Store32(b,globals[0],source->ai); Store32(b,globals[1],source->ai);
  Store32(b,globals[2],source->actor); Store32(b,globals[3],source->actor);
  const uint32_t result = scratch.Vec(0, 0, 0, 0x100);
  Store32(b, result, UINT32_MAX); Store32(b, result+4, 0);
  s.injecting_damage = true;
  ctx.r3.u64 = result; ctx.r4.u64 = d.part; ctx.r5.u64 = d.flags;
  ctx.r6.u64 = source->actor; ctx.r8.u64 = h->hero; ctx.f1.f64 = duration;
  if (d.projectile) {
    scratch.Vec(d.direction.x,d.direction.y,d.direction.z,80);
    scratch.Vec(d.point.x,d.point.y,d.point.z,96);
    ctx.r7.u64=0; ctx.r9.u64=d.surface; ctx.r10.u64=d.magnitude;
    // The native builder recomputes victim-local impact vectors for local Jack.
    __imp__sub_82634428(ctx,b);
  } else {
    StoreF(b, ctx.r1.u32+88, d.direction.z); ctx.r9.u64=d.magnitude;
    ctx.r10.u64=(uint64_t(std::bit_cast<uint32_t>(d.direction.x))<<32)|std::bit_cast<uint32_t>(d.direction.y);
    __imp__sub_8261DE68(ctx,b);
  }
  s.injecting_damage = false;
  const bool queued=Load32(b,result)!=UINT32_MAX && Load32(b,result+4)!=0;
  if (queued && d.attack_sound && combat::NativeAttackSound(d)) {
    const auto resource=combat::NativeAttackSoundResource(d);
    ctx.r3.u64=source->actor; ctx.r4.u64=resource;
    if (d.enemy.family==EnemyFamily::Raptor) {
      ctx.r5.u64=0; __imp__sub_823FCEB0(ctx,b);
    } else __imp__sub_823FCF58(ctx,b);
    REXLOG_INFO("COOP SOUND guest native-attack source={:08X} family={} number={} code={} created={} native-hit-queued=true",
        source->actor,uint16_t(d.enemy.family),d.number,resource,ctx.r3.s32!=-1);
  }
  for (unsigned i = 0; i < 4; ++i) Store32(b, globals[i], saved[i]);
  return queued;
}
void CombatBeforeAi(PPCContext& ctx, uint8_t* b) {
  auto& s = Get();
  CombatContext(b);
  for (auto& entry : TakeCombat()) {
    if (entry.message.type == MessageType::PlayerStatus) {
      const auto old = s.status_replay.Status();
      if (s.status_replay.Accept(entry.message, entry.received, Now())) {
        s.partner_status_time = Now();
        if (old.incarnation != s.status_replay.Status().incarnation) { s.next_damage = 0; s.outgoing_damage.clear(); }
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
  {
    while (!s.outgoing_damage.empty()) {
      const auto& pending = s.outgoing_damage.front();
      if (!combat::Fresh(pending.received, Now())) { ++s.hits_refused; s.outgoing_damage.pop_front(); continue; }
      if (!SubmitCombat(pending.message, pending.received)) break;
      ++s.hits_queued; s.outgoing_damage.pop_front();
    }
  }
  {
    while (!s.pending_damage.empty()) {
      // C101: relayed damage makes a native type 4 event; wait while paused.
      if (!event_budget::DevReplay26a() && event_budget::DeferWhilePaused(Load32(b,event_budget::kPoolPauseWord)!=0)) break;
      const auto& entry = s.pending_damage.front();
      // Wait briefly for the normal enemy matcher, without advancing replay.
      if ((!s.aliases.contains(entry.message.damage.enemy) || !s.link.partner) && combat::Fresh(entry.received, Now())) break;
      const bool accepted = s.link.partner && s.link.partner->level == s.identity.level &&
          combat::DamageClock(entry.message.damage.time, s.link.partner->time) &&
          s.damage_replay.Accept(entry.message, entry.received, Now());
      const bool queued = accepted && NativeDamage(ctx, b, entry.message.damage);
      if (queued) ++s.hits_native; else ++s.hits_refused;
#if defined(KK_DEV_TOOLS)
      REXLOG_INFO("BITE guest number={} incarnation={} replay-accepted={} native-queued={} processed={} injured=observe-native-consumer role={} family={} generation={}",
          entry.message.damage.number, entry.message.damage.incarnation, accepted, queued, s.damage_replay.Last(),
          s.link.role==Role::Host ? "host" : "guest",uint16_t(entry.message.damage.enemy.family),entry.message.damage.generation);
#endif
      s.pending_damage.pop_front();
    }
    PublishPlayerStatus(b);
  }
}
#if defined(KK_DEV_TOOLS)
void DevDie(PPCContext& ctx, uint8_t* b) {
  struct Die {
    double due=-1, next_log=0, repeat=0, alive_since=-1; bool done=false, host=false, hurt=false;
    unsigned queued=0, deaths=0, limit=1;
    uint32_t world=0, hero=0, vars=0;
    std::optional<PlayerCondition> observed;
    Die() {
      const char* role=std::getenv("KK_DEV_DIE_HOST"); host=role && std::strcmp(role,"1")==0;
      // C104: KK_DEV_DIE_REPEAT=<seconds> dies again that long after each
      // death, KK_DEV_DIE_COUNT deaths in all (default 6), for revive counts.
      if (const char* r=std::getenv("KK_DEV_DIE_REPEAT")) {
        char* end=nullptr; const double v=std::strtod(r,&end);
        if (end!=r && !*end && std::isfinite(v) && v>=8 && v<=120) { repeat=v; limit=6; }
      }
      if (const char* c=std::getenv("KK_DEV_DIE_COUNT")) {
        char* end=nullptr; const auto v=std::strtoul(c,&end,10);
        if (end!=c && !*end && v>=1 && v<=30 && repeat>0) limit=unsigned(v);
      }
      const char* value=std::getenv("KK_DEV_DIE"); if (!value || !*value) return;
      char* end=nullptr; const double t=std::strtod(value,&end);
      if (end!=value && !*end && std::isfinite(t) && t>=0 && t<=3600) due=t;
      else REXLOG_WARN("KK dev die: invalid KK_DEV_DIE, expected seconds in 0..3600");
    }
  };
  static Die die;
  if (die.done || die.due<0) return;
  const double t=kk::DevSecondsSinceMenu(); if (t<die.due) return;
  const auto link=EnemyLinkSnapshot();
  if (link.role!=(die.host ? Role::Host : Role::Guest)) return;
  if (t>die.due+30) {
    die.done=true;
    REXLOG_WARN("KK dev die: expired role={} queued={} hurt-observed={} death-observed=false",
        die.host ? "host" : "guest",die.queued,die.hurt); return;
  }
  const auto flow=SessionFlowSnapshot();
  const auto world=Load32(b,0x828E8388);
  if (!link.connected || !flow.active || flow.phase!=FlowPhase::Playing ||
      flow.policy!=ChapterPolicy::Together || !DevWorld(b,world)) return;
  const auto h=ReadHumanStatus(b);
  if (!h) return;
  if (die.queued && (die.world!=world || die.hero!=h->hero || die.vars!=h->vars)) {
    die.done=true; REXLOG_WARN("KK dev die: stopped, world/Jack changed before observed death"); return;
  }
  if (die.observed!=h->condition) {
    REXLOG_INFO("KK dev die: at {:.2f} role={} observed condition={}->{} value={:.3f} queued={} dead={:.3f} hurt={:.3f} recovered={:.3f}",
        t,die.host ? "host" : "guest",die.observed ? int(*die.observed) : 0,int(h->condition),
        h->value,die.queued,h->dead,h->hurt,h->recovered);
    die.observed=h->condition;
  }
  if (h->condition==PlayerCondition::Dead) {
    ++die.deaths;
    REXLOG_INFO("KK dev die: native death {} of {} observed after {} queued bites; inspect PlayerDowned/private-load/teleport/PlayerRevived",
        die.deaths,die.limit,die.queued);
    if (die.deaths>=die.limit) { die.done=true; return; }
    die.queued=0; die.hurt=false; die.observed.reset(); die.due=t+die.repeat;
    return;
  }
  if (die.queued==1 && h->condition==PlayerCondition::Hurt) die.hurt=true;
  if (die.queued==2) return; // Queue acceptance is not proof of native death.
  // Repeat mode: the first bite waits for 4 s of life, so the revive placement
  // and its void guard are judged before the next death starts.
  if (h->condition!=PlayerCondition::Alive || die.queued) die.alive_since=-1;
  else if (die.alive_since<0) die.alive_since=t;
  if (die.repeat>0 && !die.queued && (die.alive_since<0 || t-die.alive_since<4)) return;
  const float immunity=LoadF(b,h->vars+3232), allowed=LoadF(b,0x8203BFF0);
  const float clock=LoadF(b,0x82CCAE1C)-LoadF(b,0x82CCC02C);
  const float elapsed=clock-LoadF(b,h->vars+11852);
  const float repeat_delay=LoadF(b,0x820B0CF0), recovery=LoadF(b,h->vars+13288);
  // 826A0B58 ignores self-sourced damage when V+2724==0, before fatal flags.
  // 826A0BC0 requires immunity's native zero; 826A0DBC gates repeat damage.
  // Hit again after that delay, while still hurt, BEFORE timed healing at 826CB6B8.
  const bool timing=std::isfinite(immunity) && std::isfinite(allowed) && immunity==allowed &&
      (!die.queued || (die.hurt && h->condition==PlayerCondition::Hurt && std::isfinite(elapsed) &&
        std::isfinite(repeat_delay) && repeat_delay>=0 && elapsed>=repeat_delay &&
        std::isfinite(recovery) && elapsed<recovery));
  const auto flags=Load32(b,h->bank+3056);
  const auto state=Load32(b,h->vars+2712);
  const bool ready=!(flags&0x201) && !Load32(b,h->vars+13180) && !Load32(b,h->vars+13492) &&
      !Load32(b,h->vars+15228) && !Load32(b,h->vars+13208) && state!=610 && state!=709 &&
      (die.queued || h->condition==PlayerCondition::Alive);
  if (!timing || !ready || !JackReady(b) || !Scratch::CanUse(ctx)) {
    if (t>=die.next_log) {
      die.next_log=t+1;
      REXLOG_INFO("KK dev die: waiting step={} condition={} immunity={:.3f} allowed={:.3f} hit-age={:.3f} repeat-delay={:.3f} heal-delay={:.3f} cheats={:08X} state={} gates={}/{}/{}/{}",
          die.queued+1,int(h->condition),immunity,allowed,elapsed,repeat_delay,recovery,flags,state,
          Load32(b,h->vars+13180),Load32(b,h->vars+13492),Load32(b,h->vars+15228),Load32(b,h->vars+13208));
    }
    return;
  }
  const auto matrix=Ptr(b,h->hero,12,64);
  const auto origin=Vector(b,matrix+48);
  uint32_t source=0, source_ai=0, source_matrix=0;
  float nearest=std::numeric_limits<float>::max();
  for (const auto& e : DevEnemies(b,world,origin)) {
    if (!e.initializing && e.life>0 && e.distance<nearest) {
      nearest=e.distance; source=e.actor; source_ai=e.ai; source_matrix=e.matrix;
    }
  }
  // Entry 2 may have no live enemy yet. A registered, living H is also a valid
  // native source. Never use Jack, a spawner or an actor outside the registry.
  if (!source) {
    uint32_t begin,end;
    if (Table(b,world,begin,end,true)) for (auto p=begin;p<end;p+=8) {
      const auto actor=Load32(b,p);
      if (actor==h->hero || !PoseAddress(actor,32) || (Load32(b,actor)&0x00400001)) continue;
      const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88), m=Ptr(b,actor,12,64);
      if (!ai || !m) continue;
      const auto first=Load16(b,ai+80), last=Load16(b,ai+82);
      if (first>last || last>4096 || !PoseAddress(ai,(first+7)*4)) continue;
      const auto tag=Ptr(b,ai,(first+6)*4,24), vars=Ptr(b,ai,64,8);
      if (!tag || Load32(b,tag+20)!=0x826718B8 || !vars || ai==h->ai || vars==h->vars || m==matrix) continue;
      const auto id=Load32(b,vars+4);
      if (id<1 || id>12 || !PoseAddress(h->bank,7096+4*id)) continue;
      const auto life=LoadF(b,h->bank+7092+4*id);
      if (!std::isfinite(life) || life==h->dead) continue;
      const float distance=ShotDistance(origin,Vector(b,m+48));
      if (std::isfinite(distance) && distance<nearest) {
        nearest=distance; source=actor; source_ai=ai; source_matrix=m;
      }
    }
  }
  if (!source) {
    if (t>=die.next_log) { die.next_log=t+1; REXLOG_WARN("KK dev die: waiting for a live non-Jack damage source"); }
    return;
  }
  constexpr uint32_t pool=0x82C8EF30;
  const auto count=Load32(b,pool+12);
  const float duration=LoadF(b,0x820B0E48);
  if (!count || count>4096 || Load32(b,pool+8)>=count || !Ptr(b,pool,0,count*116) ||
      !std::isfinite(duration) || duration<=0 || duration>10) return;
  Scratch scratch(ctx,b);
  constexpr uint32_t globals[]={0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  uint32_t saved[4]; for (unsigned i=0;i<4;++i) saved[i]=Load32(b,globals[i]);
  Store32(b,globals[0],source_ai); Store32(b,globals[1],source_ai);
  Store32(b,globals[2],source); Store32(b,globals[3],source);
  auto direction=Direction(Vector(b,source_matrix+48),origin);
  if (ShotDistance(direction,{})<.001f) direction={1,0,0};
  const auto result=scratch.Vec(0,0,0,0x100);
  Store32(b,result,UINT32_MAX); Store32(b,result+4,0); StoreF(b,ctx.r1.u32+88,direction.z);
  ctx.r3.u64=result; ctx.r4.s64=-1; ctx.r5.u64=0x4004;
  ctx.r6.u64=source; ctx.r8.u64=h->hero; ctx.r9.u64=6;
  ctx.r10.u64=(uint64_t(std::bit_cast<uint32_t>(direction.x))<<32)|std::bit_cast<uint32_t>(direction.y);
  ctx.f1.f64=duration;
  auto& s=Get(); const bool injecting=s.injecting_damage; s.injecting_damage=true;
  __imp__sub_8261DE68(ctx,b); s.injecting_damage=injecting;
  for (unsigned i=0;i<4;++i) Store32(b,globals[i],saved[i]);
  const bool queued=Load32(b,result)!=UINT32_MAX && Load32(b,result+4)!=0;
  if (queued) {
    ++die.queued; die.world=world; die.hero=h->hero; die.vars=h->vars;
    REXLOG_INFO("KK dev die: at {:.2f} role={} bite step={} queued source={:08X} victim={:08X} flags=00004004 magnitude=6 handle={:08X}/{} condition={} hit-age={:.3f} repeat-delay={:.3f} heal-delay={:.3f}; awaiting native transition",
        t,die.host ? "host" : "guest",die.queued,source,h->hero,Load32(b,result),Load32(b,result+4),
        int(h->condition),elapsed,repeat_delay,recovery);
  }
}
std::optional<ShotVector> DevBaitFloor(PPCContext& ctx,uint8_t* b,uint32_t world,ShotVector point) {
  if (!Scratch::CanUse(ctx) || !PoseAddress(world,0x16284)) return {};
  const auto hero=Load32(b,0x82CCBF7C),body=kk::PartnerBodyCombatActor(b);
  Scratch scratch(ctx,b);
  const auto start=scratch.Vec(point.x,point.y,point.z+2,0x100);
  const auto down=scratch.Vec(0,0,-4,0x110);
  // Same native general ground query as BC reflex, with all three query flags.
  // World scratch1626C/16278 contains its hit point/normal (H read_world).
  ctx.r3.u64=hero; ctx.r4.u64=start; ctx.r5.u64=down;
  ctx.r6.u64=UINT32_MAX; ctx.r7.u64=2; ctx.r8.u64=0; ctx.r9.u64=1;
  __imp__sub_82403E38(ctx,b);
  const auto hit=ctx.r3.u32;
  if (!hit || hit==hero || (body && hit==body)) return {};
  const auto floor=Vector(b,world+0x1626C);
  const auto normal=Vector(b,world+0x16278);
  if (!std::isfinite(floor.x) || !std::isfinite(floor.y) || !std::isfinite(floor.z) ||
      !std::isfinite(normal.z) || normal.z<.7f ||
      std::hypot(floor.x-point.x,floor.y-point.y)>.05f || std::abs(floor.z-point.z)>2) return {};
  const ShotVector standing{floor.x,floor.y,floor.z+.1f};
  const auto clearance=scratch.Vec(standing.x,standing.y,standing.z+.1f,0x120);
  const auto up=scratch.Vec(0,0,1.8f,0x130);
  ctx.r3.u64=hero; ctx.r4.u64=clearance; ctx.r5.u64=up;
  ctx.r6.u64=UINT32_MAX; ctx.r7.u64=2; ctx.r8.u64=0; ctx.r9.u64=1;
  __imp__sub_82403E38(ctx,b);
  if (ctx.r3.u32) return {};
  // A floor and headroom ray do not establish capsule clearance or a walkable
  // route. Keep the fixture qualification and inspect actual post-placement play.
  return standing;
}
void DevBait(PPCContext& ctx, uint8_t* b) {
  struct Bait {
    double due = -1;
    bool done = false;
    EnemyFamily family = EnemyFamily::Raptor;
    uint64_t source = 0;
    double repeat = 0;
    float distance = 3, height = 0, max_height = 1000000;
    bool keep_height = false, ground = false, grab_floor = false;
    unsigned placements = 0;
    Bait() {
      const auto placement=[](const char* name,float fallback,float low,float high) {
        if (const char* value=std::getenv(name)) {
          char* end=nullptr; const auto parsed=std::strtof(value,&end);
          if (end!=value && !*end && std::isfinite(parsed) && parsed>=low && parsed<=high) return parsed;
        }
        return fallback;
      };
      distance=placement("KK_DEV_BAIT_DISTANCE",3,.5f,10);
      height=placement("KK_DEV_BAIT_HEIGHT",0,-1,5);
      max_height=placement("KK_DEV_BAIT_MAX_HEIGHT",1000000,0,100);
      if (const char* value=std::getenv("KK_DEV_BAIT_KEEP_HEIGHT")) keep_height=std::strcmp(value,"1")==0;
      if (const char* value=std::getenv("KK_DEV_BAIT_GROUND")) ground=std::strcmp(value,"1")==0;
      if (const char* value=std::getenv("KK_DEV_BAIT_GRAB_FLOOR")) grab_floor=std::strcmp(value,"1")==0;
      if (const char* value=std::getenv("KK_DEV_BAIT_REPEAT")) {
        char* end=nullptr; const auto parsed=std::strtod(value,&end);
        if (end!=value && !*end && std::isfinite(parsed) && parsed>=3 && parsed<=30) repeat=parsed;
      }
      if (const char* value=std::getenv("KK_DEV_BAIT_FAMILY")) {
        char* end=nullptr; const auto parsed=std::strtoul(value,&end,10);
        if (end!=value && !*end && parsed>=unsigned(EnemyFamily::Raptor) &&
            parsed<=unsigned(EnemyFamily::NativeKna) && parsed!=unsigned(EnemyFamily::Prey))
          family=EnemyFamily(parsed);
      }
      if (const char* value=std::getenv("KK_DEV_BAIT_SOURCE")) {
        char* end=nullptr; const auto parsed=std::strtoull(value,&end,16);
        if (end!=value && !*end) source=parsed;
      }
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
  if (t > bait.due+40) { bait.done = true; REXLOG_WARN("KK dev bait: expired waiting for matched live family={} on guest",uint16_t(bait.family)); return; }
  auto& s = Get();
  if (!s.prepared || s.link.role != Role::Guest || !Connected() || !Environment(b, s.world)) return;
  const auto condition=LocalPlayerCondition(b);
  if (!condition || *condition==PlayerCondition::Dead) return;
  const auto jack = ReadJackPose([&](uint32_t p) { return Load32(b, p); },
      [&](uint32_t p) { return Load16(b, p); }, [&](uint32_t p) { return LoadF(b, p); }, Now());
  if (!jack.pose) return;
  const ShotVector origin{jack.pose->x, jack.pose->y, jack.pose->z};
  const Record* nearest = nullptr; float distance = 300;
  for (const auto& [actor, record] : s.driving) {
    if (record.kind != EnemyEventKind::Spawned || record.enemy.life <= 0 || record.enemy.id.family != bait.family) continue;
    if (bait.source && record.enemy.id.source!=bait.source) continue;
    // Optional fixture limit: a flying or perched enemy on another floor is
    // not evidence that Jack can stand underneath it. This does not prove
    // collision or ground even for an accepted placement.
    if (std::abs(record.enemy.z-origin.z)>bait.max_height) continue;
    const auto it = s.bindings.find(actor); const auto alias = s.aliases.find(record.enemy.id);
    if (it == s.bindings.end() || alias == s.aliases.end() || alias->second != it->second.id || !ValidBinding(b, it->second)) continue;
    const float d = ShotDistance(origin, {record.enemy.x, record.enemy.y, record.enemy.z});
    if (d < distance) { distance = d; nearest = &record; }
  }
  if (!nearest) return;
  const auto& e = nearest->enemy;
  const float dx = e.x-origin.x, dy = e.y-origin.y, length = std::hypot(dx, dy);
  float fx = length > .001f ? dx/length : std::cos(e.facing);
  float fy = length > .001f ? dy/length : std::sin(e.facing);
  ShotVector target{e.x-bait.distance*fx, e.y-bait.distance*fy, (bait.keep_height ? origin.z : e.z)+bait.height};
  if (bait.ground) {
    std::optional<ShotVector> floor; float closest=std::numeric_limits<float>::infinity();
    const auto angle=std::atan2(fy,fx);
    for (unsigned i=0;i<8;++i) {
      const float a=angle+float(i)*.785398163f;
      const ShotVector candidate{e.x-bait.distance*std::cos(a),e.y-bait.distance*std::sin(a),target.z};
      const auto found=DevBaitFloor(ctx,b,s.world,candidate);
      // Scorpion82616490 frees a real grab at navigation code5. Keep this
      // optional fixture on normal native cells instead of changing that rule.
      if (found && bait.grab_floor && bait.family==EnemyFamily::Scorpion) {
        const auto grid=Ptr(b,s.world,Load32(b,0x82CCBFC0) ? 560:556,64);
        if (!grid || !Scratch::CanUse(ctx)) continue;
        Scratch query(ctx,b); const auto position=query.Vec(found->x,found->y,found->z,0x100);
        ctx.r3.u64=grid; ctx.r4.u64=position; __imp__sub_8284D638(ctx,b);
        if ((ctx.r3.u32&15)!=1) continue;
      }
      if (found && ShotDistance(origin,*found)<closest) { floor=found; closest=ShotDistance(origin,*found); }
    }
    if (!floor) {
      static double next_floor_log=0;
      if (t>=next_floor_log) {
        next_floor_log=t+2;
        REXLOG_INFO("KK dev bait: native floor/headroom/navigation refused family={} enemy=({:.3f},{:.3f},{:.3f}) desired-z={:.3f} radius={:.2f} grab-floor={}; no teleport",
            uint16_t(e.id.family),e.x,e.y,e.z,target.z,bait.distance,bait.grab_floor);
      }
      return;
    }
    target=*floor;
    const float facing=std::atan2(e.y-target.y,e.x-target.x); fx=std::cos(facing); fy=std::sin(facing);
  }
  if (!kk::DevTeleportJack(ctx, b, Load32(b, 0x82CCBF7C), target, fx, fy)) return;
  bait.done = !bait.repeat || ++bait.placements>=12;
  if (!bait.done) bait.due=t+bait.repeat;
  REXLOG_INFO("KK dev bait: at {:.2f} guest teleported by native 82538B70 family={} enemy={:016X}/{} from=({:.3f},{:.3f},{:.3f}) to=({:.3f},{:.3f},{:.3f}) stand-off={:.2f} height={:.2f} floor-rays={} grab-floor={} collision-unverified=true",
      t, uint16_t(e.id.family), e.id.source, e.id.sequence, origin.x, origin.y, origin.z, target.x, target.y, target.z,bait.distance,bait.height,bait.ground,bait.grab_floor);
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
  const bool observe = REXCVAR_GET(kk_coop_enabled) && Connected() && Load32(b, 0x82CCBF6C) == Load32(b, 0x82CCBF7C);
  const auto before = observe ? ReadHumanStatus(b) : std::nullopt;
  __imp__sub_826A0920(ctx, b);
  const auto after = before ? ReadHumanStatus(b) : std::nullopt;
  if (before && after && before->hero==after->hero && before->vars==after->vars && before->value!=after->value)
    REXLOG_INFO("COOP HEALTH native-hit role={} level={} condition={}->{} source={:08X} flags={:08X} magnitude={} replay={} pos=({:.3f},{:.3f},{:.3f})",
        Get().link.role==Role::Host ? "host" : "guest",Get().identity.level,uint8_t(before->condition),uint8_t(after->condition),
        Load32(b,after->vars+3212),Load32(b,after->vars+3204),LoadF(b,after->vars+3228),Get().damage_replay.Last(),
        LoadF(b,after->vars+11656),LoadF(b,after->vars+11660),LoadF(b,after->vars+11664));
#if defined(KK_DEV_TOOLS)
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
void ObserveHumanReflex(PPCContext& ctx,uint8_t* b) {
  const bool observe=REXCVAR_GET(kk_coop_enabled) && Connected() && Load32(b,0x82CCBF6C)==Load32(b,0x82CCBF7C);
  const auto before=observe ? ReadHumanStatus(b):std::nullopt;
  __imp__sub_826C12C8(ctx,b);
  const auto after=before ? ReadHumanStatus(b):std::nullopt;
  if (before && after && before->hero==after->hero && before->vars==after->vars && before->value!=after->value)
    REXLOG_INFO("COOP HEALTH native-reflex role={} level={} condition={}->{} bad-ground-seconds={} off-ground-seconds={} ground={:08X} source={:08X} flags={:08X}",
        Get().link.role==Role::Host ? "host" : "guest",Get().identity.level,uint8_t(before->condition),uint8_t(after->condition),
        LoadF(b,after->vars+13432),LoadF(b,after->vars+2892),Load32(b,after->vars+4008),
        Load32(b,after->vars+3212),Load32(b,after->vars+3204));
}
void ObserveHumanGround(PPCContext& ctx,uint8_t* b) {
  const bool observe=REXCVAR_GET(kk_coop_enabled) && Connected() && Load32(b,0x82CCBF6C)==Load32(b,0x82CCBF7C);
  const auto before=observe ? ReadHumanStatus(b):std::nullopt;
  __imp__sub_826A1178(ctx,b);
  const auto after=before ? ReadHumanStatus(b):std::nullopt;
  if (before && after && before->hero==after->hero && before->vars==after->vars && before->value!=after->value)
    REXLOG_INFO("COOP HEALTH native-ground role={} level={} condition={}->{} fall={} off-ground-seconds={} ground-kind={} void={} last-floor-z={}",
        Get().link.role==Role::Host ? "host":"guest",Get().identity.level,uint8_t(before->condition),uint8_t(after->condition),
        LoadF(b,after->vars+13268),LoadF(b,after->vars+2840),Load32(b,after->vars+2844),Load32(b,after->vars+15440),LoadF(b,after->vars+2872));
}
// The remote body cannot take part in native NPC weapon exchanges. When it
// overlaps Jack, the interaction ray otherwise selects it in every direction
// and H_exec_read_joy clears both firing wishes. Let only Jack's interaction
// and firearm queries look through the validated body. Native NPCs, terrain,
// ground checks, enemy perception and all other queries keep their filters.
thread_local uint32_t equipment_query_partner=0;
struct EquipmentQueryScope {
  uint32_t previous=equipment_query_partner;
  EquipmentQueryScope(const uint8_t* b,bool enabled) {
    if (enabled && REXCVAR_GET(kk_coop_enabled) && Connected() &&
        Load32(b,0x82CCBF6C)==Load32(b,0x82CCBF7C))
      if (const auto body=kk::PartnerBodyCombatActor(b)) equipment_query_partner=body;
  }
  ~EquipmentQueryScope() { equipment_query_partner=previous; }
};
#if defined(KK_DEV_TOOLS)
void DevEquipmentInput(PPCContext& ctx,uint8_t* b) {
  __imp__sub_826C4E98(ctx,b);
  static const bool enabled=std::getenv("KK_DEV_EQUIPMENT")!=nullptr;
  static double next=0;
  static bool last_rt=false;
  const auto h=enabled && Connected() && Load32(b,0x82CCBF6C)==Load32(b,0x82CCBF7C) ? ReadHumanStatus(b):std::nullopt;
  if (!h) return;
  const auto pad=Load32(b,0x82C63704);
  const bool rt=pad<4 && b[0x82C63858+16*pad+7]!=0;
  const bool lt=pad<4 && b[0x82C63858+16*pad+6]!=0;
  if (Now()<next && rt==last_rt) return;
  last_rt=rt;
  next=Now()+1;
  const auto v=h->vars,g=h->bank;
  REXLOG_INFO("COOP EQUIPMENT gate role={} t={:.3f} view={:08X} aim={:08X} blocked={} target={:08X} proxy={:08X} rt={} lt={} wish={}/{} draw-wait={}",
      Get().link.role==Role::Host ? "host":"guest",kk::DevSecondsSinceMenu(),Ptr(b,Ptr(b,Get().world,112,164),160,32),
      Load32(b,0x82CC8868),Load32(b,v+2784),Load32(b,v+11520),kk::PartnerBodyCombatActor(b),rt,lt,
      Load32(b,v+12336),Load32(b,v+12340),LoadF(b,v+3084));
  REXLOG_INFO("COOP EQUIPMENT input role={} t={:.3f} player={} slot={} bank={:08X} weapon={} rounds={} aim-off={} aiming={} aim-weight={} shoot={}/{} disabled={} loading={}/{} action-wait={} input-disabled={} pad={} pickup={}/{:08X} friend-ahead={:08X}/{:08X}/{:08X}/{:08X}/{:08X}",
      Get().link.role==Role::Host ? "host":"guest",kk::DevSecondsSinceMenu(),Load32(b,v),Load32(b,v+4),g,
      Load32(b,g+13128),Load32(b,g+13504),Load32(b,v+2984),Load32(b,v+2988),LoadF(b,v+6192),
      Load32(b,v+2896),Load32(b,v+2956),Load32(b,v+3060),Load32(b,v+4364),Load32(b,v+4376),
      LoadF(b,v+3092),Load32(b,0x82CCAE2C),Load32(b,0x82C63704),Load32(b,g+17772),Load32(b,g+17776),
      Load32(b,g+7328),Load32(b,g+7332),Load32(b,g+7336),Load32(b,g+7340),Load32(b,g+7344));
}
#endif
// A null reason means observation began. Keep every refusal distinguishable in
// dev diagnostics instead of treating an unobserved callback as a missed shot.
const char* BeginShot(const uint8_t* b) {
  auto& s = Get();
  if (!Connected()) return "disconnected";
  if (s.link.role != Role::Guest) return "host-local";
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
  copy.ray_length = ShotDistance(copy.origin,Vector(b,point));
  if (ValidShotIntent(copy)) {
    s.shot.origin = copy.origin; s.shot.direction = copy.direction; s.shot.ray_length = copy.ray_length; s.shot_resolved = true;
  }
#if defined(KK_DEV_TOOLS)
  if (PoseAddress(object)) DevShotHit(b, Load32(b, object));
#else
  (void)object;
#endif
}
bool ForwardShotEvent(PPCContext& ctx, uint8_t* b) {
  auto& s = Get();
  const auto link = EnemyLinkSnapshot();
  // Solo chapters keep independent native worlds even while the lobby stays
  // connected. Withhold guest hits only in the shared world policy.
  const auto flow = SessionFlowSnapshot();
  if (flow.active && flow.policy != ChapterPolicy::Together) return false;
  // Preserve host authority during transient loss too. A stale worker snapshot
  // can withhold forwarding, but must not turn the guest into a damage owner.
  if (!REXCVAR_GET(kk_coop_enabled) || link.role != Role::Guest ||
      (link.session_state != SessionState::Connected && link.session_state != SessionState::Lost) || !Environment(b, s.world) ||
      ctx.r6.u32 != Load32(b, 0x82CCBF7C)) return false;
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr != 0x82696E74 && lr != 0x82697494 && lr != 0x82697970 && lr != 0x82698C00) return false;
#if defined(KK_DEV_TOOLS)
  DevShotHit(b, ctx.r8.u32);
#endif
  // A supported but unmatched enemy must never die privately. Suppress its
  // native hit even when identity discovery or shot capture is not ready.
  const auto target_ai = Ptr(b, Ptr(b, ctx.r8.u32, 24, 8), 4, 88);
  bool spawner = false; uint32_t state = 0;
  if (Tracks(b, target_ai, state, spawner) == EnemyFamily::None) return false;
  auto block = [&] (const char* reason) {
    ++s.shot_blocked; ShotSendLog(reason);
    if (PoseAddress(ctx.r3.u32, 8)) { Store32(b, ctx.r3.u32, UINT32_MAX); Store32(b, ctx.r3.u32+4, 0); }
    return true;
  };
  if (!s.shooting) return block("capture-not-ready");
  const auto it = s.bindings.find(ctx.r8.u32);
  if (it == s.bindings.end() || !ValidBinding(b, it->second)) return block("unbound-target");
  auto alias = std::find_if(s.aliases.begin(), s.aliases.end(), [&](const auto& a) { return a.second == it->second.id; });
  if (alias == s.aliases.end()) return block("unmatched-target");
  const auto* authority = AuthorityFor(it->second.id);
  if (!authority || authority->owner != EnemyOwner::Host || authority->Frozen()) return block("authority-not-ready");
  // Builder's caller stack+96 is the resolved impact, +80 its normal/vector.
  // See .47:26501-26539 and .39:23618-23652. No damage/bone value goes on wire.
  if (PoseAddress(ctx.r1.u32, 108) && s.shot.hits.size() < (s.shot.weapon == 3 ? kMaxShotHits : 1)) {
    const ShotHit hit{alias->first, Vector(b, ctx.r1.u32+96), authority->generation};
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
  ++s.shot_blocked;
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
void DevShotLog(const uint8_t* b, bool guest, bool host_jack, bool observed, uint32_t weapon,
    uint32_t before, uint32_t after, uint32_t caller, uint32_t result, const char* reason) {
  static unsigned host_lines = 0;
  if (host_jack && host_lines < 128) {
    ++host_lines;
    REXLOG_INFO("KK dev host fire callback: fn=82696A28 call={} t={:.3f} lr={:08X} weapon={} ammo={}/{} result={} cap=128",
        host_lines, kk::DevSecondsSinceMenu(), caller, weapon, before, after, result);
  }
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
  const auto* authority = AuthorityFor(target.id);
  if (!authority || !RoutesShot(*authority, Side(), hit) || !Connected() || !Environment(b, s.world) ||
      !Scratch::CanUse(ctx) || !ValidBinding(b, target) || !PoseAddress(s.game, 18500) || !JackReady(b)) return false;
  const auto hero = Load32(b, 0x82CCBF7C), ai = Ptr(b, Ptr(b, hero, 24, 8), 4, 88);
  if (!ai || !Ptr(b, ai, 64, 13652) || !Ptr(b, hero, 12, 64)) return false;
  // This path consumes the peer's authenticated shot intent. For the audited
  // per-hit player adapters, use the validated body so aggro names the shooter.
  // Keep local Jack's native weapon tables and host cheat choices for damage.
  const auto player_cache=PlayerDamageCache(target.id.family);
  // Na825F2698 and KNa8241ED88 both retain the real attacker and select it
  // for retaliation. Neither consumer has a Jack-only damage gate. Preserve
  // the guest as that source instead of making local Jack draw its aggro.
  const bool native=target.id.family==EnemyFamily::Native || target.id.family==EnemyFamily::NativeKna;
  const auto body_candidate=(player_cache || native) ? kk::PartnerBodyCombatActor(b) : 0;
  const auto cached_player=player_cache ? Load32(b,target.vars+player_cache) : 0;
  const auto peer_body=body_candidate && (native || cached_player==hero || cached_player==body_candidate) ? body_candidate : 0;
  const auto shooter=peer_body ? peer_body : hero;
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
  // Both sides use native weapon tables and the host's replicated cheat choices.
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
    ctx.r6.u64 = shooter; ctx.r7.u64 = 0; ctx.r8.u64 = target.actor; ctx.r9.s64 = -1; ctx.r10.u64 = uint32_t(damage);
    ctx.f1.f64 = duration;
    __imp__sub_82634428(ctx, b);
    queued = Load32(b, result) != UINT32_MAX && Load32(b, result+4) != 0;
#if defined(KK_DEV_TOOLS)
    static unsigned attribution_logs=0;
    if (queued && attribution_logs++<64)
      REXLOG_INFO("COOP THREAT native-shot number={} shooter={:08X} partner={} target={:08X} family={} magnitude={}",
          shot.number,shooter,bool(peer_body),target.actor,uint16_t(target.id.family),damage);
#endif
  }
  for (unsigned i = 0; i < 4; ++i) Store32(b, globals[i], saved[i]);
  return queued;
}
void NativeShotThreat(PPCContext& ctx, uint8_t* b, const ShotIntent& shot) {
  // The caller has consumed the authenticated shot number and validated its
  // world, age, inventory, cadence and historical shooter pose. The segment
  // ends at the actual native ray result, including misses, rather than
  // extending through the wall behind it. No damage or target claim is made.
  const auto& s=Get();
  const auto body=kk::PartnerBodyCombatActor(b);
  if (!body || !Scratch::CanUse(ctx) || !Environment(b,s.world)) return;
  Scratch scratch(ctx,b);
  const uint32_t payload=scratch.start+12288+0x180, result=payload+112;
  const auto vector=[&](uint32_t offset,ShotVector v) {
    StoreF(b,payload+offset,v.x); StoreF(b,payload+offset+4,v.y); StoreF(b,payload+offset+8,v.z);
  };
  const auto enqueue=[&](uint32_t type) {
    Store32(b,result,UINT32_MAX); Store32(b,result+4,0);
    ctx.r3.u64=body; ctx.r4.u64=type; ctx.f1.f64=.0001;
    ctx.r6.u64=payload; ctx.r7.u64=1; ctx.r8.u64=result;
    __imp__sub_8271E4E8(ctx,b);
    return Load32(b,result)!=UINT32_MAX && Load32(b,result+4)!=0;
  };
  // shot_noise uses type2/0x2000 and the native radius at820B1364.
  vector(24,shot.origin); vector(36,{LoadF(b,0x820B1364),0,0}); Store32(b,payload+84,0x2000);
  const bool sound=enqueue(2);
  bool segment=false;
  if (shot.ray_length>.01f) {
    std::memset(b+payload,0,108);
    const float length=std::min(shot.ray_length,shot.weapon==3 ? 80.f:300.f);
    const ShotVector end{shot.origin.x+shot.direction.x*length,shot.origin.y+shot.direction.y*length,shot.origin.z+shot.direction.z*length};
    vector(24,shot.origin); vector(36,end);
    Store32(b,payload+84,shot.weapon); Store32(b,payload+88,UINT32_MAX);
    segment=enqueue(21);
  }
#if defined(KK_DEV_TOOLS)
  static unsigned lines=0;
  if (lines++<128) REXLOG_INFO("COOP THREAT peer-shot number={} body={:08X} sound={} segment={} length={:.3f} hits={}",
      shot.number,body,sound,segment,shot.ray_length,shot.hits.size());
#endif
}
void OwnerShots(PPCContext& ctx, uint8_t* b) {
  auto& s = Get();
  for (const auto& receipt:TakeShotReceipts()) {
    if (s.link.role!=Role::Guest || receipt.world!=s.identity || receipt.number>s.next_shot) continue;
    s.shot_audit_active=true;
    if (receipt.reason==ShotReject::None) ++s.shot_accepted;
    else s.shots.Reject(receipt.reason);
    if (receipt.queued) ++s.shots.applied;
    if (Now()>=s.next_shot_log) {
      s.next_shot_log=Now()+1;
      REXLOG_INFO("COOP SHOTS host-receipt role=guest number={} accepted={} native-queued={} reason={}",
          receipt.number,receipt.reason==ShotReject::None,receipt.queued,ShotRejectName(receipt.reason));
    }
  }
  // C101: peer shots make native type 2/21/4 events. While the world is
  // paused those never age, so leave the intents in their bounded queue.
#if defined(KK_DEV_TOOLS)
  {
    // C101 evidence: the pool pause word and the type 1 bucket, once a second.
    static double next=0;
    static uint32_t peak_visible=0, peak_partner=0;
    const auto body=kk::PartnerBodyCombatActor(b);
    const auto c=event_budget::Count([&](uint32_t p) { return Load32(b,p); },1,body);
    peak_visible=std::max(peak_visible,c.enumerable); peak_partner=std::max(peak_partner,c.owner_live);
    if (Now()>=next) {
      next=Now()+1;
      const auto pool=stimulus_debug::ReadPool([&](uint32_t p) { return Load32(b,p); },1);
      auto& v=event_budget::Vision();
      REXLOG_INFO("KK dev event pool: pause-word={} type1 used={}/{} visible={} peak-visible={} partner={:08X} partner-live={} peak-partner={} bat-scans={} scan-max-visible={} skipped={}",
          Load32(b,event_budget::kPoolPauseWord),pool.count,pool.capacity,c.enumerable,peak_visible,body,c.owner_live,peak_partner,
          v.scans,v.max_visible,v.skipped);
      peak_visible=peak_partner=0; v.max_visible=0;
    }
  }
#endif
  const bool paused=!event_budget::DevReplay26a() && event_budget::DeferWhilePaused(Load32(b,event_budget::kPoolPauseWord)!=0);
  if (paused!=s.events_paused) {
    s.events_paused=paused;
    REXLOG_INFO("KK co-op events: world {}, peer shots and relayed damage {} (deferred frames so far {})",
        paused ? "paused" : "resumed", paused ? "wait" : "run again", s.events_deferred);
  }
  if (paused) { ++s.events_deferred; return; }
  for (const auto& received : TakeShotIntents()) {
    const auto& shot = received.intent;
    auto reason = s.shot_validator.Validate(received, s.identity, received.peer ? received.peer : s.link.partner, Now());
    if (reason==ShotReject::None) NativeShotThreat(ctx,b,shot);
    s.shot_audit_active = true;
    std::vector<std::pair<Binding, ShotHit>> targets;
    bool late = false;
    if (reason == ShotReject::None) for (const auto& hit : shot.hits) {
      const auto a = s.authority.find(hit.id);
      if (a == s.authority.end() || !RoutesShot(a->second, Side(), hit)) { reason = ShotReject::Target; break; }
      const auto* binding = CanonicalBinding(b, hit.id);
      Binding observed; EnemyState e;
      const bool valid = binding && ValidBinding(b, *binding);
      if (!valid || !Read(b, binding->actor, observed, e)) {
        // Only a confirmed death tombstone can turn a missing binding into late.
        const auto known = s.records.find(binding ? binding->id : hit.id);
        if (a->second.dead && ValidateShotHit(shot, hit, a->second.last) == ShotReject::Late) { late = true; continue; }
#if defined(KK_DEV_TOOLS)
        if (s.shot_target_diagnostics++ < 64)
          REXLOG_INFO("ENEMIES shot target rejected number={} id={:016X}/{}/{} detail={} known={} life={:.2f} cap=64",
              shot.number, hit.id.source, hit.id.sequence, uint16_t(hit.id.family),
              !binding ? "missing-binding" : !valid ? "invalid-binding" : "unreadable-target",
              known != s.records.end(), known == s.records.end() ? -1.f : known->second.enemy.life);
#endif
        reason = ShotReject::Target; break;
      }
      e.id = hit.id; e.owner = a->second.owner; e.generation = a->second.generation;
      if (a->second.dead) e.life = 0;
      reason = s.shot_targets.Validate(shot, hit, e, Now());
#if defined(KK_DEV_TOOLS)
      if (reason==ShotReject::Distance && std::getenv("KK_DEV_EQUIPMENT")) {
        static unsigned logs=0;
        if (logs++<64) REXLOG_INFO("COOP EQUIPMENT hit-distance number={} family={} ray={} target-error={} hit=({},{},{}) target=({},{},{})",
            shot.number,uint16_t(e.id.family),ShotDistance(shot.origin,hit.point),ShotDistance(hit.point,{e.x,e.y,e.z}),
            hit.point.x,hit.point.y,hit.point.z,e.x,e.y,e.z);
      }
#endif
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
    if (Now() >= s.next_shot_log) {
      s.next_shot_log = Now()+1;
      const auto peer = received.peer ? received.peer : s.link.partner;
      const ShotHit first = shot.hits.empty() ? ShotHit{} : shot.hits.front();
      REXLOG_INFO("COOP SHOTS receive role=host number={} weapon={} hits={} applied={} late={} reject={} queue-age={:.3f} pose-dt={:.3f} origin-error={:.3f} inventory={} matched={} world={}/{}/{} target={:016X}/{}/{} generation={}",
          shot.number, shot.weapon, shot.hits.size(), applied, late, ShotRejectName(reason), Now()-received.received,
          peer ? shot.time-peer->time : -999., peer ? ShotDistance(shot.origin,{peer->x,peer->y,peer->z}) : -1.f,
          received.inventory.has_value(), s.aliases.size(), shot.world.level, shot.world.start, shot.world.serial,
          first.id.source, first.id.sequence, uint16_t(first.id.family), first.generation);
    }
    if (reason == ShotReject::None && (!late || applied)) ++s.shot_accepted;
    if (!SubmitShotReceipt({shot.world,shot.number,late && !applied && reason==ShotReject::None ? ShotReject::Late : reason,applied}))
      ++s.shot_send_reasons["receipt-queue-refused"];
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
  SessionReviveBeforeAi(ctx,b);
  // Capture before connection, at the first AI pass of this load. Waiting for
  // the peer could otherwise hash actors that already moved locally.
  if (s.capture) Capture(b, w);
  if (s.world == w && Now() <= s.census_until && Environment(b, w)) Capture(b, w, false);
  if (!Connected()) return;
  s.link = EnemyLinkSnapshot();
  const auto flow_revision=SessionFlowSnapshot().rejoin_revision;
  if (s.rejoin_revision!=flow_revision) {
    s.rejoin_revision=flow_revision;
    s.shot_validator={}; s.shot_targets={};
    REXLOG_INFO("KK co-op rejoin: peer shot replay window reset revision={}",flow_revision);
  }
  if (!s.game && !s.load_refused_logged) {
    s.load_refused_logged = true;
    REXLOG_INFO("ENEMIES waiting reason=no-connected-load-identity restart-both-and-load-while-connected=true");
  }
  if (!Environment(b, w)) return;
  if (s.ownership_epoch != s.link.epoch) {
    // A replacement session may have a different peer and clock origin even
    // without a native reload. Never carry that peer's replay/ammo validator.
    ShotSummary("session-end");
    s.shots={}; s.shot_validator={}; s.shot_targets={};
    s.shot_submitted=s.shot_accepted=s.shot_blocked=s.shot_confirmed_deaths=0;
    s.shot_send_reasons.clear(); s.next_send_log=s.next_result_log=s.next_audit_log=s.next_shot_log=0;
    s.authority.clear(); s.ownership_out.clear(); s.committed.clear(); s.ownership_in.clear(); s.matched_at.clear();
    s.private_reseed.clear();
    s.ownership_epoch = s.link.epoch; s.next_ownership = 0;
  }
  Scan(b); s.prepared = true; s.prepared_at = Now();
  Replicas(ctx, b, false);
  OwnershipBeforeAi(ctx, b);
  HoldReplicas(ctx, b, false);
  {
    if (PoseAddress(s.game, 13652) && JackReady(b)) {
      const auto weapon = Load32(b, s.game+13128);
      if (weapon <= 4) {
        InventoryState inventory{s.identity, Now(), uint8_t(weapon)};
        for (unsigned i = 0; i < 4; ++i) inventory.ammo[i] = Load32(b, s.game+13432+i*72);
        PublishInventoryState(inventory);
      }
    }
  }
  OwnerShots(ctx, b);
  CombatBeforeAi(ctx, b);
  PeerGrabTick(ctx,b);
}
#if defined(KK_DEV_TOOLS)
void DevScorpionActivity(PPCContext& ctx,uint8_t* b) {
  static const double due=[] {
    const auto* value=std::getenv("KK_DEV_SCORPION_ACTIVITY");
    if (!value || !*value) return -1.;
    char* end=nullptr; const auto t=std::strtod(value,&end);
    return end!=value && !*end && std::isfinite(t) && t>=0 && t<=3600 ? t:-1.;
  }();
  static bool done=false;
  const auto t=kk::DevSecondsSinceMenu();
  auto& s=Get();
  if (done || due<0 || t<due || !s.prepared || !Connected() || s.link.role!=Role::Host ||
      !Environment(b,s.world) || !EnemyPartnerCombatReady() || !Scratch::CanUse(ctx)) return;
  if (t>due+15) {
    done=true;
    REXLOG_INFO("COOP GRAB activation-fixture expired without a qualified native source and peer copy");
    return;
  }
  // One known authored actor only. No generic spawn, state write, invented
  // grab handle or hit. Native initialization and the shipping bridge still
  // have to produce the actual encounter after this controlled activation.
  if (s.identity.level!=203 || (s.identity.start!=1 && s.identity.start!=5) || !kk::PartnerBodyCombatCopy(b)) return;
  const auto hero=Load32(b,0x82CCBF7C),matrix=Ptr(b,hero,12,64);
  if (!matrix) return;
  const auto all=DevEnemies(b,s.world,Vector(b,matrix+48));
  const auto it=std::find_if(all.begin(),all.end(),[](const auto& e) {
    return !e.scheduled && e.family==EnemyFamily::Scorpion && (e.initializing || e.life>0) &&
        ShotDistance(e.position,{51.110620f,132.907700f,19.930140f})<=.5f;
  });
  if (it==all.end()) return;
  Scratch scratch(ctx,b);
  const auto record=scratch.Vec(0,0,0,0x200);
  std::memset(b+record,0,108); Store32(b,record+4,it->actor); Store32(b,record+84,1);
  constexpr uint32_t globals[]{0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  uint32_t saved[4]; for (unsigned i=0;i<4;++i) saved[i]=Load32(b,globals[i]);
  ctx.r3.u64=record; __imp__sub_8250EDE8(ctx,b);
  for (unsigned i=0;i<4;++i) Store32(b,globals[i],saved[i]);
  done=true;
  REXLOG_INFO("COOP GRAB activation-fixture actor={:08X} native=8250EDE8 controlled-activation=true native-init-and-attacks-required=true direct-hit=false normal-traversal-unverified=true",it->actor);
}
void EnemyThrowProbe(PPCContext& ctx,uint8_t* b) {
  static const double due=[] {
    const auto* text=std::getenv("KK_DEV_ENEMY_THROW"); return text ? std::atof(text):-1.;
  }();
  static double next=due;
  static unsigned count=0;
  static const bool controller=std::getenv("KK_DEV_ENEMY_THROW_CONTROLLER")!=nullptr;
  static const uint32_t volley=[] {
    const auto* text=std::getenv("KK_DEV_ENEMY_THROW_VOLLEY");
    const auto value=text ? std::strtoul(text,nullptr,10):3ul;
    return value>=1 && value<=8 ? uint32_t(value):3u;
  }();
  static const uint32_t stop_hits=[] {
    const auto* text=std::getenv("KK_DEV_ENEMY_THROW_STOP_HITS");
    const auto value=text ? std::strtoul(text,nullptr,10):0ul;
    return value<=32 ? uint32_t(value):0u;
  }();
  static const float advance=[] {
    const auto* text=std::getenv("KK_DEV_ENEMY_THROW_ADVANCE");
    const auto value=text ? std::strtof(text,nullptr):0.f;
    return std::isfinite(value) && value>=0 && value<=5 ? value:0.f;
  }();
  const auto time=kk::DevSecondsSinceMenu();
  if (due<0 || count>=12 || time<next || time>due+90 || !EnemyPartnerCombatReady() ||
      !Scratch::CanUse(ctx)) return;
  auto& s=Get();
  if (stop_hits && s.hits_queued>=stop_hits) return;
  const auto body=kk::PartnerBodyCombatActor(b);
  if (!body) return;
  const Binding* source=nullptr;
  float distance=40;
  for (const auto& [actor,binding]:s.bindings) {
    if (binding.id.family!=EnemyFamily::Native && binding.id.family!=EnemyFamily::NativeKna) continue;
    const auto* authority=AuthorityFor(binding.id);
    const auto d=PlayerDistance(b,actor,body);
    if (!authority || !authority->Runs(Side()) || authority->last.life<=0 ||
        !ValidBinding(b,binding) || !(d<distance)) continue;
    source=&binding; distance=d;
  }
  if (!source || distance<2) return;
  const auto binding=*source;
  const bool kna=binding.id.family==EnemyFamily::NativeKna;
  const uint32_t launcher_offset=kna ? 4092:4096,target_offset=kna ? 4084:4088;
  uint32_t projectile=Load32(b,binding.vars+(kna ? 4408:4400));
  // Use the actor's own native lance template. A missing template means this
  // encounter cannot exercise throwing; do not borrow a player's inventory.
  const auto projectile_ai=Ptr(b,Ptr(b,projectile,24,8),4,88);
  if (!projectile_ai || !Ptr(b,projectile_ai,64,568)) return;
  if (Load32(b,binding.vars+launcher_offset)) return;
  Scratch scratch(ctx,b);
  constexpr uint32_t globals[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  struct Restore {
    uint8_t* base; Binding binding; uint32_t globals[7],target,offset,launcher_offset;
    ShotVector history;
    ~Restore() {
      if (Ptr(base,binding.ai,64,offset+4)==binding.vars && RetainedDamageSource(base,binding)) {
        Store32(base,binding.vars+offset,target);
        Store32(base,binding.vars+launcher_offset,0);
        if (binding.id.family==EnemyFamily::Native) {
          StoreF(base,binding.vars+4472,history.x); StoreF(base,binding.vars+4476,history.y);
          StoreF(base,binding.vars+4480,history.z);
        }
      }
      constexpr uint32_t cells[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
      for (unsigned i=0;i<7;++i) Store32(base,cells[i],globals[i]);
    }
  } restore{b,binding,{},Load32(b,binding.vars+target_offset),target_offset,launcher_offset,
      kna ? ShotVector{}:Vector(b,binding.vars+4472)};
  for (unsigned i=0;i<7;++i) restore.globals[i]=Load32(b,globals[i]);
  if (controller) {
    const auto block=Load32(b,kna ? 0x82CC9B5C:0x82CC8D00);
    if (!PoseAddress(block,24) || Load32(b,block+20)!=(kna ? 0x82443F10u:0x82617858u)) return;
    Store32(b,0x82CCBF64,binding.ai); Store32(b,0x82CCBF68,binding.ai);
    Store32(b,0x82CCBF6C,binding.actor); Store32(b,0x82CCBF70,binding.actor);
    // Controlled encounter setup, not a generated hit. Run the actual native
    // launch state on subsequent AI frames, including animation readiness,
    // its own spear creation, player selection, aim error and ballistics.
    Store32(b,binding.vars+(kna ? 380:384),volley);
    Store32(b,binding.vars+(kna ? 384:388),0);
    Store32(b,binding.vars+292,0);
    const auto activation=scratch.Vec(0,0,0,0x200);
    std::memset(b+activation,0,108); Store32(b,activation+4,binding.actor); Store32(b,activation+84,1);
    ctx.r3.u64=activation; __imp__sub_8250EDE8(ctx,b);
    Store32(b,0x82CCBF64,binding.ai); Store32(b,0x82CCBF68,binding.ai);
    Store32(b,0x82CCBF6C,binding.actor); Store32(b,0x82CCBF70,binding.actor);
    ctx.r3.u64=2; ctx.r4.u64=block; __imp__sub_826FA3B0(ctx,b);
    count=12;
    REXLOG_INFO("COOP PROJECTILE fixture-native-controller actor={:08X} family={} block={:08X} native={:08X} volley={} synthetic-activation=true native-state-dispatch=true observe-native-animation-and-attacks=true normal-traversal-unverified=true",
        binding.actor,uint16_t(binding.id.family),block,Load32(b,block+20),volley);
    return;
  }
  const auto bm=Ptr(b,body,12,64); if (!bm) return;
  const float dx=LoadF(b,bm+48)-LoadF(b,binding.matrix+48),dy=LoadF(b,bm+52)-LoadF(b,binding.matrix+52);
  const auto length=std::hypot(dx,dy);
  const auto move=length>2 ? std::min(advance,length-2):0.f;
  const ShotVector start{LoadF(b,binding.matrix+48)+(move ? move*dx/length:0),
      LoadF(b,binding.matrix+52)+(move ? move*dy/length:0),LoadF(b,binding.matrix+56)+1.2f};
  const auto origin=scratch.Vec(start.x,start.y,start.z,0x100);
  ctx.r3.u64=projectile; ctx.r4.u64=origin; __imp__sub_82715AC8(ctx,b);
  projectile=ctx.r3.u32;
  if (!PoseAddress(projectile,32) || !Ptr(b,Ptr(b,projectile,24,24),20,12)) return;
  Store32(b,0x82CCBF64,binding.ai); Store32(b,0x82CCBF68,binding.ai);
  Store32(b,0x82CCBF6C,binding.actor); Store32(b,0x82CCBF70,binding.actor);
  Store32(b,binding.vars+launcher_offset,projectile); Store32(b,binding.vars+target_offset,body);
  // The staged throw has no preceding launch frame for Na's velocity sample.
  // Its normal state supplies that sample once per frame; preserve it on exit.
  if (!kna) {
    StoreF(b,binding.vars+4472,LoadF(b,bm+48)); StoreF(b,binding.vars+4476,LoadF(b,bm+52));
    StoreF(b,binding.vars+4480,LoadF(b,bm+56));
  }
  if (kna) __imp__sub_8241F0A8(ctx,b); else __imp__sub_825F2890(ctx,b);
  ++count; next=time+4;
  REXLOG_INFO("COOP PROJECTILE fixture-native-throw actor={:08X} family={} projectile={:08X} target={:08X} posted={} count={} distance={:.3f} origin=({:.3f},{:.3f},{:.3f}) advance={:.3f} synthetic-launch=true native-collision-and-damage=true authored-behavior-unverified=true",
      binding.actor,uint16_t(binding.id.family),projectile,body,!Load32(b,binding.vars+launcher_offset),count,distance,
      start.x,start.y,start.z,move);
}
#endif
bool PeerGrabReady() {
  const auto& p=Get().status_replay.Status();
  return EnemyPartnerCombatReady() && (p.grab_ready || p.grab_enemy.source);
}
stimulus_debug::Record PeerGrabRecord(const uint8_t* b) {
  const auto& g=Get().peer_grab;
  const auto read=[&](uint32_t p){return Load32(b,p);};
  const auto pool=stimulus_debug::ReadPool(read,65);
  if (!pool.valid || pool.capacity>4096 || (g.id>>24)!=65) return {};
  const auto record=stimulus_debug::ReadSlot(read,pool,g.id&0xFFFFFF);
  if (!record.valid || record.generation!=g.generation || record.source!=g.actor) return {};
  return record;
}
void ReleasePeerGrabOffer(PPCContext& ctx,uint8_t* b,const char* reason) {
  auto& s=Get(); auto& g=s.peer_grab;
  if (PeerGrabRecord(b).valid && Scratch::CanUse(ctx)) {
    Scratch scratch(ctx,b); const auto handle=scratch.Vec(0,0,0,0x100);
    Store32(b,handle,g.id); Store32(b,handle+4,g.generation);
    ctx.r3.u64=handle; ctx.r4.u64=0; __imp__sub_8271ECF8(ctx,b);
  }
  s.sent_grab.active=false; g.source={}; g.first_damage=0;
#if defined(KK_DEV_TOOLS)
  REXLOG_INFO("COOP GRAB owner-release actor={:08X} reason={} experimental={}",g.actor,reason,g.experimental);
#endif
}
void PeerGrabTick(PPCContext& ctx,uint8_t* b) {
  bool experimental=false,client_enabled=true;
#if defined(KK_DEV_TOOLS)
  static const bool probe=[] { const auto* v=std::getenv("KK_DEV_NATIVE_GRABS"); return v && std::strcmp(v,"1")==0; }();
  static const bool client_probe=[] { const auto* v=std::getenv("KK_DEV_NATIVE_GRAB_CLIENT"); return v && std::strcmp(v,"1")==0; }();
  experimental=probe; if (experimental) client_enabled=client_probe;
#endif
  if (!Scratch::CanUse(ctx)) return;
  auto& s=Get(); auto& grab=s.peer_grab;
  const auto body=(experimental ? EnemyPartnerCombatReady():PeerGrabReady()) ? kk::PartnerBodyCombatCopy(b):0;
  const auto ai=Ptr(b,Ptr(b,body,24,8),4,88),model=Ptr(b,ai,0,4),definition=Ptr(b,model,0,8);
  const auto bytes=definition ? Load32(b,definition+4):0;
  // The native H proposal reads as far as15228. A PNJInd or another short
  // human model must never receive H's server/client fields in this fixture.
  const auto vars=bytes>=15688 && bytes<=1048576 ? Ptr(b,ai,64,bytes):0;
  const auto read=[&](uint32_t p) { return Load32(b,p); };
  if (grab.actor && (body!=grab.actor || ai!=grab.ai || vars!=grab.vars)) {
    // A disconnect, downed partner or changed lease releases only the exact
    // native event this fixture created. No copied donor handle is consumed.
    const bool same=Ptr(b,Ptr(b,grab.actor,24,8),4,88)==grab.ai && Ptr(b,grab.ai,64,15688)==grab.vars;
    if (same) ReleasePeerGrabOffer(ctx,b,"body-or-peer-unavailable");
    else s.sent_grab.active=false;
    if (grab.client && same) {
      Scratch scratch(ctx,b);
      ctx.r3.u64=grab.actor; ctx.r4.u64=grab.actor; ctx.r5.u64=20; ctx.r6.u64=grab.client;
      __imp__sub_826F9C60(ctx,b);
      if (Load32(b,grab.vars+4544)==1) Store32(b,grab.vars+4544,grab.enabled);
      const auto proposal=Load32(b,0x82CC8830);
      if (Load32(b,grab.vars+4664)==proposal) Store32(b,grab.vars+4664,grab.proposal);
#if defined(KK_DEV_TOOLS)
      REXLOG_INFO("COOP GRAB native-client-removed actor={:08X} experimental={}",grab.actor,grab.experimental);
#endif
    }
    grab={};
  }
  if (!body || !ai || !vars) {
#if defined(KK_DEV_TOOLS)
    static unsigned refused=0;
    if (body && ai && !vars && refused++<8)
      REXLOG_INFO("COOP GRAB peer-body layout-refused actor={:08X} model={:08X} vars-bytes={} required-H-bytes=15688 bridge=scorpion",body,model,bytes);
#endif
    return;
  }
  if (!grab.actor) {
    grab={body,ai,vars};
    grab.experimental=experimental;
    // The copy has independent vars but was duplicated from a story human.
    // Its old server handle may still name the donor. Create a fresh server.
    Store32(b,vars+4432,UINT32_MAX); Store32(b,vars+4436,UINT32_MAX);
    if (PoseAddress(vars,4668)) {
      const auto count=Load32(b,ai+68),events=count<=64 ? Ptr(b,ai,72,12*count):0;
      uint32_t clients=0,client_owner=0,client_native=0,active_callbacks=0;
      if (events) for (uint32_t i=0;i<count;++i) {
        const auto entry=events+12*i,fn=Ptr(b,entry,0,24);
        active_callbacks+=Load32(b,entry)!=0;
        if (fn && Load32(b,entry+4)==20) {
          ++clients; client_owner=Load32(b,entry+8); client_native=Load32(b,fn+20);
        }
      }
#if defined(KK_DEV_TOOLS)
      const auto hero=Load32(b,0x82CCBF7C),human=Ptr(b,Ptr(b,hero,24,8),4,88),hv=Ptr(b,human,64,4668);
      REXLOG_INFO("COOP GRAB peer-body-layout actor={:08X} ai={:08X} user={} class={} enabled={} proposal={:08X} events={} clients={} client-owner={:08X} client-native={:08X} hero={:08X} hero-enabled={} hero-proposal={:08X}",
          body,ai,Load32(b,vars),Load32(b,vars+4),Load32(b,vars+4544),Load32(b,vars+4664),count,clients,client_owner,client_native,
          hero,hv ? Load32(b,hv+4544):0,hv ? Load32(b,hv+4664):0);
#endif
      // AI duplicate8271DC78 clears registrations. Repeat only H init's
      // audited event20 registration, never its global player/story setup.
      // Native825491B0 and82674380 still decide acceptance and score.
      const auto client=Load32(b,0x82CC8770),proposal=Load32(b,0x82CC8830);
      if (client_enabled && !clients && (!count || (events && !active_callbacks)) && PoseAddress(client,24) && PoseAddress(proposal,24) &&
          Load32(b,client+20)==0x82670980 && Load32(b,proposal+20)==0x82674380) {
        grab.client=client; grab.enabled=Load32(b,vars+4544); grab.proposal=Load32(b,vars+4664);
        Scratch register_client(ctx,b);
        ctx.r3.u64=body; ctx.r4.u64=body; ctx.r5.u64=20; ctx.r6.u64=client;
        __imp__sub_826FB0A0(ctx,b);
        Store32(b,vars+4544,1); Store32(b,vars+4664,proposal);
        grab.client_ready=true;
#if defined(KK_DEV_TOOLS)
        REXLOG_INFO("COOP GRAB native-client-installed actor={:08X} client={:08X} proposal={:08X} vars-bytes={} native-acceptance-required=true experimental={}",body,client,proposal,bytes,experimental);
#endif
      } else if (clients==1 && client_owner==body && client_native==0x82670980 &&
          Load32(b,vars+4544)==1 && Load32(b,vars+4664)==proposal &&
          PoseAddress(proposal,24) && Load32(b,proposal+20)==0x82674380) {
        grab.client_ready=true;
      }
    }
  }
  if (!grab.client_ready && !experimental) return;
  auto prior=PeerGrabRecord(b);
  if (!experimental && !prior.target && s.status_replay.Status().grab_enemy.source) {
#if defined(KK_DEV_TOOLS)
    if (s.sent_grab.active) REXLOG_INFO("COOP GRAB owner-native-ended stage=before-poll actor={:08X} valid={} handle={:08X}/{} first-damage={}",body,prior.valid,grab.id,grab.generation,grab.first_damage);
#endif
    s.sent_grab.active=false; grab.source={}; grab.first_damage=0; return;
  }
  Scratch scratch(ctx,b);
  constexpr uint32_t globals[]={0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  uint32_t saved[4]; for (unsigned i=0;i<4;++i) saved[i]=Load32(b,globals[i]);
  Store32(b,globals[0],ai); Store32(b,globals[1],ai); Store32(b,globals[2],body); Store32(b,globals[3],body);
  ctx.r3.u64=1; ctx.r4.u64=vars+4432; ctx.r5.u64=1; ctx.r6.u64=0;
  __imp__sub_825491B0(ctx,b);
  Store32(b,vars+4604,ctx.r3.u32);
  for (unsigned i=0;i<4;++i) Store32(b,globals[i],saved[i]);
  const auto record=stimulus_debug::ReadHandle(read,vars+4432);
  if (record.valid && record.source==body && (record.id>>24)==65) {
    if (grab.id!=record.id || grab.generation!=record.generation)
      REXLOG_INFO("COOP GRAB offer available actor={:08X} id={:08X}/{} experimental={}",body,record.id,record.generation,experimental);
    grab.id=record.id; grab.generation=record.generation;
  }
  if (!record.valid || record.source!=body || (record.id>>24)!=65 || !record.target) {
#if defined(KK_DEV_TOOLS)
    if (s.sent_grab.active) REXLOG_INFO("COOP GRAB owner-native-ended stage=after-poll actor={:08X} valid={} owner={:08X} target={:08X} first-damage={}",body,record.valid,record.source,record.target,grab.first_damage);
#endif
    s.sent_grab.active=false; grab.source={}; grab.first_damage=0; return;
  }
  const auto source=s.bindings.find(record.target);
  const auto* authority=source==s.bindings.end() ? nullptr:AuthorityFor(source->second.id);
  if (!authority || !authority->Runs(Side()) || authority->dead || authority->last.life<=0 ||
      source->second.id.family!=EnemyFamily::Scorpion || !RetainedDamageSource(b,source->second)) {
    ReleasePeerGrabOffer(ctx,b,"unavailable-source"); return;
  }
  if (grab.source.actor!=source->second.actor || grab.ownership_generation!=authority->generation ||
      grab.incarnation!=s.status_replay.Status().incarnation) {
    grab.source=source->second; grab.ownership_generation=authority->generation;
    grab.incarnation=s.status_replay.Status().incarnation; grab.first_damage=0; grab.started=Now();
  }
  if (!experimental) {
    if (!grab.first_damage) { if (Now()-grab.started>combat::kMaxAge) ReleasePeerGrabOffer(ctx,b,"no-native-start-event"); }
    else if (combat::NativeGrabFeedback(s.status_replay.Status(),s.identity,grab.incarnation,
        Canonical(grab.source.id),grab.ownership_generation,grab.first_damage,grab.started,s.partner_status_time,Now())==combat::GrabFeedback::Release)
      ReleasePeerGrabOffer(ctx,b,"guest-controller-ended-or-refused");
  }
}
void AfterAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
  auto& s = Get();
  if (Environment(b,w)) { PeerGrabTick(ctx,b); LocalNativeGrabTick(ctx,b); }
#if defined(KK_DEV_TOOLS)
  if (Environment(b,w)) { DevScorpionActivity(ctx,b); EnemyThrowProbe(ctx,b); }
#endif
  if (!s.prepared || !Connected() || !Environment(b, w)) { s.driving.clear(); return; }
  Scan(b);
  Replicas(ctx, b, true);
  HoldReplicas(ctx, b, true);
#if defined(KK_DEV_TOOLS)
  if (Now() >= s.next_targets) {
    s.next_targets = Now()+1;
    const auto body = kk::PartnerBodyCombatActor(b), hero = Load32(b, 0x82CCBF7C);
    for (const auto& [actor, binding] : s.bindings) {
      const auto* authority = AuthorityFor(binding.id);
      if (!authority || !authority->Runs(Side()) || !ValidBinding(b, binding)) continue;
      uint32_t handle = 0, target = 0;
      switch (binding.id.family) {
        case EnemyFamily::Raptor: handle = 5216; break;
        case EnemyFamily::Worm: handle = 2948; break;
        case EnemyFamily::Scorpion: handle = 2992; break;
        case EnemyFamily::Centipede: handle = 5888; break;
        case EnemyFamily::Spider: handle = 1420; break;
        case EnemyFamily::Crab: handle = 1048; break;
        case EnemyFamily::Bat: target = Load32(b, binding.vars+848); break;
        case EnemyFamily::Native: target = Load32(b, binding.vars+4088); break;
        case EnemyFamily::NativeKna: target = Load32(b,binding.vars+4084); break;
        case EnemyFamily::SwampCrawler: target = Load32(b,binding.vars+556); break;
        case EnemyFamily::SwampCrawlerSmall: target = Load32(b,binding.vars+1912); break;
        case EnemyFamily::Marine: target = Load32(b,binding.vars+348); break;
        case EnemyFamily::VRex: target = Load32(b,binding.vars+516); break;
        default: continue;
      }
      if (handle && PoseAddress(binding.vars, handle+8)) {
        const auto record = stimulus_debug::ReadHandle([&](uint32_t p) { return Load32(b,p); }, binding.vars+handle);
        if (record.valid) target = record.target;
      }
      if (s.combat_targets[actor] == target) continue;
      s.combat_targets[actor] = target;
      const bool host=s.link.role==Role::Host;
      if (target == hero) { if (host) ++s.targets_host; else ++s.targets_guest; }
      if (body && target == body) { if (host) ++s.targets_guest; else ++s.targets_host; }
      REXLOG_INFO("COOP TARGET family={} actor={:08X} target={:08X} player={} choices-host={} choices-guest={} authority={}",
          uint16_t(binding.id.family), actor, target, target == hero ? "local" : body && target == body ? "partner" : "other",
          s.targets_host, s.targets_guest, s.link.role == Role::Host ? "host" : "guest");
    }
    REXLOG_INFO("COOP COMBAT level={} start={} targets-host={} targets-guest={} hits-host={} hits-guest={} guest-native-queued={} refused={} body={:08X} body-unrelayed-enemy-here={} body-unrelayed-enemy-remote={} body-world={}",
        s.identity.level, s.identity.start, s.targets_host, s.targets_guest,
        s.link.role==Role::Host ? s.hits_host+s.hits_native : s.hits_produced,
        s.link.role==Role::Host ? s.hits_produced : s.hits_host+s.hits_native, s.hits_native, s.hits_refused, body,
        s.body_enemy_here, s.body_enemy_remote, s.body_world);
  }
#endif
  {
    Frame frame{s.identity, Now(), {}};
    for (const auto& [local, r] : s.records) {
      auto* a = AuthorityFor(local); if (!a) continue;
      if (r.kind == EnemyEventKind::Spawned && a->last.life > 0 && !a->dead) {
        if (a->owner == Side()) ++s.counts.owned_me; else ++s.counts.owned_partner;
      }
      if (!a->Runs(Side()) || s.private_reseed.contains(Canonical(local))) continue;
      Record owned = r; owned.enemy.id = Canonical(local);
      owned.enemy.owner = a->owner; owned.enemy.generation = a->generation;
      a->Observe(owned.enemy);
      frame.records.push_back(owned);
    }
    PublishEnemyFrame(frame);
#if defined(KK_DEV_TOOLS)
    if (Now() >= s.next_numbers) {
      s.next_numbers = Now()+1;
      for (const auto& [id, r] : s.records) if (r.kind != EnemyEventKind::Removed)
        REXLOG_INFO("ENEMIES local role={} id={:016X}/{}/{} pos=({:.3f},{:.3f},{:.3f}) life={:.2f} action={} owner={} generation={} secondary={:.2f}",
            s.link.role == Role::Host ? "host" : "guest", id.source, id.sequence, uint16_t(id.family),
            r.enemy.x, r.enemy.y, r.enemy.z, r.enemy.life, r.enemy.action,
            r.enemy.owner == EnemyOwner::Host ? "host" : "guest", r.enemy.generation, r.enemy.secondary_life);
    }
#endif
  }
  PublishPlayerStatus(b);
  s.counts.sent = s.link.sent; SetEnemyCounters(s.counts);
  SetShotCounters(s.shots);
  if (Now() >= s.next_audit_log) {
    s.next_audit_log = Now()+10;
    ShotSummary("periodic", false);
  }
#if defined(KK_DEV_TOOLS)
  if (Now() >= s.next_log) {
    s.next_log = Now()+5;
    REXLOG_INFO("ENEMIES status role={} sent={} matched={} unmatched={} driven={} tracked={} owned-by-me={} owned-by-partner={}",
        s.link.role == Role::Host ? "host" : "guest", s.link.sent, s.counts.matched,
        s.counts.unmatched, s.counts.driven, s.records.size(), s.counts.owned_me, s.counts.owned_partner);
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
#if defined(KK_DEV_TOOLS)
  // Encounter isolation only: other enemy families otherwise kill a bat or
  // injure the fixture player before its real attack can be observed. This
  // never changes the selected family's AI, targets, contact or damage.
  static const auto only_family=[] {
    const auto* v=std::getenv("KK_DEV_ENEMY_ONLY_FAMILY");
    return v ? std::strtoul(v,nullptr,10):0ul;
  }();
  if (only_family>=1 && only_family<=unsigned(EnemyFamily::NativeKna) && s.prepared && Connected() &&
      uint32_t(ctx.lr)==0x8271D304 && Now()-s.prepared_at<=.5 && Environment(b,s.world)) {
    const auto actor=Load32(b,0x82CCBF6C);
    const auto found=s.bindings.find(actor);
    // Keep native variants together: one can complete the authored route that
    // activates the other. Isolation must not manufacture a missing encounter.
    const bool native_group=(only_family==unsigned(EnemyFamily::Native) || only_family==unsigned(EnemyFamily::NativeKna)) &&
        found!=s.bindings.end() && (found->second.id.family==EnemyFamily::Native || found->second.id.family==EnemyFamily::NativeKna);
    if (found!=s.bindings.end() && !native_group && unsigned(found->second.id.family)!=only_family &&
        found->second.id.family!=EnemyFamily::Prey && found->second.ai==Load32(b,0x82CCBF64) && ValidBinding(b,found->second)) {
      const auto* owner=AuthorityFor(found->second.id);
      if (owner && owner->Runs(Side())) {
        static std::set<uint32_t> logged;
        if (logged.size()<128 && logged.insert(actor).second)
          REXLOG_INFO("KK dev C89: other family held actor={:08X} family={} selected-family={} fixture-only=true",actor,unsigned(found->second.id.family),only_family);
        ctx.r3.u64=0; return true;
      }
    }
  }
#endif
  if (!s.prepared || s.driving.empty() || uint32_t(ctx.lr) != 0x8271D304 ||
      Now()-s.prepared_at > .5 || !Connected()) return false;
  const auto actor = Load32(b, 0x82CCBF6C), ai = Load32(b, 0x82CCBF64);
  const auto it = s.driving.find(actor); if (it == s.driving.end()) return false;
  const auto binding = s.bindings.find(actor);
  if (binding == s.bindings.end() || binding->second.ai != ai || !Connected() ||
      !Environment(b, s.world) || !ValidBinding(b, binding->second)) return false;
  WriteLife(b, binding->second, it->second.enemy);
  ctx.r3.u64 = 0; return true;
}
void Spawned(const uint8_t* b, uint32_t spawner, uint32_t source, uint32_t actor) {
  auto& s = Get();
  // Record successful native creations before the peer's load barrier opens
  // too. Connection timing must not change a spawner's immutable sequence.
  if (!REXCVAR_GET(kk_coop_enabled) || !Environment(b,s.world) || !PoseAddress(actor, 32)) return;
  auto it = s.spawners.find(spawner); if (it == s.spawners.end()) return;
  auto& sp = it->second;
  if (Ptr(b, Ptr(b, spawner, 24, 8), 4, 88) != sp.ai || Ptr(b, sp.ai, 64, 7808) != sp.vars) return;
  uint32_t wave_templates[17]{};
  for (unsigned i=0;i<17;++i) wave_templates[i]=Load32(b,sp.vars+1972+4*i);
  // 826F9154 advances/wraps V+7804 BEFORE the native factory call.
  const auto wave_index = SpawnerPreviousWaveIndex(wave_templates,Load32(b,sp.vars+7804),source);
  if (wave_templates[0] && wave_index==UINT32_MAX) return;
  const auto wave_source = wave_index<17 ? wave_templates[wave_index] : 0;
  const auto template_key=SpawnerTemplateKey(sp.key,source,Ptr(b,sp.vars,1592,32),Ptr(b,sp.vars,1776,32),wave_index,wave_source);
  if (!template_key || sp.sequence == UINT32_MAX) return;
  bool is_spawner = false; uint32_t state = 0;
  const auto family = Tracks(b, Ptr(b, Ptr(b, source, 24, 8), 4, 88), state, is_spawner);
  // C107: after a private reload this native rebirth takes over the partner
  // world's living identity of the same spawner template, without a new sequence.
  const auto adopted = replay_birth ? std::nullopt :
      private_rebirths.Take(SpawnId(sp.key, template_key, sp.sequence+1, family));
  if (!adopted) ++sp.sequence;
  const auto id = replay_birth ? replay_birth->id : adopted ? adopted->id : SpawnId(sp.key, template_key, sp.sequence, family);
  if (replay_birth && (replay_birth->birth_spawner!=sp.key || id.sequence!=sp.sequence || id.family!=family)) return;
  const auto ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88), matrix = Ptr(b, actor, 12, 64);
  if (!id.source || !ai || !matrix || s.records.size()+s.pending.size() >= kMaxEnemies || s.records.contains(id)) {
    if (adopted) private_rebirths.Remember(*adopted);
    return;
  }
  // A pooled pointer is a new generation. Preserve a tombstone for its previous ID.
  if (auto old = s.bindings.find(actor); old != s.bindings.end()) {
    auto record = s.records.find(old->second.id);
    if (record != s.records.end()) record->second.kind = EnemyEventKind::Removed;
    s.bindings.erase(old);
  }
  // The new copy may join the scheduled AI table on a later pass. Bind only
  // once that exact AI/matrix is observed there, after its normal init.
  s.pending[actor] = {id, ai, matrix, LoadF(b, matrix+48), LoadF(b, matrix+52), LoadF(b, matrix+56)};
  if (adopted) {
    // Keep the original birth evidence; the partner already holds this identity.
    auto& pending=s.pending[actor];
    pending.x=adopted->spawn_x; pending.y=adopted->spawn_y; pending.z=adopted->spawn_z;
    pending.birth_spawner=adopted->birth_spawner;
    pending.birth_template=adopted->birth_template; pending.birth_point=adopted->birth_point;
    ++private_rebirths_adopted;
    REXLOG_INFO("ENEMIES rebirth adopted role={} spawner={:016X} id={:016X}/{}/{} next-sequence={} left={}",
        SessionFlowSnapshot().host ? "host":"guest",sp.key,id.source,id.sequence,uint16_t(id.family),
        sp.sequence+1,private_rebirths.Pending());
  } else if (sp.shared_births) {
    const auto slot=replay_birth ? replay_birth->birth_template : wave_index<17 ? wave_index+2 : source==Load32(b,sp.vars+1592) ? 0u:1u;
    const auto point=replay_birth ? replay_birth->birth_point : Load32(b,sp.vars+0x1FC0);
    if (point<16) {
      auto& pending=s.pending[actor]; pending.birth_spawner=sp.key;
      pending.birth_template=uint8_t(slot); pending.birth_point=uint8_t(point);
      REXLOG_INFO("ENEMIES native birth role={} spawner={:016X} sequence={} template={} point={} family={}",
          SessionFlowSnapshot().host ? "host":"guest",sp.key,sp.sequence,slot,point,uint16_t(family));
    }
  }
}
bool SpawnerStep(PPCContext& ctx,uint8_t* b) {
  const auto view=SessionFlowSnapshot();
  if (!REXCVAR_GET(kk_coop_enabled) || !view.active || view.host || view.policy!=ChapterPolicy::Together) return false;
  auto& s=Get();
  const auto actor=Load32(b,0x82CCBF6C),ai=Load32(b,0x82CCBF64);
  const auto it=s.spawners.find(actor);
  if (it==s.spawners.end() || !it->second.shared_births || !Environment(b,s.world)) return false;
  auto& sp=it->second;
  const auto ext=Ptr(b,actor,24,42);
  if (sp.ai!=ai || Ptr(b,ext,4,88)!=ai || Ptr(b,ai,64,0x2094)!=sp.vars ||
      !Member(b,s.world,actor,true) || (Load32(b,actor+4)&0x00600000)) return false;
  ctx.r3.u32=0;
  if (!Connected() || s.link.remote.world!=s.identity) return true;
  // Preserve native post-birth visibility, wave limits and callbacks. Only the
  // choice to create a new root comes from the host's reliable lifecycle.
  if (Load16(b,ext+40)&4) { __imp__sub_826F8BC8(ctx,b); return true; }
  const Record* next=nullptr;
  for (const auto& record:s.link.remote.records) {
    const auto& e=record.enemy;
    if (e.birth_spawner!=sp.key || sp.sequence==UINT32_MAX || e.id.sequence!=sp.sequence+1) continue;
    if (next) return true; // A sequence cannot select competing templates.
    next=&record;
  }
  if (!next) return true;
  const auto e=next->enemy;
  if (!ValidEnemy(e) || e.birth_template>=19 || e.birth_point>=16) return true;
  const auto offset=e.birth_template<2 ? 1592+184*e.birth_template : 1972+4*(e.birth_template-2);
  const auto source=Ptr(b,sp.vars,offset,32);
  bool nested=false; uint32_t fn=0;
  const auto family=Tracks(b,Ptr(b,Ptr(b,source,24,8),4,88),fn,nested);
  const auto expected=SpawnId(sp.key,Mix(sp.key,offset),e.id.sequence,family);
  const auto point_count=Load32(b,sp.vars+0x1FBC);
  const auto point=Ptr(b,sp.vars,0x1ED0+4*e.birth_point,32);
  const auto matrix=Ptr(b,point,12,64);
  if (!source || nested || expected!=e.id || point_count>16 || !point || !matrix ||
      (point_count ? e.birth_point>=point_count : e.birth_point!=0)) return true;
  // A private reload restores its checkpoint effects natively. Retired roots
  // consume their ordinal without resurrecting a corpse or its birth callback.
  if (next->kind==EnemyEventKind::Removed) { sp.sequence=e.id.sequence; return true; }
  unsigned free=0;
  while (free<16 && Load32(b,sp.vars+0x1E84+4*free)) ++free;
  if (free==16 || !Scratch::CanUse(ctx) || s.records.size()+s.pending.size()>=kMaxEnemies) return true;
  // All addresses below are recovered from this local authored spawner. No
  // function, pointer or arbitrary state value is accepted from the packet.
  struct RestoreWords {
    uint8_t* b; std::vector<std::pair<uint32_t,uint32_t>> words;
    void Set(uint32_t address,uint32_t value) { words.emplace_back(address,Load32(b,address)); Store32(b,address,value); }
    ~RestoreWords() { for (auto it=words.rbegin();it!=words.rend();++it) Store32(b,it->first,it->second); }
  } restore{b};
  restore.Set(sp.vars+0x1FD4,0); // Host has already passed its timer/point watch.
  if (e.birth_template>=2) {
    Store32(b,sp.vars+0x1E7C,e.birth_template-2);
    restore.Set(sp.vars+0x1340+0xB4*(e.birth_template-2),0);
  } else {
    if (Load32(b,sp.vars+1972)) return true;
    restore.Set(sp.vars+1592,source); restore.Set(sp.vars+0x640,0);
  }
  Store32(b,sp.vars+0x1FC0,e.birth_point); Store32(b,sp.vars+0x1EC8,free);
  Store32(b,sp.vars+0x2088,1); StoreF(b,sp.vars+0x2090,-1);
  // Reenter the complete native generate function. It registers the local
  // child, applies authored colors/visibility and runs its own birth record.
  const auto saved=ctx;
  constexpr uint32_t globals[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  uint32_t values[7]; for (unsigned i=0;i<7;++i) values[i]=Load32(b,globals[i]);
  replay_birth=&e;
  __imp__sub_826F8BC8(ctx,b);
  replay_birth=nullptr;
  const auto result=ctx.r3.u32;
  for (unsigned i=0;i<7;++i) Store32(b,globals[i],values[i]);
  ctx=saved; ctx.r3.u32=result;
  return true;
}
void SpiderCopied(const uint8_t* b,uint32_t parent,uint32_t ordinal,uint32_t actor) {
  auto& s=Get();
  if (!Environment(b,s.world) || !PoseAddress(actor,32) || actor==parent) return;
  const auto source=s.authored.find(parent);
  if (source==s.authored.end() || source->second.ambiguous || source->second.family!=EnemyFamily::Spider) return;
  const auto& authored=source->second;
  const auto parent_ai=Ptr(b,Ptr(b,parent,24,8),4,88);
  if (parent_ai!=authored.ai || Ptr(b,parent,12,64)!=authored.matrix || Load32(b,0x82CCBF6C)!=parent) return;
  const auto vars=Ptr(b,parent_ai,64,1836);
  if (!vars || (Load32(b,parent)&0x20000)) return;
  const auto id=SpiderCopyId(authored.key,ordinal,Load32(b,vars+4));
  const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88),matrix=Ptr(b,actor,12,64);
  if (!id.source || !ai || !matrix || s.records.size()+s.pending.size()>=kMaxEnemies || s.records.contains(id)) return;
  if (auto old=s.bindings.find(actor);old!=s.bindings.end()) {
    if (auto record=s.records.find(old->second.id);record!=s.records.end()) record->second.kind=EnemyEventKind::Removed;
    s.bindings.erase(old);
  }
  s.pending[actor]={id,ai,matrix,LoadF(b,matrix+48),LoadF(b,matrix+52),LoadF(b,matrix+56)};
#if defined(KK_DEV_TOOLS)
  REXLOG_INFO("ENEMIES spider copy parent={:016X} ordinal={} count={} id={:016X}/{}/12",authored.key,ordinal,Load32(b,vars+4),id.source,id.sequence);
#endif
}
} // namespace
#if defined(KK_DEV_TOOLS)
void EnemyDevGameFrame(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(kk_coop_enabled) || !base) return;
  // Timers must advance even without the AI pass, a body lease or enemy matches.
  // Each injector retains its own native actor/world validation before acting.
  DevDie(ctx, base);
  DevBait(ctx, base);
}
bool EnemyDevWorldDrift(uint8_t* base) {
  auto& s=Get();
  if (!Connected() || s.link.role!=Role::Guest || !Environment(base,s.world)) return false;
  for (const auto& r:s.link.remote.records) {
    const auto& e=r.enemy;
    if (e.owner!=EnemyOwner::Host || e.life<=0 || e.life>999900) continue;
    const auto* binding=CanonicalBinding(base,e.id);
    const auto auth=s.authority.find(e.id);
    if (!binding || auth==s.authority.end() || auth->second.Frozen() || !auth->second.Accepts(e)) continue;
    const auto cell=LifeCell(base,*binding);
    if (!cell) continue;
    StoreF(base,cell,NativeLife(binding->id.family,e.life+20));
    REXLOG_INFO("KK dev desync: injected native enemy health drift {:.1f} -> {:.1f}",e.life,e.life+20);
    return true;
  }
  return false;
}
bool EnemyDevWorldGenerationDrift() {
  auto& s=Get();
  if (!Connected() || s.link.role!=Role::Guest || !ValidEnemyWorld(s.identity)) return false;
  const auto before=s.identity.serial;
  s.identity.serial=before==UINT32_MAX ? before-1 : before+1;
  REXLOG_INFO("KK dev desync: injected native enemy world visit {} -> {}",before,s.identity.serial);
  return true;
}
#endif
std::optional<PlayerCondition> LocalPlayerCondition(const uint8_t* b) {
  const auto h=ReadHumanStatus(b); return h ? std::optional{h->condition} : std::nullopt;
}
namespace {
// Jack's ground block in H vars (decomp H_exec_read_world 826A1178): 0xB18 air
// time, 0xB30 last ground position (0xB38 its z, the fall reference) and
// 0x3C50 the void flag (no floor 8.5 under the fall: death past a fall of 3).
constexpr uint32_t kAirTime=0xB18,kLastGround=0xB30,kVoid=0x3C50;
struct NativeHit { uint32_t hit=0; ShotVector point,normal; };
// Same native general query as DevBaitFloor/BC reflex: 82403E38 with all three
// query flags; the world scratch 1626C/16278 holds the hit point and normal.
NativeHit NativeRay(PPCContext& ctx,uint8_t* b,uint32_t world,uint32_t hero,uint32_t from,uint32_t dir) {
  ctx.r3.u64=hero; ctx.r4.u64=from; ctx.r5.u64=dir;
  ctx.r6.u64=UINT32_MAX; ctx.r7.u64=2; ctx.r8.u64=0; ctx.r9.u64=1;
  __imp__sub_82403E38(ctx,b);
  NativeHit h; h.hit=ctx.r3.u32;
  if (h.hit) { h.point=Vector(b,world+0x1626C); h.normal=Vector(b,world+0x16278); }
  return h;
}
int8_t NativeNav(PPCContext& ctx,uint8_t* b,Scratch& scratch,uint32_t world,const ShotVector& p) {
  const auto grid=Ptr(b,world,Load32(b,0x82CCBFC0) ? 560:556,64);
  if (!grid) return 0;
  ctx.r3.u64=grid; ctx.r4.u64=scratch.Vec(p.x,p.y,p.z,0x180); __imp__sub_8284D638(ctx,b);
  return int8_t(ctx.r3.u32&0xFF);
}
// Floor under a point, searched from `top` down over `length` (native ray,
// never Jack or the partner body). nullopt: no face, or the hit is not under it.
std::optional<ShotVector> NativeFloor(PPCContext& ctx,uint8_t* b,Scratch& scratch,uint32_t world,uint32_t hero,
    uint32_t body,float x,float y,float top,float length,float& normal_z) {
  const auto h=NativeRay(ctx,b,world,hero,scratch.Vec(x,y,top,0x100),scratch.Vec(0,0,-length,0x110));
  if (!h.hit || h.hit==hero || (body && h.hit==body) || !std::isfinite(h.point.z) || !std::isfinite(h.normal.z) ||
      std::hypot(h.point.x-x,h.point.y-y)>.05f || h.point.z>top+.01f || h.point.z<top-length-.01f) return {};
  normal_z=h.normal.z;
  return h.point;
}
revive::Probe ProbeSpot(PPCContext& ctx,uint8_t* b,Scratch& scratch,uint32_t world,uint32_t hero,uint32_t body,
    const ShotVector& spot,const ShotVector& partner_floor) {
  revive::Probe p;
  const float top=partner_floor.z+revive::kMaxStep+.2f, length=2*revive::kMaxStep+.4f;
  const auto floor=NativeFloor(ctx,b,scratch,world,hero,body,spot.x,spot.y,top,length,p.normal_z);
  if (!floor) return p;
  p.floor=true; p.floor_z=floor->z;
  const auto up=NativeRay(ctx,b,world,hero,scratch.Vec(spot.x,spot.y,floor->z+.2f,0x120),scratch.Vec(0,0,1.6f,0x130));
  p.headroom=!up.hit || (body && up.hit==body);
  // From the spot toward the partner at chest height, stopping short of the
  // partner body: a wall or rock between them refuses the spot.
  const float dx=partner_floor.x-spot.x, dy=partner_floor.y-spot.y, dz=partner_floor.z-floor->z;
  const float d=std::hypot(dx,dy);
  const float k=d>.6f ? (d-.5f)/d : 0;
  const auto sight=NativeRay(ctx,b,world,hero,scratch.Vec(spot.x,spot.y,floor->z+1,0x140),scratch.Vec(dx*k,dy*k,dz*k,0x150));
  p.sight=!sight.hit || sight.hit==hero || (body && sight.hit==body);
  p.nav=NativeNav(ctx,b,scratch,world,*floor);
  return p;
}
revive::Placement placement;
revive::VoidGuard void_guard;
// Dev only, for before/after counts in one binary: 1 = the old unchecked spot,
// 2 = the old spot with the void guard armed (proves the guard in game).
int LegacyPlacement() {
#if defined(KK_DEV_TOOLS)
  static const int legacy=[] {
    const char* v=std::getenv("KK_DEV_REVIVE_LEGACY");
    return !v ? 0 : std::strcmp(v,"1")==0 ? 1 : std::strcmp(v,"2")==0 ? 2 : 0;
  }();
  return legacy;
#else
  return 0;
#endif
}
void NativeTeleport(PPCContext& ctx,uint8_t* b,uint32_t hero,const ShotVector& position,float facing) {
  Scratch scratch(ctx,b);
  constexpr uint32_t globals[]={0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  uint32_t saved[4]; for (unsigned i=0;i<4;++i) saved[i]=Load32(b,globals[i]);
  const uint32_t trigger=ctx.r1.u32+0x100,dest=trigger+0x20,mat=trigger+0x40;
  Store32(b,trigger+4,hero); Store32(b,trigger+8,dest); Store32(b,dest+12,mat);
  const float fx=std::cos(facing),fy=std::sin(facing);
  StoreF(b,mat,fy); StoreF(b,mat+4,-fx); StoreF(b,mat+16,-fx); StoreF(b,mat+20,-fy);
  StoreF(b,mat+40,1); StoreF(b,mat+60,1);
  StoreF(b,mat+48,position.x); StoreF(b,mat+52,position.y); StoreF(b,mat+56,position.z);
  ctx.r3.u64=trigger; __imp__sub_82538B70(ctx,b);
  for (unsigned i=0;i<4;++i) Store32(b,globals[i],saved[i]);
}
// The fall and landing checks measure from 0xB30 (the last ground). After a
// move it still holds the old spot: a lower new floor reads as a long fall
// (over 5 hurts, over 8 kills on landing, 826A1178). Native code itself resets
// 0xB30 to the current position (its 0x1D88 and 0x337C paths); do the same.
void ResetGround(uint8_t* b,uint32_t vars,const ShotVector& p,bool clear_void) {
  StoreF(b,vars+kLastGround,p.x); StoreF(b,vars+kLastGround+4,p.y); StoreF(b,vars+kLastGround+8,p.z);
  if (clear_void) Store32(b,vars+kVoid,0);
}
} // namespace
revive::Place ReviveTeleport(PPCContext& ctx,uint8_t* b,const PlayerPose& partner) {
  const auto h=ReadHumanStatus(b);
  const auto world=Load32(b,0x828E8388);
  if (!h || h->condition!=PlayerCondition::Alive || !JackReady(b) || !PoseAddress(h->vars,kVoid+4) ||
      !ControllerSafe(b,h->hero) || !Scratch::CanUse(ctx) || !PoseAddress(world,0x16284) ||
      !std::isfinite(partner.x) || !std::isfinite(partner.y) || !std::isfinite(partner.z) ||
      !std::isfinite(partner.facing)) return revive::Place::Wait;
  const auto matrix=Ptr(b,h->hero,12,64);
  const ShotVector before=Vector(b,matrix+48);
  const bool grounded=LoadF(b,h->vars+kAirTime)<.1f && !Load32(b,h->vars+kVoid);
  if (const int legacy=LegacyPlacement()) {
    const auto position=revive::Beside(partner);
    NativeTeleport(ctx,b,h->hero,position,partner.facing);
    if (legacy==2) { ResetGround(b,h->vars,position,false); void_guard.Arm(before,grounded,Now()); }
    REXLOG_INFO("KK co-op placement: legacy dev spot ({:.3f},{:.3f},{:.3f}) unchecked guard={}",position.x,position.y,position.z,
        void_guard.Armed());
    return revive::Place::Spot;
  }
  const auto body=kk::PartnerBodyCombatActor(b);
  std::optional<ShotVector> chosen; unsigned index=0;
  revive::Verdict verdicts[revive::kCandidates]{};
  int8_t navs[revive::kCandidates]{};
  ShotVector partner_floor{partner.x,partner.y,partner.z};
  int8_t partner_nav=0; bool partner_grounded=false;
  {
    Scratch scratch(ctx,b);
    float normal=0;
    // The partner's own floor: up to 0.5 over and 3 under its reported height.
    if (const auto f=NativeFloor(ctx,b,scratch,world,h->hero,body,partner.x,partner.y,partner.z+.5f,3.5f,normal)) {
      partner_floor=*f; partner_grounded=true;
    }
    partner_nav=NativeNav(ctx,b,scratch,world,partner_floor);
    for (unsigned i=0;i<revive::kCandidates && !chosen;++i) {
      const auto spot=revive::Candidate(partner,partner_floor.z,i);
      const auto probe=ProbeSpot(ctx,b,scratch,world,h->hero,body,spot,partner_floor);
      verdicts[i]=revive::Judge(probe,partner_floor.z,partner_nav); navs[i]=probe.nav;
      if (verdicts[i]==revive::Verdict::Ok) { chosen=ShotVector{spot.x,spot.y,probe.floor_z+.1f}; index=i; }
    }
  }
  const auto result=placement.Step(chosen.has_value(),Now());
  if (result==revive::Place::Wait) return result;
  std::string refused;
  for (unsigned i=0;i<(chosen ? index : revive::kCandidates);++i) {
    if (!refused.empty()) refused+=',';
    refused+=revive::VerdictName(verdicts[i]);
    if (verdicts[i]==revive::Verdict::Nav) refused+=':'+std::to_string(int(uint8_t(navs[i])));
  }
  if (result==revive::Place::Stay) {
    REXLOG_INFO("KK co-op placement: no safe spot by partner ({:.3f},{:.3f},{:.3f}) floor={} nav={} for {:.0f} s; staying on native spot ({:.3f},{:.3f},{:.3f}) refused={}",
        partner.x,partner.y,partner.z,partner_grounded,int(partner_nav),revive::kPlaceWait,before.x,before.y,before.z,refused);
    return result;
  }
  NativeTeleport(ctx,b,h->hero,*chosen,partner.facing);
  ResetGround(b,h->vars,*chosen,false);
  void_guard.Arm(before,grounded,Now());
  REXLOG_INFO("KK co-op placement: spot {} radius {:.1f} turn {:.2f} at ({:.3f},{:.3f},{:.3f}) partner ({:.3f},{:.3f},{:.3f}) floor={} nav={} from ({:.3f},{:.3f},{:.3f}) guard={} refused={}",
      index,revive::kRadii[index/8],revive::CandidateTurn(index),chosen->x,chosen->y,chosen->z,partner.x,partner.y,partner.z,
      partner_grounded,int(partner_nav),before.x,before.y,before.z,void_guard.Armed(),refused.empty() ? "none" : refused);
  return result;
}
void ReviveGuardFrame(PPCContext& ctx,uint8_t* b) {
  if (!void_guard.Armed()) return;
  const auto h=ReadHumanStatus(b);
  if (!h || !PoseAddress(h->vars,kVoid+4) || !Scratch::CanUse(ctx)) return;
  const bool void_flag=h->condition!=PlayerCondition::Dead && Load32(b,h->vars+kVoid)!=0;
  const auto rescue=void_guard.Check(void_flag,Now());
  if (!rescue) return;
  const auto matrix=Ptr(b,h->hero,12,64);
  const auto at=Vector(b,matrix+48);
  const float fall_ref=LoadF(b,h->vars+kLastGround+8);
  NativeTeleport(ctx,b,h->hero,*rescue,std::atan2(LoadF(b,matrix),-LoadF(b,matrix+4)));
  ResetGround(b,h->vars,*rescue,true);
  REXLOG_WARN("KK co-op placement: void under Jack at ({:.3f},{:.3f},{:.3f}) fall-ref={:.3f}; put back on ({:.3f},{:.3f},{:.3f})",
      at.x,at.y,at.z,fall_ref,rescue->x,rescue->y,rescue->z);
}
void EnemyBeforeLoad() {
#if defined(KK_DEV_TOOLS)
  DevWakeBeforeLoad();
#endif
  if (!REXCVAR_GET(kk_coop_enabled)) return;
  private_identity=SessionPrivateReload() ? std::optional{Get().identity} : std::nullopt;
  const auto view=SessionFlowSnapshot(); const auto link=EnemyLinkSnapshot();
  if (private_identity && SessionCheckpointCatchup() && !view.host && guard_world &&
      guard_world->epoch==link.epoch && guard_world->generation==view.world.generation &&
      guard_world->world.level==view.world.level && guard_world->world.start==view.world.start) {
    private_identity=guard_world->world;
    if (*private_identity!=Get().identity)
      REXLOG_INFO("KK co-op desync: restoring guest enemy world to host level={} start={} visit={}",
          private_identity->level,private_identity->start,private_identity->serial);
  }
  guard_world.reset();
  private_spawner_sequences.clear();
  private_rebirths.Clear(); private_rebirths_adopted=0;
  private_sequences={};
  private_ownership={};
  if (private_identity) {
    const auto& s=Get();
    private_sequences={s.next_shot,s.next_damage,s.combat_epoch,s.shot_validator,s.status_replay};
    if (private_identity->level==s.identity.level && private_identity->start==s.identity.start) {
      for (const auto& [actor,sp] : s.spawners) private_spawner_sequences[sp.key]=sp.sequence;
      // C107: the partner keeps these living spawned enemies through this reload.
      for (const auto& [id,r] : s.records) {
        if (r.kind!=EnemyEventKind::Spawned) continue;
        if (const auto a=s.authority.find(id); a!=s.authority.end() && a->second.dead) continue;
        private_rebirths.Remember(r.enemy);
      }
      private_ownership={s.ownership_epoch,s.aliases,s.authority,s.ownership_out,s.committed,s.ownership_in};
    }
  }
  ClearCombat();
  ClearOwnership();
  Get().ResetWorld(); PublishEnemyFrame({}); SetEnemyCounters({}); SetShotCounters({});
}
void EnemyAfterLoad(const uint8_t* b, uint32_t game) {
  // Record every load while co-op is on, not only while Connected: the host usually loads before the guest
  // joins (Claude, run c18 17:53: "no-connected-load-identity"). Matching still waits for Connected.
  if (!REXCVAR_GET(kk_coop_enabled) || !PoseAddress(game, 28)) return;
  auto& s = Get();
  const auto level = Load32(b, game+8), start = Load32(b, game+12);
  if (level < 100 || level > 100000 || start > 10000) return;
  s.game = game;
  // A private player recovery is the same logical world/visit. Native actor
  // pointers are rebuilt, but the living peer's events keep their scope.
  const bool private_world=private_identity && private_identity->level==level;
  if (private_world) {
    s.identity=*private_identity; s.next_shot=private_sequences.shot;
    s.visits[{s.identity.level,s.identity.start}]=s.identity.serial;
    s.shot_validator=private_sequences.shots;
    const auto link=EnemyLinkSnapshot();
    if (private_ownership.epoch && private_ownership.epoch==link.epoch) {
      s.ownership_epoch=private_ownership.epoch;
      s.aliases=std::move(private_ownership.aliases);
      s.authority=std::move(private_ownership.authority);
      s.ownership_out=std::move(private_ownership.outgoing);
      s.committed=std::move(private_ownership.committed);
      s.ownership_in=std::move(private_ownership.incoming);
      for (const auto& [id,a] : s.authority) s.private_reseed.insert(id);
    }
    s.next_damage=private_sequences.damage; s.combat_epoch=private_sequences.epoch;
    s.status_replay=private_sequences.status;
    REXLOG_INFO("ENEMIES private reload level={} start={} visit={} rebirths-pending={} reseed={} aliases={}",
        s.identity.level,s.identity.start,s.identity.serial,private_rebirths.Pending(),s.private_reseed.size(),s.aliases.size());
  }
  else if (const auto flow=SessionFlowSnapshot(); flow.active && flow.start.rejoin &&
      flow.start.generation==flow.world.generation && flow.start.level==level) {
    s.identity={level,flow.start.world_start,flow.start.visit};
    s.visits[{level,s.identity.start}]=s.identity.serial;
    REXLOG_INFO("KK co-op rejoin: enemy identity level={} start={} visit={}",level,s.identity.start,s.identity.serial);
  }
  else s.identity = {level, start, ++s.visits[{level, start}]};
  // Rebirths adopt identities only inside the same logical world.
  if (!private_world) private_rebirths.Clear();
  private_identity.reset();
  private_ownership={};
  s.capture = ValidEnemyWorld(s.identity);
}
EnemyWorld EnemyCurrentWorld() { return Get().identity; }
#if defined(KK_DEV_TOOLS)
world::Json world::EnemySnapshot(const uint8_t* b,world::Capture& capture) {
  auto& s=Get(); std::map<std::string,Json> rows;
  if (!Environment(b,s.world)) capture.Error("enemy-native-world-unavailable");
  uint32_t begin=0,end=0; unsigned unbound=0,initializing_count=0;
  std::vector<Json> unresolved;
  if (!Table(b,s.world,begin,end,true)) capture.Error("enemy-native-registry-unreadable");
  for (auto p=begin;p<end;p+=8) {
    const auto actor=Load32(b,p);
    if (!PoseAddress(actor,32) || (Load32(b,actor)&0x00400001)) continue;
    const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88),matrix=Ptr(b,actor,12,64);
    bool spawner=false,initializing=false; uint32_t state=0;
    const auto family=Tracks(b,ai,state,spawner,&initializing);
    if (spawner || family==EnemyFamily::None) continue;
    if (!matrix) { ++unbound; capture.Error("enemy-native-matrix-unreadable"); continue; }
    const bool scheduled=Member(b,s.world,actor);
    EnemyId id;
    const auto binding=s.bindings.find(actor);
    if (binding!=s.bindings.end() && binding->second.ai==ai && binding->second.matrix==matrix)
      id=Canonical(binding->second.id);
    // The engine census already captured this immutable authored transform
    // before init/motion. This is not a hash of the current moving position.
    if (!id.source) {
      const auto authored=s.authored.find(actor);
      if (authored!=s.authored.end() && !authored->second.ambiguous && authored->second.ai==ai &&
          authored->second.matrix==matrix && authored->second.family==family)
        id={authored->second.key,0,family};
    }
    const auto key=Hex(id.source)+":"+std::to_string(id.sequence)+":"+std::to_string(unsigned(id.family));
    if (!id.source) {
      ++unbound; capture.Error("enemy-native-identity-unresolved");
      const auto authored=s.authored.find(actor);
      unresolved.push_back(Json::Object({{"family",unsigned(family)},{"initializing",initializing},
          {"scheduled",scheduled},{"flags",Load32(b,actor+4)},
          {"reason",authored==s.authored.end() ? "not-captured-before-initialization":authored->second.ambiguous ? "ambiguous-authored-key":"native-binding-changed"},
          {"position",Json::Array({LoadF(b,matrix+48),LoadF(b,matrix+52),LoadF(b,matrix+56)})}}));
      continue;
    }
    const auto authority=s.authority.find(id);
    const auto* a=authority==s.authority.end() ? nullptr:&authority->second;
    // Read just the field's own proven native chain. The replication pose
    // reader also validates facing/animation, which dormant bats need not have.
    Binding observed; observed.actor=actor; observed.ai=ai; observed.matrix=matrix; observed.id.family=family;
    observed.vars=Ptr(b,ai,64,LifeBytes(family));
    if (!initializing && !observed.vars) { capture.Error("enemy-native-vars-unreadable:"+key); continue; }
    // Missing authority is observable state, not a failed native read. Keep
    // 'unassigned' so a guest-only activation can produce a named mismatch.
    const auto cell=initializing ? 0:LifeCell(b,observed);
    const float native_life=initializing ? 0:StateLife(family) ? ReplicatedLife(family,state,0):cell ? LoadF(b,cell):NAN;
    if (!std::isfinite(native_life)) capture.Error("enemy-native-health-unreadable:"+key);
    const auto secondary_offset=SecondaryLifeOffset(family);
    const float secondary=initializing || !secondary_offset ? 0:LoadF(b,observed.vars+secondary_offset);
    if (!std::isfinite(secondary)) capture.Error("enemy-native-secondary-health-unreadable:"+key);
    const bool alive=ReplicatedLife(family,state,native_life)>0 && (!secondary_offset || secondary>0);
    const float x=LoadF(b,matrix+48),y=LoadF(b,matrix+52),z=LoadF(b,matrix+56);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) capture.Error("enemy-native-position-unreadable:"+key);
    initializing_count+=initializing;
    const auto row=Json::Object({{"family",unsigned(family)},{"phase",initializing ? "initializing":"initialized"},
        {"health_kind",StateLife(family) ? "native-track-lifecycle":"native-scalar"},
        {"health",initializing ? Json():Json(native_life)},{"secondary_health",initializing ? Json():Json(secondary)},
        {"alive",initializing ? Json():Json(alive)},{"owner",a ? a->owner==EnemyOwner::Host ? "host":"guest":"unassigned"},
        {"authority_present",a!=nullptr},{"owner_generation",a ? a->generation:0},{"native_state",state},
        {"scheduled",scheduled},{"position",Json::Array({x,y,z})}});
    if (!rows.emplace(key,row).second) capture.Error("enemy-id-collision:"+key);
  }
  if (unbound) capture.Error("unbound-native-enemies:"+std::to_string(unbound));
  return Json::Object({{"rows",Json::Object(rows)},{"unbound",unbound},{"initializing",initializing_count},
      {"unresolved",Json::Array(unresolved)}});
}
#endif
void EnemyGuardRecoveryWorld(EnemyWorld world) {
  guard_world.reset();
  const auto v=SessionFlowSnapshot(); const auto link=EnemyLinkSnapshot();
  if (ValidEnemyWorld(world) && v.active && !v.host && v.policy==ChapterPolicy::Together && link.connected &&
      world.level==v.world.level && world.start==v.world.start)
    guard_world=GuardWorld{world,link.epoch,v.world.generation};
}
std::vector<WorldEnemy> EnemyWorldDigest(const uint8_t* b) {
  auto& s=Get(); std::vector<WorldEnemy> out;
  if (!Connected() || !Environment(b,s.world)) return out;
  for (const auto& [local,r]:s.records) {
    auto* a=AuthorityFor(local); if (!a || r.kind==EnemyEventKind::Removed) continue;
    auto e=r.enemy;
    for (const auto& [actor,binding]:s.bindings) if (binding.id==local && ValidBinding(b,binding)) {
      Binding observed; EnemyState current;
      if (Read(b,actor,observed,current)) {
        e.life=current.life;
        e.secondary_life=current.secondary_life;
      }
      break;
    }
    if (!ValidEnemy(e)) continue;
    if (a->dead || r.kind==EnemyEventKind::Died) e.life=std::min(0.f,e.life);
    out.push_back({Canonical(local),a->owner,a->generation,e.life,e.secondary_life});
  }
  return out;
}
desync::Evidence EnemyCompareDigest(uint8_t* b,const desync::State& state,bool reconcile) {
  desync::Evidence result;
  EnemyId anchor;
  const auto missing=[&](EnemyId id) {
    result.structural=true; result.progress=Mix(Mix(result.progress,id.source),id.sequence);
    if (!anchor.source || id<anchor) {
      anchor=id;
      result.structural_anchor=Mix(Mix(Mix(14695981039346656037ull,id.source),id.sequence),uint16_t(id.family));
      if (!result.structural_anchor) result.structural_anchor=1;
    }
  };
  auto& s=Get();
  if (!Connected() || s.link.role!=Role::Guest) return result;
  if (ValidEnemyWorld(s.identity) && s.identity!=state.header.enemy_world) { result.wrong_level=true; return result; }
  if (!Environment(b,s.world)) return result;
  const auto rough=[](float a,float b) { return (a>0)!=(b>0) || std::abs(a-b)>5.f; };
  for (const auto& h:state.enemies) {
    // Host digests never override guest-owned AI or an in-flight ownership change.
    if (h.owner!=EnemyOwner::Host) continue;
    const auto* binding=CanonicalBinding(b,h.id);
    if (!binding) { if (h.life>0) missing(h.id); continue; }
    const auto auth=s.authority.find(h.id);
    if (auth==s.authority.end() || auth->second.owner!=h.owner || auth->second.generation!=h.generation ||
        auth->second.Frozen()) continue;
    Binding observed; EnemyState current;
    if (!Read(b,binding->actor,observed,current)) continue;
    const float life=current.life,secondary=current.secondary_life;
    if (!rough(h.life,life) && !rough(h.secondary_life,secondary)) continue;
    // Digest values can be old by the time the sweep completes. Repair only
    // from the normal fresh replicated stream, preserving terminal-event wins.
    const Record* latest=nullptr;
    for (const auto& r:s.link.remote.records) if (r.enemy.id==h.id && r.enemy.owner==h.owner &&
        r.enemy.generation==h.generation) { latest=&r; break; }
    if (!latest || (auth->second.dead && latest->enemy.life>0) || !s.link.connected || Now()-s.link.remote.time>.5 ||
        (!rough(latest->enemy.life,life) && !rough(latest->enemy.secondary_life,secondary))) continue;
    result.quiet=true;
    if (reconcile) WriteLife(b,*binding,latest->enemy);
  }
  std::set<EnemyId> host_ids;
  for (const auto& h:state.enemies) host_ids.insert(h.id);
  for (const auto& [actor,binding]:s.bindings) {
    const auto id=Canonical(binding.id); const auto auth=s.authority.find(id);
    if (!id.source || host_ids.contains(id) || auth==s.authority.end() || auth->second.dead ||
        auth->second.owner!=EnemyOwner::Host || auth->second.Frozen() || !ValidBinding(b,binding)) continue;
    Binding observed; EnemyState current;
    if (Read(b,actor,observed,current) && current.life>0) missing(id);
  }
  return result;
}
void EnemyGameFrame() {
  if (!Connected()) {
    // Release immediately on loss, but retain this load's identity so recovery
    // can revalidate it. The synchronous load boundary owns pointer teardown.
    auto& s = Get(); ShotSummary("session-inactive"); s.prepared = false; s.driving.clear(); SetEnemyCounters({});
    s.pending_damage.clear(); s.outgoing_damage.clear(); s.sent_status.reset(); s.partner_status_time = -1;
    s.combat_epoch = s.incarnation = 0; s.damage_replay = {};
    ClearCombat();
  }
}
bool EnemyCombatReference(const uint8_t* b, uint32_t owner, uint32_t ai, uint32_t vars, uint32_t offset) {
  auto& s = Get();
  if (!s.prepared || !Connected() || !Environment(b, s.world) ||
      Now()-s.prepared_at > .5) return false;
  // A native spear can retain the struck body or a homing target for several
  // frames. These are combat references even though projectiles are not enemy
  // snapshot actors. Require the live projectile layout and its independently
  // bound, locally simulated enemy source before exempting an exact field.
  if (Member(b,s.world,owner,true) && combat::ProjectileReference(owner,ai,vars,offset,
      [&](uint32_t p) { return Load32(b,p); },[&](uint32_t p) { return Load16(b,p); },
      [](uint32_t p,uint32_t bytes) { return PoseAddress(p,bytes); },[&](uint32_t actor) {
        const auto source=s.bindings.find(actor);
        if (source==s.bindings.end() || !RetainedDamageSource(b,source->second)) return false;
        const auto* authority=AuthorityFor(source->second.id);
        return authority && authority->Runs(Side());
      })) return true;
  const auto binding = s.bindings.find(owner);
  if (binding == s.bindings.end() || binding->second.ai != ai || binding->second.vars != vars ||
      !ValidBinding(b, binding->second)) return false;
  const auto table = combat::Candidates(binding->second.id.family);
  // Native PAF consumers retain real attackers as well as perception targets.
  // These exact fields are combat evidence, not authored story references.
  if ((binding->second.id.family==EnemyFamily::Raptor && offset==7248) ||
      (binding->second.id.family==EnemyFamily::Crab && offset==764) ||
      (binding->second.id.family==EnemyFamily::Bat && offset==1032) ||
      (binding->second.id.family==EnemyFamily::Native && offset==4384) ||
      (binding->second.id.family==EnemyFamily::NativeKna && offset==4388)) return true;
  if (binding->second.id.family==EnemyFamily::Raptor && PoseAddress(vars,7388) &&
      combat::CandidateReference(combat::CandidateTable{7296,7304,20},offset,Load32(b,vars+7296))) return true;
  if (binding->second.id.family==EnemyFamily::Raptor && (offset==5100 || offset==14192 || offset==7756)) {
    const auto cache=s.raptor_players.find(owner);
    return cache!=s.raptor_players.end() && cache->second.binding.id==binding->second.id &&
        cache->second.binding.ai==ai && cache->second.binding.vars==vars;
  }
  if (binding->second.id.family==EnemyFamily::Crab && PoseAddress(vars,1708) &&
      combat::CandidateReference(combat::CandidateTable{1536,1544,20},offset,Load32(b,vars+1536))) return true;
  if ((binding->second.id.family == EnemyFamily::Crab && offset == 1056) ||
      (binding->second.id.family == EnemyFamily::Bat && (offset == 848 || offset == 852)) ||
      (binding->second.id.family == EnemyFamily::Native && offset == 4088) ||
      (binding->second.id.family == EnemyFamily::NativeKna && offset == 4084) ||
      (binding->second.id.family == EnemyFamily::VRex && offset == 516)) return true;
  // KT_exec_zdf_zdc records actual native contacts at824, count904. These
  // are combat references, like its selected target516, rather than a story
  // claim on the leased human. The pending grab target544 remains excluded.
  if (binding->second.id.family==EnemyFamily::VRex && PoseAddress(vars,908) &&
      combat::CandidateReference(combat::CandidateTable{904,824,20},offset,Load32(b,vars+904))) return true;
  return table.limit && PoseAddress(vars, table.actors+4*table.limit) &&
      combat::CandidateReference(table, offset, Load32(b, vars+table.count));
}
bool EnemyNearPlayer(const uint8_t* b,const PlayerPose& player,float radius) {
  uint32_t begin=0,end=0;
  if (!Table(b,Load32(b,0x828E8388),begin,end)) return true;
  for (auto p=begin;p<end;p+=8) {
    Binding binding; EnemyState enemy;
    if (Read(b,Load32(b,p),binding,enemy) && enemy.life>0 &&
        std::hypot(enemy.x-player.x,enemy.y-player.y,enemy.z-player.z)<=radius) return true;
  }
  // Include the other owner's current replicas, including a briefly unscheduled
  // local actor. Missing native identity cannot make a known nearby foe safe.
  for (const auto& record : EnemyLinkSnapshot().remote.records) {
    const auto& enemy=record.enemy;
    if (record.kind!=EnemyEventKind::Removed && enemy.life>0 &&
        std::hypot(enemy.x-player.x,enemy.y-player.y,enemy.z-player.z)<=radius) return true;
  }
  return false;
}
bool EnemyPartnerCombatReady() {
  const auto& s = Get(); const auto& p = s.status_replay.Status();
  const auto flow=SessionFlowSnapshot();
  if (flow.active && flow.partner_life.downed) return false;
  return s.prepared && Connected() && s.combat_epoch == s.link.epoch &&
      p.world == s.identity && p.incarnation && p.condition != PlayerCondition::Dead &&
      combat::Fresh(s.partner_status_time, Now()) && s.link.partner && s.link.partner->level == s.identity.level;
}
uint32_t EnemyPartnerGrabAction(const uint8_t* b,uint32_t body) {
  const auto& s=Get(); const auto& p=s.status_replay.Status();
  if (!body || body!=kk::PartnerBodyCombatCopy(b) || !EnemyPartnerCombatReady() ||
      p.grab_kind!=25 || p.grab_enemy.family!=EnemyFamily::Scorpion || !p.grab_enemy.source) return UINT32_MAX;
  for (const auto& [actor,binding]:s.bindings) {
    if (Canonical(binding.id)!=p.grab_enemy || !RetainedDamageSource(b,binding)) continue;
    const auto* authority=AuthorityFor(binding.id);
    if (authority && !authority->dead && authority->last.life>0 && !authority->Frozen() &&
        authority->generation==p.grab_generation) return 0x5B; // Native H scorpion hold826C83A8.
  }
  return UINT32_MAX;
}
std::optional<uint16_t> EnemyPartnerCapabilities() {
  const auto& s=Get();
  if (!EnemyPartnerCombatReady()) return {};
  return s.status_replay.Status().capabilities;
}
bool EnemyDamageEnqueue(PPCContext& ctx, uint8_t* b, uint32_t body) {
  auto& s = Get();
  if (!REXCVAR_GET(kk_coop_enabled) || ctx.r4.u32 != 4 || !PoseAddress(ctx.r6.u32, 108)) return false;
  const auto payload = ctx.r6.u32, source = ctx.r3.u32, victim = Load32(b, payload+4);
#if defined(KK_DEV_TOOLS)
  if (s.prepared && Connected() && victim==Load32(b,0x82CCBF7C)) {
    static unsigned logs=0;
    const auto binding=s.bindings.find(source);
    if (logs++<128) REXLOG_INFO("COOP HEALTH enqueue role={} source={:08X} family={} victim={:08X} flags={:08X} magnitude={} caller={:08X} replay={}",
        s.link.role==Role::Host ? "host":"guest",source,binding==s.bindings.end() ? 0:uint16_t(binding->second.id.family),
        victim,Load32(b,payload+88),Load32(b,payload+92),uint32_t(ctx.lr),s.injecting_damage);
  }
#endif
  bool matched_source = false;
  if (s.prepared && Connected() && Environment(b, s.world) && Now()-s.prepared_at <= .5) {
    const auto it = s.bindings.find(source);
    if (it != s.bindings.end() && RetainedDamageSource(b, it->second)) {
      const auto* a = AuthorityFor(it->second.id);
      matched_source = a && !a->Runs(Side());
    }
  }
  const auto action = combat::FilterVictim(4, victim, body, Load32(b, 0x82CCBF7C),
      true, matched_source, s.injecting_damage);
#if defined(KK_DEV_TOOLS)
  if (action == combat::VictimAction::Pass && victim == Load32(b, 0x82CCBF7C) && !s.injecting_damage) {
    const auto binding = s.bindings.find(source);
    if (binding != s.bindings.end()) {
      const auto* a = AuthorityFor(binding->second.id);
      if (a && a->Runs(Side())) ++s.hits_host;
    }
  }
#endif
  if (action == combat::VictimAction::Pass) return false;
  if (action == combat::VictimAction::ProtectBody) {
    ++s.hits_produced;
    const auto binding = s.bindings.find(source);
    DamageEvent damage{s.identity, s.status_replay.Status().incarnation, s.next_damage+1, {},
        Load32(b, payload+88), Load32(b, payload+92),
        {LoadF(b, payload+24), LoadF(b, payload+28), LoadF(b, payload+32)}};
    damage.time = Now();
    damage.part = Load32(b, payload+84);
    damage.duration = float(ctx.f1.f64);
    const bool direct_impact=binding!=s.bindings.end() &&
        combat::NativeImpactProducer(binding->second.id.family,uint32_t(ctx.lr));
    damage.projectile=uint32_t(ctx.lr)==0x826345F8 || direct_impact;
    if (damage.projectile) {
      damage.point={LoadF(b,payload+48),LoadF(b,payload+52),LoadF(b,payload+56)};
      damage.surface=combat::ImpactChild(Load32(b,payload+96));
    }
    if (binding != s.bindings.end() && AuthorityFor(binding->second.id))
      damage.generation = AuthorityFor(binding->second.id)->generation;
    if (binding != s.bindings.end()) damage.enemy = Canonical(binding->second.id);
    const auto sound=s.sting_sounds.find(damage.enemy);
    if (sound!=s.sting_sounds.end()) damage.attack_sound=combat::AttackSoundObserved(damage,sound->first,
        sound->second.generation,sound->second.time,damage.time,sound->second.consumed);
    const bool audited = binding != s.bindings.end() &&
        AuthorityFor(binding->second.id) && AuthorityFor(binding->second.id)->Runs(Side()) &&
        RetainedDamageSource(b, binding->second) &&
        (uint32_t(ctx.lr)==0x8261DEEC || uint32_t(ctx.lr)==0x826345F8 || direct_impact) &&
        combat::NativeHit(binding->second.id.family, damage.flags, damage.magnitude,damage.projectile);
    const bool ready=EnemyPartnerCombatReady(), wire_valid=ValidDamageEvent(damage);
    const bool qualified = audited && ready && wire_valid;
    {
      const auto* owner_authority=binding!=s.bindings.end() ? AuthorityFor(binding->second.id):nullptr;
      const auto hit_class=combat::ClassifyBodyHit(binding!=s.bindings.end(),
          owner_authority && owner_authority->Runs(Side()),audited);
      if (hit_class==combat::BodyHit::EnemyHere) ++s.body_enemy_here;
      else if (hit_class==combat::BodyHit::EnemyRemote) ++s.body_enemy_remote;
      else if (hit_class==combat::BodyHit::World) ++s.body_world;
#if defined(KK_DEV_TOOLS)
      // One line per distinct producer and class, so an unrelayed enemy attack
      // form or a world hazard is named by its native caller in the logs.
      static std::set<uint64_t> producers;
      const uint64_t key=uint64_t(uint32_t(ctx.lr))<<8|uint8_t(hit_class);
      if (hit_class!=combat::BodyHit::Relayed && producers.size()<64 && producers.insert(key).second)
        REXLOG_INFO("COOP BODYHIT class={} caller={:08X} source={:08X} family={} flags={:08X} magnitude={} enemy-here={} enemy-remote={} world={}",
            hit_class==combat::BodyHit::EnemyHere ? "enemy-here":hit_class==combat::BodyHit::EnemyRemote ? "enemy-remote":"world",
            uint32_t(ctx.lr),source,binding==s.bindings.end() ? 0:uint16_t(binding->second.id.family),
            damage.flags,damage.magnitude,s.body_enemy_here,s.body_enemy_remote,s.body_world);
#endif
    }
#if defined(KK_DEV_TOOLS)
    if (audited && damage.projectile && Load32(b,payload+96)!=damage.surface) {
      static unsigned selectors=0;
      if (selectors++<64) {
        auto original=damage; original.surface=Load32(b,payload+96);
        REXLOG_INFO("COOP PROJECTILE native-selector family={} native={:08X} child={} original-wire={} canonical-wire={} native-low-byte-lookup=true",
            uint16_t(damage.enemy.family),original.surface,damage.surface,ValidDamageEvent(original),wire_valid);
      }
    }
    if (audited && ready && !wire_valid) {
      static unsigned refusals=0;
      if (refusals++<64)
        REXLOG_INFO("COOP PROJECTILE wire-refused family={} id={:016X}/{} world={}/{}/{} projectile={} surface={:08X} point=({},{},{}) flags={:08X} magnitude={} native-pair={}",
            uint16_t(damage.enemy.family),damage.enemy.source,damage.enemy.sequence,damage.world.level,damage.world.start,damage.world.serial,
            damage.projectile,damage.surface,damage.point.x,damage.point.y,damage.point.z,damage.flags,damage.magnitude,
            combat::NativeHit(binding->second.id.family,damage.flags,damage.magnitude,damage.projectile));
    }
#endif
    bool queued = false;
    if (qualified) {
      Message m; m.type = MessageType::DamageEvent; m.epoch = s.combat_epoch; m.damage = damage;
      // Bat contact produces its sound immediately after its hit. Defer this
      // one native call until the next normal flush so that an actual successful
      // sound creation can annotate the unsent hit. Other attacks keep their path.
      // Crab and small crawler cues follow their contacts in the same native
      // state call, with that actor running.
      const bool defer_sound=combat::NativeAttackSound(damage) && combat::CueAfterContact(damage.enemy.family) &&
          (damage.enemy.family==EnemyFamily::Bat ? source==bat_contact_actor : source==Load32(b,0x82CCBF6C));
      if (s.outgoing_damage.empty() && !defer_sound && SubmitCombat(m, Now())) { queued = true; ++s.hits_queued; }
      else if (s.outgoing_damage.size() < combat::kQueueLimit) { s.outgoing_damage.push_back({m, Now()}); queued = true; }
      if (queued) ++s.next_damage;
      if (queued && damage.attack_sound) sound->second.consumed=true;
      if (queued && combat::NativeScorpionGrabStart(damage)) {
        auto& grab=s.peer_grab;
        const auto offer=PeerGrabRecord(b);
        if (grab.actor==body && grab.client_ready && offer.valid && offer.target==source &&
            (!grab.first_damage || grab.source.actor!=source || grab.incarnation!=damage.incarnation ||
             grab.ownership_generation!=damage.generation)) {
          grab.source=binding->second; grab.incarnation=damage.incarnation; grab.first_damage=damage.number;
          grab.ownership_generation=damage.generation; grab.started=Now();
          s.sent_grab={damage.enemy,damage.incarnation,damage.number,damage.generation,true};
        }
      }
    }
    if (!queued) ++s.hits_refused;
    if (!audited) kk::PartnerBodyTriggerReference(b, payload); // unknown reference still releases auto leases
#if defined(KK_DEV_TOOLS)
    static unsigned logged = 0;
    if (logged++ < 256) REXLOG_INFO("BITE host melee-enqueue source={:08X} victim={:08X} body={:08X} flags={:08X} magnitude={} qualified={} queued={} number={} body-protected=true produced={} sent={} refused={} audited={} ready={} wire={} direction-length={:.3f} part={} duration={:.6f} generation={} caller={:08X} role={} family={} incarnation={}",
        source, victim, body, damage.flags, damage.magnitude, qualified, queued, damage.number,
        s.hits_produced, s.hits_queued, s.hits_refused, audited, ready, wire_valid,
        std::hypot(damage.direction.x,damage.direction.y,damage.direction.z), damage.part, damage.duration, damage.generation,uint32_t(ctx.lr),
        s.link.role==Role::Host ? "host" : "guest",uint16_t(damage.enemy.family),damage.incarnation);
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
bool AllowedPeerGrabClaim(const uint8_t* b,uint32_t victim,uint32_t kind) {
  const auto& grab=Get().peer_grab;
  const auto source=Get().bindings.find(Load32(b,0x82CCBF6C));
  const auto* authority=source==Get().bindings.end() ? nullptr:AuthorityFor(source->second.id);
  if (!victim || kind!=1 || victim!=grab.actor || victim!=kk::PartnerBodyCombatCopy(b) ||
      !grab.client_ready || !authority || !authority->Runs(Side()) || authority->dead || authority->last.life<=0 ||
      source->second.id.family!=EnemyFamily::Scorpion || !ValidBinding(b,source->second) || !EnemyPartnerCombatReady()) return false;
  const auto offer=PeerGrabRecord(b); const auto& p=Get().status_replay.Status();
  if (!offer.valid || (offer.target && offer.target!=source->second.actor)) return false;
  if (grab.experimental) return true;
  return PeerGrabReady() && (!p.grab_enemy.source ||
      (offer.target==source->second.actor && p.grab_enemy==Canonical(source->second.id) && p.grab_generation==authority->generation));
}
thread_local bool peer_grab_query_allowed=false;
bool HiddenPeerGrabOffer(const uint8_t* b,uint32_t handle) {
  if (peer_grab_query_allowed || !PoseAddress(handle,8)) return false;
  const auto& g=Get().peer_grab;
  if (!g.actor || Load32(b,handle)!=g.id || Load32(b,handle+4)!=g.generation) return false;
  return PeerGrabRecord(b).valid;
}
struct PeerOfferQueryScope {
  bool old;
  explicit PeerOfferQueryScope(bool allow):old(peer_grab_query_allowed) { peer_grab_query_allowed=old || allow; }
  ~PeerOfferQueryScope() { peer_grab_query_allowed=old; }
};
void FilterPeerGrabQuery(PPCContext& ctx,uint8_t* b,bool filtered) {
  const auto type=ctx.r3.u32,cursor=ctx.r4.u32,arg5=ctx.r5.u32,arg6=ctx.r6.u32;
  const auto out=filtered ? arg6:arg5;
  const auto native=filtered ? __imp__sub_8271E290:__imp__sub_8271E450;
  native(ctx,b);
  // The body owns one exact offer. It is private to the audited targeted
  // scorpion claim; unrelated native selectors must not adopt that offer.
  if (type!=65) return;
  for (unsigned n=0;n<2 && HiddenPeerGrabOffer(b,out);++n) {
    ctx.r3.u64=type; ctx.r4.u64=cursor; ctx.r5.u64=arg5; ctx.r6.u64=arg6;
    native(ctx,b);
  }
  if (HiddenPeerGrabOffer(b,out)) { Store32(b,out,UINT32_MAX); Store32(b,out+4,0); }
}
bool PartnerBodyGrabRefusal(const uint8_t* b, uint32_t victim,uint32_t kind) {
  // 82548BC0 explicitly returns zero without side effects for a null victim
  // (.28:19664 -> 82548E7C). Protect only the leased body with that native path.
  // Raptor LR8256D900 falls through to bites; Worm LR82533BA4 returns before
  // damage, and Scorpion LR825FBEAC skips attachment. No other shared combat
  // support is implied by refusing their attachment to an authored human.
  if (!victim || kk::PartnerBodyCombatActor(b)!=victim) return false;
  return !AllowedPeerGrabClaim(b,victim,kind);
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
  if (!Connected() || !Environment(b, s.world) || Now()-s.prepared_at > .5) return {};
  std::optional<ShotVector> best; float distance = 300;
  EnemyId best_id{}; uint32_t best_actor = 0;
  static const auto family=[] {
    const auto* value=std::getenv("KK_DEV_AIM_FAMILY");
    return value ? std::strtoul(value,nullptr,10):0ul;
  }();
  static const float height=[] {
    const auto* value=std::getenv("KK_DEV_AIM_HEIGHT");
    const auto f=value ? std::strtof(value,nullptr):0.f;
    return std::isfinite(f) && std::abs(f)<=5 ? f:0.f;
  }();
  for (const auto& [actor, binding] : s.bindings) {
    if (family && unsigned(binding.id.family)!=family) continue;
    const auto* a = AuthorityFor(binding.id);
    if (!a || a->dead || a->last.life <= 0 || !ValidBinding(b, binding)) continue;
    const auto m = binding.matrix;
    ShotVector point = Vector(b, m+48);
    point.z += (binding.id.family == EnemyFamily::Raptor ? 1.2f : .4f)+height;
    const float d = ShotDistance(origin, point);
    if (d > .1f && d < distance) { distance = d; best = point; best_id = a->last.id; best_actor = actor; }
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

namespace kk::coop { bool EnemySpawnerStep(PPCContext& ctx,uint8_t* base) { return SpawnerStep(ctx,base); } }
namespace kk::coop {
bool EnemyHoldDestroy(PPCContext& ctx,uint8_t* base) {
  auto& s=Get();
  if (!REXCVAR_GET(kk_coop_enabled) || !Connected() || !s.prepared ||
      Now()-s.prepared_at>.5 || !Environment(base,s.world)) return false;
  const auto it=s.bindings.find(ctx.r3.u32);
  if (it==s.bindings.end() || !ValidBinding(base,it->second)) return false;
  const auto* authority=AuthorityFor(it->second.id);
  if (!authority || authority->owner==Side() || authority->Frozen()) return false;
  for (const auto& r:s.link.remote.records) if (r.enemy.id==Canonical(it->second.id) &&
      authority->Accepts(r.enemy) && r.kind==EnemyEventKind::Removed) return false;
  // Native corpse timers vary with local visibility. Keep this replica until
  // the owner's explicit removal, while preserving the caller's stop result.
  ctx.r3.u32=it->first==Load32(base,0x82CCBF6C) ? 1:0;
  return true;
}
void EnemyDestroying(const PPCContext& ctx,uint8_t* base) {
  auto& s=Get();
  if (!REXCVAR_GET(kk_coop_enabled) || !Connected() || !Environment(base,s.world)) return;
  const auto it=s.bindings.find(ctx.r3.u32);
  if (it==s.bindings.end() || !ValidBinding(base,it->second)) return;
  const auto* authority=AuthorityFor(it->second.id);
  if (!authority || authority->owner!=Side()) return;
  if (auto record=s.records.find(it->second.id);record!=s.records.end()) {
    record->second.kind=EnemyEventKind::Removed;
    REXLOG_INFO("ENEMIES native retirement role={} id={:016X}/{}/{}",s.link.role==Role::Host ? "host":"guest",
        it->second.id.source,it->second.id.sequence,uint16_t(it->second.id.family));
  }
}
}
#if !defined(KK_DEV_TOOLS)
REX_HOOK_RAW(sub_826E1EE8) {
  if (!kk::coop::SpawnerStep(ctx,base)) __imp__sub_826E1EE8(ctx,base);
}
#endif
REX_HOOK_RAW(sub_826F8BC8) {
  if (!kk::coop::SpawnerStep(ctx,base)) __imp__sub_826F8BC8(ctx,base);
}

#if defined(KK_DEV_TOOLS)
REX_HOOK_RAW(sub_82670980) {
  const auto body=kk::PartnerBodyCombatCopy(base);
  const auto actor=kk::hfr::Load32(base,0x82CCBF6C),ai=kk::hfr::Load32(base,0x82CCBF64);
  const auto vars=body && actor==body ? kk::coop::Ptr(base,ai,64,13500):0;
  const auto kind=kk::hfr::Load32(base,0x82CCBFFC);
  const bool observe=vars && kind==1;
  const auto before=observe ? kk::coop::stimulus_debug::ReadHandle(
      [&](uint32_t p){return kk::hfr::Load32(base,p);},vars+4432):kk::coop::stimulus_debug::Record{};
  __imp__sub_82670980(ctx,base);
  static unsigned lines=0;
  if (observe && lines++<128) {
    const auto after=kk::coop::stimulus_debug::ReadHandle([&](uint32_t p){return kk::hfr::Load32(base,p);},vars+4432);
    REXLOG_INFO("COOP GRAB native-client actor={:08X} user={} class={} enabled={} proposal={:08X} protected={} before={:08X}/{} after={:08X}/{} valid={} claimed={:08X} second={:08X}",
        actor,kk::hfr::Load32(base,vars),kk::hfr::Load32(base,vars+4),kk::hfr::Load32(base,vars+4544),
        kk::hfr::Load32(base,vars+4664),kk::hfr::Load32(base,vars+13492),before.id,before.generation,after.id,after.generation,
        after.valid,after.target,after.valid ? kk::hfr::Load32(base,after.address+16):0);
  }
}
REX_HOOK_RAW(sub_8266D880) {
  static const bool trace=std::getenv("KK_DEV_PROJECTILE_TRACE")!=nullptr;
  const auto* owner=trace ? kk::coop::CombatOwner(base,kk::coop::EnemyFamily::Native):nullptr;
  if (trace && !owner) owner=kk::coop::CombatOwner(base,kk::coop::EnemyFamily::NativeKna);
  const auto binding=owner ? *owner:kk::coop::enemies::Binding{};
  const auto vector=[](uint64_t xy,uint64_t z) {
    return kk::coop::ShotVector{std::bit_cast<float>(uint32_t(xy>>32)),
        std::bit_cast<float>(uint32_t(xy)),std::bit_cast<float>(uint32_t(z>>32))};
  };
  const auto origin=vector(ctx.r4.u64,ctx.r5.u64),target=vector(ctx.r6.u64,ctx.r7.u64);
  const auto caller=uint32_t(ctx.lr),result=ctx.r3.u32;
  __imp__sub_8266D880(ctx,base);
  static unsigned lines=0;
  if (binding.actor && kk::coop::PoseAddress(result,12) && lines++<128)
    REXLOG_INFO("COOP PROJECTILE native-aim actor={:08X} family={} caller={:08X} origin=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) velocity=({:.3f},{:.3f},{:.3f})",
        binding.actor,uint16_t(binding.id.family),caller,origin.x,origin.y,origin.z,target.x,target.y,target.z,
        kk::hfr::LoadF(base,result),kk::hfr::LoadF(base,result+4),kk::hfr::LoadF(base,result+8));
}
REX_HOOK_RAW(sub_8241F0A8) {
  const auto* owner=kk::coop::CombatOwner(base,kk::coop::EnemyFamily::NativeKna);
  const auto binding=owner ? *owner:kk::coop::enemies::Binding{};
  const auto target=owner ? kk::hfr::Load32(base,binding.vars+4084):0;
  const auto launcher=owner ? kk::hfr::Load32(base,binding.vars+4092):0;
  __imp__sub_8241F0A8(ctx,base);
  static unsigned lines=0;
  if (binding.actor && lines++<128 && kk::coop::RetainedDamageSource(base,binding))
    REXLOG_INFO("COOP PROJECTILE native-throw actor={:08X} target={:08X} partner={} launcher={:08X} native-posted={} variant=KNa",
        binding.actor,target,target==kk::PartnerBodyCombatActor(base),launcher,
        launcher && !kk::hfr::Load32(base,binding.vars+4092));
}
REX_HOOK_RAW(sub_82663550) {
  __imp__sub_82663550(ctx,base);
  const auto* owner=kk::coop::CombatOwner(base,kk::coop::EnemyFamily::Bat);
  if (!owner) return;
  static std::map<uint32_t,std::pair<double,unsigned>> samples;
  if (samples.size()>=32 && !samples.contains(owner->actor)) return;
  auto& sample=samples[owner->actor];
  if (kk::coop::Now()<sample.first || sample.second++>=64) return;
  sample.first=kk::coop::Now()+2;
  const auto v=owner->vars;
  REXLOG_INFO("COOP BAT actor={:08X} state={} kind={} aggression={} target={:08X} body={:08X} attack={} flags={:08X} phase={} shot={} sound={} health={:.2f}",
      owner->actor,kk::hfr::Load32(base,v+3084),kk::hfr::Load32(base,v+4),kk::hfr::Load32(base,v+88),
      kk::hfr::Load32(base,v+848),kk::PartnerBodyCombatActor(base),kk::hfr::Load32(base,v+880),kk::hfr::Load32(base,v+928),
      kk::hfr::Load32(base,v+936),kk::hfr::Load32(base,v+7044),kk::hfr::Load32(base,v+6752),kk::hfr::LoadF(base,v+56));
}
// Observe the real scripts' activation execution, without changing their
// conditions, arguments or result. See TrigExec_SetActivity (.115:18091) and
// TrigExec_SetActiveAndVisible: +4=actor, +84=active (1 on, 0 off).
REX_HOOK_RAW(sub_8250EE50) {
  const auto record = ctx.r3.u32;
  const bool readable = kk::coop::PoseAddress(record, 88);
  const auto actor = readable ? kk::hfr::Load32(base, record+4) : 0;
  const auto active = readable ? kk::hfr::Load32(base, record+84) : 0;
  __imp__sub_8250EE50(ctx, base);
  if (readable) kk::coop::DevWakeActivity(actor, active, "SetActivity");
}
REX_HOOK_RAW(sub_8250EDE8) {
  const auto record = ctx.r3.u32;
  const bool readable = kk::coop::PoseAddress(record, 88);
  const auto actor = readable ? kk::hfr::Load32(base, record+4) : 0;
  const auto active = readable ? kk::hfr::Load32(base, record+84) : 0;
  __imp__sub_8250EDE8(ctx, base);
  if (readable) kk::coop::DevWakeActivity(actor, active, "SetActiveAndVisible");
}
#endif

// One owner for each shared hook in both dev and production builds.
REX_HOOK_RAW(sub_8270FCE0) {
  const auto actor=ctx.r3.u32,out=ctx.r4.u32,caller=uint32_t(ctx.lr);
  __imp__sub_8270FCE0(ctx,base);
  if (kk::coop::combat::RaptorViewRead(caller)) kk::coop::RaptorViewPosition(ctx,base,actor,out,caller);
}
REX_HOOK_RAW(sub_826A0920) {
  kk::coop::ObserveHumanConsumption(ctx, base);
}
REX_HOOK_RAW(sub_826C12C8) { kk::coop::ObserveHumanReflex(ctx,base); }
REX_HOOK_RAW(sub_826A1178) { kk::coop::ObserveHumanGround(ctx,base); }
REX_HOOK_RAW(sub_820D2298) {
  if (kk::coop::equipment_query_partner && ctx.r3.u32==kk::coop::equipment_query_partner) {
#if defined(KK_DEV_TOOLS)
    static unsigned count=0;
    if (count++<16) REXLOG_INFO("COOP EQUIPMENT proxy-filter body={:08X} caller={:08X}",ctx.r3.u32,uint32_t(ctx.lr));
#endif
    ctx.r3.u64=0;
    return;
  }
  __imp__sub_820D2298(ctx,base);
}
REX_HOOK_RAW(sub_82403E38) {
  const kk::coop::EquipmentQueryScope scope(base,(ctx.r9.u32&0x7FFFFFFF)==0xB && ctx.r3.u32==kk::hfr::Load32(base,0x82CCBF7C));
  __imp__sub_82403E38(ctx,base);
}
REX_HOOK_RAW(sub_82404490) {
  const kk::coop::EquipmentQueryScope scope(base,(ctx.r10.u32&0x7FFFFFFF)==0xB && ctx.r3.u32==kk::hfr::Load32(base,0x82CCBF7C));
  __imp__sub_82404490(ctx,base);
}
REX_HOOK_RAW(sub_82425038) {
  const kk::coop::EquipmentQueryScope scope(base,true);
  __imp__sub_82425038(ctx,base);
}
#if defined(KK_DEV_TOOLS)
REX_HOOK_RAW(sub_826C4E98) { kk::coop::DevEquipmentInput(ctx,base); }
#endif
REX_HOOK_RAW(sub_82548BC0) {
  if (kk::coop::PartnerBodyGrabRefusal(base, ctx.r3.u32,ctx.r4.u32)) {
#if defined(KK_DEV_TOOLS)
    static unsigned count = 0;
    if (count++ < 32) REXLOG_INFO("BITE body grab refused victim={:08X} caller={:08X} raptor-bite-fallback={}",
        ctx.r3.u32, uint32_t(ctx.lr), uint32_t(ctx.lr) == 0x8256D900);
#endif
    ctx.r3.u64 = 0;
    __imp__sub_82548BC0(ctx, base); // native null-victim return, preserving native call semantics
    return;
  }
  const kk::coop::PeerOfferQueryScope offer_scope(kk::coop::AllowedPeerGrabClaim(base,ctx.r3.u32,ctx.r4.u32));
#if defined(KK_DEV_TOOLS)
  const auto victim=ctx.r3.u32,kind=ctx.r4.u32,handle=ctx.r5.u32,caller=uint32_t(ctx.lr);
  const bool copy=victim && victim==kk::PartnerBodyCombatCopy(base);
#endif
  __imp__sub_82548BC0(ctx, base);
#if defined(KK_DEV_TOOLS)
  if (copy) {
    static unsigned lines=0;
    const auto record=kk::coop::stimulus_debug::ReadHandle([&](uint32_t p){return kk::hfr::Load32(base,p);},handle);
    if (lines++<128) REXLOG_INFO("COOP GRAB native-claim victim={:08X} kind={} caller={:08X} result={:08X} id={:08X}/{} source={:08X} target={:08X} native={}",
        victim,kind,caller,ctx.r3.u32,record.id,record.generation,record.source,record.target,record.valid);
  }
#endif
}
REX_HOOK_RAW(sub_8271E450) { kk::coop::FilterPeerGrabQuery(ctx,base,false); }
REX_HOOK_RAW(sub_8271E290) { kk::coop::FilterPeerGrabQuery(ctx,base,true); }
REX_HOOK_RAW(sub_823FCEB0) {
  const auto actor=ctx.r3.u32,resource=ctx.r4.u32,selector=ctx.r5.u32,caller=uint32_t(ctx.lr);
  __imp__sub_823FCEB0(ctx,base);
  if (ctx.r3.s32==-1 || !kk::coop::combat::RaptorBiteSound(kk::coop::EnemyFamily::Raptor,caller,resource,selector)) return;
  const auto* owner=kk::coop::CombatOwner(base,kk::coop::EnemyFamily::Raptor);
  if (!owner || owner->actor!=actor) return;
  const auto* authority=kk::coop::AuthorityFor(owner->id);
  kk::coop::Get().sting_sounds[kk::coop::Canonical(owner->id)]={authority->generation,kk::coop::Now(),false};
#if defined(KK_DEV_TOOLS)
  static unsigned lines=0;
  if (lines++<64) REXLOG_INFO("COOP SOUND owner native-bite source={:08X} generation={} code=7 selector=0 created=true caller={:08X} native-geometry-passed=true",
      actor,authority->generation,caller);
#endif
}
REX_HOOK_RAW(sub_823FCF58) {
  const auto actor=ctx.r3.u32,resource=ctx.r4.u32,caller=uint32_t(ctx.lr);
  __imp__sub_823FCF58(ctx,base);
  if (ctx.r3.s32==-1) return;
  if (resource==10 && caller==0x82654038 && actor==kk::coop::bat_contact_actor) {
    const auto* owner=kk::coop::CombatOwner(base,kk::coop::EnemyFamily::Bat);
    if (!owner || owner->actor!=actor) return;
    const auto* authority=kk::coop::AuthorityFor(owner->id);
    auto& outgoing=kk::coop::Get().outgoing_damage;
    for (auto it=outgoing.rbegin();it!=outgoing.rend();++it) {
      auto& d=it->message.damage;
      if (it->message.type!=kk::coop::MessageType::DamageEvent ||
          !kk::coop::combat::BatContactSound(d,kk::coop::Canonical(owner->id),authority->generation,caller,resource,kk::coop::Now())) continue;
      d.attack_sound=true;
#if defined(KK_DEV_TOOLS)
      REXLOG_INFO("COOP SOUND owner native-contact source={:08X} number={} generation={} code=10 created=true native-hit-unsent=true",actor,d.number,authority->generation);
#endif
      break;
    }
    return;
  }
  if (const auto after=kk::coop::combat::AfterContactSite(caller,resource); after!=kk::coop::EnemyFamily::None) {
    const auto* owner=kk::coop::CombatOwner(base,after);
    if (!owner || owner->actor!=actor) return;
    const auto* authority=kk::coop::AuthorityFor(owner->id);
    auto& outgoing=kk::coop::Get().outgoing_damage;
    for (auto it=outgoing.rbegin();it!=outgoing.rend();++it) {
      auto& d=it->message.damage;
      if (it->message.type!=kk::coop::MessageType::DamageEvent ||
          !kk::coop::combat::AfterContactSound(d,kk::coop::Canonical(owner->id),authority->generation,caller,resource,kk::coop::Now())) continue;
      d.attack_sound=true;
#if defined(KK_DEV_TOOLS)
      static unsigned lines=0;
      if (lines++<64) REXLOG_INFO("COOP SOUND owner native-after-contact source={:08X} family={} number={} generation={} code={} caller={:08X} created=true native-hit-unsent=true",
          actor,uint16_t(after),d.number,authority->generation,resource,caller);
#endif
      break;
    }
    return;
  }
  if (kk::coop::combat::CentipedeContactSound(kk::coop::EnemyFamily::Centipede,caller,resource)) {
    const auto* owner=kk::coop::CombatOwner(base,kk::coop::EnemyFamily::Centipede);
    if (!owner || owner->actor!=actor) return;
    const auto* authority=kk::coop::AuthorityFor(owner->id);
    kk::coop::Get().sting_sounds[kk::coop::Canonical(owner->id)]={authority->generation,kk::coop::Now(),false};
#if defined(KK_DEV_TOOLS)
    static unsigned lines=0;
    if (lines++<64) REXLOG_INFO("COOP SOUND owner native-centipede-leap source={:08X} generation={} code=8 created=true caller={:08X}",
        actor,authority->generation,caller);
#endif
    return;
  }
  if (resource!=8) return;
  const auto* owner=kk::coop::CombatOwner(base,kk::coop::EnemyFamily::Scorpion);
  if (!owner || owner->actor!=actor) return;
  const auto* authority=kk::coop::AuthorityFor(owner->id);
  auto& s=kk::coop::Get();
  s.sting_sounds[kk::coop::Canonical(owner->id)]={authority->generation,kk::coop::Now(),false};
#if defined(KK_DEV_TOOLS)
  static unsigned lines=0;
  if (lines++<64) REXLOG_INFO("COOP SOUND owner native-sting source={:08X} generation={} code=8 created=true",actor,authority->generation);
#endif
}
#if defined(KK_DEV_TOOLS)
REX_HOOK_RAW(sub_8271ECF8) {
  const auto& g=kk::coop::Get().peer_grab;
  const auto h=ctx.r3.u32;
  if (g.actor && kk::coop::PoseAddress(h,8) && kk::hfr::Load32(base,h)==g.id && kk::hfr::Load32(base,h+4)==g.generation) {
    static unsigned lines=0;
    if (lines++<64) REXLOG_INFO("COOP GRAB native-free owner={:08X} source={:08X} caller={:08X} running={:08X} handle={:08X}/{} first-damage={}",
        g.actor,g.source.actor,uint32_t(ctx.lr),kk::hfr::Load32(base,0x82CCBF6C),g.id,g.generation,g.first_damage);
  }
  __imp__sub_8271ECF8(ctx,base);
}
#endif
#if defined(KK_DEV_TOOLS)
REX_HOOK_RAW(sub_82614DC0) {
  const auto ai = kk::hfr::Load32(base, 0x82CCBF64);
  const auto actor = kk::hfr::Load32(base, 0x82CCBF6C);
  const auto vars = kk::coop::Ptr(base, ai, 64, 6380);
  const bool observe = vars && kk::coop::Connected() && kk::coop::Get().bindings.contains(actor);
  const float before = observe ? kk::hfr::LoadF(base, vars+5444) : -1;
  const float secondary = observe ? kk::hfr::LoadF(base, vars+5792) : -1;
  kk::coop::PlayerDamage(ctx,base,kk::coop::EnemyFamily::Scorpion,__imp__sub_82614DC0);
  if (observe && kk::coop::Ptr(base, ai, 64, 6380) == vars) {
    const float after = kk::hfr::LoadF(base, vars+5444), remaining = kk::hfr::LoadF(base, vars+5792);
    static unsigned lines = 0;
    if ((before != after || secondary != remaining) && lines++ < 128)
      REXLOG_INFO("ENEMIES scorpion consume actor={:08X} firearm={:.2f}->{:.2f} body={:.2f}->{:.2f} flags={:08X} result={} owner={}",
          actor, before, after, secondary, remaining, kk::hfr::Load32(base, vars+5400), ctx.r3.u32,
          kk::coop::Get().link.role == kk::coop::Role::Host ? "host" : "guest");
  }
}
#else
REX_HOOK_RAW(sub_82614DC0) {
  kk::coop::PlayerDamage(ctx,base,kk::coop::EnemyFamily::Scorpion,__imp__sub_82614DC0);
}
#endif
REX_HOOK_RAW(sub_8271E760) {
  const auto packed=ctx.r3.u32;
  __imp__sub_8271E760(ctx,base);
  kk::coop::PlayerDamageRecord(base,packed,ctx.r3.u32);
}
REX_HOOK_RAW(sub_8271E968) {
  const auto handle=ctx.r3.u32;
  __imp__sub_8271E968(ctx,base);
  kk::coop::PlayerDamageHandle(base,handle);
}
REX_HOOK_RAW(sub_8271EBE8) {
  const auto handle=ctx.r3.u32;
  __imp__sub_8271EBE8(ctx,base);
  kk::coop::PlayerDamageHandle(base,handle);
}
REX_HOOK_RAW(sub_825CE478) { kk::coop::PlayerDamage(ctx,base,kk::coop::EnemyFamily::Raptor,__imp__sub_825CE478); }
REX_HOOK_RAW(sub_826627D0) { kk::coop::PlayerDamage(ctx,base,kk::coop::EnemyFamily::Bat,__imp__sub_826627D0); }
REX_HOOK_RAW(sub_82626618) { kk::coop::PlayerDamage(ctx,base,kk::coop::EnemyFamily::Bat,__imp__sub_82626618,2); }
REX_HOOK_RAW(sub_8263F790) { kk::coop::PlayerDamage(ctx,base,kk::coop::EnemyFamily::Crab,__imp__sub_8263F790); }
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
#endif
REX_HOOK_RAW(sub_8254A8F8) {
  kk::coop::PlayerPriority(base,kk::coop::EnemyFamily::Raptor);
#if defined(KK_DEV_TOOLS)
  kk::coop::DevRaptorSelection(ctx, base);
#else
  __imp__sub_8254A8F8(ctx,base);
#endif
  kk::coop::RaptorPlayer(base,false);
}
REX_HOOK_RAW(sub_82572E40) { __imp__sub_82572E40(ctx,base); kk::coop::RaptorPlayer(base,true); }
REX_HOOK_RAW(sub_8255C9D8) { kk::coop::RaptorPlayer(base,false); __imp__sub_8255C9D8(ctx,base); }
REX_HOOK_RAW(sub_825ED648) {
  kk::coop::PlayerPriority(base,kk::coop::EnemyFamily::Scorpion);
  __imp__sub_825ED648(ctx,base);
}
REX_HOOK_RAW(sub_825EDC70) {
  kk::coop::PlayerPriority(base,kk::coop::EnemyFamily::Spider);
  __imp__sub_825EDC70(ctx,base);
}
REX_HOOK_RAW(sub_825043E0) {
  kk::coop::PlayerPriority(base,kk::coop::EnemyFamily::Worm);
  __imp__sub_825043E0(ctx,base);
}
REX_HOOK_RAW(sub_82634978) { kk::coop::CrabPlayerTarget(ctx,base); }
REX_HOOK_RAW(sub_8265BF60) { kk::coop::CrabFight(ctx,base); }
REX_HOOK_RAW(sub_8266F740) { kk::coop::NativePlayerTarget(ctx,base); }
REX_HOOK_RAW(sub_824509E0) { kk::coop::NativePlayerTarget(ctx,base,true); }
REX_HOOK_RAW(sub_8242DB60) { kk::coop::NativePlayerReflex(ctx,base,true); }
REX_HOOK_RAW(sub_82443F10) { kk::coop::NativePlayerAuthoredState(ctx,base,true,__imp__sub_82443F10); }
REX_HOOK_RAW(sub_8243EB40) { kk::coop::NativePlayerAuthoredState(ctx,base,true,__imp__sub_8243EB40); }
REX_HOOK_RAW(sub_8243ED78) { kk::coop::NativePlayerAuthoredState(ctx,base,true,__imp__sub_8243ED78); }
REX_HOOK_RAW(sub_826131D8) { kk::coop::NativePlayerAuthoredState(ctx,base,false,__imp__sub_826131D8); }
REX_HOOK_RAW(sub_82613428) { kk::coop::NativePlayerAuthoredState(ctx,base,false,__imp__sub_82613428); }
REX_HOOK_RAW(sub_824EF8A0) { kk::coop::BatPlayerTarget(ctx,base); }
REX_HOOK_RAW(sub_82658ED8) { kk::coop::BatPlayerFight(ctx,base,__imp__sub_82658ED8); }
REX_HOOK_RAW(sub_82652FF8) { kk::coop::BatPlayerFight(ctx,base,__imp__sub_82652FF8); }
REX_HOOK_RAW(sub_824ECBC0) { kk::coop::BatNpcLink(ctx,base); }
REX_HOOK_RAW(sub_8260EB68) { kk::coop::NativePlayerReflex(ctx,base); }
REX_HOOK_RAW(sub_82617858) { kk::coop::NativePlayerLaunch(ctx,base); }
REX_HOOK_RAW(sub_82456778) { kk::coop::VRexPlayerTarget(ctx,base); }
REX_HOOK_RAW(sub_825F2890) {
#if defined(KK_DEV_TOOLS)
  const auto* owner=kk::coop::CombatOwner(base,kk::coop::EnemyFamily::Native);
  if (owner) {
    static unsigned lines=0;
    if (lines++<128) REXLOG_INFO("COOP PROJECTILE native-throw actor={:08X} target={:08X} partner={} launcher={:08X} scripted={}",
        owner->actor,kk::hfr::Load32(base,owner->vars+4088),
        kk::hfr::Load32(base,owner->vars+4088)==kk::PartnerBodyCombatActor(base),
        kk::hfr::Load32(base,owner->vars+4096),kk::hfr::Load32(base,owner->vars+308));
  }
#endif
  __imp__sub_825F2890(ctx,base);
}
REX_HOOK_RAW(sub_8261DE68) {
  kk::coop::ContactVictim(ctx,base);
#if defined(KK_DEV_TOOLS)
  if (REXCVAR_GET(kk_coop_enabled) && kk::coop::EnemyConnectionReady()) {
    static unsigned count = 0;
    if (count++ < 256) REXLOG_INFO("BITE melee-builder lr={:08X} source={:08X} victim={:08X} flags={:08X} magnitude={}",
        uint32_t(ctx.lr), ctx.r6.u32, ctx.r8.u32, ctx.r5.u32, ctx.r9.u32);
  }
#endif
  __imp__sub_8261DE68(ctx, base);
}
REX_HOOK_RAW(sub_8271D498) {
  const uint32_t world = ctx.r3.u32;
#if defined(KK_DEV_TOOLS)
  kk::DevSampleTick(&ctx, base, world);
#endif
  if (uint32_t(ctx.lr)==0x82881CF0) kk::coop::RestoreRaptorPlayers(base);
  kk::PartnerBodyBeforeAi(ctx, base, world);
  kk::coop::ObjectsBeforeAi(ctx,base,world);
  kk::coop::BeforeAi(ctx, base, world);
  __imp__sub_8271D498(ctx, base);
  kk::PartnerBodyAfterAi(ctx, base, world);
  kk::coop::AfterAi(ctx, base, world);
  kk::coop::ObjectsAfterAi(ctx,base,world);
}
REX_HOOK_RAW(sub_8271CCB8) {
  if (kk::coop::Hold(ctx, base)) return;
  if (kk::PartnerBodyHold(ctx, base)) return;
  __imp__sub_8271CCB8(ctx, base);
}
REX_HOOK_RAW(sub_82715AC8) {
  const bool observe = uint32_t(ctx.lr) == 0x826F9240 && REXCVAR_GET(kk_coop_enabled);
  // Verified 82617130: r27 is the zero-based loop index, r3 is the parent.
  const bool spider_copy = uint32_t(ctx.lr)==0x82617434 && REXCVAR_GET(kk_coop_enabled);
  const auto spider_ordinal=ctx.r27.u32;
  const auto source = ctx.r3.u32;
  const auto spawner = observe ? kk::hfr::Load32(base, 0x82CCBF6C) : 0;
#if defined(KK_DEV_TOOLS)
  const auto caller = uint32_t(ctx.lr), current = kk::hfr::Load32(base, 0x82CCBF6C);
  bool is_spawner = false; uint32_t state = 0;
  const auto source_family = kk::coop::Tracks(base,
      kk::coop::Ptr(base, kk::coop::Ptr(base, source, 24, 8), 4, 88), state, is_spawner);
  const bool crab = REXCVAR_GET(kk_coop_enabled) && source_family == kk::coop::EnemyFamily::Crab;
#endif
  __imp__sub_82715AC8(ctx, base);
  if (spider_copy && ctx.r3.u32) kk::coop::SpiderCopied(base,source,spider_ordinal,ctx.r3.u32);
#if defined(KK_DEV_TOOLS)
  static std::map<uint32_t,unsigned> factory_lines;
  if (REXCVAR_GET(kk_coop_enabled) && source_family!=kk::coop::EnemyFamily::None && ctx.r3.u32 && factory_lines[caller]++<24)
    REXLOG_INFO("ENEMIES factory caller={:08X} parent={:08X} template={:08X} child={:08X} family={} known-spawner={} observed={}",
        caller,current,source,ctx.r3.u32,uint16_t(source_family),kk::coop::Get().spawners.contains(current),observe);
  static unsigned crab_lines = 0;
  if (crab && ctx.r3.u32 && crab_lines++ < 16) {
    const auto vars = kk::coop::Ptr(base, kk::coop::Ptr(base, kk::coop::Ptr(base, current, 24, 8), 4, 88), 64, 7808);
    REXLOG_INFO("COOP crab factory caller={:08X} current={:08X} source={:08X} child={:08X} observed={} known-spawner={} primary={:08X} secondary={:08X} wave-index={} wave-first={:08X} cap=16",
        caller, current, source, ctx.r3.u32, observe, kk::coop::Get().spawners.contains(current),
        vars ? kk::hfr::Load32(base, vars+1592) : 0, vars ? kk::hfr::Load32(base, vars+1776) : 0,
        vars ? kk::hfr::Load32(base, vars+7804) : UINT32_MAX, vars ? kk::hfr::Load32(base, vars+1972) : 0);
  }
#endif
  if (observe && ctx.r3.u32) kk::coop::Spawned(base, spawner, source, ctx.r3.u32);
}
REX_HOOK_RAW(sub_82696A28) {
  if (kk::PartnerBodySuppressShot(base)) {
    // Shot intents already own damage. A proxy may play recoil but must not
    // spend another player's ammo or enqueue another hit through an event.
    ctx.r3.u64=0;
    return;
  }
#if defined(KK_DEV_TOOLS)
  // Controlled C88 encounter fixture: story allies otherwise kill the test
  // enemies before the guest can reach them. This never changes player fire,
  // creature attacks, collision, damage, life or the shipping build.
  static const bool hold_npcs=[] {
    const auto* value=std::getenv("KK_DEV_NPC_HOLD_FIRE"); return value && std::strcmp(value,"1")==0;
  }();
  if (hold_npcs && kk::coop::EnemyConnectionReady()) {
    const auto actor=kk::hfr::Load32(base,0x82CCBF6C), hero=kk::hfr::Load32(base,0x82CCBF7C);
    const auto ai=kk::coop::Ptr(base,kk::coop::Ptr(base,actor,24,8),4,88);
    const auto hero_ai=kk::coop::Ptr(base,kk::coop::Ptr(base,hero,24,8),4,88);
    const auto model=kk::coop::Ptr(base,ai,0,16);
    if (actor!=hero && model && model==kk::coop::Ptr(base,hero_ai,0,16)) {
      static unsigned count=0;
      if (count++<16) REXLOG_INFO("KK dev C88: story ally firearm held actor={:08X} fixture-only=true",actor);
      ctx.r3.u64=0; return;
    }
  }
#endif
  const char* reason = kk::coop::BeginShot(base);
  const bool observe = reason == nullptr;
  const auto game = kk::hfr::Load32(base, 0x82CC97C0);
  const auto weapon = kk::coop::PoseAddress(game, 13652) ? kk::hfr::Load32(base, game+13128) : 0;
  constexpr uint32_t ammo[] = {13432, 13504, 13576, 13648};
  const bool armed = weapon >= 1 && weapon <= 4;
  const auto before = armed ? kk::hfr::Load32(base, game+ammo[weapon-1]) : 0;
  const auto hero = kk::hfr::Load32(base, 0x82CCBF7C);
  const auto hero_ai=kk::coop::Ptr(base,kk::coop::Ptr(base,hero,24,8),4,88);
  const bool own_jack=REXCVAR_GET(kk_coop_enabled) && hero_ai &&
      kk::hfr::Load32(base,0x82CCBF6C)==hero && kk::hfr::Load32(base,0x82CCBF64)==hero_ai;
  kk::coop::puppet::FireCapture visual_shot(own_jack);
#if defined(KK_DEV_TOOLS)
  const bool guest = kk::coop::EnemyLinkSnapshot().role == kk::coop::Role::Guest;
  const bool host_jack = !guest && kk::coop::EnemyConnectionReady() && kk::coop::PoseAddress(hero,32) &&
      kk::hfr::Load32(base,0x82CCBF6C) == hero;
  const auto caller = uint32_t(ctx.lr);
#endif
  {
    const kk::coop::EquipmentQueryScope query_scope(base,true);
    __imp__sub_82696A28(ctx, base);
  }
  const bool same = game == kk::hfr::Load32(base, 0x82CC97C0);
  const auto after = same && armed ? kk::hfr::Load32(base, game+ammo[weapon-1]) : before;
  // Appearance must survive a pending enemy load identity, a missed ray or a
  // refused damage packet. Native evidence still gives exactly one visual.
  if (visual_shot.Fired(same,after<before)) kk::coop::PartnerAnimationShot(uint8_t(weapon));
  if (observe) {
    reason = kk::coop::EndShot(base, same && after < before);
  }
  if (kk::coop::Get().link.role == kk::coop::Role::Guest &&
      ((same && after < before) || (observe && kk::coop::Get().shot_resolved)))
    kk::coop::ShotSendLog(reason ? reason : (kk::coop::Get().shot.hits.empty() ? "queued-miss" : "queued-hit"), reason == nullptr);
#if defined(KK_DEV_TOOLS)
  kk::coop::DevShotLog(base, guest, host_jack, observe, weapon, before, after, caller, ctx.r3.u32, reason);
#endif
}
REX_HOOK_RAW(sub_8243A3C0) {
  const auto origin = ctx.r3.u32, point = ctx.r4.u32, object = ctx.r6.u32, caller = uint32_t(ctx.lr);
  __imp__sub_8243A3C0(ctx, base);
  kk::coop::puppet::FireCapture::Ray(caller);
  kk::coop::ShotRay(base, origin, point, object, caller);
}
REX_HOOK_RAW(sub_82634428) {
#if defined(KK_DEV_TOOLS)
  static const bool trace=std::getenv("KK_DEV_PROJECTILE_TRACE")!=nullptr;
  const auto source=ctx.r6.u32,victim=ctx.r8.u32,result=ctx.r3.u32,flags=ctx.r5.u32,magnitude=ctx.r10.u32;
  const auto caller=uint32_t(ctx.lr),projectile=kk::hfr::Load32(base,0x82CCBF6C);
  const auto body=trace ? kk::PartnerBodyCombatActor(base):0;
  const auto enemy=kk::coop::Get().bindings.find(source);
  const bool observe=trace && kk::coop::EnemyConnectionReady() &&
      enemy!=kk::coop::Get().bindings.end() && kk::coop::RetainedDamageSource(base,enemy->second);
  const auto family=observe ? uint16_t(enemy->second.id.family):0;
#endif
  kk::coop::ObjectsFirearm(ctx,base);
  if (!kk::coop::ForwardShotEvent(ctx, base)) __imp__sub_82634428(ctx, base);
#if defined(KK_DEV_TOOLS)
  static unsigned lines=0;
  if (observe && lines++<256) {
    const auto event=kk::coop::stimulus_debug::ReadHandle([&](uint32_t p){return kk::hfr::Load32(base,p);},result);
    REXLOG_INFO("COOP PROJECTILE native-impact source={:08X} family={} projectile={:08X} victim={:08X} partner={} flags={:08X} magnitude={} caller={:08X} local-queued={} id={:08X}/{}",
        source,family,projectile,victim,body && victim==body,flags,magnitude,caller,event.valid,event.id,event.generation);
  }
#endif
}

// C101 second line of defence. BC_exec_check_vision appends every visible type 1
// event to a list with room for 74 (vars+2492, count +2484) and no bound. Never
// run it when more are visible; the native scan would overwrite the bat's own
// state and player pointer (26 Oct 21:03 crash, guest 0100005C).
REX_HOOK_RAW(sub_8262FE20) {
  if (REXCVAR_GET(kk_coop_enabled)) {
    namespace eb=kk::coop::event_budget;
    const auto count=eb::Count([&](uint32_t p) { return kk::hfr::Load32(base,p); },1,0);
    auto& stats=eb::Vision(); ++stats.scans; stats.max_visible=std::max(stats.max_visible,count.enumerable);
    if (count.valid && !eb::DevReplay26a() && !eb::VisionScanSafe(count.enumerable)) {
      ++stats.skipped;
      static unsigned logged=0;
      if (logged++<16) REXLOG_WARN("KK co-op bat vision: scan skipped, {} visible type 1 events exceed its list of {} (actor={:08X})",
          count.enumerable,eb::kVisionListRoom,kk::hfr::Load32(base,0x82CCBF6C));
      ctx.r3.u64=0;
      return;
    }
  }
  __imp__sub_8262FE20(ctx,base);
}
