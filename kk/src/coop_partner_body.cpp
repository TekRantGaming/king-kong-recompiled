// Shared partner body service. Automatic borrowing is the shipping default.
// No transport calls or save writes. Native copies are the last visible fallback.
// KK_DEV_PUPPET=scan (or 1), borrow:nearest[:<max guest units>],
// borrow:index:<zero-based active-table index>[:<action>:<item>], or the legacy
// borrow:<hex key>:<ordinal>[:<action>:<item>]. Indices belong to the current epoch.
// partner:index:<n> or partner:nearest[:<max>] follows the copied partner pose.
// partner:spawn:<n> creates an independent engine copy of that loaded human.
// KK_DEV_PUPPET_SPAWN_MODE=drive (default), hold, or free (UNSAFE story scripts).
// KK_DEV_PUPPET_SPAWN_OFFSET=x,y,z uses source+offset, permits solo diagnosis,
// and gives drive mode a stationary target at that creation point.
#include "coop_partner_body.h"
#if defined(KK_DEV_TOOLS)
#include "art.h"
#include "dev_puppet_mesh.h"
#include "coop_stimulus_debug.h"
#include "dev_puppet_variant.h"
#endif

#include <algorithm>
#include <array>
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
#include "coop_partner_selection.h"
#include "coop_runtime.h"
#include "coop_nametag.h"
#include "coop_observer.h"
#include "coop_story_game.h"
#include "coop_enemies_game.h"
#include "photo_mode.h"
#include "settings.h"

REX_EXTERN(__imp__sub_8268D8E8);
REX_EXTERN(__imp__sub_827117E0);
REX_EXTERN(__imp__sub_82711278);
REX_EXTERN(__imp__sub_82401AA8);
REX_EXTERN(__imp__sub_82850100);
REX_EXTERN(__imp__sub_823F8AF0);
REX_EXTERN(__imp__sub_823F8FB0);
REX_EXTERN(__imp__sub_8280F328);
REX_EXTERN(__imp__sub_82715AC8);
REX_EXTERN(__imp__sub_826ECDF8);
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_826EB8C0);
#endif
REX_EXTERN(__imp__sub_8284B028);
REX_EXTERN(__imp__sub_82881B70);
REX_EXTERN(__imp__sub_82454690);
REX_EXTERN(__imp__sub_8271E4E8);
REX_EXTERN(__imp__sub_82449F18);
REX_EXTERN(__imp__sub_8242AA88);
REX_EXTERN(__imp__sub_8271A6F8);
REX_EXTERN(__imp__sub_827128F0);
#if defined(KK_DEV_TOOLS)
namespace kk {
void DevFishAfterWorld(PPCContext&, uint8_t*);
void DevTrigCapaResult(uint8_t*, uint32_t, uint32_t, uint32_t);
}
#endif

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
  bool on = true, borrow = true, partner = true, automatic = true;
  bool spawn = false, nearest = false, index = false, valid = true, action_set = false;
  uint32_t key = 0, ordinal = 0, census_index = 0, action = 0, item = 0;
  float max_units = 15.0f;
  Options() {
#if defined(KK_DEV_TOOLS)
    const char* env = std::getenv("KK_DEV_PUPPET");
    if (!env || !*env) return;
    if (std::string(env) == "partner:auto") return;
    automatic = false; partner = false; borrow = false;
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
#endif
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
  bool following = false, cinematic = false, grabbed = false, ann = false;
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
    r.dead_track |= fn == 0x826B95A0 || fn == 0x826F7AD0;  // H / PNJInd death.
    r.following |= fn == 0x826C3A98;  // H_ETAT_IA_suivi.
    r.cinematic |= fn == 0x826C3668 || fn == 0x826ABE88 || fn == 0x826AB3A8 ||
        fn == 0x826AAF18 || fn == 0x826AC808;
    r.grabbed |= fn == 0x826C63A8 || fn == 0x826A5CB8 || fn == 0x826C9900 ||
        fn == 0x826C83A8 || fn == 0x826C7BE8 || fn == 0x826C6990 ||
        fn == 0x826A4FE8 || fn == 0x826B6A40;
    r.ann |= fn == 0x8268BF48 || fn == 0x82667468 || fn == 0x8268B9F0 || fn == 0x82685138 ||
        fn == 0x826B4810 || fn == 0x826AD1F8;
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
  if (!r.matrix || !r.vars || !r.table || !r.layer ||
      !coop::puppet::AnimationEventHeader(r.anim, [&](uint32_t p, uint32_t off, uint32_t size) {
        return Ptr(b, p, off, size);
      })) return false;
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
#if defined(KK_DEV_TOOLS)
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
#endif
bool LegalAction(const uint8_t* b, const Rig& r, uint32_t action) {
  if (action >= 0x8000 || action >= Load16(b, r.table)) return false;
  return action == 0 || Ptr(b, r.table, 4 * (action + 4), 4) != 0;
}
bool LoadedAction(const uint8_t* b, const Rig& r, uint32_t action, uint32_t variant = 0) {
  if (!LegalAction(b,r,action)) return false;
  const auto items = Ptr(b,r.table,4*(action+4),20);
  return coop::puppet::AnimationItemHeader(items,variant,[&](uint32_t p) { return b[p]; },
      [&](uint32_t p,uint32_t off,uint32_t size) { return Ptr(b,p,off,size); }) != 0;
}
std::optional<uint8_t> LoadedItem(const uint8_t* b,const Rig& r,uint32_t action) {
  if (!LegalAction(b,r,action)) return {};
  const auto items=Ptr(b,r.table,4*(action+4),20);
  if (!items) return {};
  for (unsigned item=0;item<b[items];++item)
    if (LoadedAction(b,r,action,item)) return uint8_t(item);
  return {};
}
uint32_t BodyWeapon(const uint8_t* b,const Rig& r) {
  const auto game=Load32(b,0x82CC97C0), slot=Load32(b,r.vars+4);
  // H_callback_tir indexes the same G+13124 table by the human's slot.
  return slot>=1 && slot<=12 && Range(game,13176) ? Load32(b,game+13124+4*slot) : 0;
}
uint8_t AnimationSupport(const uint8_t* b, const Rig& r, uint8_t weapon) {
  if (Load32(b,r.vars) != 0) return 0;
  const auto held=BodyWeapon(b,r);
  const bool gun=held>=1 && held<=4;
  if (gun) weapon=uint8_t(held);
  const bool gait = LoadedItem(b,r,0).has_value() && LoadedItem(b,r,10).has_value();
  const bool fire = coop::puppet::FireAction(weapon,false,
      [&](uint16_t action) { return LoadedItem(b,r,action); }).has_value();
  return 4*uint8_t(gait) + 2*uint8_t(fire) + uint8_t(gun);
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
#if defined(KK_DEV_TOOLS)
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

#endif
struct Observation {
  Rig rig;
  uint64_t generation = 0;
  uint32_t index = 0;
  double stable = 0;
  bool present = false;
  bool bound = false;
  bool observed_ally = false;
  bool scheduled = false;
  bool other_human = false;  // Natural PNJInd_Init, never inferred from a rig.
  bool story_copy_failed = false;  // Per observed identity; allow the lease if native copying fails.
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
const char* AutoSafety(const uint8_t* b, uint32_t world, const Rig& r);
bool AutoMembership(const uint8_t* b, uint32_t world, const Rig& r);

struct State {
  uint64_t epoch = 0, generation = 0, held = 0, passed = 0;
  uint32_t world = 0, game = 0, level = 0, hero = 0;
  bool loading = true, ready = false, driving = false, attempted = false, terminal = false;
  bool finish_pending = false, measure = false, transition_pending = false;
  bool partner_eligible = false, prepared = false;
  bool combat_publishing = false;
  bool selected_copy = false;
  int selected_tier = -1;
  unsigned missing_action_kind = 0;
  coop::puppet::AnimationDrive animation;
  float aim_clip_frame = -1;
  uint32_t aim_tuple = UINT32_MAX;
  uint32_t rate_layer = 0;
  uint8_t* rate_base = nullptr; // game thread only; restored before relinquishing the exact rig
  uint8_t saved_rate = 60, playback_rate = 60;
  uint32_t requested_tuple = UINT32_MAX;
  uint32_t animation_shots = 0;
#if defined(KK_DEV_TOOLS)
  unsigned capture_left = 0;
#endif
  std::set<uint32_t> retired;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> deferred_prg;
  std::map<uint32_t, Clock::time_point> story_cooldown;
  Clock::time_point retry_at{}, copy_wait_until{};
  uint64_t frame = 0, updated_frame = ~uint64_t{0}, updates = 0;
  coop::PuppetSnapshot partner;
  std::vector<coop::puppet::Action> observed_motion;
  std::vector<coop::puppet::Gait> native_gaits;
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
    Rig current;
    if (rate_base && rate_layer && ReadRig(rate_base,selected.actor,current) &&
        Same(current,selected) && current.layer == rate_layer) rate_base[rate_layer+2] = saved_rate;
    rate_base = nullptr; rate_layer = 0;
    driving = false; finish_pending = false; measure = false; terminal = true;
    if (Opt().partner) {
      coop::SetPartnerBodyDriven(false);
      REXLOG_INFO("PUPPET partner released reason={} epoch={} actor={:08X} tier={} copy={} held={} passed={} updates={} AI-resumes-at-current-spot={}",
                  reason, epoch, selected.actor, selected_tier, selected_copy, held, passed, updates, !selected_copy);
      if (Opt().spawn || selected_copy) {
        REXLOG_INFO("PUPPET spawn {} reason={} epoch={} actor={:08X} no-free script-quarantine={}",
                    selected.actor ? "released" : "refused", reason, epoch, selected.actor,
                    Opt().spawn_mode == Options::SpawnMode::Free ? "DISABLED-UNSAFE" : "until-unload");
      }
    } else {
      REXLOG_INFO("PUPPET abort reason={} epoch={} actor={:08X} held={} passed={} reload-required",
                  reason, epoch, selected.actor, held, passed);
    }
    selected = {};
    selected_copy = false; selected_tier = -1;
    if (Opt().automatic) {
      terminal = attempted = false;
      retry_at = Clock::now() + std::chrono::seconds(1);
      copy_wait_until = retry_at + std::chrono::milliseconds(250);
      // Replacements need fresh continuous membership. A census frozen during
      // driving must not certify that another actor stayed loaded all along.
      for (auto& [actor, obs] : humans) {
        obs.stable = 0; obs.present = false; obs.sampled = false;
      }
    }
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
    if (Opt().spawn || selected_copy) {
      bool own = false;
      for (const auto& body : spawned) own |= body.rig.actor == selected.actor && SpawnIdentity(b, body);
      if (!own) { Abort("spawn-identity-changed"); return false; }
    }
    const auto it = humans.find(selected.actor);
    Rig jack;
    if (it == humans.end() || it->second.generation != selected_generation ||
        !((Opt().spawn || selected_copy) ? Registered(b, w, selected.actor) : Opt().automatic ?
          AutoMembership(b, w, selected) : Member(b, w, selected.actor)) ||
        !ReadRig(b, selected.actor, now) || !Same(now, selected) ||
        !ReadRig(b, hero, jack) || *Qualify(b, now, jack) || !LegalAction(b, now, action)) {
      if (Opt().automatic) retired.insert(selected.actor);
      Abort("selection-identity-or-rig-changed"); return false;
    }
    if (Opt().automatic && !selected_copy) {
      if (const char* reason = AutoSafety(b, w, now)) {
        Abort(reason); return false;
      }
    }
    return true;
  }
};
State& Get() { static State state; return state; }

uint32_t OwnedAimBone(const uint8_t* b,const Rig& rig,uint8_t tag) {
  const auto list=Ptr(b,Ptr(b,rig.anim,8,4),0,20);
  if (!list) return 0;
  const auto begin=Load32(b,list+16), end=Load32(b,list+4);
  if (end<begin || (end-begin)%8 || (end-begin)/8>128 || !Range(begin,end-begin)) return 0;
  const auto count=(end-begin)/8, tags=Ptr(b,rig.anim,84,count+4);
  for (uint32_t i=0;i<count;++i) {
    if ((tags ? b[tags+4+i] : i)!=tag) continue;
    const auto bone=Load32(b,begin+8*i);
    if (!Range(bone,32) || !Ptr(b,bone,12,240)) return 0;
    auto owner=bone;
    // The local matrix's owner chain is the same one used by 8270FCE0.
    // Refuse shared/source bones, malformed chains and unattached rigid parts.
    for (unsigned depth=0;depth<64 && owner!=rig.actor;++depth) {
      if (!(Load32(b,owner)&1)) return 0;
      const auto local=Ptr(b,Ptr(b,owner,20,16),12,88);
      owner=Ptr(b,local,80,32);
      if (!owner) return 0;
    }
    return owner==rig.actor ? bone : 0;
  }
  return 0;
}

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
  std::array<uint32_t,64> seen{};
  size_t seen_count = 0;
  auto visit = [&](uint32_t node) {
    if (seen_count == seen.size() || std::find(seen.begin(),seen.begin()+seen_count,node) != seen.begin()+seen_count) return false;
    seen[seen_count++] = node; return true;
  };
  while (Load32(b, root) & 1) {
    if (!visit(root)) return false;
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
  seen_count = 0;
  for (uint32_t node = actor; node != root;) {
    if (!visit(node)) return false;
    const auto m = Ptr(b, Ptr(b, node, 20, 16), 12, 88);
    if (!m || !transform(m)) return false;
    node = Ptr(b, m, 84, 32);
    if (!node) return false;
  }
  const auto world = Ptr(b, root, 12, 80);
  return world && transform(world);
}
std::optional<coop::ShotVector> PartnerHead(const uint8_t* b, const Rig& r) {
  const coop::nametag::Point root{r.pos.x,r.pos.y,r.pos.z};
  std::optional<coop::nametag::Point> highest;
  const auto list = Ptr(b, Ptr(b, r.anim, 8, 4), 0, 20);
  const uint32_t begin = list ? Load32(b,list+16) : 0, end = list ? Load32(b,list+4) : 0;
  if (list && end > begin && (end-begin)%8 == 0 && (end-begin)/8 <= 128 && Range(begin,end-begin)) {
    for (uint32_t i = 0; i < (end-begin)/8; ++i) {
      Vec joint;
      if (BoneWorldPosition(b,Load32(b,begin+8*i),joint))
        coop::nametag::ConsiderHead(root,{joint.x,joint.y,joint.z},highest);
    }
  }
  // Even without readable bones, anchor to the rendered body's root, not the
  // separately interpolated network pose. No dirty refresh or guest writes.
  const auto anchor = coop::nametag::AboveHead(root,highest);
  return coop::ShotVector{anchor.x,anchor.y,anchor.z};
}
#if defined(KK_DEV_TOOLS)
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
#endif
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

std::optional<Vec> NativeRootVelocity(PPCContext& ctx, uint8_t* b, const Rig& r,
                                    uint16_t action, uint8_t item) {
  auto unavailable=[&](const char* reason,uint32_t first=0,uint32_t second=0)->std::optional<Vec> {
#if defined(KK_DEV_TOOLS)
    REXLOG_INFO("PUPPET gait unavailable actor={:08X} action={} item={} reason={} data={}/{}",
        r.actor,action,item,reason,first,second);
#endif
    return {};
  };
  if (!LoadedAction(b,r,action,item)) return {};
  const auto items=Ptr(b,r.table,4*(action+4),20);
  const auto descriptor=coop::puppet::AnimationItemDescriptor(items,item,
      [&](uint32_t p) { return b[p]; },
      [&](uint32_t p,uint32_t off,uint32_t size) { return Ptr(b,p,off,size); });
  const auto header=Ptr(b,descriptor,0,20);
  if (!header) return unavailable("header");
  const auto count=Load16(b,header+4);
  const auto tracks=count<=512 ? Ptr(b,header,0,24*count) : 0;
  if (!count || count>512 || !tracks) return unavailable("track-list",count,tracks);
  // The native reader uses the last two translation keys. Reject a partial
  // root track before it could step before the key array or divide by zero.
  for (unsigned i=0;i<count;++i) {
    const auto track=tracks+24*i;
    if (Load16(b,track+20)!=65535) continue;
    const auto keys=Load16(b,track+16);
    const auto data=Ptr(b,track,4,8*keys);
    if (keys<2 || !data) return unavailable("root-keys",keys,data);
    if (!(Load16(b,data+8*(keys-2))&0x7fff)) return unavailable("root-key-duration",keys,Load16(b,data+8*(keys-2)));
  }
  Scratch scratch(ctx,b);
  const auto layer=scratch.Vector({},0x100), result=scratch.Vector({},0x110);
  b[layer+2]=60; Store32(b,layer+4,descriptor);
  const auto saved_header=Load32(b,0x82CCBF94), saved_cursor=Load32(b,0x82CCBF98);
  ctx.r3.u64=layer; ctx.r4.u64=result;
  // 8280F328 (.59) returns (last root key - previous root key) * fps / frames.
  // A scratch layer leaves the live clip, time cursor and resource unchanged.
  __imp__sub_8280F328(ctx,b);
  const auto velocity=ReadVec(b,result);
  Store32(b,0x82CCBF94,saved_header); Store32(b,0x82CCBF98,saved_cursor);
#if defined(KK_DEV_TOOLS)
  REXLOG_INFO("PUPPET gait read actor={:08X} action={} item={} tracks={} velocity={}",r.actor,action,item,count,XYZ(velocity));
#endif
  return Finite(velocity) ? std::optional<Vec>{velocity} : std::nullopt;
}

void CalibrateGaits(PPCContext& ctx,uint8_t* b,const Rig& r) {
  auto& s=Get(); s.native_gaits.clear();
  // H's native locomotion families, including directional player clips only
  // when this full-body bank really contains a translating root track.
  for (const uint16_t action : {10,12,15,27,37,40,41,42,43,44,45,50,51,52,53,54,55,160,163,166,167}) {
    const auto item=LoadedItem(b,r,action);
    if (!item) continue;
    const auto root=NativeRootVelocity(ctx,b,r,action,*item);
    float speed=root ? std::hypot(root->x,root->y) : 0;
    float yaw=root ? std::remainder(std::atan2(root->y,root->x)+1.570796327f,6.283185307f) : 0;
    const char* source="root-key-velocity";
    // Some loaded banks have in-place clips, with no translating root track.
    // A naturally played instance of this exact action/item supplies its native
    // speed and heading instead. Observations are normalized by the live fps.
    if (speed<coop::puppet::kIdleSpeed) for (const auto& sample:s.observed_motion) {
      if (sample.action==action && sample.item==*item && sample.speed>speed) {
        speed=sample.speed; yaw=sample.yaw; source="native-observed";
      }
    }
    if (speed<coop::puppet::kIdleSpeed || speed>30) continue;
    // Native actor forward is local -Y (8270FA58).
    s.native_gaits.push_back({{action,*item,speed},yaw,action==27 || action==37 || (action>=50 && action<=55)});
    REXLOG_INFO("PUPPET native gait actor={:08X} action={} item={} speed={:.4f} local-yaw={:.4f} source={} fps=60",
        r.actor,action,*item,speed,yaw,source);
  }
}

void PartnerBodyAim(PPCContext& ctx,uint8_t* b,uint32_t actor,uint32_t ai) {
  auto& s=Get();
  Rig rig;
  if (!Enabled() || !Opt().partner || !s.driving ||
      actor!=s.selected.actor || ai!=s.selected.ai || !s.partner.partner ||
      !s.partner.fresh || !s.Validate(b,s.world,rig) || !Scratch::CanUse(ctx) ||
      s.humans.at(actor).other_human) return;
  const auto clip_frame=Frame(ctx,b,actor);
  const auto tuple=(rig.action<<8)|rig.item;
  if (clip_frame==s.aim_clip_frame && tuple==s.aim_tuple) return;
  s.aim_clip_frame=clip_frame; s.aim_tuple=tuple;
  const auto& p=*s.partner.partner;
  const float body_yaw=std::atan2(s.facing.y,s.facing.x);
  const float yaw=body_yaw+std::clamp(std::remainder(p.facing-body_yaw,6.283185307f),-1.22f,1.22f);
  const float pitch=std::clamp(p.pitch,-.96f,.96f);
  const Vec aim{std::cos(yaw)*std::cos(pitch),std::sin(yaw)*std::cos(pitch),std::sin(pitch)};
  unsigned applied=0;
#if defined(KK_DEV_TOOLS)
  float matrix_delta=0;
#endif
  // H_callback_after_blend uses tags 3 and 2 with 8271A6F8 (inverse
  // orientation) then 827128F0 (weighted local rotation), .22:32690+.
  // The authored H callback is held with its AI. Apply only its isolated bone
  // rotation after the native world/animation pass, once per advancing clip
  // sample. A stopped/culled pose must not accumulate another rotation.
  for (const uint8_t tag:{3,2}) {
    const auto bone=OwnedAimBone(b,rig,tag);
    if (!bone) continue;
    Scratch scratch(ctx,b);
    const auto forward=scratch.Vector(s.facing,0x100), target=scratch.Vector(aim,0x110);
    const auto local_forward=scratch.Vector({},0x120), local_target=scratch.Vector({},0x130);
    ctx.r3.u64=bone; ctx.r4.u64=forward; ctx.r5.u64=local_forward;
    __imp__sub_8271A6F8(ctx,b);
    ctx.r3.u64=bone; ctx.r4.u64=target; ctx.r5.u64=local_target;
    __imp__sub_8271A6F8(ctx,b);
    if (!Finite(ReadVec(b,local_forward)) || !Finite(ReadVec(b,local_target))) continue;
#if defined(KK_DEV_TOOLS)
    const auto bone_matrix=Ptr(b,bone,12,80);
    const auto before=bone_matrix ? ReadVec(b,bone_matrix+16) : Vec{};
#endif
    ctx.r3.u64=bone; ctx.r4.u64=local_forward; ctx.r5.u64=local_target; ctx.f1.f64=.5;
    __imp__sub_827128F0(ctx,b);
#if defined(KK_DEV_TOOLS)
    if (bone_matrix) matrix_delta+=Distance(before,ReadVec(b,bone_matrix+16));
#endif
    ++applied;
  }
#if defined(KK_DEV_TOOLS)
  if (s.updates%120==0) REXLOG_INFO("PUPPET aim actor={:08X} bones={} yaw-offset={:.3f} pitch={:.3f} matrix-delta={:.5f} native-world-pose=1",
      actor,applied,yaw-body_yaw,pitch,matrix_delta);
#endif
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
  // before PartnerBodyBeforeLoad clears it at the synchronous unload boundary.
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
  auto refuse = [&](const char* reason) {
#if defined(KK_DEV_TOOLS)
    DevPuppetVariantReset();
#endif
    s.Abort(reason); return false;
  };
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
#if defined(KK_DEV_TOOLS)
    DevPuppetVariantBegin(b, source.actor, t, s.epoch);
#endif
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
#if defined(KK_DEV_TOOLS)
  DevPuppetMeshSpawnFixPreflight(b, source.actor, actor, s.epoch);
  // Arm read-only diagnostics even if an unseeded rig fails a later guard.
  // Free/hold deliberately retain the engine's initial action (possibly -1).
  if (PassiveSpawn()) DevPuppetMeshArm(b, source.actor, actor, s.epoch);
#endif
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
#if defined(KK_DEV_TOOLS)
  {
    Scratch scratch(ctx, b);
    DevPuppetVariantEnd(ctx, b, actor, w);
  }
#endif
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
#if defined(KK_DEV_TOOLS)
  if (!PassiveSpawn()) DevPuppetMeshArm(b, source.actor, actor, s.epoch);
#endif
  return true;
}


struct ScriptEvidence {
  bool complete = false;
  const char* failure = "not-scanned";
  uint32_t owner = 0;
  // Actor -> first external owner retaining a reference. Scan every owner's
  // declared vars, not guessed object memory. Self references do not count.
  std::map<uint32_t, uint32_t> references;
  std::set<uint32_t> current_references;
  std::map<uint32_t, uint32_t> mission_distance;
  uint32_t skipped = 0;  // owners whose layout could not be read
};
ScriptEvidence script_evidence;

uint32_t VarsSize(const uint8_t* b, uint32_t ai) {
  // 8271DC78 (.112): vars allocation size = *[ *[ *[AI+0]+0 ]+4 ].
  const auto definition = Ptr(b, Ptr(b, ai, 0, 4), 0, 8);
  return definition ? Load32(b, definition + 4) : ~uint32_t{0};
}
#include "coop_partner_registry.inl"

void RefreshScriptEvidence(const uint8_t* b, uint32_t world) {
  script_evidence = {};
  script_evidence.failure = "active-table";
  uint32_t begin, end;
  if (!Table(b, world, begin, end)) { RefreshRegistryEvidence(b, world); return; }
  size_t budget = 8 * 1024 * 1024;
  // An owner whose layout can't be read is skipped (counted), not a reason to refuse every human: only a
  // positive reference rules a candidate out (Claude, run c25 19:35: one unreadable owner gave no body).
  auto scan_owner = [&](uint32_t owner) -> bool {
    script_evidence.owner = owner;
    script_evidence.failure = "actor-or-extension";
    if (!Range(owner, 32)) return false;
    const auto ext = Load32(b, owner+24);
    if (!ext) return true;  // Non-AI objects cannot run mission records.
    if (!Range(ext, 8)) return false;
    const auto ai = Load32(b, ext + 4);
    if (!ai) return true;
    if (!Range(ai, 88)) return false;
    script_evidence.failure = "vars-layout-or-budget";
    const auto size = VarsSize(b, ai);
    if (size > 1024 * 1024 || size > budget) return false;
    budget -= size;
    const auto vars = Load32(b, ai + 64);
    if (size && !Range(vars, size)) return false;
    script_evidence.failure = "track-layout";
    const auto first = Load16(b, ai + 80), last = Load16(b, ai + 82);
    if (first > last || last > 4096 || last-first >= 64 || !Range(ai, (last+12)*4)) return false;
    for (uint32_t t = first; t <= last; ++t) {
      const auto block = Load32(b, ai + (t+6)*4);
      if (!block) continue;
      if (!Range(block, 24)) return false;
      const auto fn = Load32(b, block+20);
      // The interpreted fallback may have references outside declared vars.
      // Without a decoded bytecode contract it is not evidence of absence.
      if (!fn) { script_evidence.failure = "interpreted-track"; return false; }
      if (fn == 0x82454690) {
        // PRG_SSLoop (.128): vars+19812 is the persistent record index;
        // record=vars+920+180*i, callback=+4, payload=+72 (27 words).
        script_evidence.failure = "PRG-record-layout";
        if (size < 19820) return false;
        const auto index = Load32(b, vars+19812);
        if (index >= 104) return false;
        const auto record = vars+920+180*index;
        if (!Range(record, 180)) return false;
        // The full declared-vars scan below includes current AND future records,
        // including CapaBit's payload+4 actor and Hayes-style stored waits.
      }
    }
    for (uint32_t off = 0; off+4 <= size; off += 4) {
      const auto ref = Load32(b, vars+off);
      // Only a live leased body's audited raptor candidate slots are combat
      // references. PRG records and all unknown offsets still disqualify it.
      if (Get().driving && ref == Get().selected.actor &&
          coop::EnemyCombatReference(b, owner, ai, vars, off)) continue;
      if (ref != owner && Get().humans.count(ref)) script_evidence.references.try_emplace(ref, owner);
    }
    return true;
  };
  uint32_t skipped = 0;
  for (uint32_t p = begin; p < end; p += 8) {
    const uint32_t owner = Load32(b, p);
    script_evidence.owner = owner;
    if (!scan_owner(owner)) ++skipped;
  }
  script_evidence.skipped = skipped;
  script_evidence.complete = true;
  script_evidence.failure = "none"; script_evidence.owner = 0;
  RefreshRegistryEvidence(b, world);
}
coop::puppet::AutoCandidate AutoFacts(const uint8_t* b, const Rig& r, bool valid, bool stable, float distance) {
  coop::puppet::AutoCandidate c;
  c.actor = r.actor; c.distance = distance;
  const auto observation = Get().humans.find(r.actor);
  c.natural_h = observation != Get().humans.end() && !observation->second.other_human;
  c.other_human = observation != Get().humans.end() && observation->second.other_human;
  c.rig_valid = valid; c.stable = stable;
  c.hero = r.actor == Get().hero;
  // Protect H's Ann slot even during shared H_CINE tracks, when no Ann-named
  // callback is active. H_init uses vars+4 as the shared human registry slot.
  c.ann = r.ann || (c.natural_h && Load32(b, r.vars+4) == 2);
  c.dead = r.dead_track; c.grabbed = r.grabbed;
  c.following = r.following; c.cinematic = r.cinematic;
  c.script_evidence_complete = script_evidence.complete;
  c.script_reference = script_evidence.references.count(r.actor) != 0;
  c.current_script_use = script_evidence.current_references.count(r.actor) != 0;
  if (const auto it = Get().deferred_prg.find(r.actor); it != Get().deferred_prg.end()) {
    const auto [vars, index] = it->second;
    c.current_script_use |= Range(vars, 19820) && Load32(b, vars+19812) == index;
  }
  if (const auto it = Get().story_cooldown.find(r.actor); it != Get().story_cooldown.end())
    c.current_script_use |= Clock::now() < it->second;
  if (const auto it = script_evidence.mission_distance.find(r.actor); it != script_evidence.mission_distance.end())
    c.mission_distance = it->second;
  const auto& e = registry_evidence;
  const auto member = e.members.find(r.actor), scheduled = e.scheduled.find(r.actor);
  c.scheduled = scheduled != e.scheduled.end() && scheduled->second == 1;
  c.registry_unclaimed = e.valid && e.complete && e.pool_valid &&
      member != e.members.end() && member->second == 1 && !e.pool_claims.count(r.actor) &&
      !c.script_reference;
  c.unscheduled_setters_and_draw_confirmed = UnscheduledConfirmed(r);
  // H uses shared model code. No proven sailor/native/ally asset join exists;
  // leave kind Unknown rather than turn a slot number into a character ID.
  // Positive ally behavior is still useful for scoring/debugging.
  const auto it = Get().humans.find(r.actor);
  if (r.following || (it != Get().humans.end() && it->second.observed_ally))
    c.kind = coop::puppet::HumanKind::Ally;
  return c;
}
const char* AutoSafety(const uint8_t* b, uint32_t, const Rig& r) {
  const auto f = AutoFacts(b, r, true, true, 0);
  return Get().selected_tier < 4 ? coop::puppet::AutoReject(f) : coop::puppet::FallbackReject(f);
}
bool AutoMembership(const uint8_t* b, uint32_t world, const Rig& r) {
  return Member(b, world, r.actor) ||
      (Registered(b, world, r.actor) && UnscheduledConfirmed(r));
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
  if (log) {
    s.census_at = now;
#if defined(KK_DEV_TOOLS)
    if (!Opt().automatic) s.keys = Keys(b);
#endif
  }
  std::map<uint32_t, uint32_t> ordinals;
  std::vector<Candidate> candidates;
  std::set<uint32_t> seen;
  std::map<uint32_t, unsigned> membership;
  for (uint32_t p = begin; p < end; p += 8) ++membership[Load32(b, p)];
  std::vector<std::pair<uint32_t, uint32_t>> entries;
  if (Opt().automatic && registry_evidence.valid) {
    uint32_t rb, re;
    if (RegistryTable(b, w, rb, re)) for (uint32_t p = rb; p < re; p += 8)
      entries.emplace_back(Load32(b, p), (p - rb) / 8);
    // Keep the legacy scheduled tiers if a scheduled actor is absent from the
    // resident table. It can never receive tier zero in that case.
    for (uint32_t p = begin; p < end; p += 8)
      if (!registry_evidence.members.count(Load32(b, p))) entries.emplace_back(Load32(b, p), (p - begin) / 8);
  } else for (uint32_t p = begin; p < end; p += 8) entries.emplace_back(Load32(b, p), (p - begin) / 8);
  for (const auto& [actor, index] : entries) {
    if (IsSpawned(actor)) continue;  // Never learn natural motion or clone a prior copy.
    auto it = s.humans.find(actor);
    if (it == s.humans.end()) {
#if defined(KK_DEV_TOOLS)
      if (log && !Opt().automatic) CensusOtherRig(b, actor, index, s.epoch, w, s.level, s.hero);
#endif
      continue;  // Other families remain census-only, including possible KAnn.
    }
    auto& obs = it->second;
    seen.insert(actor);
    const bool scheduled = membership.count(actor) && membership.at(actor) == 1;
    const bool duplicate = Opt().automatic && registry_evidence.members.count(actor) ?
        registry_evidence.members.at(actor) != 1 || (membership.count(actor) && !scheduled) : !scheduled;
    const auto key_it = s.keys.find(actor);
    const uint32_t key = key_it == s.keys.end() ? 0 : key_it->second;
    const uint32_t ordinal = ordinals[key]++;
    Rig rig;
    if (!ReadRig(b, actor, rig) || (obs.bound ? !Same(rig, obs.rig) :
        rig.ai != obs.rig.ai || rig.vars != obs.rig.vars)) {
      obs.stable = 0; obs.present = false;
      if (log) REXLOG_INFO("PUPPET census epoch={} world={:08X} level={} hero={:08X} index={} key={:08X} ordinal={} actor={:08X} ai={:08X} vars={:08X} matrix={:08X} anim={:08X} xyz=unavailable distance=unavailable tracks={} action={} item={} frame=unavailable qualified=false reason=missing-or-changed-rig-chain",
                           s.epoch, w, s.level, s.hero, index, key, ordinal, actor, rig.ai, rig.vars,
                           rig.matrix, rig.anim, rig.tracks, rig.action, rig.item);
      continue;
    }
    if (!obs.bound) { obs.rig = rig; obs.bound = true; }
    obs.observed_ally |= rig.following;
    // Index is local to this epoch, not a persistent actor ID. Require the same
    // slot and full identity for the stability interval, then bind the actor.
    obs.stable = obs.present && obs.index == index && obs.scheduled == scheduled && !duplicate ? obs.stable + s.dt : 0;
    obs.index = index;
    obs.scheduled = scheduled;
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
          const auto fps=b[rig.layer+2];
          const float native_speed=moving ? (fps ? speed*60.f/fps : 0) : speed;
          const float yaw=moving ? std::remainder(std::atan2(rig.pos.y-obs.previous.y,rig.pos.x-obs.previous.x)-
              std::atan2(rig.forward.y,rig.forward.x),6.283185307f) : 0;
          const uint32_t sample_key = (tuple << 1) | uint32_t(moving);
          auto [sample, added] = obs.motion.emplace(sample_key,
              coop::puppet::Action{uint16_t(rig.action), uint8_t(rig.item), native_speed,yaw});
          // Retain the fastest natural example to distinguish walk from run.
          if (native_speed>=sample->second.speed) { sample->second.speed=native_speed; sample->second.yaw=yaw; }
          if (added) REXLOG_INFO("PUPPET observed actor={:08X} action={} item={} motion={} speed={:.3f}",
              actor, rig.action, rig.item, moving ? "moving" : "idle", native_speed);
        }
      }
      obs.previous = rig.pos; obs.previous_tuple = tuple; obs.sampled = true;
    } else obs.sampled = false;
    if (log && !Opt().automatic) {
      REXLOG_INFO("PUPPET census epoch={} world={:08X} level={} hero={:08X} index={} key={:08X} ordinal={} actor={:08X} ai={:08X} vars={:08X} matrix={:08X} rig={:08X} anim={:08X} table={:08X} layer={:08X} human={}/{} xyz={} distance={:.3f} tracks={} action={} item={} frame={:.3f} qualified={} reason={}",
                  s.epoch, w, s.level, s.hero, index, key, ordinal, actor, rig.ai, rig.vars,
                  rig.matrix, rig.rig, rig.anim, rig.table, rig.layer, Load32(b, rig.vars), Load32(b, rig.vars+4),
                  XYZ(rig.pos), jack_ok ? Distance(rig.pos, jack.pos) : -1.0f, rig.tracks, rig.action, rig.item,
                  Frame(ctx, b, actor), !*reason, *reason ? reason : "ok");
    }
    candidates.push_back({rig, key, ordinal, index, reason});
  }
  for (auto& [actor, obs] : s.humans) {
    if (!seen.count(actor)) { obs.present = false; obs.stable = 0; }
  }
  if (log && seen.empty()) REXLOG_INFO("PUPPET census epoch={} world={:08X} level={} hero={:08X} candidates=0 reason=no-natural-H-init-in-active-table",
                                      s.epoch, w, s.level, s.hero);
  if (!Opt().borrow || (Opt().partner && !s.partner_eligible) || s.attempted || s.terminal || now < s.retry_at ||
      now - s.loaded < std::chrono::seconds(5)) return;
  s.attempted = true;
  const Candidate* pick = nullptr;
  float closest = Opt().max_units;
  bool tied = false;
  std::vector<coop::puppet::AutoCandidate> facts;
  Candidate hands_source;
  bool copy_before_lease = false;
  if (Opt().automatic) {
    for (const auto& c : candidates) {
      auto f = AutoFacts(b, c.rig, !*c.reason && !s.retired.count(c.rig.actor),
          s.humans.at(c.rig.actor).stable >= 1, Distance(c.rig.pos, jack.pos));
      f.animation_support = f.other_human ? 0 : AnimationSupport(b,c.rig,s.partner.partner->weapon);
      facts.push_back(f);
      if (log) REXLOG_INFO("PUPPET auto candidate epoch={} index={} actor={:08X} score={} tier={} animations={} scheduled={} registry-unclaimed={} movement-and-draw-confirmed={} kind={} distance={:.3f} external-ref={:08X} reason={}",
          s.epoch, c.index, f.actor, coop::puppet::BodyScore(f), coop::puppet::BodyTier(f),
          f.animation_support,
          f.scheduled, f.registry_unclaimed, f.unscheduled_setters_and_draw_confirmed,
          f.kind == coop::puppet::HumanKind::Ally ? "observed-ally" : "unknown", f.distance,
          script_evidence.references.count(f.actor) ? script_evidence.references.at(f.actor) : 0,
          coop::puppet::BodyTier(f) >= 0 ? "eligible" : coop::puppet::FallbackReject(f));
    }
    if (const auto best = coop::puppet::SelectBody(facts)) pick = &candidates[*best];
    if (!pick) {
      // Tier C: copy an intact natural human even when its story state prevents
      // borrowing. Native duplication preserves the original; rigid head/props
      // are an accepted visible stand-in until full skin copies are repaired.
      for (const auto& c : candidates) {
        const auto& o = s.humans.at(c.rig.actor);
        if (*c.reason || o.other_human || AutoFacts(b, c.rig, true, true, 0).ann || c.rig.dead_track || c.rig.grabbed ||
            o.stable < 1 || s.retired.count(c.rig.actor)) continue;
        const auto support=AnimationSupport(b,c.rig,s.partner.partner->weapon);
        const auto previous=pick ? AnimationSupport(b,pick->rig,s.partner.partner->weapon) : 0;
        if (!pick || support>previous || (support==previous &&
            Distance(c.rig.pos, jack.pos) > Distance(pick->rig.pos, jack.pos))) pick = &c;
      }
      // The one-second retry may precede a full second of simulation samples
      // by one or two frames. Give a level-human copy source a bounded grace
      // interval before choosing Jack's arms (which can be outside a cinematic
      // camera). Never relax the source's identity/stability requirement.
      if (!pick && now < s.copy_wait_until) {
        s.attempted = false; s.retry_at = now + std::chrono::milliseconds(100);
        return;
      }
      if (!pick && jack_ok && Active(b, jack) && LegalAction(b, jack, jack.action)) {
        // If this chapter has no other usable human, duplicate Jack's loaded
        // rig as a hands/rigid-parts stand-in. The hero itself is never leased.
        hands_source = {jack, 0, 0, ~0u, ""};
        pick = &hands_source;
        auto& observation = s.humans[s.hero];
        if (!observation.bound) {
          observation.rig = jack; observation.bound = true;
          observation.generation = ++s.generation;
          observation.other_human = true;  // No fabricated H initialization.
        }
        observation.actions.insert((jack.action << 8) | jack.item);
      }
      if (!pick) {
        REXLOG_INFO("PUPPET auto no-body epoch={} level={} candidates={} scripts-complete={} skipped-owners={} reason=no-intact-non-Ann-copy-source",
            s.epoch, s.level, candidates.size(), script_evidence.complete, script_evidence.skipped);
        s.attempted = false; s.retry_at = now + std::chrono::seconds(1);
        return;
      }
      s.selected_tier = 6;
    } else {
      s.selected_tier = coop::puppet::BodyTier(facts[pick-candidates.data()]);
      copy_before_lease = coop::puppet::PreferStoryCopy(facts[pick-candidates.data()],
          s.humans.at(pick->rig.actor).story_copy_failed) && Registered(b, w, pick->rig.actor);
      if (copy_before_lease) {
        s.selected_tier = 6;
        REXLOG_INFO("PUPPET story-copy preferred source={:08X} reason=preserve-authored-human-before-lease",pick->rig.actor);
      }
    }
    const auto chosen = pick == &hands_source ? coop::puppet::AutoCandidate{} : facts[pick-candidates.data()];
    REXLOG_INFO("PUPPET auto pick epoch={} level={} actor={:08X} index={} score={} tier={} reason={} scheduled={} kind={}",
        s.epoch, s.level, pick->rig.actor, pick->index, coop::puppet::BodyScore(chosen),
        s.selected_tier, s.selected_tier == 6 ? "C-native-rigid-copy" : s.selected_tier == 5 ? "B-PNJInd-human-rig" :
        s.selected_tier == 4 ? "A-story-human-temporary-lease" : coop::puppet::AutoTierReason(chosen), chosen.scheduled,
        chosen.kind == coop::puppet::HumanKind::Ally ? "observed-ally" : "unknown");
  }
  for (const auto& c : candidates) {
    if (Opt().automatic) break;
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
  if (!(Opt().automatic && s.selected_tier == 6 ? Registered(b, w, pick->rig.actor) :
        Opt().automatic ? AutoMembership(b, w, pick->rig) : Member(b, w, pick->rig.actor))) {
    s.Abort("duplicate-or-unconfirmed-membership"); return;
  }
  auto& obs = s.humans.at(pick->rig.actor);
  s.action = Opt().action_set ? Opt().action : pick->rig.action;
  s.item = Opt().action_set ? Opt().item : pick->rig.item;
  if (!obs.actions.count((s.action << 8) | s.item) || !LegalAction(b, pick->rig, s.action)) {
    s.Abort("action-item-not-naturally-observed-on-this-rig"); return;
  }
  Rig selected = pick->rig;
  s.selected_copy = Opt().spawn || (Opt().automatic && s.selected_tier == 6);
  if (s.selected_copy) {
    bool reused = false;
    if (Opt().automatic) for (const auto& copy : s.spawned) {
      if (SpawnIdentity(b, copy) && ReadRig(b, copy.rig.actor, selected) &&
          Same(selected, copy.rig) && !*s.Qualify(b, selected, jack)) { reused = true; break; }
    }
    if (!reused && !SpawnCopy(ctx, b, w, pick->rig, jack, selected)) {
      if (copy_before_lease) {
        obs.story_copy_failed = true;
        s.Abort("story-copy-unavailable-lease-on-next-selection");
      }
      return;
    }
  }
  if (PassiveSpawn()) {
    s.terminal = true;
    REXLOG_INFO("PUPPET spawn diagnostic-only actor={:08X} mode={} no-post-create-setters no-animation-requests",
        selected.actor, SpawnModeName());
    return;
  }
  s.selected = selected; s.selected_generation = s.humans.at(selected.actor).generation;
  s.animation = {};
  s.aim_clip_frame=-1; s.aim_tuple=UINT32_MAX;
  s.requested_tuple = UINT32_MAX;
  s.animation_shots = 0;
#if defined(KK_DEV_TOOLS)
  s.capture_left=0;
#endif
  s.rate_base = b; s.rate_layer = selected.layer; s.saved_rate = b[selected.layer+2]; s.playback_rate = s.saved_rate;
  if (s.selected_copy) { s.action = selected.action; s.item = selected.item; }
  if (Opt().partner) {
    s.observed_motion.clear();
    for (const auto& [key, sample] : obs.motion) {
      s.observed_motion.push_back(sample);
      REXLOG_INFO("PUPPET partner observed-set actor={:08X} action={} item={} speed={:.3f} motion={}",
          pick->rig.actor, sample.action, sample.item, sample.speed,
          sample.speed < coop::puppet::kIdleSpeed ? "idle" : "moving");
    }
    REXLOG_INFO("PUPPET partner observed-set count={} actor={:08X}", s.observed_motion.size(), pick->rig.actor);
    CalibrateGaits(ctx,b,selected);
  }
  s.transition_pending = s.action != selected.action || s.item != selected.item;
  s.origin = selected.pos; s.initial_forward = selected.forward;
  const float len = std::hypot(s.initial_forward.x, s.initial_forward.y);
  s.initial_forward = {s.initial_forward.x / len, s.initial_forward.y / len, 0};
  s.desired = s.origin; s.facing = s.initial_forward; s.elapsed = 0; s.driving = true;
  REXLOG_INFO("PUPPET select epoch={} mode={} key={:08X} ordinal={} index={} actor={:08X} ai={:08X} vars={:08X} matrix={:08X} anim={:08X} generation={} human={}/{} distance={:.3f} units=guest nearest-limit={} action={} item={} observed-action={} observed-item={}",
              s.epoch, Opt().automatic ? "auto" : Opt().spawn ? "spawn" : Opt().nearest ? "nearest" : Opt().index ? "index" : "key", pick->key, pick->ordinal, pick->index,
              s.selected.actor, s.selected.ai, s.selected.vars, s.selected.matrix, s.selected.anim,
              s.selected_generation, Load32(b, s.selected.vars), Load32(b, s.selected.vars + 4),
              Distance(s.origin, jack.pos), Opt().nearest ? Opt().max_units : 0.0f, s.action, s.item,
              s.selected.action, s.selected.item);
}
}  // namespace

void PartnerBodyGameFrame(uint8_t* b) {
  if (!Opt().on || !Opt().partner) return;
  auto& s = Get();
  if (!Enabled()) {
    if (s.driving) s.Abort("coop-disabled");
    s.partner_eligible = false;
    return;
  }
  ++s.frame;
#if defined(KK_DEV_TOOLS)
  DevPuppetMeshFrame(s.frame);
#endif
  // Also release while photo mode or loading prevents normal AI passes.
  if (s.ready) PartnerReady(b, s.world);
}

void PartnerBodyBeforeLoad() {
#if defined(KK_DEV_TOOLS)
  DevPuppetMeshReset();
  DevPuppetVariantReset();
#endif
  if (!Enabled() && Get().spawned.empty()) return;
  auto& s = Get();
  if (s.driving || s.finish_pending) s.Abort("level-load-started");
  s.selected = {}; s.humans.clear(); s.keys.clear(); ++s.epoch;
  s.retired.clear(); s.deferred_prg.clear(); s.story_cooldown.clear(); s.retry_at = {}; s.copy_wait_until = {};
  script_evidence = {};
  registry_evidence = {};
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
void PartnerBodyAfterLoad(const uint8_t* b, uint32_t game) {
  if (!Enabled()) return;
  auto& s = Get();
  s.game = game; s.level = Range(game, 28) ? Load32(b, game + 8) : 0;
  s.loading = false; s.loaded = Clock::now();
}

void PartnerBodyBeforeAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
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
    if (!PartnerReady(b, w)) return;
    if (s.updated_frame == s.frame || photo::Active()) return;
  }
  s.dt = FrameTime(b);
  if (!std::isfinite(s.dt) || s.dt < 0 || s.dt > 0.25f) { s.Abort("invalid-simulation-dt"); return; }
  if (!Scratch::CanUse(ctx)) { s.Abort("invalid-guest-scratch-stack"); return; }
  if (Opt().automatic && s.partner_eligible) RefreshScriptEvidence(b, w);
#if defined(KK_DEV_TOOLS)
  else RefreshScriptEvidence(b, w);  // Solo scan also joins sleeping owners.
  LogRegistry(b);
#endif
  if (s.driving) { Rig rig; s.Validate(b, w, rig); }
  else if (!s.terminal) CensusAndSelect(ctx, b, w);
  s.prepared = true;
}

void PartnerBodyAfterAi(PPCContext& ctx, uint8_t* b, uint32_t w) {
  if (!Enabled()) return;
  auto& s = Get();
  if (!s.driving) return;
  if (Opt().partner && (!s.prepared || !PartnerReady(b, w) || s.updated_frame == s.frame)) return;
  if (Opt().automatic) RefreshScriptEvidence(b, w);
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
    s.animation.Update(p,s.dt);
    if (s.animation.new_shot) ++s.animation_shots;
#if defined(KK_DEV_TOOLS)
    static const bool capture_shots=[] { const char* v=std::getenv("KK_DEV_PARTNER_SHOTS"); return v && *v=='1'; }();
    if (capture_shots && s.animation.new_shot && s.animation_shots<=4) s.capture_left=8;
#endif
    const bool log_shot = s.animation_shots <= 8 || s.animation_shots%60 == 0;
    auto choice = coop::puppet::ChooseAction(s.animation.moving ? s.animation.speed : 0, s.observed_motion);
    const bool human = !s.humans.at(rig.actor).other_human && Load32(b,rig.vars) == 0;
    const auto held=human ? BodyWeapon(b,rig) : 0;
    const auto weapon=held>=1 && held<=4 ? uint8_t(held) : s.animation.fire_left>0 ? p.fire_weapon : p.weapon;
    const bool firing=s.animation.fire_left>0;
    const auto native = coop::puppet::HumanAction(weapon,s.animation.moving,p.crouched,firing);
    if (human) {
      if (const auto item=LoadedItem(b,rig,native)) {
        float speed=choice && choice->action==native && choice->item==*item ? choice->speed : 0;
        for (const auto& gait:s.native_gaits)
          if (gait.clip.action==native && gait.clip.item==*item) speed=gait.clip.speed;
        choice=coop::puppet::Action{native,*item,speed};
      }
      if (s.animation.moving && !firing) {
        if (const auto gait=coop::puppet::ChooseGait(p.vx,p.vy,p.facing,p.crouched,s.native_gaits)) {
          choice=gait->clip;
          s.facing={std::cos(gait->facing),std::sin(gait->facing),0};
        } else {
          const float direction=std::atan2(p.vy,p.vx);
          s.facing={std::cos(direction),std::sin(direction),0};
        }
      }
      if (firing) {
        const auto fire=coop::puppet::FireAction(weapon,p.crouched,
            [&](uint16_t action) { return LoadedItem(b,rig,action); });
        if (fire) choice=fire;
        if (s.animation.new_shot && log_shot)
          REXLOG_INFO("PUPPET fire visual actor={:08X} shot={} weapon={} requested={} chosen={} item={} fallback={} loaded={}",
              rig.actor,p.fire,p.fire_weapon,native,fire ? int(fire->action) : -1,
              fire ? int(fire->item) : -1,fire && fire->action!=native,bool(fire));
      }
      // H's native airborne/landing chain is 20 -> 21/22 (826A80B4).
      // Position still comes entirely from the peer; these clips add no jump force.
      if (!firing && (p.airborne || s.animation.land_left>0)) {
        const uint16_t air=p.airborne ? 20 : 21;
        if (const auto item=LoadedItem(b,rig,air)) choice=coop::puppet::Action{air,*item,0};
        else if (!p.airborne) if (const auto item=LoadedItem(b,rig,22)) choice=coop::puppet::Action{22,*item,0};
      }
    } else if (s.animation.moving && !firing) {
      const float direction=std::atan2(p.vy,p.vx);
      s.facing={std::cos(direction),std::sin(direction),0};
    }
    // Tier C may be a copy of Jack's first-person rig. Its loaded bank can
    // replay the peer's arms actions, but it cannot acquire missing leg clips.
    Rig jack_bank;
    if (s.selected_copy && !human && p.item == 0 && ReadRig(b,s.hero,jack_bank) &&
        rig.table == jack_bank.table && LoadedAction(b,rig,p.action))
      choice = coop::puppet::Action{p.action,0,0};
    if (choice && !LoadedAction(b,rig,choice->action,choice->item)) choice.reset();
    if (choice) {
      s.action = choice->action; s.item = choice->item;
      const uint32_t tuple = (s.action<<8)|s.item;
      const bool changed = tuple != s.requested_tuple;
      // A completed non-looping recoil may replay for a later round. Never
      // rewind an advancing burst on every incoming shot.
      s.transition_pending = changed || (firing && s.animation.new_shot && (Load16(b,rig.layer)&0x80));
      s.requested_tuple = tuple;
      s.missing_action_kind = 0;
      // The engine stores frames/second in layer+2 (826A9338, 826A93BC).
      // Scale a proven gait instead of restarting it for every pose packet.
      s.playback_rate = s.animation.moving && choice->speed > .1f ?
          uint8_t(std::clamp(std::lround(60*s.animation.speed/choice->speed),15l,150l)) : 60;
      if (s.transition_pending) REXLOG_INFO("PUPPET partner action={} item={} speed={:.3f} moving={} weapon={} body-weapon={} crouch={} fire={} restart={} copy={} pitch={:.3f}",
          s.action, s.item, s.animation.speed,s.animation.moving,p.weapon,held,p.crouched,p.fire,!changed,s.selected_copy,p.pitch);
    } else {
      s.action = rig.action; s.item = rig.item; s.transition_pending = false; s.requested_tuple = UINT32_MAX;
      const unsigned kind = std::abs(p.speed) < coop::puppet::kIdleSpeed ? 1 : 2;
      if (s.missing_action_kind != kind) REXLOG_INFO("PUPPET partner keep-current reason={} motion={} action={} item={}",
          "no-loaded-native-or-observed-action",
          std::abs(p.speed) < coop::puppet::kIdleSpeed ? "idle" : "moving", s.action, s.item);
      s.missing_action_kind = kind;
    }
    s.elapsed += double(s.dt);
    s.updated_frame = s.frame;
    ++s.updates;
  }
#if defined(KK_DEV_TOOLS)
  else {
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
#endif
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
    // Native H shooting keeps its state playing (826F4C88). Repeated network
    // rounds extend that state, never reset the clip to its first frame.
    if (s.transition_pending) {
      if (rig.layer==s.rate_layer) b[rig.layer+2]=s.saved_rate;
      ctx.r3.u64 = rig.actor; ctx.r4.u64 = s.action; ctx.r5.u64 = s.item;
      ctx.r6.u64 = 0;
      __imp__sub_82850100(ctx, b);
      s.transition_pending = false;
    }
  }
  if (!s.Validate(b, w, rig)) return;
  if (Opt().partner) {
    if (rig.layer!=s.rate_layer) { s.rate_layer=rig.layer; s.saved_rate=b[rig.layer+2]; }
    b[rig.layer+2] = s.playback_rate;
  }
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
  // Full H Post/AI remains held. Refresh only its audited perception publisher
  // under the body's native context, after committing the driven transform.
  const auto link = coop::EnemyLinkSnapshot();
  if (Opt().partner && !s.selected_copy && !s.humans.at(rig.actor).other_human && link.connected && link.role == coop::Role::Host && coop::EnemyPartnerCombatReady() &&
      Range(rig.vars, 5152) && Range(Load32(b, 0x82CC9C30), 7144) &&
      Load32(b, rig.vars+4) >= 1 && Load32(b, rig.vars+4) <= 12) {
    Scratch scratch(ctx, b);
    constexpr uint32_t globals[] = {0x82CCBF64, 0x82CCBF68, 0x82CCBF6C, 0x82CCBF70};
    uint32_t saved[4];
    for (unsigned i = 0; i < 4; ++i) saved[i] = Load32(b, globals[i]);
    Store32(b, globals[0], rig.ai); Store32(b, globals[1], rig.ai);
    Store32(b, globals[2], rig.actor); Store32(b, globals[3], rig.actor);
    s.combat_publishing = true;
#if defined(KK_DEV_TOOLS)
    static uint64_t attempts = 0;
    if (++attempts <= 8 || attempts%120 == 0)
      REXLOG_INFO("BITE perception-gate body={:08X} class={} disabled+2728={} state+2712={} refresh={}",
          rig.actor, Load32(b, rig.vars+4), Load32(b, rig.vars+2728), Load32(b, rig.vars+2712), attempts);
#endif
    __imp__sub_8242AA88(ctx, b);
    s.combat_publishing = false;
    for (unsigned i = 0; i < 4; ++i) Store32(b, globals[i], saved[i]);
    if (!s.Validate(b, w, rig)) return;
  }
  if (Opt().partner) coop::SetPartnerBodyDriven(true);
  else if (s.elapsed >= 8) { s.driving = false; s.finish_pending = true; }
}

void PartnerBodyLateWorld(PPCContext& ctx, uint8_t* b, uint32_t w) {
  if (!Enabled()) return;
#if defined(KK_DEV_TOOLS)
  DevPuppetMeshTick(b);
  LogSpawnBones(b);
#endif
  auto& s = Get();
  if (!s.measure) return;
  if (Opt().partner && !PartnerReady(b, w)) return;
  s.measure = false;
  Rig rig;
  if (!s.Validate(b, w, rig)) return;
  if (Opt().partner) PartnerBodyAim(ctx,b,rig.actor,rig.ai);
#if defined(KK_DEV_TOOLS)
  if (Opt().partner && s.capture_left && s.updates%3==0 && Scratch::CanUse(ctx)) {
    --s.capture_left;
    const auto filename=fmt::format("partner-shot-{}-{}-{}.png",s.epoch,s.animation_shots,s.updates);
    art::CaptureFrame(filename);
    REXLOG_INFO("PUPPET shot-frame file={} shot={} action={} item={} frame={:.3f}",
        filename,s.partner.partner->fire,rig.action,rig.item,Frame(ctx,b,rig.actor));
  }
#endif
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
    else if (Opt().partner) REXLOG_INFO("PUPPET partner driving epoch={} updates={} game-frame={} speed={:.3f} remote-action={} remote-item={} vx={:.3f} vy={:.3f} gait-speed={:.3f} rate={} weapon={} fire={} late-error={:.6f} late-z-error={:.6f} large-late-error={}",
        s.epoch, s.updates, s.frame, s.partner.partner->speed, s.partner.partner->action, s.partner.partner->item,
        s.partner.partner->vx,s.partner.partner->vy,s.animation.speed,s.playback_rate,s.partner.partner->weapon,s.partner.partner->fire,
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
void PuppetObserveInit(uint8_t* b, uint32_t actor, uint32_t ai, bool other_human = false) {
  if (!Enabled()) return;
  auto& s = Get();
  if (s.driving && actor == s.selected.actor) {
    if (Opt().automatic) s.retired.insert(actor);
    s.Abort("selected-H-reinitialized");
  }
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
  observation.other_human = other_human;
  s.humans[actor] = std::move(observation);
}
#if defined(KK_DEV_TOOLS)
void PuppetObserveSpawner(uint8_t* b, uint32_t actor, uint32_t ai) {
  if (!Enabled()) return;
  if (Ptr(b, Ptr(b, actor, 24, 8), 4, 88) != ai) return;
  const uint32_t vars = Ptr(b, ai, 64, 8380);
  if (vars) Get().spawners[actor] = {ai, vars};
}
#endif
bool HoldSpawned(PPCContext& ctx, uint8_t* b) {
  if (Opt().spawn_mode == Options::SpawnMode::Free || uint32_t(ctx.lr) != 0x8271D304) return false;
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
  if (Opt().spawn_mode == Options::SpawnMode::Free || capacity != 32 || ctx.r3.s32 <= 0 || ctx.r3.u32 > capacity ||
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
bool PartnerBodyHold(PPCContext& ctx, uint8_t* b) {
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
void PartnerBodyTriggerReference(const uint8_t* b, uint32_t record) {
  auto& s = Get();
  if (!Opt().automatic || !s.driving || s.selected_copy || !Range(record, 108)) return;
  // Trigger/event payloads are 27 words (PRG_SSLoop and 8271E4E8).
  // CapaBit's explicit actor is payload+4; zero means the current actor.
  bool referenced = Load32(b, 0x82CCBF6C) == s.selected.actor;
  for (uint32_t off = 0; off < 108; off += 4)
    referenced |= Load32(b, record+off) == s.selected.actor;
  if (referenced) {
    s.story_cooldown[s.selected.actor] = Clock::now() + std::chrono::seconds(3);
    s.Abort("live-trigger-or-stimulus-reference");
  }
}
bool PartnerBodySuppressShot(const uint8_t* b) {
  const auto& s=Get();
  const auto actor=Load32(b,0x82CCBF6C), ai=Load32(b,0x82CCBF64);
  for (const auto& copy:s.spawned)
    if (actor==copy.rig.actor && ai==copy.rig.ai && SpawnIdentity(b,copy)) return true;
  return Opt().partner && s.driving && s.Environment(b,s.world) &&
      actor==s.selected.actor && ai==s.selected.ai &&
      Ptr(b,ai,64,8)==s.selected.vars && Ptr(b,actor,20,16)==s.selected.rig &&
      Ptr(b,s.selected.rig,0,176)==s.selected.anim;
}
uint32_t PartnerBodyCombatActor(const uint8_t* b) {
  const auto& s = Get();
  if (!Enabled() || !Opt().partner || s.selected_copy || !s.driving || !s.prepared ||
      !s.partner_eligible || !s.partner.fresh || !s.Environment(b, s.world)) return 0;
  const auto it = s.humans.find(s.selected.actor);
  Rig rig;
  // Read-only even when used by dev diagnostics. The normal before/after AI
  // validators own release; this snapshot never changes lease state.
  return it != s.humans.end() && !it->second.other_human && it->second.generation == s.selected_generation &&
      Member(b, s.world, s.selected.actor) && ReadRig(b, s.selected.actor, rig) && Same(rig, s.selected) ? rig.actor : 0;
}
bool PartnerBodyStimulus(PPCContext& ctx, uint8_t* b) {
  auto& s = Get();
  const auto body = PartnerBodyCombatActor(b);
  if (coop::EnemyDamageEnqueue(ctx, b, body)) return true;
  if (body && s.combat_publishing && ctx.r4.u32 == 1 && ctx.r3.u32 == body &&
      uint32_t(ctx.lr) == 0x82635DF0 && Range(ctx.r6.u32, 108)) {
    // The publisher alone may mention its own body; no arbitrary stimulus or
    // mission/cinematic payload receives this exemption.
#if defined(KK_DEV_TOOLS)
    const auto output = ctx.r8.u32, payload = ctx.r6.u32;
    const bool readable = Range(output, 8);
    const auto old_id = readable ? Load32(b, output) : UINT32_MAX;
    const auto old_generation = readable ? Load32(b, output+4) : 0;
    const auto classification = Load32(b, payload+96);
    const auto lifetime = ctx.f1.f64;
#endif
    __imp__sub_8271E4E8(ctx, b);
#if defined(KK_DEV_TOOLS)
    const auto u32 = [&](uint32_t p) { return Load32(b, p); };
    const auto record = coop::stimulus_debug::ReadHandle(u32, output);
    const bool queued = readable && record.valid && record.source == body && (record.id >> 24) == 1 &&
        (record.id != old_id || record.generation != old_generation);
    static uint64_t count = 0, succeeded = 0, refused = 0;
    ++count; if (queued) ++succeeded; else ++refused;
    if (count <= 8 || count%120 == 0 || (!queued && refused <= 8)) {
      const auto pool = coop::stimulus_debug::ReadPool(u32, 1);
      REXLOG_INFO("BITE human-publish body={:08X} class={} lifetime={:.3f} queued={} id={:08X}/{} record={:08X} pool-flags={:08X} enumerable={} source={:08X} pool={}/{} count={} succeeded={} refused={}",
          body, classification, lifetime, queued, record.id, record.generation, record.address, record.flags,
          record.enumerable, record.source, pool.count, pool.capacity, count, succeeded, refused);
    }
#endif
    return true;
  }
  return false;
}
void PartnerBodyScriptRecord(const uint8_t* b) {
  auto& s = Get();
  if (!Opt().automatic || !s.driving || s.selected_copy) return;
  const auto ai = Load32(b, 0x82CCBF64);
  const auto vars = Ptr(b, ai, 64, 19820);
  if (!vars || VarsSize(b, ai) < 19820 || Load32(b, vars+19812) >= 104) {
    s.Abort("live-PRG-record-layout-unavailable"); return;
  }
  const auto index = Load32(b, vars+19812);
  // SSLoop can advance through several callbacks inside one native dispatch.
  // Release conservatively before it enters ANY remaining record that refers
  // to the body, so a same-dispatch successor cannot run while it is leased.
  for (uint32_t i = index; i < 104; ++i) {
    const uint32_t record = vars+920+180*i;
    if (!Load32(b, record+4)) break;
    for (uint32_t off = 72; off < 180; off += 4) {
      if (Load32(b, record+off) != s.selected.actor) continue;
      s.deferred_prg[s.selected.actor] = {vars, index};
      REXLOG_INFO("PUPPET story-release actor={:08X} owner={:08X} current={} needed-record={} callback={:08X} reason=PRG-current-or-same-dispatch-successor",
          s.selected.actor, Load32(b, 0x82CCBF6C), index, i, Load32(b, record+4));
      s.Abort("PRG-current-or-same-dispatch-successor"); return;
    }
  }
}
}  // namespace kk

REX_HOOK_RAW(sub_8280F328) {
  if (REXCVAR_GET(kk_coop_enabled) && kk::Range(ctx.r4.u32, 12) &&
      !kk::coop::puppet::AnimationLayerHeader(ctx.r3.u32,
          [&](uint32_t p, uint32_t off, uint32_t size) { return kk::Ptr(base, p, off, size); })) {
    // Native empty-track handling leaves a zero motion delta and clears the
    // current header/cursor. A temporarily absent header has the same result.
    for (uint32_t off = 0; off < 12; off += 4) kk::hfr::Store32(base, ctx.r4.u32 + off, 0);
    kk::hfr::Store32(base, 0x82CCBF94, 0);
    kk::hfr::Store32(base, 0x82CCBF98, 0);
    static unsigned logged = 0;
    if (logged++ < 16) REXLOG_INFO("PUPPET root motion deferred layer={:08X} actor={:08X} reason=missing-layer-header",
        ctx.r3.u32, kk::hfr::Load32(base, 0x82CCBF7C));
    return;
  }
  __imp__sub_8280F328(ctx, base);
}

REX_HOOK_RAW(sub_823F8FB0) {
  if (REXCVAR_GET(kk_coop_enabled)) {
    const auto actor = ctx.r3.u32;
    const auto rig = kk::Ptr(base, actor, 20, 16);
    const auto animation = kk::Ptr(base, rig, 0, 176);
    const auto header = kk::coop::puppet::AnimationEventHeader(animation,
        [&](uint32_t p, uint32_t off, uint32_t size) { return kk::Ptr(base, p, off, size); });
    if (!header) {
      // Match the native missing-layer result instead of dereferencing its
      // temporarily empty descriptor. Do not manufacture animation events.
      static unsigned logged = 0;
      if (logged++ < 16) REXLOG_INFO("PUPPET animation event lookup deferred actor={:08X} hero={} borrowed={} event={} caller={:08X} reason=missing-layer-header",
          actor, actor == kk::hfr::Load32(base, 0x82CCBF7C), actor == kk::Get().selected.actor,
          ctx.r4.u32, uint32_t(ctx.lr));
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_823F8FB0(ctx, base);
}

REX_HOOK_RAW(sub_82454690) {
  kk::PartnerBodyScriptRecord(base);
  if (!kk::coop::StoryLoop(ctx, base)) __imp__sub_82454690(ctx, base);
}
REX_HOOK_RAW(sub_8271E4E8) {
  if (kk::PartnerBodyStimulus(ctx, base)) return;
  kk::PartnerBodyTriggerReference(base, ctx.r6.u32);
  __imp__sub_8271E4E8(ctx, base);
}
REX_HOOK_RAW(sub_82449F18) {
  const auto record = ctx.r3.u32;
#if defined(KK_DEV_TOOLS)
  const auto caller = uint32_t(ctx.lr);
#endif
  kk::PartnerBodyTriggerReference(base, record);
  __imp__sub_82449F18(ctx, base);
#if defined(KK_DEV_TOOLS)
  kk::DevTrigCapaResult(base, record, caller, ctx.r3.u32);
#endif
}

REX_HOOK_RAW(sub_82881B70) {
  const uint32_t world = ctx.r3.u32;
  __imp__sub_82881B70(ctx, base);
  kk::PartnerBodyLateWorld(ctx, base, world);
  kk::coop::observer::LateWorld(ctx, base);
#if defined(KK_DEV_TOOLS)
  kk::DevFishAfterWorld(ctx, base);
#endif
}

REX_HOOK_RAW(sub_8268D8E8) {
  const bool on = kk::Enabled();
  const uint32_t actor = on ? kk::hfr::Load32(base, 0x82CCBF6C) : 0;
  const uint32_t ai = on ? kk::hfr::Load32(base, 0x82CCBF64) : 0;
  __imp__sub_8268D8E8(ctx, base);
  if (on) kk::PuppetObserveInit(base, actor, ai);
}
// PNJInd_Init is the independently identified human-shaped native family.
// KAnn is deliberately never observed into the candidate set.
REX_HOOK_RAW(sub_826ECDF8) {
  const bool on = kk::Enabled() && kk::Opt().automatic;
  const uint32_t actor = on ? kk::hfr::Load32(base, 0x82CCBF6C) : 0;
  const uint32_t ai = on ? kk::hfr::Load32(base, 0x82CCBF64) : 0;
  __imp__sub_826ECDF8(ctx, base);
  if (on) kk::PuppetObserveInit(base, actor, ai, true);
}
#if defined(KK_DEV_TOOLS)
REX_HOOK_RAW(sub_826EB8C0) {
  const bool on = kk::Enabled() && kk::Opt().spawn;
  const uint32_t actor = on ? kk::hfr::Load32(base, 0x82CCBF6C) : 0;
  const uint32_t ai = on ? kk::hfr::Load32(base, 0x82CCBF64) : 0;
  __imp__sub_826EB8C0(ctx, base);
  if (on) kk::PuppetObserveSpawner(base, actor, ai);
}
#endif
REX_HOOK_RAW(sub_8284B028) {
  const bool filter = !kk::Get().spawned.empty() && uint32_t(ctx.lr) == 0x8245D334;
  const uint32_t output = ctx.r9.u32, capacity = ctx.r4.u32;
  __imp__sub_8284B028(ctx, base);
  if (filter) kk::FilterSpawnedTrigger(ctx, base, output, capacity);
}
