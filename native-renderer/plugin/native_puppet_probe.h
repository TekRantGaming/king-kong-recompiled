// C11, developer builds only. Optional diagnostic export, separate from NrApi.
// Counts decoded draws received by the backend, not GPU submissions or pixels.
#pragma once
#include <stdint.h>

struct NrPuppetMeshScope { uint32_t owner = 0, mesh = 0; };
struct NrPuppetMeshDraws {
  uint64_t seen = 0, last_frame = 0;
  uint32_t vertex_buffer = 0, index_buffer = 0, vertex_shader = 0;
  uint32_t start = 0, count = 0;
};
// C14 bounded capture, copied values only. kind=0 draw, kind=1 completed frame.
// Frame records include zeros for either actor and count unattributed draws.
struct NrPuppetDrawEvent {
  uint32_t kind = 0, frame = 0, ordinal = 0, owner = 0, mesh = 0;
  uint32_t vertex_buffer = 0, vertex_physical = 0, index_buffer = 0, index_physical = 0;
  uint32_t vertex_shader = 0, pixel_shader = 0, declaration = 0, stride = 0;
  uint32_t start = 0, count = 0, indexed = 0, primitive = 0;
  int32_t base_vertex = 0, conditional = -1;
  uint32_t target = 0, depth = 0;
  uint32_t source_draws = 0, copy_draws = 0, unattributed = 0, dropped = 0;
};
struct NrPuppetProbeApi {
  uint32_t size, version;
  NrPuppetMeshScope (*scope)(NrPuppetMeshScope next);
  uint32_t (*read)(uint32_t owner, uint32_t mesh, NrPuppetMeshDraws* out);
  void (*reset)();
  void (*capture)(uint32_t source, uint32_t copy, uint32_t frames);
  uint32_t (*drain)(NrPuppetDrawEvent* out, uint32_t capacity);
};
inline constexpr uint32_t kNrPuppetProbeVersion = 2;
