// C34: a bounded, dev-only test of the exact native variant installer. No load,
// activation, shared-group cloning, flags, guessed bone indices or H init.
#if defined(KK_DEV_TOOLS)
#include "dev_puppet_variant.h"
#include "hfr.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <rex/logging.h>

REX_EXTERN(__imp__sub_8286ADA0);
REX_EXTERN(__imp__sub_82868D50);
REX_EXTERN(__imp__sub_8287D7B8);
REX_EXTERN(__imp__sub_8286D4A8);
REX_EXTERN(__imp__sub_8286D690);

namespace kk {
namespace {
using hfr::Load32;
bool Range(uint32_t p, uint32_t bytes) {
  return p >= 0x40000000 && p < 0xA0000000 && !(p & 3) && bytes <= 0xA0000000 - p;
}
uint32_t Word(const uint8_t* b, uint32_t p, uint32_t off = 0) {
  return Range(p, off + 4) ? Load32(b, p + off) : 0;
}
uint32_t Ptr(const uint8_t* b, uint32_t p, uint32_t off, uint32_t bytes) {
  const auto v = Word(b, p, off); return Range(v, bytes) ? v : 0;
}
struct List {
  uint32_t group = 0, begin = 0, count = 0;
  bool valid = false;
};
List Group(const uint8_t* b, uint32_t group) {
  List l; l.group = group;
  const auto list = Ptr(b, group, 0, 20);
  l.begin = Word(b, list, 16);
  const auto end = Word(b, list, 4);
  if (!list || end < l.begin || (end - l.begin) % 8 || (end - l.begin) / 8 > 128) return l;
  l.count = (end - l.begin) / 8;
  l.valid = Range(l.begin, l.count * 8) && Word(b, list, 12) == l.count;
  return l;
}
uint32_t NodeAt(const uint8_t* b, const List& l, uint32_t i) {
  const auto n = l.valid && i < l.count ? Load32(b, l.begin + 8 * i) : 0;
  return Range(n, 36) ? n : 0;  // Tagged entries refuse, never mask into actors.
}
struct Root {
  uint32_t actor = 0, holder = 0, anim = 0, defaults = 0, tags = 0, palette = 0, bones = 0;
  List live, authored;
};
Root ReadRoot(const uint8_t* b, uint32_t actor) {
  Root r; r.actor = actor; r.holder = Ptr(b, actor, 20, 20); r.anim = Ptr(b, r.holder, 0, 180);
  r.defaults = Ptr(b, r.anim, 16, 4); r.tags = Ptr(b, r.anim, 84, 4);
  r.live = Group(b, Word(b, r.anim, 8)); r.authored = Group(b, Word(b, r.anim, 4));
  const auto skin = Ptr(b, r.holder, 16, 8);
  r.bones = Word(b, skin); r.palette = Ptr(b, skin, 4, r.bones <= 256 ? r.bones * 80 : ~0u);
  return r;
}
uint32_t Variant(const uint8_t* b, const Root& r, uint32_t slot) {
  if (!r.tags || slot >= 48) return 255;  // Inline bytes are anim+20..67.
  if (r.tags == r.defaults) return b[r.anim + 20 + slot];
  return Range(r.tags, 68 + slot + 1) ? b[r.tags + 68 + slot] : 255;
}
struct Node {
  uint32_t actor = 0, holder = 0, ext = 0, mesh = 0, geometry = 0, material = 0;
  uint32_t skin = 0, count = 0, data = 0, variants = 0, flags = 0;
  bool links = false;
};
Node ReadNode(const uint8_t* b, uint32_t actor) {
  Node n; n.actor = actor;
  if (!Range(actor, 36)) return n;
  n.flags = Word(b, actor); n.holder = Ptr(b, actor, 20, 20); n.ext = Ptr(b, actor, 24, 48);
  n.mesh = (n.flags & 0x4000) ? Ptr(b, n.holder, 0, 68) : 0;
  n.geometry = Word(b, n.mesh); n.material = Word(b, n.mesh, 4);
  n.variants = (n.flags & 0x800000) ? Word(b, n.ext, 12) : 0;
  if (n.flags & 0x200000) {
    n.skin = Ptr(b, n.holder, 16, 8); n.count = Word(b, n.skin);
    n.links = (n.flags & 0x1000000) != 0;
    n.data = n.count <= 256 ? Ptr(b, n.skin, 4, n.count * (n.links ? 12 : 80)) : 0;
  }
  return n;
}
uint32_t Template(const uint8_t* b, const Node& n, uint32_t variant) {
  return variant < 253 ? NodeAt(b, Group(b, n.variants), variant) : 0;
}
bool Skinned(const uint8_t* b, const Node& n, const Root& root) {
  if (!n.mesh || !n.geometry || !n.material || !n.links || !n.count || !n.data || !root.palette || !root.bones) return false;
  for (uint32_t i = 0; i < n.count; ++i) {
    const auto p = n.data + i * 12, bone = Word(b, p, 8);
    if (bone >= root.bones || Word(b, p, 4) != root.actor || Word(b, p) != root.palette + 80 * bone) return false;
  }
  return true;
}
struct Trial {
  bool armed = false, creating = false;
  bool reset_witnessed = false;
  uint32_t source = 0, native_source = 0, copy = 0, lines = 0;
  uint64_t epoch = 0;
};
Trial trial;
bool On() {
  static const bool on = [] { const auto* v = std::getenv("KK_DEV_PUPPET_SPAWN_VARIANT"); return v && std::string(v) == "1"; }();
  return on;
}
bool Tracked(uint32_t actor) {
  return trial.armed && actor && (actor == trial.source || actor == trial.native_source || actor == trial.copy);
}
bool LogLine() {
  if (trial.lines >= 8192) return false;
  if (++trial.lines == 8192) REXLOG_INFO("PUPPET variant diagnostic-cap lines=8192 further-state-logs-suppressed=true");
  return true;
}
void LogSlot(const uint8_t* b, const Root& r, uint32_t slot, const char* stage, uint32_t requested = 255, uint32_t mode = 0) {
  if (!LogLine()) return;
  const auto variant = Variant(b, r, slot);
  const auto n = ReadNode(b, NodeAt(b, r.live, slot));
  const auto selected = Template(b, n, variant);
  const auto templ = ReadNode(b, selected);
  const uint32_t tag = Range(r.tags, 5 + slot) ? b[r.tags + 4 + slot] : 255;
  std::string links;
  if (n.links && n.data) for (uint32_t i = 0; i < n.count; ++i)
    links += fmt::format(" {}:{:08X}/{:08X}/{}", i, Word(b, n.data + 12 * i), Word(b, n.data + 12 * i, 4), Word(b, n.data + 12 * i, 8));
  REXLOG_INFO("PUPPET variant stage={} epoch={} role={} root={:08X} anim={:08X} defaults16={:08X} tags84={:08X} live={:08X} slot={} tag={} variant={} requested={} lookup-mode={} node={:08X} template={:08X} template-skin-count={} template-skin-data={:08X} variants={:08X} mesh={:08X} geometry={:08X} material={:08X} flags={:08X}/{:08X} skin={:08X} count={} data={:08X} palette={:08X} bones={} valid-root-links={} links=matrix/owner/bone{}",
      stage, trial.epoch, r.actor == trial.copy ? "copy" : r.actor == trial.source ? "source" : "native-source",
      r.actor, r.anim, r.defaults, r.tags, r.live.group, slot, tag, variant, requested, mode, n.actor, selected, templ.count, templ.data,
      n.variants, n.mesh, n.geometry, n.material, n.flags, Word(b, n.actor, 4), n.skin, n.count, n.data,
      r.palette, r.bones, Skinned(b, n, r), links);
}
void LogRoot(const uint8_t* b, uint32_t actor, const char* stage) {
  if (!Tracked(actor)) return;
  const auto r = ReadRoot(b, actor);
  if (!r.live.valid && LogLine()) REXLOG_INFO("PUPPET variant stage={} root={:08X} live-invalid=true", stage, actor);
  if (r.live.valid) for (uint32_t i = 0; i < r.live.count; ++i) LogSlot(b, r, i, stage);
}
void LogPair(const uint8_t* b, const char* stage) {
  LogRoot(b, trial.source, stage); LogRoot(b, trial.copy, stage);
  if (trial.native_source != trial.source) LogRoot(b, trial.native_source, stage);
}
uint64_t HashBytes(const uint8_t* b, uint64_t hash, uint32_t p, uint32_t bytes) {
  if (!Range(p, bytes)) return hash ^ ~uint64_t{0};
  for (uint32_t i = 0; i < bytes; ++i) { hash ^= b[p + i]; hash *= 1099511628211ull; }
  return hash;
}
uint64_t NodeHash(const uint8_t* b, const Node& n) {
  auto hash = HashBytes(b, 14695981039346656037ull, n.actor, 36);
  hash = HashBytes(b, hash, n.holder, 20); hash = HashBytes(b, hash, n.ext, 48);
  if (n.mesh) hash = HashBytes(b, hash, n.mesh, 68);
  if (n.data && n.count <= 256) hash = HashBytes(b, hash, n.data, n.count * (n.links ? 12 : 80));
  return hash;
}
uint64_t RootHash(const uint8_t* b, const Root& r) {
  auto hash = HashBytes(b, NodeHash(b, ReadNode(b, r.actor)), r.anim, 180);
  if (r.live.valid) for (uint32_t i = 0; i < r.live.count; ++i) hash ^= NodeHash(b, ReadNode(b, NodeAt(b, r.live, i))) + i;
  if (r.palette && r.bones <= 256) hash = HashBytes(b, hash, r.palette, r.bones * 80);
  return hash;
}

// Audit every resident root and reachable authored/variant group. A visited
// group is scanned once, while incoming group/node references are counted.
// Any bound or unreadable non-null layout refuses mutation.
struct Ownership {
  bool complete = true;
  struct Span { uint32_t begin, bytes, owner; };
  std::vector<Span> spans;
  std::map<uint32_t, unsigned> members, groups;
  std::map<uint32_t, std::set<uint32_t>> mutable_owners;
  std::set<uint32_t> nodes, visited_groups;
};
Ownership Audit(const uint8_t* b, uint32_t world) {
  Ownership a;
  const auto begin = Word(b, world, 40), end = Word(b, world, 28);
  if (end < begin || end - begin > 8 * 20000 || (end - begin) % 8 || !Range(begin, end - begin)) { a.complete = false; return a; }
  std::vector<uint32_t> pending_nodes, pending_groups;
  for (uint32_t p = begin; p < end; p += 8) pending_nodes.push_back(Load32(b, p));
  auto group = [&](uint32_t p) { if (p) { ++a.groups[p]; pending_groups.push_back(p); } };
  auto mutable_pointer = [&](uint32_t p, uint32_t owner, uint32_t bytes = 4) {
    if (!p) return;
    if (!Range(p, bytes)) { a.complete = false; return; }
    a.mutable_owners[p].insert(owner); a.spans.push_back({p, bytes, owner});
  };
  size_t ni = 0, gi = 0, entries = 0;
  while (ni < pending_nodes.size() || gi < pending_groups.size()) {
    if (pending_nodes.size() + pending_groups.size() > 200000 || entries > 200000) { a.complete = false; break; }
    if (ni < pending_nodes.size()) {
      const auto actor = pending_nodes[ni++];
      if (!actor || (actor & 3)) { if (actor && (actor & 3) != 1) a.complete = false; continue; }
      if (!a.nodes.insert(actor).second) continue;
      if (!Range(actor, 36)) { a.complete = false; continue; }
      const auto n = ReadNode(b, actor);
      mutable_pointer(actor, actor, 36);
      mutable_pointer(n.holder, actor, 20); mutable_pointer(n.ext, actor, 48);
      mutable_pointer(n.mesh, actor, 68); mutable_pointer(n.skin, actor, 8);
      if (n.count > 256) a.complete = false;
      else mutable_pointer(n.data, actor, n.count * (n.links ? 12 : 80));
      mutable_pointer(Word(b, actor, 12), actor, 80); mutable_pointer(Word(b, actor, 16), actor, 48);
      mutable_pointer(Word(b, n.ext, 28), actor);
      const auto aux = Word(b, n.mesh, 12), array = Word(b, n.mesh, 32), constants = Word(b, n.mesh, 48);
      const auto aux_count = aux ? Word(b, aux) : 0, array_count = Word(b, n.mesh, 28), constant_count = Word(b, n.mesh, 36);
      if (aux_count > 4096 || array_count > 4096 || constant_count > 4096) a.complete = false;
      else {
        mutable_pointer(aux, actor, (aux_count + 1) * 4);
        mutable_pointer(array, actor, array_count * 36);
        mutable_pointer(constants, actor, constant_count * 64);  // 8286E840, not the 48-byte executor constants.
      }
      if (Word(b, actor, 20) && !n.holder) a.complete = false;
      if (Word(b, actor, 24) && !n.ext) a.complete = false;
      group(n.variants);
      if (n.flags & 2) {
        const auto root = ReadRoot(b, actor);
        if (!root.anim) { a.complete = false; continue; }
        mutable_pointer(root.anim, actor, 180);
        if (root.bones > 256) a.complete = false;
        else mutable_pointer(root.palette, actor, root.bones * 80);
        group(root.live.group); group(root.authored.group);
      }
    } else {
      const auto g = pending_groups[gi++];
      if (!a.visited_groups.insert(g).second) continue;
      const auto l = Group(b, g);
      if (!l.valid) { a.complete = false; continue; }
      a.spans.push_back({g, 12, 0});
      a.spans.push_back({Word(b, g), 20, 0});
      a.spans.push_back({l.begin, l.count * 8, 0});  // Immutable group/list storage may never be a destination.
      for (uint32_t i = 0; i < l.count; ++i) {
        const auto actor = Load32(b, l.begin + 8 * i);
        ++a.members[actor]; ++entries; pending_nodes.push_back(actor);
      }
    }
  }
  return a;
}
bool Private(const Ownership& a, uint32_t pointer, uint32_t owner, uint32_t bytes = 4) {
  if (!pointer) return true;
  const auto it = a.mutable_owners.find(pointer);
  if (it == a.mutable_owners.end() || it->second.size() != 1 || !it->second.count(owner)) return false;
  for (const auto& span : a.spans) if (span.owner != owner && pointer < uint64_t(span.begin) + span.bytes &&
      span.begin < uint64_t(pointer) + bytes) return false;
  return true;
}
const char* CanInstall(const uint8_t* b, const Root& src, const Root& dst, uint32_t slot, const Ownership& a) {
  if (!a.complete) return "ownership-audit-incomplete-or-capped";
  const auto root_node = ReadNode(b, dst.actor);
  if (!src.anim || !dst.anim || src.anim == dst.anim || !src.live.valid || !dst.live.valid ||
      src.live.count != dst.live.count || src.authored.group != dst.authored.group || !src.authored.valid ||
      src.live.group == dst.live.group || !dst.palette || !dst.bones || dst.palette == src.palette ||
      !Private(a, dst.anim, dst.actor, 180) || !Private(a, dst.palette, dst.actor, dst.bones * 80) ||
      !Private(a, dst.holder, dst.actor, 20) || !Private(a, root_node.ext, dst.actor, 48) ||
      !Private(a, root_node.skin, dst.actor, 8) || !Private(a, Word(b, dst.actor, 12), dst.actor, 80)) return "root-lineage-or-palette-not-private";
  const auto group = a.groups.find(dst.live.group);
  if (group == a.groups.end() || group->second != 1) return "shared-live-group";
  const auto s = ReadNode(b, NodeAt(b, src.live, slot)), d = ReadNode(b, NodeAt(b, dst.live, slot));
  const auto member = a.members.find(d.actor);
  if (!s.actor || !d.actor || s.actor == d.actor || member == a.members.end() || member->second != 1 ||
      d.actor == dst.actor || d.actor == src.actor || !d.holder || !d.ext || !(d.flags & 0x800000)) return "copy-slot-not-unique-live-node";
  if (!Ptr(b, d.actor, 12, 80) || !Ptr(b, d.actor, 16, 48) ||
      ((d.flags & 0x4000) && !d.mesh) || ((d.flags & 0x200000) && (!d.skin || (d.count && !d.data))) ||
      (!(d.flags & 0x200000) && Word(b, d.holder, 16)) ||
      (Word(b, d.skin, 4) && !d.data) || (d.geometry && !Range(d.geometry, 32)) ||
      (d.material && !Range(d.material, 32))) return "copy-slot-component-layout-invalid";
  for (const auto p : {d.holder, d.ext, d.mesh, d.skin, d.data, Word(b, d.actor, 12), Word(b, d.ext, 28)})
    if (!Private(a, p, d.actor)) return "copy-slot-shares-mutable-state";
  if (!Private(a, d.holder, d.actor, 20) || !Private(a, d.ext, d.actor, 48) ||
      !Private(a, d.mesh, d.actor, 68) || !Private(a, d.skin, d.actor, 8) ||
      !Private(a, d.data, d.actor, d.count * (d.links ? 12 : 80)) ||
      !Private(a, Word(b, d.actor, 12), d.actor, 80) || !Private(a, Word(b, d.actor, 16), d.actor, 48))
    return "copy-slot-overlaps-other-mutable-state";
  // 82815948 releases mesh+12; 8286E840 frees +48 and can walk +32 and
  // render queues at +40/+44. Do not touch an unaudited queued/array layout.
  if (d.mesh && (Word(b, d.mesh, 28) || Word(b, d.mesh, 32) || Word(b, d.mesh, 40) || Word(b, d.mesh, 44)))
    return "copy-mesh-queues-or-arrays-not-audited";
  for (auto p : {Word(b, d.actor, 16), Word(b, d.mesh, 12), Word(b, d.mesh, 48)})
    if (!Private(a, p, d.actor)) return "copy-mesh-buffer-or-bounds-shared";
  const auto aux = Word(b, d.mesh, 12), constants = Word(b, d.mesh, 48);
  if (constants && !Word(b, d.mesh, 36)) return "copy-mesh-buffer-allocation-unknown";
  if (!Private(a, aux, d.actor, (aux ? Word(b, aux) + 1 : 0) * 4) ||
      !Private(a, constants, d.actor, Word(b, d.mesh, 36) * 64)) return "copy-mesh-buffer-overlaps-other-state";
  if (Word(b, d.ext, 28)) return "copy-old-modifier-destructor-ownership-not-audited";
  const auto variant = Variant(b, src, slot);
  const auto t = ReadNode(b, Template(b, s, variant));
  if (variant >= 253 || !t.actor || Template(b, d, variant) != t.actor || s.variants != d.variants ||
      !Skinned(b, s, src) || !t.mesh || !t.geometry || !t.material || !t.links || !t.count || !t.data ||
      !Range(t.geometry, 32) || !Range(t.material, 32) ||
      s.geometry != t.geometry || s.material != t.material || s.count != t.count) return "source-skinned-variant-not-proven";
  for (uint32_t i = 0; i < t.count; ++i) {
    const auto bone = Word(b, t.data + 12 * i, 8);
    if (bone >= src.bones || bone >= dst.bones || bone != Word(b, s.data + 12 * i, 8)) return "template-bone-map-invalid";
  }
  if (Skinned(b, d, dst) && s.geometry == d.geometry && s.material == d.material &&
      s.count == d.count && Variant(b, dst, slot) == variant) return "copy-already-has-skinned-variant";
  return nullptr;
}
}  // namespace

void DevPuppetVariantReset() { trial = {}; }
void DevPuppetVariantBegin(const uint8_t* b, uint32_t source, uint32_t native_source, uint64_t epoch) {
  if (!On()) return;
  trial = {}; trial.armed = trial.creating = true; trial.source = source; trial.native_source = native_source; trial.epoch = epoch;
  LogPair(b, "before-native-copy");
}
void DevPuppetVariantEnd(PPCContext& ctx, uint8_t* b, uint32_t copy, uint32_t world) {
  if (!trial.armed || !trial.creating) return;
  trial.creating = false;
  if (!Range(copy, 36) || copy == trial.source || copy == trial.native_source || (trial.copy && trial.copy != copy)) {
    REXLOG_INFO("PUPPET variant refused reason=copy-identity-not-proven returned={:08X} observed={:08X}", copy, trial.copy);
    trial.armed = false; return;
  }
  // Require the exact mode-13 reset witness inside 82715AC8, also for pool reuse.
  if (!trial.copy || !trial.reset_witnessed) { REXLOG_INFO("PUPPET variant refused reason=no-copy-reset-witness"); trial.armed = false; return; }
  LogPair(b, "after-native-copy-and-mode13");
  const auto src = ReadRoot(b, trial.source), dst = ReadRoot(b, copy);
  const auto saved = ctx;
  uint32_t calls = 0;
  auto ownership = Audit(b, world);
  REXLOG_INFO("PUPPET variant ownership complete={} nodes={} groups={} spans={} registry-bound=20000 traversal-bound=200000 group-bound=128 mutation-requires-exact-and-overlap-exclusivity=true",
      ownership.complete, ownership.nodes.size(), ownership.visited_groups.size(), ownership.spans.size());
  if (dst.live.valid) for (uint32_t slot = 0; slot < dst.live.count; ++slot) {
    if (const auto* reason = CanInstall(b, src, dst, slot, ownership)) {
      REXLOG_INFO("PUPPET variant trial root={:08X} slot={} called=false reason={}", copy, slot, reason); continue;
    }
    const auto variant = Variant(b, src, slot);
    const auto templ = ReadNode(b, Template(b, ReadNode(b, NodeAt(b, src.live, slot)), variant));
    const auto source_hash = RootHash(b, src), template_hash = NodeHash(b, templ);
    const auto current_ai = Load32(b, 0x82CCBF64), current_actor = Load32(b, 0x82CCBF6C);
    LogPair(b, "before-trial-install");
    ctx = saved; ctx.r3.u64 = copy; ctx.r4.u64 = slot; ctx.r5.u64 = variant; ctx.r6.u64 = 0;
    __imp__sub_8286ADA0(ctx, b); ++calls;
    hfr::Store32(b, 0x82CCBF64, current_ai); hfr::Store32(b, 0x82CCBF6C, current_actor);
    ctx = saved;
    LogPair(b, "after-trial-install");
    const bool unchanged = source_hash == RootHash(b, ReadRoot(b, src.actor)) && template_hash == NodeHash(b, ReadNode(b, templ.actor));
    const auto after = ReadNode(b, NodeAt(b, ReadRoot(b, copy).live, slot));
    REXLOG_INFO("PUPPET variant trial tuple=8286ADA0({:08X},{},{},0) source-and-template-unchanged={} copy-skinned={} geometry={:08X} material={:08X} result-is-not-body-draw-proof=true",
        copy, slot, variant, unchanged, Skinned(b, after, ReadRoot(b, copy)), after.geometry, after.material);
    if (!unchanged) { REXLOG_INFO("PUPPET variant stop reason=source-or-template-readback-changed"); trial.armed = false; break; }
    ownership = Audit(b, world);  // Installer may replace private skin/mesh storage.
  }
  ctx = saved;
  REXLOG_INFO("PUPPET variant trial complete root={:08X} calls={} source-never-called=true shared-groups-never-written=true visibility=requires-Claude-capture", copy, calls);
}

// Hooks observe originals normally, including source resets. Installation from
// End uses __imp__, so trial calls cannot recursively invoke these observers.
void VariantCloneBefore(const uint8_t* b, uint32_t copy, uint32_t source) {
  if (!trial.creating || source != trial.native_source || copy == source || copy == trial.source) return;
  trial.copy = copy; LogPair(b, "8286D690-before");
}
void VariantResetBefore(const uint8_t* b, uint32_t actor, uint32_t mode, uint32_t lr) {
  if (trial.creating && mode == 13 && lr == 0x82715C60 && actor != trial.source && actor != trial.native_source &&
      (!trial.copy || trial.copy == actor)) { trial.copy = actor; trial.reset_witnessed = true; }
  if (Tracked(actor)) LogPair(b, mode == 13 ? "8287D7B8-mode13-before" : "8287D7B8-before");
}
void VariantStage(const uint8_t* b, uint32_t root, const char* stage) { if (Tracked(root)) LogPair(b, stage); }
void VariantInstall(const uint8_t* b, uint32_t root, uint32_t slot, uint32_t variant, uint32_t mode, const char* stage) {
  if (!Tracked(root)) return;
  const auto r = ReadRoot(b, root);
  if (!mode && slot < r.live.count) LogSlot(b, r, slot, stage, variant, mode);
  else LogRoot(b, root, stage);  // Tag lookup is observed without guessing its physical slot.
}
}  // namespace kk

REX_HOOK_RAW(sub_8286D690) {
  const auto copy = ctx.r4.u32, source = ctx.r5.u32;
  kk::VariantCloneBefore(base, copy, source);
  __imp__sub_8286D690(ctx, base);
  kk::VariantStage(base, copy, "8286D690-after");
}
REX_HOOK_RAW(sub_8286ADA0) {
  const auto root = ctx.r3.u32, slot = ctx.r4.u32, variant = ctx.r5.u32, mode = ctx.r6.u32;
  kk::VariantInstall(base, root, slot, variant, mode, "8286ADA0-before");
  __imp__sub_8286ADA0(ctx, base);
  kk::VariantInstall(base, root, slot, variant, mode, "8286ADA0-after");
}
REX_HOOK_RAW(sub_82868D50) {
  const auto root = ctx.r3.u32;
  kk::VariantStage(base, root, "82868D50-before");
  __imp__sub_82868D50(ctx, base);
  kk::VariantStage(base, root, "82868D50-after");
}
REX_HOOK_RAW(sub_8287D7B8) {
  const auto root = ctx.r3.u32, mode = ctx.r4.u32;
  kk::VariantResetBefore(base, root, mode, uint32_t(ctx.lr));
  __imp__sub_8287D7B8(ctx, base);
  kk::VariantStage(base, root, "8287D7B8-after");
}
REX_HOOK_RAW(sub_8286D4A8) {
  const auto root = ctx.r3.u32;
  kk::VariantStage(base, root, "8286D4A8-before");
  __imp__sub_8286D4A8(ctx, base);
  kk::VariantStage(base, root, "8286D4A8-after");
}
#endif
