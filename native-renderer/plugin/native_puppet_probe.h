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
struct NrPuppetProbeApi {
  uint32_t size, version;
  NrPuppetMeshScope (*scope)(NrPuppetMeshScope next);
  uint32_t (*read)(uint32_t owner, uint32_t mesh, NrPuppetMeshDraws* out);
  void (*reset)();
};
inline constexpr uint32_t kNrPuppetProbeVersion = 1;
