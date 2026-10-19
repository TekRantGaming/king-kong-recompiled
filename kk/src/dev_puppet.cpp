// Disposable, dev-only H-rig experiment. No transport calls or save writes.
// KK_DEV_PUPPET=scan (or 1), borrow:nearest[:<max guest units>],
// borrow:index:<zero-based active-table index>[:<action>:<item>], or the legacy
// borrow:<hex key>:<ordinal>[:<action>:<item>]. Indices belong to the current epoch.
// partner:index:<n> or partner:nearest[:<max>] follows the copied partner pose.
// partner:spawn:<n> creates an independent engine copy of that loaded human.
// KK_DEV_PUPPET_SPAWN_MODE=drive (default), hold, or free (UNSAFE story scripts).
// KK_DEV_PUPPET_SPAWN_OFFSET=x,y,z uses source+offset, permits solo diagnosis,
// and gives drive mode a stationary target at that creation point.
#if defined(KK_DEV_TOOLS)

#include "dev_puppet.h"
#include "dev_puppet_mesh.h"

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
#include "coop_puppet.h"
#include "coop_runtime.h"
#include "photo_mode.h"
#include "settings.h"

REX_EXTERN(__imp__sub_8268D8E8);
REX_EXTERN(__imp__sub_827117E0);
REX_EXTERN(__imp__sub_82711278);
REX_EXTERN(__imp__sub_82401AA8);
REX_EXTERN(__imp__sub_82850100);
REX_EXTERN(__imp__sub_823F8AF0);
REX_EXTERN(__imp__sub_82715AC8);
REX_EXTERN(__imp__sub_826EB8C0);
REX_EXTERN(__imp__sub_8284B028);

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
  enum class SpawnMode { Drive, Free, Hold };
  SpawnMode spawn_mode = SpawnMode::Drive;
  bool spawn_offset_set = false;
  Vec spawn_offset;
  bool on = false, borrow = false, partner = false, spawn = false, nearest = false, index = false, valid = true, action_set = false;
  uint32_t key = 0, ordinal = 0, census_index = 0, action = 0, item = 0;
  float max_units = 15.0f;
  Options() {
    const char* env = std::getenv("KK_DEV_PUPPET");
    if (!env || !*env) return;
    on = true;
    const std::string s(env);
    if (s == "scan" || s == "1") return;
    borrow = true;
    std::vector<std::string> fields;
    std::stringstream input(s);
    for (std::string f; std::getline(input, f, ':');) fields.push_back(f);
    auto number = [](std::string f, uint32_t& n, int radix) {
      if (radix == 16 && f.size() > 2 && f.substr(0, 2) == "0x") f.erase(0, 2);
      const auto r = std::from_chars(f.data(), f.data() + f.size(), n, radix);
      return !f.empty() && r.ec == std::errc{} && r.ptr == f.data() + f.size();
    };
    partner = fields[0] == "partner";
    valid = s.back() != ':' && fields.size() >= 2 && (fields[0] == "borrow" || partner);
    if (!valid) return;
    if (fields[1] == "nearest") {
      nearest = true;
      valid = fields.size() == 2 || fields.size() == 3;
      if (valid && fields.size() == 3) {
        const auto& f = fields[2];
        const auto r = std::from_chars(f.data(), f.data() + f.size(), max_units);
        valid = !f.empty() && r.ec == std::errc{} && r.ptr == f.data() + f.size() &&
                std::isfinite(max_units) && max_units > 0;
      }
      return;
    }
    spawn = partner && fields[1] == "spawn";
    if (spawn) {
      if (const char* mode = std::getenv("KK_DEV_PUPPET_SPAWN_MODE")) {
        const std::string value(mode);
        if (value == "free") spawn_mode = SpawnMode::Free;
        else if (value == "hold") spawn_mode = SpawnMode::Hold;
        else if (!value.empty() && value != "drive") { valid = false; return; }
      }
      if (const char* offset = std::getenv("KK_DEV_PUPPET_SPAWN_OFFSET")) {
        const std::string value(offset);
        const auto a = value.find(','), c = a == std::string::npos ? a : value.find(',', a+1);
        if (a == std::string::npos || c == std::string::npos || value.find(',', c+1) != std::string::npos) {
          valid = false; return;
        }
        auto component = [&](size_t begin, size_t end, float& out) {
          const auto r = std::from_chars(value.data()+begin, value.data()+end, out);
          return begin != end && r.ec == std::errc{} && r.ptr == value.data()+end;
        };
        valid = component(0, a, spawn_offset.x) && component(a+1, c, spawn_offset.y) &&
                component(c+1, value.size(), spawn_offset.z) && Finite(spawn_offset);
        if (!valid) return;
        spawn_offset_set = true;
      }
    }
    index = fields[1] == "index" || spawn;
    if (partner && (!index || fields.size() != 3)) { valid = false; return; }
    valid = (fields.size() == 3 || fields.size() == 5) &&
            (index ? number(fields[2], census_index, 10) :
             number(fields[1], key, 16) && key != 0 && number(fields[2], ordinal, 10));
    if (valid && fields.size() == 5) {
      action_set = true;
      valid = number(fields[3], action, 10) && action < 0x8000 &&
              number(fields[4], item, 10) && item <= 255;
    }
  }
};
const Options& Opt() { static const Options options; return options; }
bool OffsetSpawn() { return Opt().spawn && Opt().spawn_offset_set; }
bool PassiveSpawn() { return Opt().spawn && Opt().spawn_mode != Options::SpawnMode::Drive; }
const char* SpawnModeName() {
  return Opt().spawn_mode == Options::SpawnMode::Free ? "free" :
         Opt().spawn_mode == Options::SpawnMode::Hold ? "hold" : "drive";
}
bool Enabled() { return Opt().on && (!Opt().partner || OffsetSpawn() || REXCVAR_GET(kk_coop_enabled)); }

struct Rig {
  uint32_t actor = 0, ai = 0, vars = 0, matrix = 0, rig = 0, anim = 0, table = 0, layer = 0;
  uint32_t action = 0, item = 0;
  Vec pos, forward;
  std::string tracks;
  bool dead_track = false;
};
coop::puppet::MutableRig Mutable(const Rig& r) {
  return {r.actor, r.ai, r.vars, r.matrix, r.rig, r.anim, r.layer};
}
bool Same(const Rig& a, const Rig& b) {
  return a.actor == b.actor && a.ai == b.ai && a.vars == b.vars && a.matrix == b.matrix &&
         a.rig == b.rig && a.anim == b.anim && a.table == b.table;
}
bool ReadTracks(const uint8_t* b, Rig& r) {
  if (!Range(r.ai, 88)) return false;
  const uint32_t first = Load16(b, r.ai + 80), last = Load16(b, r.ai + 82);
  if (first > last || last > 4096 || last - first >= 64 || !Range(r.ai, (last + 12) * 4)) return false;
  for (uint32_t t = first; t <= last; ++t) {
    const uint32_t block = Load32(b, r.ai + (t + 6) * 4);
    if (!block) { r.tracks += fmt::format("{}{}:0", t == first ? "" : "|", t); continue; }
    if (!Range(block, 24)) return false;
    const uint32_t fn = Load32(b, block + 20);
    r.tracks += fmt::format("{}{}:{:08X}@{:08X}", t == first ? "" : "|", t, fn, block);
    r.dead_track |= fn == 0x826B95A0;  // H_ETAT_IA_mort (pause_hooks.cpp).
  }
  return true;
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
  return ReadTracks(b, r) && Finite(r.pos) && Finite(r.forward);
}
// Census only: a matrix and a rig component are enough to report an actor.
// No H proof, vars, action table or live layer is required and no guest call is made.
void CensusOtherRig(const uint8_t* b, uint32_t actor, uint32_t index, uint64_t epoch,
                    uint32_t world, uint32_t level, uint32_t hero) {
  if (!Range(actor, 32)) return;
  Rig r;
  r.actor = actor; r.matrix = Ptr(b, actor, 12, 60); r.rig = Ptr(b, actor, 20, 16);
  if (!r.matrix || !r.rig) return;
  r.anim = Ptr(b, r.rig, 0, 176);
  r.ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
  r.vars = Ptr(b, r.ai, 64, 8);
  r.pos = ReadVec(b, r.matrix + 48);
  const bool tracks_ok = ReadTracks(b, r);
  REXLOG_INFO("PUPPET census-other epoch={} world={:08X} level={} hero={:08X} index={} actor={:08X} ai={:08X} vars={:08X} matrix={:08X} rig={:08X} anim={:08X} xyz={} family=not-observed-H tracks={} tracks-valid={} qualified=false reason=census-only-no-natural-H-init",
              epoch, world, level, hero, index, actor, r.ai, r.vars, r.matrix, r.rig, r.anim,
              Finite(r.pos) ? XYZ(r.pos) : "unavailable", r.tracks, tracks_ok);
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
bool Registered(const uint8_t* b, uint32_t world, uint32_t actor) {
  // 82847300 (.46) inserts into world+24's 8-byte object registry immediately.
  // Its begin/end are world+40/+28. The AI census at +304/+292 is separate.
  if (!Range(world, 44)) return false;
  const uint32_t begin = Load32(b, world + 40), end = Load32(b, world + 28);
  if (end < begin || end-begin > 8*20000 || ((end-begin) & 7) || !Range(begin, end-begin)) return false;
  unsigned n = 0;
  for (uint32_t p = begin; p < end; p += 8) n += Load32(b, p) == actor;
  return n == 1;
}

// Resource lookup, not an instance identity: 827061B0 (.88) uses these tables
// via 82732250 (.0). Live actors need not be registered at a loaded file address.
// Keep keys for diagnostics/legacy selectors; nearest/index do not require one.
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
  uint32_t index = 0;
  double stable = 0;
  bool present = false;
  bool bound = false;
  std::set<uint32_t> actions;
  // Separate idle/moving examples of each tuple; never learn from a driven body.
  std::map<uint32_t, coop::puppet::Action> motion;
  Vec previous;
  uint32_t previous_tuple = 0;
  bool sampled = false;
};
struct Candidate { Rig rig; uint32_t key = 0, ordinal = 0, index = 0; const char* reason = ""; };
struct Spawned {
  Rig rig;
  uint32_t world = 0, game = 0, level = 0, model = 0;
  uint64_t epoch = 0, held = 0;
};
struct Spawner { uint32_t ai = 0, vars = 0; };
bool SpawnIdentity(const uint8_t* b, const Spawned& body);

struct State {
  uint64_t epoch = 0, generation = 0, held = 0, passed = 0;
  uint32_t world = 0, game = 0, level = 0, hero = 0;
  bool loading = true, ready = false, driving = false, attempted = false, terminal = false;
  bool finish_pending = false, measure = false, transition_pending = false;
  bool partner_eligible = false, prepared = false;
  unsigned missing_action_kind = 0;
  uint64_t frame = 0, updated_frame = ~uint64_t{0}, updates = 0;
  coop::PuppetSnapshot partner;
  std::vector<coop::puppet::Action> observed_motion;
  Clock::time_point loaded{}, census_at{}, tick_at{};
  std::map<uint32_t, Observation> humans;
  // Retain script quarantine after release except in explicit unsafe free mode.
  // No transform restore or destructor/pool return. Clear before unload.
  std::vector<Spawned> spawned;
  Rig bone_source, bone_copy;
  Clock::time_point bones_at{};
  std::map<uint32_t, Spawner> spawners;
  std::map<uint32_t, uint32_t> keys;
  Rig selected;
  uint64_t selected_generation = 0;
  uint32_t action = 0, item = 0, last_block = 0, last_track = 0;
  unsigned env_fail_logs = 0;
  double elapsed = 0;
  float dt = 0;
  Vec origin, initial_forward, desired, facing, early_pos, early_forward;
  float early_frame = 0;
  float max_late_error = 0, max_forward_error = 0;

  void Abort(const char* reason) {
    if (terminal) return;
    driving = false; finish_pending = false; measure = false; terminal = true;
    if (Opt().partner) {
      coop::SetPartnerBodyDriven(false);
      REXLOG_INFO("PUPPET partner released reason={} epoch={} actor={:08X} held={} passed={} updates={}",
                  reason, epoch, selected.actor, held, passed, updates);
      if (Opt().spawn) {
        REXLOG_INFO("PUPPET spawn {} reason={} epoch={} actor={:08X} no-free script-quarantine={}",
                    selected.actor ? "released" : "refused", reason, epoch, selected.actor,
                    Opt().spawn_mode == Options::SpawnMode::Free ? "DISABLED-UNSAFE" : "until-unload");
      }
    } else {
      REXLOG_INFO("PUPPET abort reason={} epoch={} actor={:08X} held={} passed={} reload-required",
                  reason, epoch, selected.actor, held, passed);
    }
    selected = {};
  }
  bool Environment(const uint8_t* b, uint32_t w) const {
    return ready && !loading && w == world && Range(game, 28) &&
           Load32(b, 0x82CC97C0) == game &&  // Universe G, play_session.cpp; world comes from r3.
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
    if (Opt().spawn) {
      bool own = false;
      for (const auto& body : spawned) own |= body.rig.actor == selected.actor && SpawnIdentity(b, body);
      if (!own) { Abort("spawn-identity-changed"); return false; }
    }
    const auto it = humans.find(selected.actor);
    Rig jack;
    if (it == humans.end() || it->second.generation != selected_generation ||
        !(Opt().spawn ? Registered(b, w, selected.actor) : Member(b, w, selected.actor)) ||
        !ReadRig(b, selected.actor, now) || !Same(now, selected) ||
        !ReadRig(b, hero, jack) || *Qualify(b, now, jack) || !LegalAction(b, now, action)) {
      Abort("selection-identity-or-rig-changed"); return false;
    }
    return true;
  }
};
State& Get() { static State state; return state; }

// Read-only reconstruction of the position chain in 8270FCE0 (.4:27633+),
// with the owner walk from 8286CC40 (.43:33799+). Never refresh dirty matrices.
bool BoneWorldPosition(const uint8_t* b, uint32_t actor, Vec& out) {
  if (!Range(actor, 32)) return false;
  auto position = [&](uint32_t a) {
    const auto m = Ptr(b, a, 12, 80);
    if (!m) return false;
    out = ReadVec(b, m+48);
    return Finite(out);
  };
  if (!(Load32(b, actor) & 1)) return position(actor);
  const auto local = Ptr(b, Ptr(b, actor, 20, 16), 12, 88);
  if (!local) return false;
  if (!Load32(b, local+84)) return position(actor);
  uint32_t root = actor;
  std::set<uint32_t> seen;
  while (Load32(b, root) & 1) {
    if (seen.size() >= 64 || !seen.insert(root).second) return false;
    const auto m = Ptr(b, Ptr(b, root, 20, 16), 12, 88);
    root = Ptr(b, m, 80, 32);
    if (!root) return false;
  }
  auto transform = [&](uint32_t m) {
    const Vec x = ReadVec(b, m), y = ReadVec(b, m+16), z = ReadVec(b, m+32), t = ReadVec(b, m+48);
    out = {out.x*x.x + out.y*y.x + out.z*z.x + t.x,
           out.x*x.y + out.y*y.y + out.z*z.y + t.y,
           out.x*x.z + out.y*y.z + out.z*z.z + t.z};
    return Finite(out);
  };
  out = {};
  seen.clear();
  for (uint32_t node = actor; node != root;) {
    if (seen.size() >= 64 || !seen.insert(node).second) return false;
    const auto m = Ptr(b, Ptr(b, node, 20, 16), 12, 88);
    if (!m || !transform(m)) return false;
    node = Ptr(b, m, 84, 32);
    if (!node) return false;
  }
  const auto world = Ptr(b, root, 12, 80);
  return world && transform(world);
}
std::optional<coop::ShotVector> PartnerHead(const uint8_t* b, const Rig& r) {
  const auto list = Ptr(b, Ptr(b, r.anim, 8, 4), 0, 20);
  const uint32_t begin = list ? Load32(b,list+16) : 0, end = list ? Load32(b,list+4) : 0;
  if (!list || end <= begin || (end-begin)%8 || (end-begin)/8 > 128 || !Range(begin,end-begin)) return {};
  const uint32_t count = (end-begin)/8;
  const auto tags = Ptr(b,r.anim,84,count+4);
  // Human tag 20 is a head candidate, not an assumed universal bone index.
  // Require a central point at human head height; incompatible rigs fall back
  // to the network pose +1.8 in the overlay. No dirty refresh or engine call.
  for (uint32_t i = 0; i < count; ++i) {
    if ((tags ? b[tags+4+i] : i) != 20) continue;
    Vec head;
    if (!BoneWorldPosition(b,Load32(b,begin+8*i),head) ||
        std::hypot(head.x-r.pos.x,head.y-r.pos.y) > .65f ||
        head.z-r.pos.z < .8f || head.z-r.pos.z > 2.4f) return {};
    return coop::ShotVector{head.x,head.y,head.z+.25f};
  }
  return {};
}
void LogSpawnBones(const uint8_t* b) {
  auto& s = Get();
  if (!s.bone_copy.actor || s.loading || !s.Environment(b, s.world)) return;
  const auto now = Clock::now();
  if (s.bones_at != Clock::time_point{} && now-s.bones_at < std::chrono::seconds(1)) return;
  s.bones_at = now;
  unsigned role = 0;
  for (const auto& r : {s.bone_source, s.bone_copy}) {
    const char* name = role++ ? "copy" : "source";
    if (Ptr(b, r.actor, 12, 80) != r.matrix || Ptr(b, r.actor, 20, 16) != r.rig ||
        Ptr(b, r.rig, 0, 176) != r.anim) {
      REXLOG_INFO("PUPPET bone-world stopped role={} identity-changed", name);
      s.bone_copy = {}; return;
    }
    const auto list = Ptr(b, Ptr(b, r.anim, 8, 4), 0, 20);
    const uint32_t begin = list ? Load32(b, list+16) : 0, end = list ? Load32(b, list+4) : 0;
    const uint32_t count = end >= begin ? (end-begin)/8 : 0;
    if (!list || end < begin || (end-begin)%8 || count > 128 || !Range(begin, count*8)) {
      REXLOG_INFO("PUPPET bone-world role={} actor={:08X} unavailable=hierarchy", name, r.actor);
      continue;
    }
    // 8286A660 maps human foot tag 60 through anim+84's byte map (+4).
    // H_callback_after_blend uses foot tags 60/80 (.22:38824+).
    const auto tags = Ptr(b, r.anim, 84, count+4);
    uint32_t foot = count;
    for (uint32_t i = 0; i < count; ++i) if ((tags ? b[tags+4+i] : i) == 60) { foot = i; break; }
    const uint32_t indices[]{0, 5, foot};
    const char* labels[]{"root-candidate", "spine-candidate", "foot-tag60"};
    for (unsigned j = 0; j < 3; ++j) {
      const auto index = indices[j];
      const auto bone = index < count ? Load32(b, begin+8*index) : 0;
      const auto matrix = Ptr(b, bone, 12, 80);
      Vec world;
      const bool valid = matrix && BoneWorldPosition(b, bone, world);
      REXLOG_INFO("PUPPET bone-world role={} actor={:08X} sample={} index={} tag={} bone={:08X} matrix={:08X} actor-xyz={} world={} cached-matrix={} valid={} read-only-no-dirty-refresh",
          name, r.actor, labels[j], index, index < count ? int(tags ? b[tags+4+index] : index) : -1,
          bone, matrix, XYZ(ReadVec(b, r.matrix+48)), valid ? XYZ(world) : "unavailable",
          matrix ? XYZ(ReadVec(b, matrix+48)) : "unavailable", valid);
    }
  }
}

// Called only on the game thread. The runtime supplies mutex-protected values
// already interpolated by CoopSession; this function never calls the transport.
bool PartnerReady(const uint8_t* b, uint32_t w) {
  auto& s = Get();
  if (!OffsetSpawn()) s.partner = coop::PartnerBodySnapshot();
  const auto& v = s.partner;
  const bool loading = s.loading || play::StartLevelLoading() ||
      (Range(s.game, 28) && Load32(b, s.game + 4) != 0) ||
      (Load32(b, 0x82D64080) && (Load32(b, 0x82D64080 + 12) == 1 || Load32(b, 0x82D64080 + 12) == 2));
  // G+8 is the request slot used by the borrow epoch; G+0x18 is the live
  // level used by ReadJackPose and the network. Check both, without equating them.
  const bool same = v.local && v.partner && v.local->level == v.partner->level &&
      Range(s.game, 28) && v.local->level == Load32(b, s.game + 0x18);
  const char* reason = OffsetSpawn() ?
      (loading ? "load" : photo::Active() ? "photo-mode" : !s.Environment(b, w) ? "world-level-or-hero-changed" : nullptr) :
      coop::puppet::ReleaseReason({v.enabled, v.state, v.fresh,
          bool(v.local), bool(v.partner), same, photo::Active(), loading, s.Environment(b, w)});
  if (reason) {
    if (s.driving) s.Abort(reason);
    s.partner_eligible = false;
    return false;
  }
  if (!s.partner_eligible) {
    s.partner_eligible = true;
    s.attempted = s.terminal = false;
    s.finish_pending = s.measure = false;
    s.held = s.passed = s.updates = 0;
    s.updated_frame = ~uint64_t{0};
    s.missing_action_kind = 0;
    s.max_late_error = s.max_forward_error = 0;
    ++s.epoch;
    // Do not classify the release/teleport displacement as natural locomotion.
    for (auto& [actor, obs] : s.humans) obs.sampled = false;
    if (OffsetSpawn()) REXLOG_INFO("PUPPET spawn offset epoch={} ready solo=true mode={} offset={}",
        s.epoch, SpawnModeName(), XYZ(Opt().spawn_offset));
    else REXLOG_INFO("PUPPET partner epoch={} ready level={}", s.epoch, v.local->level);
  }
  return true;
}

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

// 82712D40 (.77:28379) and 82715438 (.32:28175): source, instance,
// bookkeeping, in-use in each 16-byte record. Never change pool metadata here.
constexpr uint32_t kPool = 0x82CC3100, kPoolCount = 0x82CCBFCC;
constexpr uint32_t kCreationWorld = 0x828E8388, kCreating = 0x82CCAAAC;
uint32_t Model(const uint8_t* b, uint32_t ai) { return Ptr(b, ai, 0, 16); }
bool IndependentControllers(const uint8_t* b, const Rig& copy, const Rig& source) {
  // 82401AA8 can write through this optional controller chain. An absent
  // controller is legal; a shared extension/controller/data/target is not.
  uint32_t a = Ptr(b, copy.actor, 24, 48), c = Ptr(b, source.actor, 24, 48);
  if (!a || !c || a == c) return false;
  a = Ptr(b, a, 0, 8); c = Ptr(b, c, 0, 8);
  if (a && a == c) return false;
  a = Ptr(b, a, 4, 60); c = Ptr(b, c, 4, 60);
  if (a && a == c) return false;
  a = Ptr(b, a, 56, 60); c = Ptr(b, c, 56, 60);
  return !a || a != c;
}
bool SpawnIdentity(const uint8_t* b, const Spawned& body) {
  const auto& s = Get();
  const auto& r = body.rig;
  // Quarantine outlives pose eligibility. A changed hero, a level request or
  // an invalid driving environment must not let this copy start story scripts
  // before DevPuppetBeforeLoad clears it at the synchronous unload boundary.
  return !s.loading && Load32(b, kCreationWorld) == body.world &&
      Load32(b, 0x82CC97C0) == body.game &&
      Registered(b, body.world, r.actor) &&
      Ptr(b, Ptr(b, r.actor, 24, 48), 4, 88) == r.ai &&
      Ptr(b, r.ai, 64, 8) == r.vars && Model(b, r.ai) == body.model;
}
bool IsSpawned(uint32_t actor) {
  for (const auto& body : Get().spawned) if (body.rig.actor == actor) return true;
  return false;
}
bool CompatibleTemplate(const uint8_t* b, const Rig& source, uint32_t actor) {
  Rig t;
  // Templates may be inactive and need no natural H init of their own. Require
  // the exact model, mesh resource, action bank and human variant of the proven H.
  return ReadRig(b, actor, t) && Model(b, t.ai) == Model(b, source.ai) &&
      t.table == source.table && Load32(b, t.anim + 4) == Load32(b, source.anim + 4) &&
      Load32(b, t.vars) == Load32(b, source.vars) &&
      Load32(b, t.vars + 4) == Load32(b, source.vars + 4);
}
uint32_t FindTemplate(const uint8_t* b, const Rig& source, uint32_t count) {
  uint32_t found = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t p = kPool + 16*i;
    if (Load32(b, p + 4) != source.actor) continue;
    const uint32_t t = Load32(b, p);
    if (!CompatibleTemplate(b, source, t)) continue;
    if (found && found != t) return 0;  // Refuse ambiguous pool lineage.
    found = t;
  }
  if (found) {
    REXLOG_INFO("PUPPET template actor={:08X} template={:08X} origin=pool-source-backlink", source.actor, found);
    return found;
  }
  unsigned listed = 0;
  const auto& s = Get();
  for (const auto& [actor, spawner] : s.spawners) {
    if (Ptr(b, Ptr(b, actor, 24, 8), 4, 88) != spawner.ai ||
        Ptr(b, spawner.ai, 64, 1780) != spawner.vars) continue;
    // Spawner_generate .84:27324-27370 loads its two alternative sources.
    for (const uint32_t offset : {1592u, 1776u}) {
      const uint32_t t = Ptr(b, spawner.vars, offset, 32);
      const uint32_t ai = Ptr(b, Ptr(b, t, 24, 8), 4, 88);
      const uint32_t model = Model(b, ai);
      bool human = false;
      for (const auto& [h, obs] : s.humans) human |= model && Model(b, obs.rig.ai) == model;
      if (!human) continue;
      const bool matches = CompatibleTemplate(b, source, t);
      REXLOG_INFO("PUPPET template-list spawner={:08X} offset={} template={:08X} model={:08X} selected-compatible={}",
                  actor, offset, t, model, matches);
      ++listed;
    }
  }
  REXLOG_INFO("PUPPET template-list count={} spawners-observed={}", listed, s.spawners.size());
  // Compatible spawner sources are diagnostic candidates, not an exact
  // backlink to this NPC. Prefer an exact engine copy of the proven live rig.
  // No immutable actor-template backlink was established. This is still the
  // engine's supported source ABI: 8286FE40 (.102:34117) reads a loaded actor's
  // flags/components, duplicates its rig and calls 8271DC78 to allocate/reset AI.
  // It never requires a special template tag. Do not memcpy or call H init.
  REXLOG_INFO("PUPPET template actor={:08X} template={:08X} origin=live-duplicate-source backlink=not-found",
              source.actor, source.actor);
  return source.actor;
}

bool SpawnCopy(PPCContext& ctx, uint8_t* b, uint32_t w, const Rig& source, const Rig& jack, Rig& copy) {
  auto& s = Get();
  auto refuse = [&](const char* reason) { s.Abort(reason); return false; };
  // This routine is called only from CensusAndSelect in BeforeAi, with the
  // caller, load+5s, scratch and one-attempt gates applied. Without an offset,
  // Connected/same-level gates also apply; offset mode uses the local environment.
  if (!PartnerReady(b, w) || (!OffsetSpawn() && !s.partner.partner)) return refuse("partner-not-ready-before-spawn");
  if (Load32(b, kCreationWorld) != w || Load32(b, kCreating)) return refuse("creation-world-or-busy");
  if (!Model(b, source.ai)) return refuse("source-AI-model-unavailable");
  for (const auto& body : s.spawned) if (body.epoch == s.epoch) return refuse("already-created-this-epoch");
  const uint32_t count = Load32(b, kPoolCount);
  if (count > 512) return refuse("invalid-pool-count");
  uint32_t begin, end;
  if (!Table(b, w, begin, end)) return refuse("active-table-unavailable-before-spawn");
  std::set<uint32_t> existing;
  for (uint32_t p = begin; p < end; p += 8) existing.insert(Load32(b, p));
  const uint32_t t = FindTemplate(b, source, count);
  if (!t) return refuse("ambiguous-template-lineage");
  uint32_t pooled = 0;
  bool pool_match = false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t p = kPool + 16*i;
    if (Load32(b, p) == t && !Load32(b, p + 12)) {
      pool_match = true; pooled = Load32(b, p + 4); break;
    }
  }
  if (pool_match && !pooled) return refuse("invalid-pool-instance");
  if (pooled && (!Range(pooled, 32) || existing.count(pooled) || IsSpawned(pooled) ||
      pooled == t || s.humans.count(pooled))) return refuse("pool-instance-not-spare");
  if (pooled) {
    Rig spare;
    if (!ReadRig(b, pooled, spare) || !coop::puppet::Independent(Mutable(spare), Mutable(source)) ||
        !coop::puppet::Independent(Mutable(spare), Mutable(jack)) ||
        !IndependentControllers(b, spare, source) || !IndependentControllers(b, spare, jack))
      return refuse("pool-components-not-independent");
    for (const uint32_t other : existing) {
      Rig r;
      if (ReadRig(b, other, r) && (!coop::puppet::Independent(Mutable(spare), Mutable(r)) ||
          !IndependentControllers(b, spare, r)))
        return refuse("pool-aliases-loaded-rig");
    }
  }
  if (!pooled && count == 512) return refuse("pool-full-avoid-cleanup");
  const Vec position = OffsetSpawn() ? Vec{source.pos.x + Opt().spawn_offset.x,
      source.pos.y + Opt().spawn_offset.y, source.pos.z + Opt().spawn_offset.z} :
      Vec{s.partner.partner->x, s.partner.partner->y, s.partner.partner->z};
  const float facing = OffsetSpawn() ? std::atan2(source.forward.y, source.forward.x) : s.partner.partner->facing;
  if (!Finite(position) || !std::isfinite(facing)) return refuse("invalid-spawn-pose");
  REXLOG_INFO("PUPPET spawn diagnostic mode={} offset={} target={} script-hold={} setters={} animation-requests={}",
      SpawnModeName(), OffsetSpawn(), XYZ(position), Opt().spawn_mode != Options::SpawnMode::Free,
      !PassiveSpawn(), !PassiveSpawn());
  if (Opt().spawn_mode == Options::SpawnMode::Free)
    REXLOG_WARN("PUPPET UNSAFE FREE COPY: story scripts and triggers run normally; disposable diagnosis only, reload afterwards");
  uint32_t actor = 0;
  {
    Scratch scratch(ctx, b);
    ctx.r3.u64 = t; ctx.r4.u64 = scratch.Vector(position, 0x100);
    __imp__sub_82715AC8(ctx, b);
    actor = ctx.r3.u32;
  }
  if (!Range(actor, 32) || existing.count(actor) || actor == t || IsSpawned(actor) ||
      (pooled && actor != pooled)) return refuse("creation-return-not-new-instance");
  ReadRig(b, actor, copy);
  // Record a new actor as soon as its AI is readable, even on refusal.
  // Matching actor + AI quarantines only the copy, except in explicit free mode.
  if (!copy.ai) return refuse("creation-AI-unavailable");
  s.spawned.push_back({copy, w, s.game, s.level, Model(b, copy.ai), s.epoch});
  s.bone_source = source; s.bone_copy = copy; s.bones_at = {};
  REXLOG_INFO("PUPPET spawn allocated actor={:08X} template={:08X} via={} epoch={} quarantine={} no-free",
              actor, t, pooled ? "pool" : "duplicate", s.epoch,
              Opt().spawn_mode == Options::SpawnMode::Free ? "DISABLED-UNSAFE" : "registered");
  DevPuppetMeshSpawnFixPreflight(b, source.actor, actor, s.epoch);
  // Arm read-only diagnostics even if an unseeded rig fails a later guard.
  // Free/hold deliberately retain the engine's initial action (possibly -1).
  if (PassiveSpawn()) DevPuppetMeshArm(b, source.actor, actor, s.epoch);
  if (!copy.vars || copy.ai == source.ai || copy.ai == jack.ai ||
      copy.vars == source.vars || copy.vars == jack.vars) return refuse("creation-AI-not-independent");
  if (!coop::puppet::Independent(Mutable(copy), Mutable(source)) ||
      !coop::puppet::Independent(Mutable(copy), Mutable(jack)) ||
      !IndependentControllers(b, copy, source) || !IndependentControllers(b, copy, jack))
    return refuse("creation-components-not-independent");
  for (const uint32_t other : existing) {
    Rig r;
    if (ReadRig(b, other, r) && (!coop::puppet::Independent(Mutable(copy), Mutable(r)) ||
        !IndependentControllers(b, copy, r)))
      return refuse("creation-aliases-loaded-rig");
  }
  if (Model(b, copy.ai) != Model(b, source.ai) || copy.table != source.table ||
      Load32(b, copy.anim + 4) != Load32(b, source.anim + 4)) return refuse("creation-resource-lineage-mismatch");
  // 8286D690 (.57) resets the new animation action to -1. Only drive mode
  // seeds an observed tuple and uses the normal facing/position setters.
  if (!PassiveSpawn()) {
    Scratch scratch(ctx, b);
    ctx.r3.u64 = actor; ctx.r4.u64 = source.action; ctx.r5.u64 = source.item; ctx.r6.u64 = 0;
    __imp__sub_82850100(ctx, b);
    ctx.r3.u64 = actor; ctx.r4.u64 = scratch.Vector({std::cos(facing), std::sin(facing), 0}, 0x110);
    __imp__sub_82711278(ctx, b);
    ctx.r3.u64 = actor; ctx.r4.u64 = scratch.Vector(position, 0x100);
    __imp__sub_82401AA8(ctx, b);
  }
  if (!PassiveSpawn() && (!ReadRig(b, actor, copy) || !Registered(b, w, actor) || *s.Qualify(b, copy, jack) ||
      !LegalAction(b, copy, copy.action))) return refuse("creation-rig-not-ready");
  Rig original;
  if (!ReadRig(b, source.actor, original) || !Same(source, original) ||
      Distance(source.pos, original.pos) != 0 || Distance(source.forward, original.forward) != 0 ||
      source.action != original.action || source.item != original.item)
    return refuse("creation-changed-original-readback");
  // The copy is proven by native duplication lineage, not a fabricated H init.
  Observation obs;
  obs.rig = copy; obs.bound = true; obs.generation = ++s.generation;
  s.humans.emplace(actor, std::move(obs));
  REXLOG_INFO("PUPPET spawn ok actor={:08X} template={:08X} via={} epoch={} source={:08X} ai={:08X} vars={:08X} xyz={} active-ai={}",
              actor, t, pooled ? "pool" : "duplicate", s.epoch, source.actor, copy.ai, copy.vars,
              XYZ(ReadVec(b, copy.matrix+48)), Member(b, w, actor));
  REXLOG_INFO("PUPPET spawn trigger-policy={} CapaBit=ext+40 life=stimulus-test universal-exclusion=unproven no-capability-or-life-writes",
      Opt().spawn_mode == Options::SpawnMode::Free ? "pass-through-UNSAFE" : "filter-ObjOfAIModelInBV-only");
  REXLOG_INFO("PUPPET spawn source actor={:08X} ai={:08X} identity-transform-action=unchanged scheduled-dispatch=pass-through", source.actor, source.ai);
  if (!PassiveSpawn()) DevPuppetMeshArm(b, source.actor, actor, s.epoch);
  return true;
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
  std::map<uint32_t, unsigned> membership;
  for (uint32_t p = begin; p < end; p += 8) ++membership[Load32(b, p)];
  for (uint32_t p = begin; p < end; p += 8) {
    const uint32_t actor = Load32(b, p);
    if (IsSpawned(actor)) continue;  // Never learn natural motion or clone a prior copy.
    const uint32_t index = (p-begin)/8;
    auto it = s.humans.find(actor);
    if (it == s.humans.end()) {
      if (log) CensusOtherRig(b, actor, index, s.epoch, w, s.level, s.hero);
      continue;  // Other families remain census-only, including possible KAnn.
    }
    auto& obs = it->second;
    seen.insert(actor);
    const bool duplicate = membership.at(actor) != 1;
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
    // Index is local to this epoch, not a persistent actor ID. Require the same
    // slot and full identity for the stability interval, then bind the actor.
    obs.stable = obs.present && obs.index == index && !duplicate ? obs.stable + s.dt : 0;
    obs.index = index;
    obs.present = true;
    const char* reason = jack_ok ? s.Qualify(b, rig, jack) : "hero-rig-unavailable";
    if (!*reason && duplicate) reason = "duplicate-active-membership";
    if (!*reason && obs.stable < 1.0) reason = "not-stable-one-second";
    if (LegalAction(b, rig, rig.action)) {
      const uint32_t tuple = (rig.action << 8) | rig.item;
      obs.actions.insert(tuple);
      if (Opt().partner && obs.sampled && obs.previous_tuple == tuple && s.dt > 0 &&
          obs.present && !duplicate && obs.stable >= 1.0) {
        const float delta = std::hypot(rig.pos.x-obs.previous.x, rig.pos.y-obs.previous.y);
        const float speed = delta / s.dt;
        if (delta <= 5 && speed < 10000) {
          const bool moving = speed >= coop::puppet::kIdleSpeed;
          const uint32_t sample_key = (tuple << 1) | uint32_t(moving);
          auto [sample, added] = obs.motion.emplace(sample_key,
              coop::puppet::Action{uint16_t(rig.action), uint8_t(rig.item), speed});
          // Retain the fastest natural example to distinguish walk from run.
          sample->second.speed = std::max(sample->second.speed, speed);
          if (added) REXLOG_INFO("PUPPET observed actor={:08X} action={} item={} motion={} speed={:.3f}",
              actor, rig.action, rig.item, moving ? "moving" : "idle", speed);
        }
      }
      obs.previous = rig.pos; obs.previous_tuple = tuple; obs.sampled = true;
    } else obs.sampled = false;
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
  if (!Opt().borrow || (Opt().partner && !s.partner_eligible) || s.attempted || s.terminal ||
      now - s.loaded < std::chrono::seconds(5)) return;
  s.attempted = true;
  const Candidate* pick = nullptr;
  float closest = Opt().max_units;
  bool tied = false;
  for (const auto& c : candidates) {
    if (*c.reason) continue;
    if (Opt().nearest) {
      const float d = Distance(c.rig.pos, jack.pos);
      if (d > Opt().max_units) continue;
      if (!pick || d < closest - 0.0001f) { pick = &c; closest = d; tied = false; }
      else if (std::fabs(d - closest) <= 0.0001f) tied = true;
    } else if (Opt().index ? c.index == Opt().census_index :
               c.key == Opt().key && c.ordinal == Opt().ordinal) {
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
  Rig selected = pick->rig;
  if (Opt().spawn && !SpawnCopy(ctx, b, w, pick->rig, jack, selected)) return;
  if (PassiveSpawn()) {
    s.terminal = true;
    REXLOG_INFO("PUPPET spawn diagnostic-only actor={:08X} mode={} no-post-create-setters no-animation-requests",
        selected.actor, SpawnModeName());
    return;
  }
  s.selected = selected; s.selected_generation = s.humans.at(selected.actor).generation;
  if (Opt().spawn) { s.action = selected.action; s.item = selected.item; }
  if (Opt().partner) {
    s.observed_motion.clear();
    for (const auto& [key, sample] : obs.motion) {
      s.observed_motion.push_back(sample);
      REXLOG_INFO("PUPPET partner observed-set actor={:08X} action={} item={} speed={:.3f} motion={}",
          pick->rig.actor, sample.action, sample.item, sample.speed,
          sample.speed < coop::puppet::kIdleSpeed ? "idle" : "moving");
    }
    REXLOG_INFO("PUPPET partner observed-set count={} actor={:08X}", s.observed_motion.size(), pick->rig.actor);
  }
  s.transition_pending = s.action != selected.action || s.item != selected.item;
  s.origin = selected.pos; s.initial_forward = selected.forward;
  const float len = std::hypot(s.initial_forward.x, s.initial_forward.y);
  s.initial_forward = {s.initial_forward.x / len, s.initial_forward.y / len, 0};
  s.desired = s.origin; s.facing = s.initial_forward; s.elapsed = 0; s.driving = true;
  REXLOG_INFO("PUPPET select epoch={} mode={} key={:08X} ordinal={} index={} actor={:08X} ai={:08X} vars={:08X} matrix={:08X} anim={:08X} generation={} human={}/{} distance={:.3f} units=guest nearest-limit={} action={} item={} observed-action={} observed-item={}",
              s.epoch, Opt().spawn ? "spawn" : Opt().nearest ? "nearest" : Opt().index ? "index" : "key", pick->key, pick->ordinal, pick->index,
              s.selected.actor, s.selected.ai, s.selected.vars, s.selected.matrix, s.selected.anim,
              s.selected_generation, Load32(b, s.selected.vars), Load32(b, s.selected.vars + 4),
              Distance(s.origin, jack.pos), Opt().nearest ? Opt().max_units : 0.0f, s.action, s.item,
              s.selected.action, s.selected.item);
}
}  // namespace

void DevPuppetGameFrame(uint8_t* b) {
  if (!Opt().on || !Opt().partner) return;
  auto& s = Get();
  if (!Enabled()) {
    if (s.driving) s.Abort("coop-disabled");
    s.partner_eligible = false;
    return;
  }
  ++s.frame;
  DevPuppetMeshFrame(s.frame);
  // Also release while photo mode or loading prevents normal AI passes.
  if (s.ready) PartnerReady(b, s.world);
}

void DevPuppetBeforeLoad() {
  DevPuppetMeshReset();
  if (!Enabled() && Get().spawned.empty()) return;
  auto& s = Get();
  if (s.driving || s.finish_pending) s.Abort("level-load-started");
  s.selected = {}; s.humans.clear(); s.keys.clear(); ++s.epoch;
  if (!s.spawned.empty()) REXLOG_INFO("PUPPET spawn unload count={} no-free script-quarantine=cleared", s.spawned.size());
  s.spawned.clear(); s.spawners.clear();
  s.bone_source = {}; s.bone_copy = {}; s.bones_at = {};
  s.world = s.hero = s.game = 0; s.loading = true; s.ready = false;
  s.census_at = {}; s.tick_at = {};
  s.env_fail_logs = 0;
  if (Opt().partner) {
    s.partner_eligible = false; s.attempted = s.terminal = false;
    s.prepared = false;
    coop::SetPartnerBodyDriven(false);
  }
}
void DevPuppetAfterLoad(const uint8_t* b, uint32_t game) {
  if (!Enabled()) return;
  auto& s = Get();
  s.game = game; s.level = Range(game, 28) ? Load32(b, game + 8) : 0;
  s.loading = false; s.loaded = Clock::now();
}

void DevPuppetBeforeAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
  if (!Enabled()) return;
  auto& s = Get();
  s.prepared = false;
  if (!Opt().valid) { s.Abort("invalid-KK_DEV_PUPPET-or-spawn-option-syntax"); return; }
  if (Opt().partner && s.driving && !PartnerReady(b, w)) return;
  if (s.loading || !s.game || play::StartLevelLoading()) return;
  // 82881B70 passes its saved world in r3 at this call (.124, return 82881CF0).
  // Check the caller before binding an epoch to the supplied world.
  if (uint32_t(ctx.lr) != 0x82881CF0) { if (s.driving) s.Abort("unexpected-AI-pass-caller"); return; }
  const uint32_t hero = Load32(b, 0x82CCBF7C);
  if (!s.ready && Range(w, 308) && Range(hero, 32)) { s.world = w; s.hero = hero; s.ready = true; }
  if (!s.ready && Clock::now() - s.loaded >= std::chrono::seconds(5)) {
    s.Abort("world-or-hero-unavailable-after-load"); return;
  }
  if (!s.Environment(b, w)) {
    if (s.ready && s.env_fail_logs++ < 3)
      REXLOG_INFO("PUPPET env-fail hook-world={:08X} s.world={:08X} game={:08X}/{:08X} level={}/{} "
                  "game+4={} hero={:08X}/{:08X} startloading={}", w, s.world, Load32(b, 0x82CC97C0), s.game,
                  Range(s.game, 28) ? Load32(b, s.game + 8) : 0, s.level,
                  Range(s.game, 28) ? Load32(b, s.game + 4) : 0, Load32(b, 0x82CCBF7C), s.hero,
                  play::StartLevelLoading());
    if (s.ready) { s.Abort("world-level-load-or-hero-changed"); s.humans.clear(); s.ready = false; }
    return;
  }
  if (Opt().partner) {
    PartnerReady(b, w);
    if (s.updated_frame == s.frame || photo::Active()) return;
  }
  s.dt = FrameTime(b);
  if (!std::isfinite(s.dt) || s.dt < 0 || s.dt > 0.25f) { s.Abort("invalid-simulation-dt"); return; }
  if (!Scratch::CanUse(ctx)) { s.Abort("invalid-guest-scratch-stack"); return; }
  if (s.driving) { Rig rig; s.Validate(b, w, rig); }
  else if (!s.terminal) CensusAndSelect(ctx, b, w);
  s.prepared = true;
}

void DevPuppetAfterAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
  if (!Enabled()) return;
  auto& s = Get();
  if (!s.driving) return;
  if (Opt().partner && (!s.prepared || !PartnerReady(b, w) || s.updated_frame == s.frame)) return;
  Rig rig;
  if (!s.Validate(b, w, rig)) return;
  if (!Scratch::CanUse(ctx)) { s.Abort("invalid-guest-scratch-stack"); return; }
  if (Opt().partner) {
    // Offset diagnosis keeps driving a stationary target at the creation point.
    // It never requires or follows a network pose, even if a partner connects.
    coop::PlayerPose p{};
    if (OffsetSpawn()) {
      p.x = s.origin.x; p.y = s.origin.y; p.z = s.origin.z;
      p.facing = std::atan2(s.initial_forward.y, s.initial_forward.x);
    } else p = *s.partner.partner;
    const auto step = coop::puppet::ClampStep({rig.pos.x, rig.pos.y, rig.pos.z}, {p.x, p.y, p.z});
    if (!step) { s.Abort("invalid-partner-position"); return; }
    if (step->teleport) REXLOG_INFO("PUPPET partner teleport epoch={} from={} to={} threshold=5",
        s.epoch, XYZ(rig.pos), XYZ({p.x, p.y, p.z}));
    s.desired = {step->target.x, step->target.y, step->target.z};
    s.facing = {std::cos(p.facing), std::sin(p.facing), 0};
    const auto choice = coop::puppet::ChooseAction(p.speed, s.observed_motion);
    if (choice) {
      s.action = choice->action; s.item = choice->item;
      s.transition_pending = rig.action != s.action || rig.item != s.item;
      s.missing_action_kind = 0;
      if (s.transition_pending) REXLOG_INFO("PUPPET partner action={} item={} speed={:.3f} restart=0",
          s.action, s.item, p.speed);
    } else {
      s.action = rig.action; s.item = rig.item; s.transition_pending = false;
      const unsigned kind = std::abs(p.speed) < coop::puppet::kIdleSpeed ? 1 : 2;
      if (s.missing_action_kind != kind) REXLOG_INFO("PUPPET partner keep-current reason=no-observed-{}-action action={} item={}",
          std::abs(p.speed) < coop::puppet::kIdleSpeed ? "idle" : "moving", s.action, s.item);
      s.missing_action_kind = kind;
    }
    s.elapsed += double(s.dt);
    s.updated_frame = s.frame;
    ++s.updates;
  } else {
    s.elapsed = std::min(8.0, s.elapsed + double(s.dt));
    const float move = s.elapsed <= 2 ? 0 : s.elapsed < 4 ? float((s.elapsed-2)/2) :
                       s.elapsed <= 6 ? 1 : float((8-s.elapsed)/2);
    const float turn = s.elapsed <= 4 ? 0 : s.elapsed < 6 ? float((s.elapsed-4)/2) : float((8-s.elapsed)/2);
    const float angle = turn * 1.57079632679f;
    s.desired = {s.origin.x + s.initial_forward.x * 0.5f * move,
                 s.origin.y + s.initial_forward.y * 0.5f * move, s.origin.z};
    s.facing = {s.initial_forward.x * std::cos(angle) - s.initial_forward.y * std::sin(angle),
                s.initial_forward.x * std::sin(angle) + s.initial_forward.y * std::cos(angle), 0};
  }
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
  // A requested action can take a few frames to show, and the rig may refuse one: never drop the body for it
  // (Claude, run c08 14:49: released after 1 update). Log, then follow what the rig really plays.
  if (rig.action != s.action || rig.item != s.item) {
    static int logged = 0;
    if (logged++ < 5)
      REXLOG_INFO("PUPPET readback action={} item={} wanted={} {} (kept driving)", rig.action, rig.item, s.action, s.item);
    s.action = rig.action; s.item = rig.item;
  }
  s.early_pos = rig.pos; s.early_forward = rig.forward; s.early_frame = Frame(ctx, b, rig.actor);
  s.measure = true;
  if (Opt().partner) coop::SetPartnerBodyDriven(true);
  else if (s.elapsed >= 8) { s.driving = false; s.finish_pending = true; }
}

void DevPuppetLateWorld(PPCContext& ctx, uint8_t* b, uint32_t w) {
  if (!Enabled()) return;
  DevPuppetMeshTick(b);
  LogSpawnBones(b);
  auto& s = Get();
  if (!s.measure) return;
  if (Opt().partner && !PartnerReady(b, w)) return;
  s.measure = false;
  Rig rig;
  if (!s.Validate(b, w, rig)) return;
  if (Opt().partner) coop::SetPartnerHead(PartnerHead(b,rig), s.partner.partner->level);
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
    if (OffsetSpawn()) REXLOG_INFO("PUPPET spawn offset driving epoch={} updates={} game-frame={} target={} late-error={:.6f}",
        s.epoch, s.updates, s.frame, XYZ(s.desired), Distance(s.desired, rig.pos));
    else if (Opt().partner) REXLOG_INFO("PUPPET partner driving epoch={} updates={} game-frame={} speed={:.3f} remote-action={} remote-item={} late-error={:.6f} late-z-error={:.6f} large-late-error={}",
        s.epoch, s.updates, s.frame, s.partner.partner->speed, s.partner.partner->action, s.partner.partner->item,
        Distance(s.desired, rig.pos), std::abs(s.desired.z-rig.pos.z), Distance(s.desired, rig.pos) > .25f);
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
  if (!Enabled()) return;
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
void PuppetObserveSpawner(uint8_t* b, uint32_t actor, uint32_t ai) {
  if (!Enabled() || !Opt().spawn) return;
  if (Ptr(b, Ptr(b, actor, 24, 8), 4, 88) != ai) return;
  const uint32_t vars = Ptr(b, ai, 64, 8380);
  if (vars) Get().spawners[actor] = {ai, vars};
}
bool HoldSpawned(PPCContext& ctx, uint8_t* b) {
  if (!Opt().spawn || Opt().spawn_mode == Options::SpawnMode::Free || uint32_t(ctx.lr) != 0x8271D304) return false;
  auto& s = Get();
  const uint32_t actor = Load32(b, 0x82CCBF6C), ai = Load32(b, 0x82CCBF64);
  for (auto& body : s.spawned) {
    if (body.rig.actor != actor || body.rig.ai != ai || !SpawnIdentity(b, body)) continue;
    ++body.held;
    // Release stops network writes, but this allocated copy must never begin
    // scheduled story scripts. Only this exact actor + independent AI is held.
    if (s.driving && s.selected.actor == actor) {
      if (!Enabled() || !PartnerReady(b, s.world)) {
        if (s.driving) s.Abort("coop-disabled");
      } else {
        s.last_block = ctx.r3.u32; s.last_track = ctx.r4.u32; ++s.held;
      }
    }
    if (body.held == 1 || body.held % 1200 == 0)
      REXLOG_INFO("PUPPET spawn hold actor={:08X} ai={:08X} count={} driving={}", actor, ai, body.held, s.driving && s.selected.actor == actor);
    ctx.r3.u64 = 0;
    return true;
  }
  return false;
}
void FilterSpawnedTrigger(PPCContext& ctx, uint8_t* b, uint32_t output, uint32_t capacity) {
  if (!Opt().spawn || Opt().spawn_mode == Options::SpawnMode::Free || capacity != 32 || ctx.r3.s32 <= 0 || ctx.r3.u32 > capacity ||
      !Range(output, 4*capacity)) return;
  // Only called for TrigTest_ObjOfAIModelInBV's query return 8245D334.
  // .92: query with r4=32, r9=stack output; then loop over r3 actor pointers.
  std::vector<uint32_t> hidden;
  for (const auto& body : Get().spawned) if (SpawnIdentity(b, body)) hidden.push_back(body.rig.actor);
  if (hidden.empty()) return;
  std::array<uint32_t, 32> actors{};
  const uint32_t count = ctx.r3.u32;
  for (uint32_t i = 0; i < count; ++i) actors[i] = Load32(b, output + 4*i);
  const auto kept = coop::puppet::FilterActors(std::span(actors.data(), count), hidden);
  if (kept == count) return;
  for (uint32_t i = 0; i < count; ++i) Store32(b, output + 4*i, i < kept ? actors[i] : 0);
  ctx.r3.u64 = kept;
  REXLOG_INFO("PUPPET spawn trigger-filter query=ObjOfAIModelInBV removed={} kept={}", count-kept, kept);
}
bool PuppetHold(PPCContext& ctx, uint8_t* b) {
  if (HoldSpawned(ctx, b)) return true;
  if (Opt().spawn) {
    if (Get().driving) ++Get().passed;
    return false;
  }
  if (!Enabled()) return false;
  auto& s = Get();
  if (!s.driving) return false;
  if (uint32_t(ctx.lr) != 0x8271D304 || Load32(b, 0x82CCBF64) != s.selected.ai ||
      Load32(b, 0x82CCBF6C) != s.selected.actor) { ++s.passed; return false; }
  if (Opt().partner && (photo::Active() || !PartnerReady(b, s.world))) {
    if (s.driving) { s.Abort("photo-mode"); s.partner_eligible = false; }
    return false;
  }
  Rig rig;
  if (!s.Validate(b, s.world, rig)) return false;
  s.last_block = ctx.r3.u32; s.last_track = ctx.r4.u32; ++s.held;
  ctx.r3.u64 = 0;
  return true;
}
}  // namespace kk

REX_HOOK_RAW(sub_8268D8E8) {
  const bool on = kk::Enabled();
  const uint32_t actor = on ? kk::hfr::Load32(base, 0x82CCBF6C) : 0;
  const uint32_t ai = on ? kk::hfr::Load32(base, 0x82CCBF64) : 0;
  __imp__sub_8268D8E8(ctx, base);
  if (on) kk::PuppetObserveInit(base, actor, ai);
}
REX_HOOK_RAW(sub_826EB8C0) {
  const bool on = kk::Enabled() && kk::Opt().spawn;
  const uint32_t actor = on ? kk::hfr::Load32(base, 0x82CCBF6C) : 0;
  const uint32_t ai = on ? kk::hfr::Load32(base, 0x82CCBF64) : 0;
  __imp__sub_826EB8C0(ctx, base);
  if (on) kk::PuppetObserveSpawner(base, actor, ai);
}
REX_HOOK_RAW(sub_8284B028) {
  const bool filter = kk::Opt().spawn && uint32_t(ctx.lr) == 0x8245D334;
  const uint32_t output = ctx.r9.u32, capacity = ctx.r4.u32;
  __imp__sub_8284B028(ctx, base);
  if (filter) kk::FilterSpawnedTrigger(ctx, base, output, capacity);
}
#endif
