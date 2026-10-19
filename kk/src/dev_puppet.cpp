// Disposable, dev-only H-rig experiment. No spawning, networking or save writes.
// KK_DEV_PUPPET=scan (or 1), borrow:nearest, or borrow:<hex key>:<decimal ordinal>
// [:<decimal action>:<decimal item>]. Ordinals are zero-based active-table order.
#if defined(KK_DEV_TOOLS)

#include "dev_puppet.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"
#include "play_internal.h"

REX_EXTERN(__imp__sub_8268D8E8);
REX_EXTERN(__imp__sub_8271CCB8);
REX_EXTERN(__imp__sub_827117E0);
REX_EXTERN(__imp__sub_82711278);
REX_EXTERN(__imp__sub_82401AA8);
REX_EXTERN(__imp__sub_82850100);
REX_EXTERN(__imp__sub_823F8AF0);

namespace kk {
namespace {
using namespace hfr;
using Clock = std::chrono::steady_clock;

bool Range(uint32_t p, uint32_t bytes) {
  return p >= 0x40000000 && p < 0xA0000000 && !(p & 3) && bytes <= 0xA0000000 - p;
}
uint32_t Ptr(const uint8_t* b, uint32_t p, uint32_t offset, uint32_t bytes) {
  if (!Range(p, offset + 4)) return 0;
  const uint32_t q = Load32(b, p + offset);
  return Range(q, bytes) ? q : 0;
}
struct Vec {
  float x = 0, y = 0, z = 0;
};
Vec ReadVec(const uint8_t* b, uint32_t p) { return {LoadF(b, p), LoadF(b, p + 4), LoadF(b, p + 8)}; }
bool Finite(Vec v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
         std::fabs(v.x) < 1000000 && std::fabs(v.y) < 1000000 && std::fabs(v.z) < 1000000;
}
float Distance(Vec a, Vec b) {
  return std::sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y) + (a.z-b.z)*(a.z-b.z));
}
std::string XYZ(Vec v) { return fmt::format("({:.4f},{:.4f},{:.4f})", v.x, v.y, v.z); }

struct Options {
  bool on = false, borrow = false, nearest = false, valid = true, action_set = false;
  uint32_t key = 0, ordinal = 0, action = 0, item = 0;
  Options() {
    const char* env = std::getenv("KK_DEV_PUPPET");
    if (!env || !*env) return;
    on = true;
    const std::string s(env);
    if (s == "scan" || s == "1") return;
    borrow = true;
    if (s == "borrow:nearest") { nearest = true; return; }
    std::vector<std::string> fields;
    std::stringstream input(s);
    for (std::string f; std::getline(input, f, ':');) fields.push_back(f);
    auto number = [](std::string f, uint32_t& n, int radix) {
      if (radix == 16 && f.size() > 2 && f.substr(0, 2) == "0x") f.erase(0, 2);
      const auto r = std::from_chars(f.data(), f.data() + f.size(), n, radix);
      return !f.empty() && r.ec == std::errc{} && r.ptr == f.data() + f.size();
    };
    valid = !s.empty() && s.back() != ':' && (fields.size() == 3 || fields.size() == 5) &&
            fields[0] == "borrow" && number(fields[1], key, 16) && key != 0 &&
            number(fields[2], ordinal, 10);
    if (valid && fields.size() == 5) {
      action_set = true;
      valid = number(fields[3], action, 10) && action < 0x8000 &&
              number(fields[4], item, 10) && item <= 255;
    }
  }
};
const Options& Opt() { static const Options options; return options; }

struct Rig {
  uint32_t actor = 0, ai = 0, vars = 0, matrix = 0, rig = 0, anim = 0, table = 0, layer = 0;
  uint32_t action = 0, item = 0;
  Vec pos, forward;
  std::string tracks;
  bool dead_track = false;
};
bool Same(const Rig& a, const Rig& b) {
  return a.actor == b.actor && a.ai == b.ai && a.vars == b.vars && a.matrix == b.matrix &&
         a.rig == b.rig && a.anim == b.anim && a.table == b.table;
}
bool ReadRig(const uint8_t* b, uint32_t actor, Rig& r) {
  r = {};
  r.actor = actor;
  if (!Range(actor, 32)) return false;
  const uint32_t ext = Ptr(b, actor, 24, 48);
  r.matrix = Ptr(b, actor, 12, 80);
  r.rig = Ptr(b, actor, 20, 16);
  r.ai = Ptr(b, ext, 4, 88);
  r.vars = Ptr(b, r.ai, 64, 8);
  r.anim = Ptr(b, r.rig, 0, 176);
  r.table = Ptr(b, r.anim, 88, 16);
  r.layer = Ptr(b, r.anim, 132, 8);
  if (!r.matrix || !r.vars || !r.table || !r.layer || !Ptr(b, r.layer, 4, 4)) return false;
  r.action = Load16(b, r.anim + 156);
  r.item = b[r.anim + 160];
  r.pos = ReadVec(b, r.matrix + 48);
  // Engine getter 8270FA58 negates the matrix's second axis.
  r.forward = ReadVec(b, r.matrix + 16);
  r.forward = {-r.forward.x, -r.forward.y, -r.forward.z};
  const uint32_t first = Load16(b, r.ai + 80), last = Load16(b, r.ai + 82);
  if (first > last || last > 4096 || last - first >= 64 || !Range(r.ai, (last + 12) * 4)) return false;
  for (uint32_t t = first; t <= last; ++t) {
    const uint32_t block = Load32(b, r.ai + (t + 6) * 4);
    if (!block) { r.tracks += fmt::format("{}{}:0", t == first ? "" : "|", t); continue; }
    if (!Range(block, 24)) return false;
    const uint32_t fn = Load32(b, block + 20);
    r.tracks += fmt::format("{}{}:{:08X}@{:08X}", t == first ? "" : "|", t, fn, block);
    r.dead_track |= fn == 0x826B95A0;  // H_ETAT_IA_mort, never a generic health offset.
  }
  return Finite(r.pos) && Finite(r.forward);
}
bool LegalAction(const uint8_t* b, const Rig& r, uint32_t action) {
  if (action >= 0x8000 || action >= Load16(b, r.table)) return false;
  return action == 0 || Ptr(b, r.table, 4 * (action + 4), 4) != 0;
}
bool Active(const uint8_t* b, const Rig& r) {
  // Plain transform path only. Scheduler flags at object+4: disabled/deferred AI.
  const uint32_t ext = Ptr(b, r.actor, 24, 48);
  if (!ext) return false;
  // 82401AA8 permits absent controllers, but dereferences these chains if set.
  const uint32_t controller = Load32(b, ext);
  if (controller) {
    if (!Range(controller, 8)) return false;
    const uint32_t data = Load32(b, controller + 4);
    if (data && (!Range(data, 60) || !Ptr(b, data, 56, 60))) return false;
  }
  return !(Load32(b, r.actor) & 0x00400001u) &&
         !(Load32(b, r.actor + 4) & 0x00420000u) && !b[r.ai + 87] &&
         (!(Load16(b, ext + 46) & 4) || b[r.actor + 28]) && !r.dead_track;
}
bool Table(const uint8_t* b, uint32_t world, uint32_t& begin, uint32_t& end) {
  if (!Range(world, 308)) return false;
  begin = Load32(b, world + 304); end = Load32(b, world + 292);
  return Range(begin, 0) && end >= begin && end - begin <= 8 * 20000 &&
         !((end - begin) & 7) && Range(begin, end - begin);
}
bool Member(const uint8_t* b, uint32_t world, uint32_t actor) {
  uint32_t begin, end;
  if (!Table(b, world, begin, end)) return false;
  unsigned n = 0;
  for (uint32_t p = begin; p < end; p += 8) n += Load32(b, p) == actor;
  return n == 1;
}

// Ambiguous address-to-key joins are deliberately unselectable. Use the exact
// loaded object's key, as dev_sample does; do not guess keys from a shared mesh.
std::map<uint32_t, uint32_t> Keys(const uint8_t* b) {
  std::map<uint32_t, uint32_t> indices, keys;
  for (uint32_t bucket = 0; bucket < 256; ++bucket) {
    const uint32_t list = Load32(b, 0x82C4EDC0 + bucket * 16);
    const uint32_t n = Load32(b, 0x82C4EDC4 + bucket * 16);
    if (n > 100000 || !Range(list, n * 8)) continue;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t index = Load32(b, list + i * 8), address = Load32(b, list + i * 8 + 4);
      auto [it, added] = indices.emplace(index, address);
      if (!added && it->second != address) it->second = 0;
    }
  }
  const uint32_t list = Load32(b, 0x82C4ED94), n = Load32(b, 0x82C4ED98);
  if (n > 1000000 || !Range(list, n * 8)) return keys;
  for (uint32_t i = 0; i < n; ++i) {
    const auto found = indices.find(Load32(b, list + i * 8 + 4));
    if (found == indices.end() || !found->second) continue;
    const uint32_t key = Load32(b, list + i * 8);
    auto [it, added] = keys.emplace(found->second, key);
    if (!added && it->second != key) it->second = 0;
  }
  return keys;
}

struct Observation {
  Rig rig;
  uint64_t generation = 0;
  double stable = 0;
  bool present = false;
  bool bound = false;
  std::set<uint32_t> actions;
};
struct Candidate { Rig rig; uint32_t key = 0, ordinal = 0, index = 0; const char* reason = ""; };

struct State {
  uint64_t epoch = 0, generation = 0, held = 0, passed = 0;
  uint32_t world = 0, game = 0, level = 0, hero = 0;
  bool loading = true, ready = false, driving = false, attempted = false, terminal = false;
  bool finish_pending = false, measure = false, transition_pending = false;
  Clock::time_point loaded{}, census_at{}, tick_at{};
  std::map<uint32_t, Observation> humans;
  std::map<uint32_t, uint32_t> keys;
  Rig selected;
  uint64_t selected_generation = 0;
  uint32_t action = 0, item = 0, last_block = 0, last_track = 0;
  double elapsed = 0;
  float dt = 0;
  Vec origin, initial_forward, desired, facing, early_pos, early_forward;
  float early_frame = 0;
  float max_late_error = 0, max_forward_error = 0;

  void Abort(const char* reason) {
    if (terminal) return;
    driving = false; finish_pending = false; measure = false; terminal = true;
    REXLOG_INFO("PUPPET abort reason={} epoch={} actor={:08X} held={} passed={} reload-required",
                reason, epoch, selected.actor, held, passed);
    selected = {};
  }
  bool Environment(const uint8_t* b, uint32_t w) const {
    return ready && !loading && w == world && Range(game, 28) &&
           Load32(b, 0x82CC97C0) == game &&  // 0x828E8378 is not the world (Claude, run 14:25)
           Load32(b, game + 8) == level && Load32(b, game + 4) == 0 &&
           Load32(b, 0x82CCBF7C) == hero && !play::StartLevelLoading();
  }
  const char* Qualify(const uint8_t* b, const Rig& r, const Rig& jack) const {
    if (r.actor == hero || r.ai == jack.ai || r.vars == jack.vars || r.matrix == jack.matrix ||
        r.rig == jack.rig || r.anim == jack.anim || r.layer == jack.layer) return "hero-or-alias";
    if (!Active(b, r)) return "inactive-tagged-dead-or-indirect";
    if (!LegalAction(b, r, r.action)) return "missing-legal-action";
    const float horizontal = std::hypot(r.forward.x, r.forward.y);
    if (horizontal < 0.001f || std::fabs(r.forward.z) > 0.01f) return "invalid-upright-facing";
    return "";
  }
  bool Validate(const uint8_t* b, uint32_t w, Rig& now) {
    if (!Environment(b, w)) { Abort("world-level-load-or-hero-changed"); return false; }
    const auto it = humans.find(selected.actor);
    Rig jack;
    if (it == humans.end() || it->second.generation != selected_generation ||
        !Member(b, w, selected.actor) || !ReadRig(b, selected.actor, now) || !Same(now, selected) ||
        !ReadRig(b, hero, jack) || *Qualify(b, now, jack) || !LegalAction(b, now, action)) {
      Abort("selection-identity-or-rig-changed"); return false;
    }
    return true;
  }
};
State& Get() { static State state; return state; }

// Helpers use a separate guest frame and guest vectors. Preserve the whole
// context and the entire reserved region, including callee frames below r1.
struct Scratch {
  PPCContext& ctx;
  uint8_t* base;
  PPCContext saved;
  uint32_t start;
  uint8_t bytes[16384];
  static bool CanUse(const PPCContext& c) {
    return !(c.r1.u32 & 15) && c.r1.u32 >= 0x40014000 && c.r1.u32 < 0xA0000000;
  }
  Scratch(PPCContext& c, uint8_t* b) : ctx(c), base(b), saved(c), start(c.r1.u32 - sizeof(bytes)) {
    std::memcpy(bytes, base + start, sizeof(bytes));
    ctx.r1.u64 = start + 12288;
    std::memset(base + start + 12288, 0, 4096);
    Store32(base, start + 12288, saved.r1.u32);
  }
  ~Scratch() { std::memcpy(base + start, bytes, sizeof(bytes)); ctx = saved; }
  uint32_t Vector(Vec v, uint32_t offset) {
    const uint32_t p = start + 12288 + offset;
    StoreF(base, p, v.x); StoreF(base, p + 4, v.y); StoreF(base, p + 8, v.z);
    return p;
  }
};
float Frame(PPCContext& ctx, uint8_t* b, uint32_t actor) {
  Scratch scratch(ctx, b);
  ctx.r3.u64 = actor; ctx.r4.u64 = 0;
  __imp__sub_823F8AF0(ctx, b);
  return float(ctx.r3.s32) / 60.0f;
}

void CensusAndSelect(PPCContext& ctx, uint8_t* b, uint32_t w) {
  auto& s = Get();
  if (!s.Environment(b, w)) return;
  Rig jack;
  uint32_t begin, end;
  const bool jack_ok = ReadRig(b, s.hero, jack);
  if (!Table(b, w, begin, end)) { s.Abort("active-table-unavailable"); return; }
  const auto now = Clock::now();
  const bool log = s.census_at == Clock::time_point{} || now - s.census_at >= std::chrono::seconds(1);
  if (log) { s.census_at = now; s.keys = Keys(b); }
  std::map<uint32_t, uint32_t> ordinals;
  std::vector<Candidate> candidates;
  std::set<uint32_t> seen;
  for (uint32_t p = begin; p < end; p += 8) {
    const uint32_t actor = Load32(b, p);
    auto it = s.humans.find(actor);
    if (it == s.humans.end()) continue;  // Only naturally observed H_TRACK_init.
    auto& obs = it->second;
    const bool duplicate = !seen.insert(actor).second;
    const auto key_it = s.keys.find(actor);
    const uint32_t key = key_it == s.keys.end() ? 0 : key_it->second;
    const uint32_t ordinal = ordinals[key]++;
    Rig rig;
    if (!ReadRig(b, actor, rig) || (obs.bound ? !Same(rig, obs.rig) :
        rig.ai != obs.rig.ai || rig.vars != obs.rig.vars)) {
      obs.stable = 0; obs.present = false;
      if (log) REXLOG_INFO("PUPPET census epoch={} world={:08X} level={} hero={:08X} index={} key={:08X} ordinal={} actor={:08X} ai={:08X} vars={:08X} matrix={:08X} anim={:08X} xyz=unavailable distance=unavailable tracks={} action={} item={} frame=unavailable qualified=false reason=missing-or-changed-rig-chain",
                           s.epoch, w, s.level, s.hero, (p-begin)/8, key, ordinal, actor, rig.ai, rig.vars,
                           rig.matrix, rig.anim, rig.tracks, rig.action, rig.item);
      continue;
    }
    if (!obs.bound) { obs.rig = rig; obs.bound = true; }
    obs.stable = obs.present ? obs.stable + s.dt : 0;
    obs.present = true;
    const char* reason = jack_ok ? s.Qualify(b, rig, jack) : "hero-rig-unavailable";
    if (!*reason && (!key || duplicate)) reason = "unresolved-key-or-duplicate";
    if (!*reason && obs.stable < 1.0) reason = "not-stable-one-second";
    if (LegalAction(b, rig, rig.action)) obs.actions.insert((rig.action << 8) | rig.item);
    if (log) {
      REXLOG_INFO("PUPPET census epoch={} world={:08X} level={} hero={:08X} index={} key={:08X} ordinal={} actor={:08X} ai={:08X} vars={:08X} matrix={:08X} rig={:08X} anim={:08X} table={:08X} layer={:08X} human={}/{} xyz={} distance={:.3f} tracks={} action={} item={} frame={:.3f} qualified={} reason={}",
                  s.epoch, w, s.level, s.hero, (p-begin)/8, key, ordinal, actor, rig.ai, rig.vars,
                  rig.matrix, rig.rig, rig.anim, rig.table, rig.layer, Load32(b, rig.vars), Load32(b, rig.vars+4),
                  XYZ(rig.pos), jack_ok ? Distance(rig.pos, jack.pos) : -1.0f, rig.tracks, rig.action, rig.item,
                  Frame(ctx, b, actor), !*reason, *reason ? reason : "ok");
    }
    candidates.push_back({rig, key, ordinal, (p-begin)/8, reason});
  }
  for (auto& [actor, obs] : s.humans) {
    if (!seen.count(actor)) { obs.present = false; obs.stable = 0; }
  }
  if (log && seen.empty()) REXLOG_INFO("PUPPET census epoch={} world={:08X} level={} hero={:08X} candidates=0 reason=no-natural-H-init-in-active-table",
                                      s.epoch, w, s.level, s.hero);
  if (!Opt().borrow || s.attempted || s.terminal || now - s.loaded < std::chrono::seconds(5)) return;
  s.attempted = true;
  const Candidate* pick = nullptr;
  float closest = 15.0f;
  bool tied = false;
  for (const auto& c : candidates) {
    if (*c.reason) continue;
    if (Opt().nearest) {
      const float d = Distance(c.rig.pos, jack.pos);
      if (d > 15.0f) continue;
      if (!pick || d < closest - 0.0001f) { pick = &c; closest = d; tied = false; }
      else if (std::fabs(d - closest) <= 0.0001f) tied = true;
    } else if (c.key == Opt().key && c.ordinal == Opt().ordinal) {
      if (pick) { tied = true; break; }
      pick = &c;
    }
  }
  if (!pick || tied) { s.Abort(tied ? "ambiguous-selector" : "no-qualified-selector-within-limit"); return; }
  if (!Member(b, w, pick->rig.actor)) { s.Abort("duplicate-active-membership"); return; }
  auto& obs = s.humans.at(pick->rig.actor);
  s.action = Opt().action_set ? Opt().action : pick->rig.action;
  s.item = Opt().action_set ? Opt().item : pick->rig.item;
  if (!obs.actions.count((s.action << 8) | s.item) || !LegalAction(b, pick->rig, s.action)) {
    s.Abort("action-item-not-naturally-observed-on-this-rig"); return;
  }
  s.selected = pick->rig; s.selected_generation = obs.generation;
  s.transition_pending = s.action != pick->rig.action || s.item != pick->rig.item;
  s.origin = pick->rig.pos; s.initial_forward = pick->rig.forward;
  const float len = std::hypot(s.initial_forward.x, s.initial_forward.y);
  s.initial_forward = {s.initial_forward.x / len, s.initial_forward.y / len, 0};
  s.desired = s.origin; s.facing = s.initial_forward; s.elapsed = 0; s.driving = true;
  REXLOG_INFO("PUPPET select epoch={} mode={} key={:08X} ordinal={} index={} actor={:08X} ai={:08X} vars={:08X} matrix={:08X} anim={:08X} generation={} distance={:.3f} units=guest action={} item={} observed-action={} observed-item={}",
              s.epoch, Opt().nearest ? "nearest" : "key", pick->key, pick->ordinal, pick->index,
              s.selected.actor, s.selected.ai, s.selected.vars, s.selected.matrix, s.selected.anim,
              s.selected_generation, Distance(s.origin, jack.pos), s.action, s.item,
              s.selected.action, s.selected.item);
}
}  // namespace

void DevPuppetBeforeLoad() {
  if (!Opt().on) return;
  auto& s = Get();
  if (s.driving || s.finish_pending) s.Abort("level-load-started");
  s.selected = {}; s.humans.clear(); s.keys.clear(); ++s.epoch;
  s.world = s.hero = s.game = 0; s.loading = true; s.ready = false;
  s.census_at = {}; s.tick_at = {};
}
void DevPuppetAfterLoad(const uint8_t* b, uint32_t game) {
  if (!Opt().on) return;
  auto& s = Get();
  s.game = game; s.level = Range(game, 28) ? Load32(b, game + 8) : 0;
  s.loading = false; s.loaded = Clock::now();
}

void DevPuppetBeforeAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
  if (!Opt().on) return;
  auto& s = Get();
  if (!Opt().valid) { s.Abort("invalid-KK_DEV_PUPPET-syntax"); return; }
  if (s.loading || !s.game || play::StartLevelLoading()) return;
  const uint32_t hero = Load32(b, 0x82CCBF7C);
  if (!s.ready && Range(w, 308) && Range(hero, 32)) { s.world = w; s.hero = hero; s.ready = true; }
  if (!s.ready && Clock::now() - s.loaded >= std::chrono::seconds(5)) {
    s.Abort("world-or-hero-unavailable-after-load"); return;
  }
  if (!s.Environment(b, w)) {
    static int why = 0;
    if (s.ready && why++ < 3)
      REXLOG_INFO("PUPPET env-fail w={:08X} s.world={:08X} game={:08X}/{:08X} world828E8378={:08X} level={}/{} "
                  "game+4={} hero={:08X}/{:08X} startloading={}", w, s.world, Load32(b, 0x82CC97C0), s.game,
                  Load32(b, 0x828E8378), Range(s.game, 28) ? Load32(b, s.game + 8) : 0, s.level,
                  Range(s.game, 28) ? Load32(b, s.game + 4) : 0, Load32(b, 0x82CCBF7C), s.hero,
                  play::StartLevelLoading());
    if (s.ready) { s.Abort("world-level-load-or-hero-changed"); s.humans.clear(); s.ready = false; }
    return;
  }
  // This hook is entered once by the normal per-world update. Reject other callers.
  if (uint32_t(ctx.lr) != 0x82881CF0) { if (s.driving) s.Abort("unexpected-AI-pass-caller"); return; }
  s.dt = FrameTime(b);
  if (!std::isfinite(s.dt) || s.dt < 0 || s.dt > 0.25f) { s.Abort("invalid-simulation-dt"); return; }
  if (!Scratch::CanUse(ctx)) { s.Abort("invalid-guest-scratch-stack"); return; }
  if (s.driving) { Rig rig; s.Validate(b, w, rig); }
  else if (!s.terminal) CensusAndSelect(ctx, b, w);
}

void DevPuppetAfterAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
  if (!Opt().on) return;
  auto& s = Get();
  if (!s.driving) return;
  Rig rig;
  if (!s.Validate(b, w, rig)) return;
  if (!Scratch::CanUse(ctx)) { s.Abort("invalid-guest-scratch-stack"); return; }
  s.elapsed = std::min(8.0, s.elapsed + double(s.dt));
  const float move = s.elapsed <= 2 ? 0 : s.elapsed < 4 ? float((s.elapsed-2)/2) :
                     s.elapsed <= 6 ? 1 : float((8-s.elapsed)/2);
  const float turn = s.elapsed <= 4 ? 0 : s.elapsed < 6 ? float((s.elapsed-4)/2) : float((8-s.elapsed)/2);
  const float angle = turn * 1.57079632679f;
  s.desired = {s.origin.x + s.initial_forward.x * 0.5f * move,
               s.origin.y + s.initial_forward.y * 0.5f * move, s.origin.z};
  s.facing = {s.initial_forward.x * std::cos(angle) - s.initial_forward.y * std::sin(angle),
              s.initial_forward.x * std::sin(angle) + s.initial_forward.y * std::cos(angle), 0};
  {
    Scratch scratch(ctx, b);
    const uint32_t xyz = scratch.Vector(s.desired, 0x100), forward = scratch.Vector(s.facing, 0x110);
    auto set = [&](auto fn, uint32_t vector) {
      ctx.r3.u64 = s.selected.actor; ctx.r4.u64 = vector; fn(ctx, b);
    };
    set(__imp__sub_827117E0, xyz);
    if (!s.Validate(b, w, rig)) return;
    set(__imp__sub_82711278, forward);
    if (!s.Validate(b, w, rig)) return;
    set(__imp__sub_82401AA8, xyz);
    if (!s.Validate(b, w, rig)) return;
    // No forced restart. Request only an actual transition; keep normal skinning.
    if (s.transition_pending) {
      ctx.r3.u64 = rig.actor; ctx.r4.u64 = s.action; ctx.r5.u64 = s.item; ctx.r6.u64 = 0;
      __imp__sub_82850100(ctx, b);
      s.transition_pending = false;
    }
  }
  if (!s.Validate(b, w, rig)) return;
  if (rig.action != s.action || rig.item != s.item) { s.Abort("action-item-readback-mismatch"); return; }
  s.early_pos = rig.pos; s.early_forward = rig.forward; s.early_frame = Frame(ctx, b, rig.actor);
  s.measure = true;
  if (s.elapsed >= 8) { s.driving = false; s.finish_pending = true; }
}

void DevPuppetLateWorld(PPCContext& ctx, uint8_t* b, uint32_t w) {
  if (!Opt().on) return;
  auto& s = Get();
  if (!s.measure) return;
  s.measure = false;
  Rig rig;
  if (!s.Validate(b, w, rig)) return;
  if (!Scratch::CanUse(ctx)) { s.Abort("invalid-guest-scratch-stack"); return; }
  const auto now = Clock::now();
  s.max_late_error = std::max(s.max_late_error, Distance(s.desired, rig.pos));
  s.max_forward_error = std::max(s.max_forward_error, Distance(s.facing, rig.forward));
  if (s.tick_at == Clock::time_point{} || now - s.tick_at >= std::chrono::seconds(1)) {
    s.tick_at = now;
    REXLOG_INFO("PUPPET hold count={} passed={} epoch={} actor={:08X} lr=8271D304 block={:08X} track={:08X}",
                s.held, s.passed, s.epoch, rig.actor, s.last_block, s.last_track);
    REXLOG_INFO("PUPPET tick t={:.3f} desired={} actual={} forward={} actual-forward={} action={} item={} frame={:.3f} early={} early-forward={} early-frame={:.3f} late-error={:.6f} max-late-error={:.6f} max-forward-error={:.6f}",
                s.elapsed, XYZ(s.desired), XYZ(rig.pos), XYZ(s.facing), XYZ(rig.forward), rig.action, rig.item,
                Frame(ctx, b, rig.actor), XYZ(s.early_pos), XYZ(s.early_forward), s.early_frame,
                Distance(s.desired, rig.pos), s.max_late_error, s.max_forward_error);
  }
  if (s.finish_pending) {
    if (!s.held) { s.Abort("no-selected-scheduled-dispatch-held"); return; }
    s.finish_pending = false; s.terminal = true;
    REXLOG_INFO("PUPPET done reload-required epoch={} actor={:08X} t={:.3f} held={} passed={} final={} final-forward={} max-late-error={:.6f} max-forward-error={:.6f}",
                s.epoch, rig.actor, s.elapsed, s.held, s.passed, XYZ(rig.pos), XYZ(rig.forward),
                s.max_late_error, s.max_forward_error);
    s.selected = {};
  }
}

// Natural init observation only. Never invoke H init to qualify an actor.
void PuppetObserveInit(uint8_t* b, uint32_t actor, uint32_t ai) {
  if (!Opt().on) return;
  auto& s = Get();
  if (s.driving && actor == s.selected.actor) s.Abort("selected-H-reinitialized");
  Rig rig;
  rig.actor = actor;
  const uint32_t ext = Ptr(b, actor, 24, 8);
  rig.ai = Ptr(b, ext, 4, 68);
  rig.vars = Ptr(b, rig.ai, 64, 8);
  if (!rig.vars || rig.ai != ai) return;
  // Init can finish before layers and active membership exist. Bind the full
  // identity on its first valid census, without losing this natural H proof.
  Observation observation;
  observation.rig = rig; observation.generation = ++s.generation;
  s.humans[actor] = std::move(observation);
}
bool PuppetHold(PPCContext& ctx, uint8_t* b) {
  if (!Opt().on) return false;
  auto& s = Get();
  if (!s.driving) return false;
  if (uint32_t(ctx.lr) != 0x8271D304 || Load32(b, 0x82CCBF64) != s.selected.ai ||
      Load32(b, 0x82CCBF6C) != s.selected.actor) { ++s.passed; return false; }
  Rig rig;
  if (!s.Validate(b, s.world, rig)) return false;
  s.last_block = ctx.r3.u32; s.last_track = ctx.r4.u32; ++s.held;
  ctx.r3.u64 = 0;
  return true;
}
}  // namespace kk

REX_HOOK_RAW(sub_8268D8E8) {
  const bool on = kk::Opt().on;
  const uint32_t actor = on ? kk::hfr::Load32(base, 0x82CCBF6C) : 0;
  const uint32_t ai = on ? kk::hfr::Load32(base, 0x82CCBF64) : 0;
  __imp__sub_8268D8E8(ctx, base);
  if (on) kk::PuppetObserveInit(base, actor, ai);
}
REX_HOOK_RAW(sub_8271CCB8) {
  if (!kk::PuppetHold(ctx, base)) __imp__sub_8271CCB8(ctx, base);
}
#endif
