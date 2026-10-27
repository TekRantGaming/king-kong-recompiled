// Developer-only (kk-decomp bug traces): what the game's own scripts do when three original bugs happen.
// What each log answers and how to read it: kk-decomp docs/bugs/*.md and port-traces/README.md.
//
// KK_DEV_BUG_TRACE=vrex,waves,checkpoint (any of them, or 1 / all for everything):
//   vrex        bug 1, the V-Rex freezes against the wall (Call Kong): the KT model's charge, skid and wait
//               states and the trigger that orders the charge. Also warns when a charge stops moving.
//   waves       raptor waves that do not appear (Sacrifice, Jimmy): every Spawner's counters and the 16 enemies
//               it is waiting on, its init (progress bit), its stop, and every destroy of one of its enemies.
//   checkpoint  bug 4, Hayes companions left behind: what a checkpoint tests when it fires, and where Hayes
//               and the second companion are. Needs the one line call added to pause_hooks.cpp's
//               ES_CheckPoint hook (port-traces/patches/pause_hooks.patch).
// Lines start with "KK dev bug". All addresses are the 360 build's (kk-decomp data/names.tsv).
#if defined(KK_DEV_TOOLS)
#include "coop_enemies_game.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"

namespace kk {
double DevSecondsSinceMenu();
void DevBugCheckpoint(uint8_t* base, uint32_t self, int32_t before, int32_t after);

namespace bugtrace {
namespace {

// Script function names (the game's own name tables), by native code address.
struct Name {
  uint32_t addr;
  const char* name;
};
constexpr Name kNames[] = {
#include "dev_bug_trace_names.inc"
};

const char* NameOf(uint32_t code) {
  auto it = std::lower_bound(std::begin(kNames), std::end(kNames), code,
                             [](const Name& n, uint32_t a) { return n.addr < a; });
  return (it != std::end(kNames) && it->addr == code) ? it->name : nullptr;
}

bool Has(const char* what) {
  static const std::string v = [] {
    const char* e = std::getenv("KK_DEV_BUG_TRACE");
    return e ? std::string(e) : std::string();
  }();
  if (v.empty()) return false;
  return v == "1" || v == "all" || v.find(what) != std::string::npos;
}
bool Vrex() {
  static const bool on = Has("vrex");
  return on;
}
bool Waves() {
  static const bool on = Has("waves");
  return on;
}
bool Checkpoint() {
  static const bool on = Has("checkpoint");
  return on;
}

// Guest memory reads that tolerate a bad pointer (the trace must never crash the game).
bool Ok(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && (a & 3) == 0; }
uint32_t W(const uint8_t* base, uint32_t a) { return Ok(a) ? hfr::Load32(base, a) : 0; }
uint16_t H(const uint8_t* base, uint32_t a) {
  return (a >= 0x40000000 && a < 0xA0000000 && (a & 1) == 0) ? hfr::Load16(base, a) : 0;
}
float F(const uint8_t* base, uint32_t a) { return Ok(a) ? hfr::LoadF(base, a) : NAN; }

constexpr uint32_t kRunningObject = 0x82CCBF6C;  // the object whose AI is running
constexpr uint32_t kRunningAi = 0x82CCBF64;      // its AI instance
constexpr uint32_t kFlagsBlock = 0x82CC864C;     // Spawner's link: vars block holding the progress bits

struct Pos {
  float x, y, z;
};
// Object +0x0C is its matrix; the position is at +0x30 .. +0x38 (as KT_exec_check_msg reads it).
Pos PosOf(const uint8_t* base, uint32_t obj) {
  const uint32_t m = W(base, obj + 0x0C);
  return {F(base, m + 0x30), F(base, m + 0x34), F(base, m + 0x38)};
}
float Dist(const Pos& a, const Pos& b) {
  return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}
uint32_t AiOf(const uint8_t* base, uint32_t obj) { return W(base, W(base, obj + 0x18) + 4); }
uint32_t VarsOf(const uint8_t* base, uint32_t ai) { return W(base, ai + 0x40); }

// The current function of an AI track (instance +0x18 + 4 * track); its native code is at function +0x14.
std::string TrackName(const uint8_t* base, uint32_t ai, int track) {
  const uint32_t fn = W(base, ai + 0x18 + 4 * track);
  if (!fn) return "-";
  const uint32_t code = W(base, fn + 0x14);
  if (const char* n = NameOf(code)) return n;
  return fmt::format("fn {:08X} code {:08X}", fn, code);
}

std::mutex g_mutex;

// True when the signature changed or `every` seconds passed since the last log for this key.
bool Due(uint64_t key, double every, const std::string& sig) {
  struct Last {
    double t;
    std::string sig;
  };
  static std::unordered_map<uint64_t, Last> last;
  const double now = DevSecondsSinceMenu();
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = last.find(key);
  if (it != last.end() && it->second.sig == sig && now - it->second.t < every) return false;
  last[key] = {now, sig};
  return true;
}

// ---- bug 1: V-Rex (KT model) ----

void VrexNote(uint8_t* base, const char* state, uint32_t obj, uint32_t ai, uint32_t ret) {
  const uint32_t v = VarsOf(base, ai);
  const Pos p = PosOf(base, obj);
  const std::string t2 = TrackName(base, ai, 2);
  const float charge_time = F(base, v + 0x3B4);

  // Stuck detector: still charging after 4 s and moved less than 0.5 in the last second.
  {
    struct Mark {
      double t;
      Pos p;
    };
    static std::unordered_map<uint32_t, Mark> marks;
    const double now = DevSecondsSinceMenu();
    bool warn = false;
    float moved = 0.0f;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      auto& m = marks[obj];
      if (now - m.t >= 1.0) {
        moved = Dist(p, m.p);
        warn = m.t > 0.0 && std::string(state) == "KT_ETAT_charge" && charge_time > 4.0f && moved < 0.5f;
        m = {now, p};
      }
    }
    if (warn)
      REXLOG_WARN("KK dev bug vrex: {:.2f} s obj {:08X} STILL CHARGING after {:.1f} s and moved {:.2f} in 1 s "
                  "(possible softlock) target {:08X} impact {} contact {:08X} 5F8 {:08X}",
                  now, obj, charge_time, moved, W(base, v + 0x38C), W(base, v + 0x390), W(base, v + 0x3A0),
                  W(base, v + 0x5F8));
  }

  const std::string sig = fmt::format("{}|{}|{}|{:08X}|{:08X}", state, t2, W(base, v + 0x390), W(base, v + 0x38C),
                                      W(base, v + 0x3A0));
  if (!Due(obj, 0.5, sig)) return;
  REXLOG_INFO("KK dev bug vrex: {:.2f} s {} obj {:08X} ret {} pos ({:.2f}, {:.2f}, {:.2f}) track2 {} | target 38C "
              "{:08X} 204 {:08X} impact 390 {} contact 3A0 {:08X} time 3B4 {:.2f} 5F8 {:08X} BB0 {} grab 2A4 {:08X}",
              DevSecondsSinceMenu(), state, obj, int32_t(ret), p.x, p.y, p.z, t2, W(base, v + 0x38C),
              W(base, v + 0x204), W(base, v + 0x390), W(base, v + 0x3A0), charge_time, W(base, v + 0x5F8),
              W(base, v + 0xBB0), W(base, v + 0x2A4));
}

// ---- waves: Spawner ----

std::unordered_map<uint32_t, uint32_t> g_enemy_spawner;  // enemy object -> the spawner holding it (g_mutex)

struct Bit {
  uint32_t on, n;
  bool done;
};
// Spawner vars +0 = progress bit on, +4 = bit number; bit n of (block + 0xC08 + (n / 32) * 4); numbers from
// 15000 mean "done when bit n - 15000 is clear" (Spawner_init 0x826EB8C0).
Bit ProgressBit(const uint8_t* base, uint32_t v) {
  const uint32_t on = W(base, v), n = W(base, v + 4);
  const uint32_t block = W(base, kFlagsBlock);
  bool done = false;
  if (on && int32_t(n) > 0) {
    const uint32_t m = n < 15000 ? n : n - 15000;
    const uint32_t word = W(base, block + 0xC08 + (m >> 5) * 4);
    const bool set = (word >> (m & 31)) & 1;
    done = n < 15000 ? set : !set;
  }
  return {on, n, done};
}

void SpawnerNote(uint8_t* base, uint32_t spawner, uint32_t ai) {
  const uint32_t v = VarsOf(base, ai);
  std::string sig = fmt::format("{} {} {} {} {}", W(base, v + 0x470), W(base, v + 0x474), W(base, v + 0x3B4),
                                W(base, v + 0x2F4), F(base, v + 0x2090) > 0.0f);
  uint32_t slots[16];
  for (int i = 0; i < 16; ++i) {
    slots[i] = W(base, v + 0x1E84 + 4 * i);
    if (slots[i]) {
      const uint32_t flags = W(base, slots[i] + 4);
      const uint16_t ext = H(base, W(base, slots[i] + 0x18) + 0x28);
      sig += fmt::format(" {:08X}:{}{}", slots[i], (flags & 0x400000) ? 'i' : 'a', (ext & 0x8000) ? 'd' : 'l');
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t e : slots)
      if (e) g_enemy_spawner[e] = spawner;
  }
  if (!Due(spawner, 2.0, sig)) return;
  const Bit bit = ProgressBit(base, v);
  REXLOG_INFO("KK dev bug waves: {:.2f} s spawner {:08X} alive 470 {} max 3B0 {} wave 3B4 {} made 474 {} waitdead "
              "3B8 {} waves left 2F4 {} pause 2090 {:.2f} deadtest 6F8 {:08X} bit on {} n {} done {} track2 {}",
              DevSecondsSinceMenu(), spawner, W(base, v + 0x470), W(base, v + 0x3B0), W(base, v + 0x3B4),
              W(base, v + 0x474), W(base, v + 0x3B8), int32_t(W(base, v + 0x2F4)), F(base, v + 0x2090),
              W(base, v + 0x6F8), bit.on, bit.n, bit.done, TrackName(base, ai, 2));
  for (int i = 0; i < 16; ++i) {
    const uint32_t e = slots[i];
    if (!e) continue;
    const uint32_t flags = W(base, e + 4);
    const uint16_t ext = H(base, W(base, e + 0x18) + 0x28);
    const Pos p = PosOf(base, e);
    REXLOG_INFO("KK dev bug waves:   slot {} obj {:08X} flags {:08X} inactive {} destroyed {} dead8000 {} pos "
                "({:.2f}, {:.2f}, {:.2f}) track2 {}",
                i, e, flags, (flags & 0x400000) != 0, (flags & 0x200000) != 0, (ext & 0x8000) != 0, p.x, p.y, p.z,
                TrackName(base, AiOf(base, e), 2));
  }
}

// ---- bug 4: checkpoint and Hayes ----

std::unordered_set<uint32_t> g_hayes;  // objects seen running Hayes states (g_mutex)
constexpr uint32_t kSecondCompanion = 0x41E15F74;  // from the Beta 11 report (task 324); may differ per run

void NoteHayes(uint32_t obj) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_hayes.insert(obj);
}

}  // namespace
}  // namespace bugtrace

// Called from pause_hooks.cpp's ES_CheckPoint hook (see the patch): self = the checkpoint object, before and
// after = the game's start point G+0xC around the original call.
void DevBugCheckpoint(uint8_t* base, uint32_t self, int32_t before, int32_t after) {
  using namespace bugtrace;
  if (!Checkpoint() || before == after) return;
  const uint32_t ai = AiOf(base, self), v = VarsOf(base, ai);
  const Pos cp = PosOf(base, self);
  const uint32_t who = W(base, v + 0x23C4), test = W(base, v + 0x2494);
  const char* test_name = NameOf(test);
  REXLOG_INFO("KK dev bug checkpoint: {:.2f} s fired: start point {} -> {} object {:08X} pos ({:.2f}, {:.2f}, "
              "{:.2f}) | zone 2544 {} needs both 2548 {} waits for 23C4 {:08X} test 2494 {:08X} {} bit 2550 {} "
              "n 2554 {} off 254C {}",
              DevSecondsSinceMenu(), before, after, self, cp.x, cp.y, cp.z, W(base, v + 0x2544),
              W(base, v + 0x2548), who, test, test_name ? test_name : "-", W(base, v + 0x2550),
              int32_t(W(base, v + 0x2554)), W(base, v + 0x254C));
  std::unordered_set<uint32_t> objs;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    objs = g_hayes;
  }
  objs.insert(kSecondCompanion);
  if (who) objs.insert(who);
  for (uint32_t o : objs) {
    if (!Ok(o)) continue;
    const Pos p = PosOf(base, o);
    const uint32_t flags = W(base, o + 4);
    REXLOG_INFO("KK dev bug checkpoint:   obj {:08X}{} flags {:08X} inactive {} pos ({:.2f}, {:.2f}, {:.2f}) "
                "distance {:.1f} track2 {}",
                o, o == who ? " (the one it waits for)" : (o == kSecondCompanion ? " (second companion?)" : ""),
                flags, (flags & 0x400000) != 0, p.x, p.y, p.z, Dist(p, cp), TrackName(base, AiOf(base, o), 2));
  }
}

}  // namespace kk

// ---- hooks ----

// bug 1: the KT states (r3 out = the state's result code; see kk-decomp docs/ai.md for the codes).
#define KK_BUG_VREX_STATE(addr, name)                                                          \
  REX_EXTERN(__imp__sub_##addr);                                                               \
  REX_HOOK_RAW(sub_##addr) {                                                                   \
    if (!kk::bugtrace::Vrex()) {                                                               \
      __imp__sub_##addr(ctx, base);                                                            \
      return;                                                                                  \
    }                                                                                          \
    const uint32_t obj = kk::hfr::Load32(base, 0x82CCBF6C), ai = kk::hfr::Load32(base, 0x82CCBF64);    \
    __imp__sub_##addr(ctx, base);                                                              \
    kk::bugtrace::VrexNote(base, name, obj, ai, ctx.r3.u32);                                   \
  }

KK_BUG_VREX_STATE(8248C380, "KT_ETAT_charge")
KK_BUG_VREX_STATE(824862C0, "KT_ETAT_derap")
KK_BUG_VREX_STATE(82490830, "KT_ETAT_attente")
KK_BUG_VREX_STATE(8248D308, "KT_ETAT_attaque")

// TrigExec_KTREX_Charge (r3 = trigger record: +4 the V-Rex, +8 the charge target).
REX_EXTERN(__imp__sub_82508C00);
REX_HOOK_RAW(sub_82508C00) {
  using namespace kk::bugtrace;
  const uint32_t rec = ctx.r3.u32;
  __imp__sub_82508C00(ctx, base);
  if (!Vrex()) return;
  const uint32_t rex = W(base, rec + 4), target = W(base, rec + 8);
  const Pos a = PosOf(base, rex), b = PosOf(base, target);
  REXLOG_INFO("KK dev bug vrex: {:.2f} s CHARGE ordered: record {:08X} vrex {:08X} at ({:.2f}, {:.2f}, {:.2f}) "
              "target {:08X} at ({:.2f}, {:.2f}, {:.2f}) distance {:.1f}",
              kk::DevSecondsSinceMenu(), rec, rex, a.x, a.y, a.z, target, b.x, b.y, b.z, Dist(a, b));
}

// waves: Spawner_loop runs every frame for every spawner.
REX_EXTERN(__imp__sub_826E1EE8);
REX_HOOK_RAW(sub_826E1EE8) {
  if (kk::coop::EnemySpawnerStep(ctx,base)) return;
  if (!kk::bugtrace::Waves()) {
    __imp__sub_826E1EE8(ctx, base);
    return;
  }
  const uint32_t obj = kk::hfr::Load32(base, 0x82CCBF6C), ai = kk::hfr::Load32(base, 0x82CCBF64);
  __imp__sub_826E1EE8(ctx, base);
  kk::bugtrace::SpawnerNote(base, obj, ai);
}

// Spawner_init: logs the progress bit and whether the spawner switched itself off.
// Co op merge (Beta 17): coop_partner_body.cpp owns the sub_826EB8C0 hook (its dev puppet spawner probe) and
// calls this in place of the original, so both observers run.
REX_EXTERN(__imp__sub_826EB8C0);
void KkDevBugSpawnerInit(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace kk::bugtrace;
  if (!Waves()) {
    __imp__sub_826EB8C0(ctx, base);
    return;
  }
  const uint32_t obj = kk::hfr::Load32(base, 0x82CCBF6C), ai = kk::hfr::Load32(base, 0x82CCBF64);
  const Bit bit = ProgressBit(base, VarsOf(base, ai));
  __imp__sub_826EB8C0(ctx, base);
  const bool off = (W(base, obj + 4) & 0x200000) != 0;
  if (bit.done || off)
    REXLOG_WARN("KK dev bug waves: {:.2f} s spawner {:08X} INIT: progress bit on {} n {} already done {} -> "
                "switched itself off {} (no wave from this spawner)",
                kk::DevSecondsSinceMenu(), obj, bit.on, bit.n, bit.done, off);
  else
    REXLOG_INFO("KK dev bug waves: {:.2f} s spawner {:08X} init: bit on {} n {} not done",
                kk::DevSecondsSinceMenu(), obj, bit.on, bit.n);
}

// Spawner_stop: the spawner is finished (sets its progress bit).
REX_EXTERN(__imp__sub_826D6750);
REX_HOOK_RAW(sub_826D6750) {
  using namespace kk::bugtrace;
  if (!Waves()) {
    __imp__sub_826D6750(ctx, base);
    return;
  }
  const uint32_t obj = kk::hfr::Load32(base, 0x82CCBF6C), ai = kk::hfr::Load32(base, 0x82CCBF64);
  __imp__sub_826D6750(ctx, base);
  const Bit bit = ProgressBit(base, VarsOf(base, ai));
  REXLOG_INFO("KK dev bug waves: {:.2f} s spawner {:08X} stop -> {} (bit on {} n {} done {})",
              kk::DevSecondsSinceMenu(), obj, int32_t(ctx.r3.u32), bit.on, bit.n, bit.done);
}

// obj_destroy 0x82714E40 (r3 = object). Its "when destroyed" callbacks are skipped when the object is already
// inactive (0x400000) or has 0x4000000 (ai_call_event 0x8271CFC8): then no 0x8000, and a spawner keeps the slot.
REX_EXTERN(__imp__sub_82714E40);
REX_HOOK_RAW(sub_82714E40) {
  using namespace kk::bugtrace;
  if (!Waves()) {
    __imp__sub_82714E40(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  uint32_t spawner = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_enemy_spawner.find(obj);
    if (it != g_enemy_spawner.end()) spawner = it->second;
  }
  const uint32_t flags = W(base, obj + 4);
  const uint32_t ext = W(base, obj + 0x18);
  const bool dead_before = (H(base, ext + 0x28) & 0x8000) != 0;
  const std::string t2 = TrackName(base, AiOf(base, obj), 2);
  __imp__sub_82714E40(ctx, base);
  if (!spawner) return;
  const bool dead_after = (H(base, ext + 0x28) & 0x8000) != 0;
  const bool skipped = (flags & 0x400000) || (flags & 0x4000000);
  if (!dead_after)
    REXLOG_WARN("KK dev bug waves: {:.2f} s DESTROY obj {:08X} of spawner {:08X} without dead flag 0x8000 "
                "(flags {:08X}, callbacks skipped {}, track2 {}): its slot stays taken",
                kk::DevSecondsSinceMenu(), obj, spawner, flags, skipped, t2);
  else
    REXLOG_INFO("KK dev bug waves: {:.2f} s destroy obj {:08X} of spawner {:08X} dead flag {} -> {} (track2 {})",
                kk::DevSecondsSinceMenu(), obj, spawner, dead_before, dead_after, t2);
}

// bug 4: remember which objects run Hayes states (hide, go there, follow), to report them at a checkpoint.
#define KK_BUG_HAYES_STATE(addr)                                                       \
  REX_EXTERN(__imp__sub_##addr);                                                       \
  REX_HOOK_RAW(sub_##addr) {                                                           \
    if (kk::bugtrace::Checkpoint()) kk::bugtrace::NoteHayes(kk::hfr::Load32(base, 0x82CCBF6C)); \
    __imp__sub_##addr(ctx, base);                                                      \
  }

KK_BUG_HAYES_STATE(826B81C8)  // H_ETAT_IA_hide
KK_BUG_HAYES_STATE(826AB3A8)  // H_CINE_Vala
KK_BUG_HAYES_STATE(826C3A98)  // H_ETAT_IA_suivi

#endif
