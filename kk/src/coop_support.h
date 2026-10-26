#pragma once
#include "coop_objects.h"

namespace kk::coop::objects {
inline bool SupportRoot(ObjectKind kind) { return kind==ObjectKind::Ode || kind==ObjectKind::Tree; }
struct PartRoot {
  uint64_t source=0;
  std::vector<uint32_t> parts;
};
struct PartIdentity { uint64_t source=0,topology=0; };
// The physics RECORD is owned by the actor. Its ODE body is ephemeral: native
// scene init detaches it and later activation can create another one. Require
// the same geom/body relationship that the native transform helper consumes.
template<class Read> std::optional<uint32_t> BodyHandle(Read read,uint32_t physics) {
  if (!PoseAddress(physics,0x78)) return {};
  const auto geom=read(physics+0x70),body=read(physics+0x74);
  if (!PoseAddress(geom,32) || (body && !PoseAddress(body,0x170)) || read(geom+16)!=body) return {};
  return body;
}
// Canonicalize ALL aliases before touching physics. Membership, never heap
// ordering or a nearest-position guess, selects the one physical operation.
inline std::map<uint32_t,PartIdentity> CanonicalParts(std::vector<PartRoot> roots) {
  if (roots.empty() || roots.size()>kCensus) return {};
  std::sort(roots.begin(),roots.end(),[](const auto& a,const auto& b){return a.source<b.source;});
  std::map<uint32_t,std::vector<std::pair<uint64_t,unsigned>>> members;
  uint64_t previous=0;
  for (const auto& root:roots) {
    if (!root.source || root.source==previous || root.parts.empty() || root.parts.size()>36) return {};
    previous=root.source;
    for (unsigned i=0;i<root.parts.size();++i) {
      if (!root.parts[i]) return {};
      members[root.parts[i]].emplace_back(root.source,i);
    }
  }
  if (members.size()>kCensus) return {};
  std::map<uint32_t,PartIdentity> out;
  std::set<uint64_t> unique;
  for (const auto& [actor,aliases]:members) {
    uint64_t topology=enemies::Mix(14695981039346656037ull,0x50415254);
    for (const auto& [root,slot]:aliases) {
      topology=enemies::Mix(enemies::Mix(topology,root),slot);
    }
    const auto source=enemies::Mix(enemies::Mix(aliases.front().first,0x424f4459),aliases.front().second);
    if (!source || !topology || !unique.insert(source).second) return {};
    out.emplace(actor,PartIdentity{source,topology});
  }
  return out;
}
inline uint64_t RootTopology(const PartRoot& root,const std::map<uint32_t,PartIdentity>& parts) {
  uint64_t hash=enemies::Mix(root.source,root.parts.size());
  for (const auto actor:root.parts) {
    const auto it=parts.find(actor); if (it==parts.end()) return 0;
    hash=enemies::Mix(enemies::Mix(hash,it->second.source),it->second.topology);
  }
  return hash;
}
// Native IOB's event collector queries its root. Normalize a hit on one of its
// explicitly authored pieces to that root, refusing parts shared by obstacles.
template<class Map,class Read,class Valid> auto* StrikeTarget(Map& bindings,Read read,Valid valid,uint32_t target) {
  using T=typename Map::mapped_type;
  T* found=nullptr;
  if (!target) return found;
  for (auto& [actor,o]:bindings) {
    if (o.kind!=ObjectKind::Breakable || !valid(o)) continue;
    bool matches=actor==target;
    for (unsigned i=0;i<10 && !matches;++i) matches=read(o.vars+20+4*i)==target;
    if (matches) { if (found) return static_cast<T*>(nullptr); found=&o; }
  }
  return found;
}
} // namespace kk::coop::objects
