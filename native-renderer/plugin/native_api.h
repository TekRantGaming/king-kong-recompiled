// The C interface between the D3D hooks in king_kong.exe and the native
// renderer plugin (rexgpu-native.dll / librexgpu-native.so). The hooks run on
// the game's threads and pass guest addresses; the plugin reads guest memory
// itself (at call time: constants and viewports may live on the guest stack).
//
// Exported by the plugin: const NrApi* nr_get_api(void). The pointer is valid
// for the life of the process. Every function takes `self` first.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_API_VERSION 2

typedef struct NrApi {
  uint32_t version;  // NR_API_VERSION
  uint32_t size;     // sizeof(NrApi)
  void* self;

  // 1 once the plugin's guest GPU is set up (SetupGuestGpu ran). Hooks fall
  // back to the game's own library while this is 0.
  int (*is_active)(void* self);

  // State setters (see docs/d3d-api-map.md for the game functions).
  void (*set_render_target)(void* self, uint32_t index, uint32_t surface);
  void (*set_depth_stencil_surface)(void* self, uint32_t surface);
  void (*set_viewport)(void* self, uint32_t viewport_guest);  // 6 dwords: X Y W H MinZ MaxZ
  void (*set_texture)(void* self, uint32_t sampler, uint32_t texture);
  void (*set_indices)(void* self, uint32_t index_buffer);
  void (*set_stream_source)(void* self, uint32_t stream, uint32_t vertex_buffer, uint32_t offset,
                            uint32_t stride);
  void (*set_pixel_shader)(void* self, uint32_t pixel_shader);
  void (*set_vertex_shader)(void* self, uint32_t vertex_shader);
  void (*set_vertex_declaration)(void* self, uint32_t declaration);
  void (*set_vs_constants_f)(void* self, uint32_t reg, uint32_t data_guest, uint32_t count);
  void (*set_ps_constants_f)(void* self, uint32_t reg, uint32_t data_guest, uint32_t count);
  void (*set_vs_constants_i)(void* self, uint32_t reg, uint32_t data_guest, uint32_t count);
  void (*set_blend_control)(void* self, uint32_t render_target, uint32_t value);
  // The device's render-state / sampler-state setter table: state is the
  // D3DRENDERSTATETYPE / D3DSAMPLERSTATETYPE value (a byte offset).
  void (*set_render_state)(void* self, uint32_t state, uint32_t value);
  void (*set_sampler_state)(void* self, uint32_t sampler, uint32_t type, uint32_t value);
  void (*begin_conditional_rendering)(void* self, uint32_t id);
  void (*end_conditional_rendering)(void* self);

  // Draw and frame.
  void (*clear)(void* self, uint32_t flags, uint32_t color_argb, float z, uint32_t stencil);
  void (*resolve)(void* self, uint32_t flags, uint32_t dest_texture);
  void (*draw_indexed)(void* self, uint32_t prim_type, int32_t base_vertex, uint32_t start_index,
                       uint32_t index_count);
  void (*draw)(void* self, uint32_t prim_type, uint32_t start_vertex, uint32_t vertex_count);
  // The game's Present: ends the frame being recorded. The picture is shown
  // when the game's VdSwap reaches the ring skimmer.
  void (*present)(void* self);
} NrApi;

#ifdef __cplusplus
}
#endif
