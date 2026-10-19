// Included only by KK_DEV_TOOLS plugin translation units. Nothing changes NrApi.
#pragma once
#include <map>
#include <mutex>
#include <utility>
#include "backend/draw_sink.h"
#include "plugin/native_puppet_probe.h"

namespace nr::puppet_probe {
inline thread_local NrPuppetMeshScope current;
inline std::mutex mutex;
inline std::map<std::pair<uint32_t, uint32_t>, NrPuppetMeshDraws> draws;
inline NrPuppetMeshScope Scope(NrPuppetMeshScope next) {
  const auto previous = current;
  current = next;
  return previous;
}
inline void Seen(const DrawCall& call) {
  if (!current.owner) return;
  std::lock_guard lock(mutex);
  const auto add = [&](uint32_t mesh) {
    const auto key = std::make_pair(current.owner, mesh);
    if (draws.size() >= 512 && !draws.contains(key)) return;
    auto& d = draws[key];
    ++d.seen; d.last_frame = call.frame;
    d.vertex_buffer = call.vertex_buffer; d.index_buffer = call.index_buffer;
    d.vertex_shader = call.vertex_shader; d.start = call.start; d.count = call.count;
  };
  add(0);  // Whole source/copy, including every recognized child scope.
  if (current.mesh) add(current.mesh);
}
inline uint32_t Read(uint32_t owner, uint32_t mesh, NrPuppetMeshDraws* out) {
  if (!out) return 0;
  std::lock_guard lock(mutex);
  const auto it = draws.find({owner, mesh});
  *out = it == draws.end() ? NrPuppetMeshDraws{} : it->second;
  return 1;  // A connected probe with zero draws is distinct from an absent export.
}
inline void Reset() {
  std::lock_guard lock(mutex);
  draws.clear();
}
inline const NrPuppetProbeApi api{sizeof(NrPuppetProbeApi), kNrPuppetProbeVersion, Scope, Read, Reset};
}  // namespace nr::puppet_probe
