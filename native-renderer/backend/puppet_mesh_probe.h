// Included only by KK_DEV_TOOLS plugin translation units. Nothing changes NrApi.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include "backend/draw_sink.h"
#include "plugin/native_puppet_probe.h"

namespace nr::puppet_probe {
inline thread_local NrPuppetMeshScope current;
inline std::mutex mutex;
inline std::map<std::pair<uint32_t, uint32_t>, NrPuppetMeshDraws> draws;
inline constexpr std::size_t kEventCapacity = 16384;
inline std::deque<NrPuppetDrawEvent> events;
inline uint32_t source = 0, copy = 0, frames_left = 0;
inline uint32_t source_draws = 0, copy_draws = 0, unattributed = 0, dropped = 0;
inline std::atomic<bool> capturing{false};
inline NrPuppetMeshScope Scope(NrPuppetMeshScope next) {
  const auto previous = current;
  current = next;
  return previous;
}
inline void Seen(const DrawCall& call) {
  if (!current.owner && !capturing.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(mutex);
  if (frames_left) {
    if (current.owner == source || current.owner == copy) {
      if (current.owner == source) ++source_draws; else ++copy_draws;
      // Reserve space for all 120 frame summaries, even if the consumer stalls.
      if (events.size() < kEventCapacity - 120) {
        NrPuppetDrawEvent e;
        e.frame = call.frame; e.ordinal = call.index_in_frame;
        e.owner = current.owner; e.mesh = current.mesh;
        e.vertex_buffer = call.vertex_buffer; e.vertex_physical = call.vertex_info.physical;
        e.index_buffer = call.index_buffer; e.index_physical = call.index_info.physical;
        e.vertex_shader = call.vertex_shader; e.pixel_shader = call.pixel_shader;
        e.declaration = call.vertex_declaration; e.stride = call.stride;
        e.start = call.start; e.count = call.count; e.indexed = call.indexed;
        e.primitive = uint32_t(call.primitive); e.base_vertex = call.base_vertex;
        e.conditional = call.conditional_id; e.target = call.render_targets[0];
        e.depth = call.depth_stencil;
        events.push_back(e);
      } else ++dropped;
    } else ++unattributed;
  }
  if (!current.owner) return;
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
inline void Presented(uint32_t frame) {
  if (!capturing.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(mutex);
  if (!frames_left) return;
  NrPuppetDrawEvent e;
  e.kind = 1; e.frame = frame;
  e.source_draws = source_draws; e.copy_draws = copy_draws;
  e.unattributed = unattributed; e.dropped = dropped;
  events.push_back(e);  // Draw records reserve space for every summary above.
  if (!--frames_left) capturing.store(false, std::memory_order_relaxed);
  source_draws = copy_draws = unattributed = dropped = 0;
}
inline void Capture(uint32_t source_actor, uint32_t copy_actor, uint32_t frames) {
  std::lock_guard lock(mutex);
  events.clear(); source = source_actor; copy = copy_actor;
  frames_left = source && copy && source != copy ? std::min(frames, 120u) : 0;
  capturing.store(frames_left != 0, std::memory_order_relaxed);
  source_draws = copy_draws = unattributed = dropped = 0;
}
inline uint32_t Drain(NrPuppetDrawEvent* out, uint32_t capacity) {
  if (!out) return 0;
  std::lock_guard lock(mutex);
  uint32_t count = 0;
  while (count < capacity && !events.empty()) {
    out[count++] = events.front(); events.pop_front();
  }
  return count;
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
  events.clear(); source = copy = frames_left = 0;
  capturing.store(false, std::memory_order_relaxed);
  source_draws = copy_draws = unattributed = dropped = 0;
}
inline const NrPuppetProbeApi api{sizeof(NrPuppetProbeApi), kNrPuppetProbeVersion, Scope, Read, Reset, Capture, Drain};
}  // namespace nr::puppet_probe
