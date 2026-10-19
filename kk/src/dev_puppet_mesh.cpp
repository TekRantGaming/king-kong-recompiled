// C11/C14 read-only comparison. KK_DEV_PUPPET_MESH=1 with partner:spawn:<index>.
// Add KK_DEV_PUPPET_DRAWS=1 for 120 render frames and actor mesh-entry inventories.
// Guest layouts: 8286D690 (.57), 82869310 (.70), 8286D0E0 (.12),
// 8286FE40 (.102). Never call initialization, change flags, or step animation.
#if defined(KK_DEV_TOOLS)
#include "dev_puppet_mesh.h"
#include "hfr.h"
#include "plugin/native_puppet_probe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace kk {
namespace {
using Clock = std::chrono::steady_clock;
using hfr::Load32;
using hfr::Load16;
constexpr size_t kMaxNodes = 128;
constexpr uint64_t kUnknown = ~uint64_t{0};
thread_local NrPuppetMeshScope preparing;
thread_local unsigned preparation_depth = 0;
thread_local NrPuppetMeshScope building_palette;
struct PaletteExecution {
  uint64_t generation = 0;
  uint32_t context = 0, material = 0, packet = 0, world_index = 0, first = 0, count = 0;
  bool enabled = false;
};
thread_local PaletteExecution executing_palette;
bool On() {
  static const bool on = [] { const char* v = std::getenv("KK_DEV_PUPPET_MESH"); return v && std::string(v) == "1"; }();
  return on;
}
bool DrawOn() {
  static const bool on = [] { const char* v = std::getenv("KK_DEV_PUPPET_DRAWS"); return v && std::string(v) == "1"; }();
  return on;
}
bool Range(uint32_t p, uint32_t bytes) {
  return p >= 0x40000000 && p < 0xA0000000 && !(p & 3) && bytes <= 0xA0000000 - p;
}
uint32_t Word(const uint8_t* b, uint32_t p, uint32_t offset = 0) {
  return Range(p, offset + 4) ? Load32(b, p + offset) : 0;
}
uint32_t Ptr(const uint8_t* b, uint32_t p, uint32_t offset, uint32_t bytes) {
  const auto q = Word(b, p, offset);
  return Range(q, bytes) ? q : 0;
}
struct List {
  uint32_t begin = 0, count = 0, tagged = 0;
  bool valid = false;
};
List Hierarchy(const uint8_t* b, uint32_t group) {
  // Group+0 is the engine's 8-byte entry list. End+4 is exclusive; start+16.
  const auto list = Ptr(b, group, 0, 20);
  List out;
  out.begin = Word(b, list, 16);
  const auto end = Word(b, list, 4);
  if (end < out.begin || (end-out.begin) % 8 || (end-out.begin)/8 > kMaxNodes) return out;
  out.count = (end-out.begin)/8;
  out.valid = list && (out.count == 0 || Range(out.begin, out.count*8));
  if (out.valid) for (uint32_t i = 0; i < out.count; ++i) out.tagged += (Word(b, out.begin+8*i) & 3) != 0;
  return out;
}
struct Node {
  uint32_t actor = 0, flags = 0, flags4 = 0, matrix = 0, bounds = 0, holder = 0, ext = 0;
  uint32_t component[5]{};
  uint32_t resource = 0, skin = 0, skin_data = 0, skin_count = 0, skin_bytes = 0;
  bool links = false;
  bool external = false;
  // First discovered reference, for evidence only. Resource reachability does
  // not establish that this node is submitted by this root at render time.
  uint32_t parent = 0, group = 0, entry = 0;
  const char* origin = "root";
};
Node ReadNode(const uint8_t* b, uint32_t actor) {
  Node n;
  if (!Range(actor, 36)) return n;
  n.actor = actor; n.flags = Word(b, actor); n.flags4 = Word(b, actor, 4);
  n.matrix = Ptr(b, actor, 12, 80); n.bounds = Ptr(b, actor, 16, 48);
  n.holder = Ptr(b, actor, 20, 20); n.ext = Ptr(b, actor, 24, 48);
  for (unsigned i = 0; i < 5; ++i) n.component[i] = Word(b, n.holder, 4*i);
  n.resource = Ptr(b, n.ext, 12, 12);
  // 82870B0C..82870BFC: bit 0x200000 owns a skin component; bit 0x1000000
  // makes its entries 12-byte {matrix*, owner*, bone-index} links, else 80-byte matrices.
  if (n.flags & 0x200000) {
    n.skin = Ptr(b, n.holder, 16, 8); n.skin_count = Word(b, n.skin);
    n.links = (n.flags & 0x1000000) != 0;
    if (n.skin_count <= 256) {
      n.skin_bytes = n.skin_count * (n.links ? 12 : 80);
      n.skin_data = Ptr(b, n.skin, 4, n.skin_bytes);
    }
  }
  return n;
}
struct Root {
  uint32_t actor = 0, holder = 0, anim = 0, ext = 0, matrix = 0;
  uint64_t animation_frame = kUnknown, animation_calls = 0, scope_calls = 0;
  uint64_t skeleton_frame = kUnknown, skeleton_calls = 0;
  uint64_t hierarchy_frame = kUnknown, hierarchy_calls = 0;
  std::vector<Node> nodes;
  std::map<uint32_t, uint64_t> previous_draws;
};
struct Watch {
  uint32_t owner = 0, data = 0, bytes = 0;
  uint64_t hash = 0, changed_frame = kUnknown, write_frame = kUnknown, writes = 0;
};
struct Probe {
  std::mutex mutex;
  std::atomic<bool> active{false};
  std::atomic<uint64_t> frame{0};
  const NrPuppetProbeApi* api = nullptr;
  Root roots[2];
  uint64_t epoch = 0;
  uint32_t world = 0, lists_left = 0;
  uint64_t list_frame = kUnknown;
  uint32_t registry_entries = 0, external_nodes = 0, ambiguous_links = 0;
  bool registry_valid = false, registry_capped = false;
  Clock::time_point next{};
  std::map<uint32_t, uint32_t> owners;
  // Address of a matrix in an engine preparation buffer -> exact preparing actor.
  // Unknown pushes erase old provenance when a buffer slot is reused.
  std::map<uint32_t, NrPuppetMeshScope> prepared;
  struct Buffer { uint32_t data = 0, count = 0; };
  std::map<uint32_t, Buffer> buffers;
  // C15: 48-byte, transposed skin constants, separate from 64-byte world matrices.
  std::map<uint32_t, NrPuppetMeshScope> palettes;
  std::map<uint32_t, Buffer> palette_buffers;
  struct PaletteUse {
    uint32_t owner = 0, node = 0, context = 0, data = 0, first = 0, count = 0;
    uint32_t material = 0, packet = 0;
    uint32_t world_entry = 0, local_entry = 0;
    uint32_t source_entries = 0, copy_entries = 0, unknown_entries = 0;
    uint64_t builds = 0, prepares = 0, draws = 0;
    bool valid = false, saw_source = false, saw_copy = false, saw_unknown = false;
  };
  using PaletteKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
  std::map<PaletteKey, PaletteUse> palette_uses;
  uint64_t palette_dropped = 0;
  uint64_t generation = 0;
  uint64_t draw_queries = 0, provenance_hits = 0;
  std::vector<Watch> watches;
};
Probe& Get() { static Probe p; return p; }
const NrPuppetProbeApi* FindApi() {
  using GetFn = const NrPuppetProbeApi* (*)();
  GetFn get = nullptr;
  const char* module = "none";
  bool loaded = false;
#if defined(_WIN32)
  for (const char* name : {"rexgpu-nativerd.dll", "rexgpu-native.dll", "rexgpu-natived.dll"})
    if (HMODULE m = GetModuleHandleA(name)) {
      loaded = true; module = name;
      if (auto f = GetProcAddress(m, "nr_get_puppet_probe")) { get = reinterpret_cast<GetFn>(f); break; }
    }
#else
  for (const char* name : {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"}) {
    if (void* m = dlopen(name, RTLD_NOW | RTLD_NOLOAD)) {
      loaded = true; module = name;
      get = reinterpret_cast<GetFn>(dlsym(m, "nr_get_puppet_probe")); dlclose(m);
      if (get) break;
    }
  }
#endif
  const auto* api = get ? get() : nullptr;
  const char* state = !loaded ? "module-not-loaded" : !get ? "export-missing-rebuild-dev-plugin" :
      !api ? "export-returned-null" : api->version != kNrPuppetProbeVersion ? "version-mismatch" :
      api->size != sizeof(NrPuppetProbeApi) ? "size-mismatch" :
      !(api->scope && api->read && api->reset && api->capture && api->drain) ? "callbacks-missing" : "connected";
  const bool connected = std::string(state) == "connected";
  if (DrawOn()) REXLOG_INFO("PUPPET draws probe state={} module={} connected={} version={}/{} size={}/{} mesh-env={} draws-env={} log-rotation=also-read-run.N.log",
      state, module, connected, api ? api->version : 0, kNrPuppetProbeVersion,
      api ? api->size : 0, sizeof(NrPuppetProbeApi), On(), DrawOn());
  return connected ? api : nullptr;
}
bool Identity(const uint8_t* b, const Root& r) {
  return r.actor && Ptr(b, r.actor, 20, 20) == r.holder && Ptr(b, r.holder, 0, 180) == r.anim &&
         Ptr(b, r.actor, 24, 48) == r.ext && Ptr(b, r.actor, 12, 80) == r.matrix;
}
uint64_t Hash(const uint8_t* b, uint32_t data, uint32_t bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (uint32_t i = 0; i < bytes; ++i) { hash ^= b[data+i]; hash *= 1099511628211ull; }
  return hash;
}
void Refresh(const uint8_t* b, Probe& p, bool inspect_resources = false) {
  p.owners.clear();
  std::vector<Watch> watches;
  const auto watch = [&](uint32_t owner, uint32_t data, uint32_t bytes) {
    if (!bytes || !Range(data, bytes)) return;
    for (const auto& w : watches) if (w.owner == owner && w.data == data && w.bytes == bytes) return;
    Watch w{owner, data, bytes};
    for (const auto& old : p.watches)
      if (old.owner == owner && old.data == data && old.bytes == bytes) { w = old; break; }
    const auto hash = Hash(b, data, bytes);
    if (w.changed_frame == kUnknown || w.hash != hash) w.changed_frame = p.frame.load();
    w.hash = hash;
    watches.push_back(w);
  };
  for (auto& r : p.roots) {
    r.nodes.clear();
    struct Reference {
      uint32_t actor, parent, group, entry;
      const char* origin;
    };
    std::vector<Reference> pending{{r.actor, 0, 0, 0, "root"}};
    std::set<uint32_t> seen;
    for (size_t i = 0; i < pending.size() && r.nodes.size() < kMaxNodes; ++i) {
      const auto ref = pending[i];
      const auto actor = ref.actor;
      if (!Range(actor, 36) || !seen.insert(actor).second) continue;
      Node n = ReadNode(b, actor);
      n.parent = ref.parent; n.group = ref.group; n.entry = ref.entry; n.origin = ref.origin;
      r.nodes.push_back(n);
      const auto [it, inserted] = p.owners.emplace(actor, r.actor);
      if (!inserted && it->second != r.actor) it->second = 0;  // Shared nodes are ambiguous, never guess.
      if (n.flags & 2) {
        const auto anim = Ptr(b, n.holder, 0, 180);
        const auto live = Hierarchy(b, Ptr(b, anim, 8, 4));
        if (live.valid) {
          // 8286D690 allocates exactly 5120 bytes at anim+92 (64 matrices).
          watch(r.actor, Ptr(b, anim, 92, 5120), std::min(live.count, 64u)*80);
          if (pending.size() < 2*kMaxNodes) for (uint32_t j = 0; j < live.count; ++j) {
            const auto child = Word(b, live.begin+8*j);
            if (child && !(child & 3)) pending.push_back({child, actor, Word(b, anim, 8), live.begin+8*j, "live-hierarchy"});
          }
        }
      }
      // Resource groups also contain actors: 828689A8 (.40:34463..34484)
      // and load cleanup 8287E498 (.62:34376..34399). Do not assume anim+8
      // is the entire renderable graph. Shared resource nodes remain ambiguous.
      if ((DrawOn() || inspect_resources) && (n.flags & 0x800000)) {
        const auto group = Hierarchy(b, n.resource);
        if (group.valid && pending.size() < 2*kMaxNodes)
          for (uint32_t j = 0; j < group.count; ++j) {
            const auto child = Word(b, group.begin+8*j);
            if (child && !(child & 3)) pending.push_back({child, actor, n.resource, group.begin+8*j, "resource-group"});
          }
      }
      if (n.flags & 0x400000) watch(r.actor, Ptr(b, n.holder, 12, 80), 80);
      if (!n.links) watch(r.actor, n.skin_data, n.skin_bytes);
    }
  }
  // C14: a level may register drawable skin instances separately from anim+8.
  // Only exact {matrix, actor, bone} links establish ownership, never a shared VB.
  // World registry ABI: 82847300 (.46:33964+); skin links: 8286D0E0 (.12:34873+).
  p.registry_entries = p.external_nodes = p.ambiguous_links = 0;
  p.registry_valid = p.registry_capped = false;
  if (DrawOn()) {
    const auto begin = Word(b, p.world, 40), end = Word(b, p.world, 28);
    p.registry_valid = end >= begin && !((end-begin) & 7) && end-begin <= 8*20000 && Range(begin, end-begin);
    if (p.registry_valid) for (uint32_t entry = begin; entry < end; entry += 8) {
      ++p.registry_entries;
      const auto actor = Word(b, entry);
      if (!Range(actor, 36) || (actor & 3)) continue;
      auto n = ReadNode(b, actor);
      if (!(n.flags & 0x4000) || !n.links || !n.skin_data || !n.skin_count) continue;
      for (auto& r : p.roots) {
        if (Word(b, r.actor) & 0x1000000) continue;  // The owner must store matrices, not links.
        const auto skin = Ptr(b, r.holder, 16, 8);
        const auto count = Word(b, skin), data = Word(b, skin, 4);
        if (!count || count > 256 || !Range(data, count*80)) continue;
        bool mentions = false, exact = true;
        for (uint32_t i = 0; i < n.skin_count; ++i) {
          const auto link = n.skin_data+12*i;
          const auto owner = Word(b, link, 4), bone = Word(b, link, 8);
          mentions |= owner == r.actor;
          exact &= owner == r.actor && bone < count && Word(b, link) == data+80*bone;
        }
        if (!mentions) continue;
        if (!exact) { ++p.ambiguous_links; continue; }
        const auto [it, inserted] = p.owners.emplace(actor, r.actor);
        if (!inserted && it->second != r.actor) { it->second = 0; ++p.ambiguous_links; continue; }
        if (std::any_of(r.nodes.begin(), r.nodes.end(), [&](const Node& old) { return old.actor == actor; })) continue;
        if (r.nodes.size() == kMaxNodes) { p.registry_capped = true; continue; }
        n.external = true; n.origin = "registry-skin-link"; n.entry = entry;
        r.nodes.push_back(n); ++p.external_nodes;
      }
    }
  }
  for (const auto& r : p.roots) p.registry_capped |= r.nodes.size() == kMaxNodes;
  p.watches = std::move(watches);
}
void DrainDraws(Probe& p) {
  if (!DrawOn() || !p.api) return;
  NrPuppetDrawEvent events[256];
  // The plugin queue holds at most 16384 records. Drain on each game frame,
  // never retain guest or DrawCall pointers and never log from the render worker.
  for (unsigned batch = 0; batch < 64; ++batch) {
    const auto count = p.api->drain(events, 256);
    for (uint32_t i = 0; i < count; ++i) {
      const auto& e = events[i];
      if (e.kind == 1) {
        REXLOG_INFO("PUPPET draw-frame epoch={} render-frame={} source={} copy={} unattributed={} dropped={} counts=decoded-all-passes-not-pixels", p.epoch, e.frame, e.source_draws, e.copy_draws, e.unattributed, e.dropped);
        continue;
      }
      const auto role = e.owner == p.roots[0].actor ? 0u : 1u;
      uint32_t instance = 0, asset = 0;
      for (const auto& n : p.roots[role].nodes) if (n.actor == e.mesh && (n.flags & 0x4000)) {
        instance = n.component[0];
        // Asset identity is supplied by the per-frame display-entry logs below.
        asset = n.resource;
        break;
      }
      REXLOG_INFO("PUPPET draw role={} epoch={} render-frame={} ordinal={} actor={:08X} node={:08X} instance={:08X} resource={:08X} vb={:08X} vb-physical={:08X} ib={:08X} ib-physical={:08X} vs={:08X} ps={:08X} declaration={:08X} stride={} indexed={} primitive={} base={} start={} count={} rt={:08X} depth={:08X} conditional={}",
          role ? "copy" : "source", p.epoch, e.frame, e.ordinal, e.owner, e.mesh, instance, asset,
          e.vertex_buffer, e.vertex_physical, e.index_buffer, e.index_physical,
          e.vertex_shader, e.pixel_shader, e.declaration, e.stride, e.indexed, e.primitive,
          e.base_vertex, e.start, e.count, e.target, e.depth, e.conditional);
    }
    if (count < 256) break;
  }
}
void LogDisplayEntries(const uint8_t* b, Probe& p) {
  if (!DrawOn() || !p.lists_left || p.list_frame == p.frame.load()) return;
  p.list_frame = p.frame.load(); --p.lists_left;
  REXLOG_INFO("PUPPET display-lists epoch={} game-frame={} registry-valid={} registry-entries={} external-nodes={} ambiguous-links={} capped={} entries=actor-mesh-inventory-not-submitted-draws",
      p.epoch, p.list_frame, p.registry_valid, p.registry_entries, p.external_nodes, p.ambiguous_links, p.registry_capped);
  for (unsigned role = 0; role < 2; ++role) {
    const auto& r = p.roots[role];
    const auto live = Hierarchy(b, Ptr(b, r.anim, 8, 4));
    const auto authored = Hierarchy(b, Ptr(b, r.anim, 4, 4));
    for (const auto& old : r.nodes) {
      const auto n = ReadNode(b, old.actor);
      if (n.actor != r.actor && !(n.flags & 0x4000)) continue;
      uint32_t slot = ~0u, authored_node = 0;
      if (live.valid) for (uint32_t i = 0; i < live.count; ++i)
        if (Word(b, live.begin+8*i) == n.actor) { slot = i; break; }
      if (authored.valid && slot < authored.count) authored_node = Word(b, authored.begin+8*slot);
      const auto authored_n = ReadNode(b, (authored_node & 3) ? 0 : authored_node);
      const auto instance = (n.flags & 0x4000) ? n.component[0] : 0;
      const auto authored_instance = (authored_n.flags & 0x4000) ? authored_n.component[0] : 0;
      REXLOG_INFO("PUPPET display-entry role={} epoch={} game-frame={} actor={:08X} node={:08X} origin={} slot={} authored-node={:08X} flags={:08X}/{:08X} instance={:08X} asset={:08X} type={} data24={:08X} identity64={:08X} authored-instance={:08X} authored-asset={:08X} skin={:08X} skin-data={:08X} skin-count={} links={} parent={:08X} group={:08X} entry={:08X}",
          role ? "copy" : "source", p.epoch, p.list_frame, r.actor, n.actor,
          old.origin, slot, authored_node,
          n.flags, n.flags4, instance, Word(b, instance), Word(b, Word(b, instance)),
          Word(b, instance, 24), Word(b, instance, 64), authored_instance, Word(b, authored_instance),
          n.skin, n.skin_data, n.skin_count, n.links, old.parent, old.group, old.entry);
    }
  }
}
std::string Stamp(uint64_t frame) { return frame == kUnknown ? "unknown" : std::to_string(frame); }
void LogModifiers(const uint8_t* b, const Root& r, const char* role) {
  // 82855D08 clones this list; 828688C8 binds +24 to the live hierarchy and
  // invokes table+24. Read only: these callbacks can have more than visual effects.
  uint32_t entry = Word(b, r.ext, 28);
  REXLOG_INFO("PUPPET mesh-modifiers role={} actor={:08X} head={:08X}", role, r.actor, entry);
  std::set<uint32_t> seen;
  for (unsigned i = 0; entry && i < 16; ++i) {
    if (!Range(entry, 28) || !seen.insert(entry).second) {
      REXLOG_INFO("PUPPET mesh-modifiers role={} actor={:08X} stopped=invalid-or-cycle entry={:08X}", role, r.actor, entry);
      return;
    }
    const auto table = Word(b, entry);
    REXLOG_INFO("PUPPET mesh-modifier role={} actor={:08X} entry={:08X} table={:08X} type={} flags={:08X} data={:08X} owner={:08X} bound={:08X} live={:08X} slots4/12/16/20/24={:08X}/{:08X}/{:08X}/{:08X}/{:08X}",
        role, r.actor, entry, table, Word(b, table), Word(b, entry, 4), Word(b, entry, 8),
        Word(b, entry, 20), Word(b, entry, 24), Word(b, r.anim, 8),
        Word(b, table, 4), Word(b, table, 12), Word(b, table, 16), Word(b, table, 20), Word(b, table, 24));
    entry = Word(b, entry, 12);
  }
  if (entry) REXLOG_INFO("PUPPET mesh-modifiers role={} actor={:08X} stopped=cap16 next={:08X}", role, r.actor, entry);
}
NrPuppetMeshScope PaletteOwner(const Probe& p, uint32_t data, uint32_t count) {
  NrPuppetMeshScope result;
  if (!data || !count || count > 256 || !Range(data, count*48)) return result;
  for (uint32_t i = 0; i < count; ++i) {
    const auto it = p.palettes.find(data+48*i);
    if (it == p.palettes.end() || !it->second.owner) return {};
    if (!i) result = it->second;
    else if (result.owner != it->second.owner || result.mesh != it->second.mesh) return {};
  }
  return result;
}
void RecordPaletteUse(const uint8_t* b, Probe& p, NrPuppetMeshScope scope,
                      uint32_t context, uint32_t material, uint32_t packet, unsigned stage) {
  // 82783580 (.106:30484..30516): float4 upload at constant parameter 5,
  // address context[468] + 48*context[484], count 3*context[488].
  const auto first = Word(b, context, stage ? 484 : 492), count = Word(b, context, stage ? 488 : 496);
  const auto data = Word(b, context, 468);
  const bool valid = count && count <= 256 && first < 16384 &&
      uint64_t(first)+count <= Word(b, context, 472) && Range(data, (first+count)*48);
  const auto address = valid ? data+first*48 : 0;
  if (!scope.owner && valid) scope = PaletteOwner(p, address, count);
  if (scope.owner != p.roots[0].actor && scope.owner != p.roots[1].actor) return;
  const Probe::PaletteKey key{scope.owner, scope.mesh, data, first, count, material};
  if (p.palette_uses.size() >= 512 && !p.palette_uses.contains(key)) { ++p.palette_dropped; return; }
  auto& use = p.palette_uses[key];
  use.owner = scope.owner; use.node = scope.mesh; use.context = context;
  use.data = data; use.first = first; use.count = count; use.material = material; use.packet = packet;
  const auto matrix_entry = [&](uint32_t offset) {
    const auto matrix_data = Word(b, context, offset), index = Word(b, context, offset+12);
    return index < Word(b, context, offset+4) && index < 16384 && Range(matrix_data, (index+1)*64) ?
        matrix_data+index*64 : 0u;
  };
  use.world_entry = matrix_entry(420); use.local_entry = matrix_entry(404);
  use.valid = valid;
  if (stage == 2) ++use.draws; else if (stage == 1) ++use.prepares; else ++use.builds;
  use.source_entries = use.copy_entries = use.unknown_entries = 0;
  if (valid) for (uint32_t i = 0; i < count; ++i) {
    const auto it = p.palettes.find(address+48*i);
    if (it == p.palettes.end()) ++use.unknown_entries;
    else if (it->second.owner == p.roots[0].actor) ++use.source_entries;
    else if (it->second.owner == p.roots[1].actor) ++use.copy_entries;
    else ++use.unknown_entries;
  }
  use.saw_source |= use.source_entries != 0;
  use.saw_copy |= use.copy_entries != 0;
  use.saw_unknown |= !valid || use.unknown_entries != 0;
}
void LogPaletteInputs(const uint8_t* b, const Probe& p, const Node& n, unsigned role) {
  if (!(n.flags & 0x4000)) return;
  const auto instance = n.component[0];
  const auto source_skin = Ptr(b, p.roots[0].holder, 16, 8);
  const auto source_data = Word(b, source_skin, 4), source_count = Word(b, source_skin);
  const auto copy_skin = Ptr(b, p.roots[1].holder, 16, 8);
  const auto copy_data = Word(b, copy_skin, 4), copy_count = Word(b, copy_skin);
  const auto in_palette = [](uint32_t matrix, uint32_t data, uint32_t count) {
    return count && count <= 256 && Range(data, count*80) && matrix >= data &&
        uint64_t(matrix)+80 <= uint64_t(data)+count*80 && (matrix-data)%80 == 0;
  };
  uint32_t source_links = 0, copy_links = 0, unknown_links = 0;
  if (n.links && n.skin_data) for (uint32_t i = 0; i < n.skin_count; ++i) {
    const auto matrix = Word(b, n.skin_data+12*i);
    if (in_palette(matrix, source_data, source_count)) ++source_links;
    else if (in_palette(matrix, copy_data, copy_count)) ++copy_links;
    else ++unknown_links;
  }
  const auto owner = p.owners.find(n.actor);
  const bool owned = owner != p.owners.end() && owner->second == p.roots[role].actor;
  const char* shared = !role ? "n/a" : !owned ? "unknown-shared-node" : source_links ? "true" :
      n.links && n.skin_count && n.skin_data && !unknown_links ? "false" : "unknown";
  uint32_t source_instance = 0;
  if (role) {
    const auto copy_live = Hierarchy(b, Ptr(b, p.roots[1].anim, 8, 4));
    const auto source_live = Hierarchy(b, Ptr(b, p.roots[0].anim, 8, 4));
    if (copy_live.valid && source_live.valid) for (uint32_t i = 0; i < copy_live.count && i < source_live.count; ++i)
      if (Word(b, copy_live.begin+8*i) == n.actor) {
        const auto source_node = ReadNode(b, Word(b, source_live.begin+8*i));
        if (source_node.flags & 0x4000) source_instance = source_node.component[0];
        break;
      }
  }
  const auto constants = Word(b, instance, 48), source_constants = Word(b, source_instance, 48);
  const char* shared_constants = !role ? "n/a" : !owned || !constants || !source_constants ? "unknown" :
      constants == source_constants ? "true" : "false";
  REXLOG_INFO("PUPPET mesh-palette-input role={} actor={:08X} node={:08X} instance={:08X} uniquely-owned={} skin-data={:08X} source-palette={:08X} copy-palette={:08X} source-links={} copy-links={} unknown-links={} copy-uses-source-palette={} mesh-constants={:08X} mesh-constant-count={} source-instance={:08X} source-constants={:08X} copy-uses-source-constants={} identity64={:08X} evidence=skin-input-not-draw anatomy=unknown",
      role ? "copy" : "source", p.roots[role].actor, n.actor, instance, owned, n.skin_data,
      source_data, copy_data, source_links, copy_links, unknown_links, shared,
      constants, Word(b, instance, 36), source_instance, source_constants, shared_constants, Word(b, instance, 64));
}
void Log(const uint8_t* b, Probe& p) {
  for (unsigned role = 0; role < 2; ++role) {
    auto& r = p.roots[role];
    NrPuppetMeshDraws total;
    const bool connected = p.api && p.api->read(r.actor, 0, &total);
    const auto live = Hierarchy(b, Ptr(b, r.anim, 8, 4));
    const auto authored = Hierarchy(b, Ptr(b, r.anim, 4, 4));
    REXLOG_INFO("PUPPET mesh role={} epoch={} frame={} actor={:08X} holder={:08X} anim={:08X} skeleton={:08X} authored={:08X} alt={:08X} bones={:08X} live-count={} live-valid={} live-tagged={} authored-count={} authored-valid={} authored-tagged={} nodes={} capped={} animation-call-frame={} animation-calls={} skeleton-pass-frame={} skeleton-passes={} render-scope-calls={} renderer-connected={} renderer-seen={} renderer-delta={}",
        role ? "copy" : "source", p.epoch, p.frame.load(), r.actor, r.holder, r.anim,
        Word(b, r.anim, 8), Word(b, r.anim, 4), Word(b, r.anim, 12), Word(b, r.anim, 92),
        live.count, live.valid, live.tagged, authored.count, authored.valid, authored.tagged,
        r.nodes.size(), r.nodes.size() == kMaxNodes,
        Stamp(r.animation_frame), r.animation_calls, Stamp(r.skeleton_frame), r.skeleton_calls,
        r.scope_calls, connected, total.seen, total.seen-r.previous_draws[0]);
    REXLOG_INFO("PUPPET mesh-pose role={} actor={:08X} hierarchy-pass-frame={} hierarchy-passes={} auxiliary-pass-frame={} auxiliary-passes={} active-layer-mask={:02X} reload-retained={} ai-gate-bit={} hierarchy-gate-bit={} modifier-gate-bit={}",
        role ? "copy" : "source", r.actor, Stamp(r.hierarchy_frame), r.hierarchy_calls,
        Stamp(r.skeleton_frame), r.skeleton_calls, b[r.anim+1] & 15,
        (b[r.actor+31] & 16) != 0, (b[r.actor+31] & 32) != 0,
        (b[r.actor+31] & 64) != 0, (b[r.actor+31] & 128) != 0);
    LogModifiers(b, r, role ? "copy" : "source");
    REXLOG_INFO("PUPPET mesh-animation actor={:08X} state={}/{} layers={:08X},{:08X},{:08X},{:08X} blends={:08X},{:08X},{:08X},{:08X} action={} item={} prepared-matrices={} global-draw-queries={} provenance-hits={}",
        r.actor, b[r.anim], b[r.anim+1], Word(b,r.anim,132), Word(b,r.anim,136), Word(b,r.anim,140), Word(b,r.anim,144),
        Word(b,r.anim,96), Word(b,r.anim,100), Word(b,r.anim,104), Word(b,r.anim,108), Load16(b,r.anim+156), b[r.anim+160],
        p.prepared.size(), p.draw_queries, p.provenance_hits);
    r.previous_draws[0] = total.seen;
    for (const auto& n : r.nodes) {
      NrPuppetMeshDraws d;
      if (p.api) p.api->read(r.actor, n.actor, &d);
      const auto owner = p.owners.find(n.actor);
      REXLOG_INFO("PUPPET mesh-node role={} actor={:08X} node={:08X} shared={} flags={:08X}/{:08X} matrix={:08X} bounds={:08X} holder={:08X} components={:08X},{:08X},{:08X},{:08X},{:08X} mesh={:08X} ext={:08X} resource={:08X} resource-data={:08X} lifecycle-flags={} importance={} descriptor-flags={:04X} skin={:08X} skin-data={:08X} skin-count={} skin-links={} renderer-seen={} renderer-delta={} draw-frame={} vb={:08X} ib={:08X} vs={:08X} start={} count={}",
          role ? "copy" : "source", r.actor, n.actor, owner != p.owners.end() && !owner->second,
          n.flags, n.flags4, n.matrix, n.bounds, n.holder, n.component[0], n.component[1], n.component[2], n.component[3], n.component[4],
          (n.flags & 0x4000) ? Word(b, n.component[0]) : 0, n.ext, n.resource, Word(b, n.resource), b[n.actor+31], b[n.actor+28], n.ext ? Load16(b, n.ext+46) : 0,
          n.skin, n.skin_data, n.skin_count, n.links, d.seen, d.seen-r.previous_draws[n.actor], d.last_frame,
          d.vertex_buffer, d.index_buffer, d.vertex_shader, d.start, d.count);
      r.previous_draws[n.actor] = d.seen;
      LogPaletteInputs(b, p, n, role);
      if (n.links && n.skin_data) {
        std::string links;
        for (uint32_t i = 0; i < n.skin_count; ++i) {
          const auto e = n.skin_data+12*i;
          links += fmt::format(" {}:{:08X}/{:08X}/{}", i, Word(b,e), Word(b,e,4), Word(b,e,8));
        }
        REXLOG_INFO("PUPPET skin-links actor={:08X} node={:08X} entries=index:matrix/owner/bone{}", r.actor, n.actor, links);
      }
    }
  }
  for (const auto& w : p.watches)
    REXLOG_INFO("PUPPET bones actor={:08X} data={:08X} bytes={} hash={:016X} last-change-frame={} last-observed-write-frame={} observed-writes={} writers=82728BC0/82728C90 coverage=partial unknown-is-not-proof-of-no-update",
        w.owner, w.data, w.bytes, w.hash, Stamp(w.changed_frame), Stamp(w.write_frame), w.writes);
  for (const auto& [key, use] : p.palette_uses) {
    const bool copy = use.owner == p.roots[1].actor;
    const char* shared = !copy ? "n/a" : use.saw_source ? "true" :
        use.saw_unknown || !use.saw_copy ? "unknown" : "false";
    REXLOG_INFO("PUPPET mesh-palette-use role={} actor={:08X} node={:08X} context={:08X} constants={:08X} first={} matrices={} address={:08X} valid={} material={:08X} packet={:08X} world-entry={:08X} local-entry={:08X} builder-prepares={} shader-prepares={} decoded-draw-attempts={} source-entries={} copy-entries={} unknown-entries={} copy-uses-source-palette={} provenance=constant-builder any-source={} any-copy={} any-unknown={} anatomy=unknown",
        copy ? "copy" : "source", use.owner, use.node, use.context, use.data, use.first, use.count,
        use.valid ? use.data+48*use.first : 0, use.valid, use.material, use.packet, use.world_entry, use.local_entry,
        use.builds, use.prepares, use.draws,
        use.source_entries, use.copy_entries, use.unknown_entries, shared,
        use.saw_source, use.saw_copy, use.saw_unknown);
  }
  if (DrawOn()) REXLOG_INFO("PUPPET draws probe state=heartbeat connected={} epoch={} tracked-constants={} palette-use-dropped={} capture-game-frames-left={} log-rotation=also-read-run.N.log",
      p.api != nullptr, p.epoch, p.palettes.size(), p.palette_dropped, p.lists_left);
  p.palette_uses.clear(); p.palette_dropped = 0;
}
// These functions set GDI+156 to their r3 actor, then invoke mesh preparation
// (.125:33826+, .110:35404+). Actual D3D work can execute later.
struct RenderScope {
  NrPuppetMeshScope previous = preparing;
  bool entered = false;
  RenderScope(const uint8_t* b, uint32_t actor, uint32_t hook, bool enabled = true) {
    if (!enabled) return;
    auto& p = Get();
    if (!p.active.load(std::memory_order_acquire)) return;
    entered = true; ++preparation_depth;
    NrPuppetMeshScope next;
    {
      std::lock_guard lock(p.mutex);
      const auto it = p.owners.find(actor);
      if (it != p.owners.end() && it->second) {
        next = {it->second, actor};
        for (auto& r : p.roots) if (r.actor == next.owner) ++r.scope_calls;
        if (DrawOn() && p.lists_left) {
          const auto n = ReadNode(b, actor);
          const auto instance = (n.flags & 0x4000) ? n.component[0] : 0;
          REXLOG_INFO("PUPPET draw-prepare role={} epoch={} game-frame={} actor={:08X} node={:08X} hook={:08X} instance={:08X} asset={:08X} data24={:08X}",
              next.owner == p.roots[0].actor ? "source" : "copy", p.epoch, p.frame.load(),
              next.owner, actor, hook, instance, Word(b, instance), Word(b, instance, 24));
        }
      }
    }
    preparing = next;  // Nested, unrelated and shared scopes must also clear it.
  }
  ~RenderScope() { if (entered) { preparing = previous; --preparation_depth; } }
};
// 827B8B80(r3=actor,r4=mesh resource) reads actor skin links, then
// 820DF468 appends transposed 48-byte constants to *828D1CD4 + 468.
struct PaletteBuildScope {
  NrPuppetMeshScope previous = building_palette;
  uint32_t actor = 0, mesh = 0;
  uint32_t context = 0, before_count = 0;
  bool entered = false;
  PaletteBuildScope(const uint8_t* b, uint32_t a, uint32_t m) : actor(a), mesh(m) {
    auto& p = Get();
    if (!DrawOn() || !p.active.load(std::memory_order_acquire)) return;
    entered = true; building_palette = {};
    context = Word(b, 0x828D1CD4); before_count = Word(b, context, 472);
    std::lock_guard lock(p.mutex);
    const auto it = p.owners.find(actor);
    if (it != p.owners.end() && it->second) building_palette = {it->second, actor};
  }
  void Complete(const uint8_t* b) {
    if (!entered || Word(b, mesh, 76)) return;  // Nonzero means CPU skinning, no GPU palette append.
    auto& p = Get();
    std::lock_guard lock(p.mutex);
    if (!p.active.load()) return;
    // Early exits leave 492/496 unchanged. Never attribute the preceding actor's range.
    const auto after_count = Word(b, context, 472);
    if (Word(b, 0x828D1CD4) != context || after_count <= before_count ||
        Word(b, context, 492) != before_count || Word(b, context, 496) != after_count-before_count) return;
    RecordPaletteUse(b, p, building_palette, context, 0, 0, 0);
  }
  ~PaletteBuildScope() { if (entered) building_palette = previous; }
};
void PaletteAppended(const uint8_t* b, uint32_t header) {
  auto& p = Get();
  if (!DrawOn() || !p.active.load(std::memory_order_acquire) ||
      header != uint64_t(Word(b, 0x828D1CD4))+468) return;
  const auto data = Word(b, header), count = Word(b, header, 4);
  if (!count || count > 16384 || !Range(data, count*48)) return;
  std::lock_guard lock(p.mutex);
  if (p.palette_buffers.size() >= 16 && !p.palette_buffers.contains(header)) {
    p.palette_buffers.clear(); p.palettes.clear(); ++p.palette_dropped;
  }
  auto& buffer = p.palette_buffers[header];
  if (buffer.data && buffer.data != data) {
    std::vector<std::pair<uint32_t, NrPuppetMeshScope>> copied;
    for (auto it = p.palettes.lower_bound(buffer.data); it != p.palettes.end() &&
         uint64_t(it->first) < uint64_t(buffer.data)+48*buffer.count;) {
      const auto slot = (it->first-buffer.data)/48;
      if (slot < count-1) copied.emplace_back(data+48*slot, it->second);
      it = p.palettes.erase(it);
    }
    for (const auto& entry : copied) p.palettes.insert_or_assign(entry.first, entry.second);
  }
  buffer = {data, count};
  const auto address = data+48*(count-1);
  if (!building_palette.owner) p.palettes.erase(address);
  else {
    if (p.palettes.size() >= 16384 && !p.palettes.contains(address)) {
      p.palettes.clear(); ++p.palette_dropped;
    }
    p.palettes[address] = building_palette;
  }
}
void ShaderPalette(const uint8_t* b, uint32_t material, uint32_t packet) {
  executing_palette = {};
  auto& p = Get();
  if (!DrawOn() || !p.active.load(std::memory_order_acquire)) return;
  const auto context = Word(b, 0x828D1CD8);
  // Exact enable predicates for the parameter-5 upload in 82783580.
  const bool enabled = Word(b, material, 32) && (Word(b, context, 0xDC44C) & 0x10000000);
  std::lock_guard lock(p.mutex);
  executing_palette = {p.generation, context, material, packet, Word(b, context, 432),
      Word(b, context, 484), Word(b, context, 488), enabled};
  if (!enabled) return;
  NrPuppetMeshScope scope = preparing;
  const auto matrix = Word(b, context, 420), index = Word(b, context, 432);
  if (index < 16384 && Range(matrix, (index+1)*64)) {
    const auto it = p.prepared.find(matrix+index*64);
    if (it != p.prepared.end()) scope = it->second;
  }
  RecordPaletteUse(b, p, scope, context, material, packet, 1);
}
void MatrixPrepared(const uint8_t* b, uint32_t context, uint32_t offset, uint32_t index) {
  auto& p = Get();
  if (!p.active.load(std::memory_order_acquire) || index >= 16384) return;
  const auto data = Ptr(b, context, offset, 64);
  if (!data || !Range(data, (index+1)*64)) return;
  const auto address = data+64*index;
  std::lock_guard lock(p.mutex);
  if (p.buffers.size() >= 16 && !p.buffers.contains(context+offset)) {
    p.buffers.clear(); p.prepared.clear();  // Bounded diagnostic, never keep stale allocation identities.
  }
  auto& buffer = p.buffers[context+offset];
  if (buffer.data && buffer.data != data) {
    // 820D87A0 grows and copies these arrays. Follow their copied provenance.
    std::vector<std::pair<uint32_t, NrPuppetMeshScope>> copied;
    for (auto it = p.prepared.lower_bound(buffer.data); it != p.prepared.end() &&
         uint64_t(it->first) < uint64_t(buffer.data)+64*buffer.count;) {
      const auto slot = (it->first-buffer.data)/64;
      if (slot < index) copied.emplace_back(data+64*slot, it->second);
      it = p.prepared.erase(it);
    }
    for (const auto& entry : copied) p.prepared.insert_or_assign(entry.first, entry.second);
  }
  buffer = {data, index+1};
  if (!preparing.owner) p.prepared.erase(address);
  else {
    if (p.prepared.size() < 16384 || p.prepared.contains(address)) p.prepared[address] = preparing;
    if (DrawOn() && p.lists_left)
      REXLOG_INFO("PUPPET draw-list-matrix role={} epoch={} game-frame={} actor={:08X} node={:08X} context={:08X} array-offset={} index={} entry={:08X}",
          preparing.owner == p.roots[0].actor ? "source" : "copy", p.epoch, p.frame.load(),
          preparing.owner, preparing.mesh, context, offset, index, address);
  }
}
void MatrixWritten(uint32_t destination) {
  auto& p = Get();
  if (!p.active.load(std::memory_order_acquire)) return;
  std::lock_guard lock(p.mutex);
  for (auto& w : p.watches) if (destination >= w.data &&
      uint64_t(destination)+80 <= uint64_t(w.data)+w.bytes) {
    w.write_frame = p.frame.load(); ++w.writes;
  }
}
void AnimationCalled(uint32_t actor) {
  auto& p = Get();
  if (!p.active.load(std::memory_order_acquire)) return;
  std::lock_guard lock(p.mutex);
  for (auto& r : p.roots) if (r.actor == actor) { r.animation_frame = p.frame.load(); ++r.animation_calls; }
}
void SkeletonCalled(uint32_t actor) {
  auto& p = Get();
  if (!p.active.load(std::memory_order_acquire)) return;
  std::lock_guard lock(p.mutex);
  // 828724C0 calls 8280F6E0 only for active layers above zero (.75:34794).
  // A stopped count with mask=1 does not imply that the base pose stopped.
  for (auto& r : p.roots) if (r.actor == actor) { r.skeleton_frame = p.frame.load(); ++r.skeleton_calls; }
}
void HierarchyCalled(uint32_t actor) {
  auto& p = Get();
  if (!p.active.load(std::memory_order_acquire)) return;
  std::lock_guard lock(p.mutex);
  // 828724C0 includes layer zero's inline matrix copy (.75:35125..35146).
  // Completion still does not prove writes to every bone or visible pixels.
  for (auto& r : p.roots) if (r.actor == actor) { r.hierarchy_frame = p.frame.load(); ++r.hierarchy_calls; }
}
}  // namespace

void DevPuppetMeshSpawnFixPreflight(const uint8_t* b, uint32_t source, uint32_t copy, uint64_t epoch) {
  static const bool requested = [] {
    const char* v = std::getenv("KK_DEV_PUPPET_SPAWN_FIX");
    return v && std::string(v) == "1";
  }();
  if (!requested) return;
  // C16's two proposed targets have no skin component (C15 flags 00097001
  // and 00015001). 8286FE40 preserves that layout; 8286D0E0 only rebinds
  // EXISTING 0x200000 skin entries. Neither call invents a bone-index map.
  // The copy reaches them through ext+12 resources, copied at 82870AB4,
  // not through its private anim+8 hierarchy. Never edit the shared lists.
  Probe snapshot;
  snapshot.world = Word(b, 0x828E8388);
  snapshot.roots[0].actor = source; snapshot.roots[1].actor = copy;
  const auto source_node = ReadNode(b, source), copy_node = ReadNode(b, copy);
  if (!source || source == copy || !(source_node.flags & 2) || !(copy_node.flags & 2)) {
    REXLOG_INFO("PUPPET spawn-fix epoch={} source={:08X} copy={:08X} state=refused reason=invalid-distinct-rig-roots cloned=0 rebound=0 guest-writes=false",
        epoch, source, copy);
    return;
  }
  const auto source_anim = Ptr(b, source_node.holder, 0, 180);
  const auto copy_anim = Ptr(b, copy_node.holder, 0, 180);
  const auto source_live = Hierarchy(b, Word(b, source_anim, 8));
  const auto copy_live = Hierarchy(b, Word(b, copy_anim, 8));
  if (!source_anim || !copy_anim || source_anim == copy_anim || !source_live.valid || !copy_live.valid) {
    REXLOG_INFO("PUPPET spawn-fix epoch={} source={:08X} copy={:08X} state=refused reason=invalid-or-aliased-live-hierarchy cloned=0 rebound=0 guest-writes=false",
        epoch, source, copy);
    return;
  }
  Refresh(b, snapshot, true);
  uint32_t shared_meshes = 0;
  for (const auto& n : snapshot.roots[1].nodes) {
    const auto owner = snapshot.owners.find(n.actor);
    if (!(n.flags & 0x4000) || owner == snapshot.owners.end() || owner->second) continue;
    ++shared_meshes;
    bool in_live = false;
    for (uint32_t i = 0; i < copy_live.count; ++i)
      in_live |= Word(b, copy_live.begin+8*i) == n.actor;
    const char* reason = !(n.flags & 0x200000) ? "no-skin-component-no-bone-map" :
        !n.links || !n.skin_data || !n.skin_count ? "no-valid-existing-skin-links" :
        !in_live ? "resource-template-not-private-live-reference" : "shared-live-node-needs-ownership-proof";
    REXLOG_INFO("PUPPET spawn-fix-node epoch={} copy={:08X} node={:08X} instance={:08X} flags={:08X} origin={} parent={:08X} group={:08X} entry={:08X} in-copy-live={} skin={:08X} skin-data={:08X} skin-count={} links={} mesh-constants={:08X} mesh-constant-count={} state=refused reason={} cloned=0 rebound=0 anatomy=unproven",
        epoch, copy, n.actor, n.component[0], n.flags, n.origin, n.parent, n.group, n.entry,
        in_live, n.skin, n.skin_data, n.skin_count, n.links,
        Word(b, n.component[0], 48), Word(b, n.component[0], 36), reason);
  }
  REXLOG_INFO("PUPPET spawn-fix epoch={} source={:08X} copy={:08X} state=refused reason=no-proven-safe-body-node-clone shared-meshes={} source-live-tagged={} copy-live-tagged={} inventory-capped={} cloned=0 rebound=0 guest-writes=false",
      epoch, source, copy, shared_meshes, source_live.tagged, copy_live.tagged,
      snapshot.roots[0].nodes.size() == kMaxNodes || snapshot.roots[1].nodes.size() == kMaxNodes || snapshot.registry_capped);
}

void DevPuppetMeshReset() {
  if (!On()) return;
  auto& p = Get();
  p.active.store(false, std::memory_order_release);
  std::lock_guard lock(p.mutex);
  p.owners.clear(); p.watches.clear(); p.prepared.clear(); p.buffers.clear();
  p.palettes.clear(); p.palette_buffers.clear(); p.palette_uses.clear(); p.palette_dropped = 0;
  ++p.generation;
  p.draw_queries = p.provenance_hits = 0;
  p.world = p.lists_left = 0; p.list_frame = kUnknown;
  for (auto& r : p.roots) r = {};
  if (p.api) p.api->reset();
}
void DevPuppetMeshFrame(uint64_t frame) {
  // Emit even if MESH was omitted or no copy ever reaches Arm.
  static const bool announced = [] {
    if (DrawOn()) REXLOG_INFO("PUPPET draws probe state={} mesh-env={} draws-env=true log-rotation=also-read-run.N.log",
        On() ? "waiting-for-spawn" : "mesh-env-disabled", On());
    return true;
  }();
  (void)announced;
  if (!On()) return;
  auto& p = Get(); p.frame.store(frame);
  if (!p.active.load(std::memory_order_acquire)) return;
  std::lock_guard lock(p.mutex);
  DrainDraws(p);
}
void DevPuppetMeshArm(const uint8_t* b, uint32_t source, uint32_t copy, uint64_t epoch) {
  if (!On()) return;
  DevPuppetMeshReset();
  auto& p = Get();
  std::lock_guard lock(p.mutex);
  p.api = FindApi(); p.epoch = epoch;
  p.world = Ptr(b, 0x828E8388, 0, 44);
  const uint32_t actors[]{source, copy};
  for (unsigned i = 0; i < 2; ++i) {
    auto& r = p.roots[i];
    r.actor = actors[i]; r.holder = Ptr(b, r.actor, 20, 20); r.anim = Ptr(b, r.holder, 0, 180);
    r.ext = Ptr(b, r.actor, 24, 48); r.matrix = Ptr(b, r.actor, 12, 80);
    if (!r.anim || !r.ext || !r.matrix) { REXLOG_INFO("PUPPET mesh refused invalid-root role={}", i); return; }
  }
  Refresh(b, p); Log(b, p); p.next = Clock::now()+std::chrono::seconds(1);
  if (DrawOn()) {
    p.lists_left = 120;
    if (p.api) p.api->capture(source, copy, 120);
    REXLOG_INFO("PUPPET draw-capture epoch={} frames=120 plugin-version={} connected={} display-entry-frames=120 anatomy=unknown-match-assets-and-ranges", p.epoch, kNrPuppetProbeVersion, p.api != nullptr);
  }
  REXLOG_INFO("PUPPET mesh armed read-only=true preparation=82876D00/82876FA0 matrix-provenance=820E0B78/820E0C40 renderer-counts=decoded-backend-received-not-GPU-visible matrix-write-coverage=partial");
  p.active.store(true, std::memory_order_release);
}
void DevPuppetMeshTick(const uint8_t* b) {
  auto& p = Get();
  if (!p.active.load(std::memory_order_acquire)) return;
  std::lock_guard lock(p.mutex);
  if (Word(b, 0x828E8388) != p.world || !Identity(b, p.roots[0]) || !Identity(b, p.roots[1])) {
    p.active.store(false, std::memory_order_release);
    p.owners.clear(); p.watches.clear(); p.prepared.clear(); p.buffers.clear();
    p.palettes.clear(); p.palette_buffers.clear(); p.palette_uses.clear(); ++p.generation;
    if (p.api) p.api->reset();
    REXLOG_INFO("PUPPET mesh stopped root-identity-changed no-stale-reads");
    return;
  }
  // Refresh membership before reading per-frame entries, including separate
  // registry skins, rather than carrying their addresses across scene changes.
  if (DrawOn() && p.lists_left) Refresh(b, p);
  LogDisplayEntries(b, p);
  if (Clock::now() < p.next) return;
  p.next = Clock::now()+std::chrono::seconds(1);
  Refresh(b, p); Log(b, p);
}
uint64_t DevPuppetMeshDrawBegin(const uint8_t* b) {
  auto& p = Get();
  if (!p.active.load(std::memory_order_acquire)) return kUnknown;
  std::lock_guard lock(p.mutex);
  NrPuppetMeshScope next = preparing;  // Also supports immediate draws inside preparation.
  ++p.draw_queries;
  // The executor uses *828D1CD8 as its context. Its current world/view matrix
  // indices are +416/+432, with arrays at +404/+420. 82793CB0 copies packet+148
  // to +432 (.81:31392-31404); 82783580 consumes these arrays (.106:30258+).
  const auto context = Ptr(b, 0x828D1CD8, 0, 452);
  if (!preparation_depth) for (const uint32_t offset : {420u, 404u}) {
    const auto index = Word(b, context, offset+12);
    const auto count = Word(b, context, offset+4);
    const auto data = Ptr(b, context, offset, 64);
    if (data && index < count && index < 16384 && Range(data, (index+1)*64)) {
      const auto it = p.prepared.find(data+64*index);
      if (it != p.prepared.end()) { next = it->second; ++p.provenance_hits; break; }
    }
  }
  if (DrawOn() && executing_palette.enabled && executing_palette.generation == p.generation &&
      (Word(b, context, 0xDC44C) & 0x10000000) &&
      executing_palette.context == context && executing_palette.world_index == Word(b, context, 432) &&
      executing_palette.first == Word(b, context, 484) && executing_palette.count == Word(b, context, 488)) {
    if (!next.owner) {
      const auto first = executing_palette.first, count = executing_palette.count;
      const auto data = Word(b, context, 468);
      if (count <= 256 && first < 16384 && Range(data, (first+count)*48))
        next = PaletteOwner(p, data+first*48, count);
    }
    RecordPaletteUse(b, p, next, context, executing_palette.material, executing_palette.packet, 2);
  }
  if (!p.api) return kUnknown;
  const auto previous = p.api->scope(next);
  return (uint64_t(previous.owner) << 32) | previous.mesh;
}
void DevPuppetMeshDrawEnd(uint64_t previous) {
  if (previous == kUnknown) return;
  auto& p = Get();
  std::lock_guard lock(p.mutex);
  if (p.api) p.api->scope({uint32_t(previous >> 32), uint32_t(previous)});
}
}  // namespace kk

REX_EXTERN(__imp__sub_827B8B80);
REX_HOOK_RAW(sub_827B8B80) {
  kk::PaletteBuildScope scope(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_827B8B80(ctx, base);
  scope.Complete(base);
}
REX_EXTERN(__imp__sub_820DF468);
REX_HOOK_RAW(sub_820DF468) {
  const uint32_t header = ctx.r3.u32;
  __imp__sub_820DF468(ctx, base);
  kk::PaletteAppended(base, header);
}
REX_EXTERN(__imp__sub_82783580);
REX_HOOK_RAW(sub_82783580) {
  kk::ShaderPalette(base, ctx.r4.u32, ctx.r5.u32);
  __imp__sub_82783580(ctx, base);
}
REX_EXTERN(__imp__sub_82876D00);
REX_HOOK_RAW(sub_82876D00) {
  kk::RenderScope scope(base, ctx.r3.u32, 0x82876D00);
  __imp__sub_82876D00(ctx, base);
}
REX_EXTERN(__imp__sub_82876FA0);
REX_HOOK_RAW(sub_82876FA0) {
  kk::RenderScope scope(base, ctx.r3.u32, 0x82876FA0);
  __imp__sub_82876FA0(ctx, base);
}
// C14 covers hierarchy and modifier preparation in addition to leaf meshes.
// The actor renderer dispatches these (.1:34107,34133,34147).
REX_EXTERN(__imp__sub_82872168);
REX_HOOK_RAW(sub_82872168) {
  kk::RenderScope scope(base, ctx.r3.u32, 0x82872168, kk::DrawOn());
  __imp__sub_82872168(ctx, base);
}
REX_EXTERN(__imp__sub_82876C60);
REX_HOOK_RAW(sub_82876C60) {
  kk::RenderScope scope(base, ctx.r3.u32, 0x82876C60, kk::DrawOn());
  __imp__sub_82876C60(ctx, base);
}
REX_EXTERN(__imp__sub_82855908);
REX_HOOK_RAW(sub_82855908) {
  kk::RenderScope scope(base, ctx.r3.u32, 0x82855908, kk::DrawOn());
  __imp__sub_82855908(ctx, base);
}
REX_EXTERN(__imp__sub_82728BC0);
REX_HOOK_RAW(sub_82728BC0) {
  const uint32_t destination = ctx.r3.u32;
  __imp__sub_82728BC0(ctx, base);
  kk::MatrixWritten(destination);
}
REX_EXTERN(__imp__sub_82728C90);
REX_HOOK_RAW(sub_82728C90) {
  const uint32_t destination = ctx.r3.u32;
  __imp__sub_82728C90(ctx, base);
  kk::MatrixWritten(destination);
}
REX_EXTERN(__imp__sub_82810D78);
REX_HOOK_RAW(sub_82810D78) {
  const uint32_t actor = ctx.r3.u32;
  __imp__sub_82810D78(ctx, base);
  kk::AnimationCalled(actor);
}
REX_EXTERN(__imp__sub_8280F6E0);
REX_HOOK_RAW(sub_8280F6E0) {
  const uint32_t actor = ctx.r3.u32;
  __imp__sub_8280F6E0(ctx, base);
  kk::SkeletonCalled(actor);
}
REX_EXTERN(__imp__sub_828724C0);
REX_HOOK_RAW(sub_828724C0) {
  const uint32_t actor = ctx.r3.u32;
  __imp__sub_828724C0(ctx, base);
  kk::HierarchyCalled(actor);
}
// The engine's append helpers return the newly stored matrix's index (.56:150+,
// .37:146+). Preserve provenance rather than matching floats or a shared VB.
REX_EXTERN(__imp__sub_820E0C40);
REX_HOOK_RAW(sub_820E0C40) {
  const uint32_t context = ctx.r3.u32;
  __imp__sub_820E0C40(ctx, base);
  kk::MatrixPrepared(base, context, 420, ctx.r3.u32);
}
REX_EXTERN(__imp__sub_820E0B78);
REX_HOOK_RAW(sub_820E0B78) {
  const uint32_t context = ctx.r3.u32;
  __imp__sub_820E0B78(ctx, base);
  kk::MatrixPrepared(base, context, 404, ctx.r3.u32);
}
#endif
