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

// 3: the device pointer on draws, clears, resolves and presents; Clear's
// rectangles; Resolve's full argument list; shader creation (Phase 2).
#define NR_API_VERSION 3

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

  // Draw and frame. `device` is the guest D3D device (r3): the renderer reads
  // the register images and constant shadows from it.
  // Clear(dev, Count, pRects, Flags, Color, Z, Stencil); rects are D3DRECTs.
  void (*clear)(void* self, uint32_t device, uint32_t rect_count, uint32_t rects_guest,
                uint32_t flags, uint32_t color_argb, float z, uint32_t stencil);
  // Resolve(dev, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel,
  // DestSliceOrFace, pClearColor, ClearZ); ClearStencil is on the stack and
  // taken as 0.
  void (*resolve)(void* self, uint32_t device, uint32_t flags, uint32_t source_rect_guest,
                  uint32_t dest_texture, uint32_t dest_point_guest, uint32_t dest_level,
                  uint32_t dest_slice, uint32_t clear_color_guest, float clear_z);
  void (*draw_indexed)(void* self, uint32_t device, uint32_t prim_type, int32_t base_vertex,
                       uint32_t start_index, uint32_t index_count);
  void (*draw)(void* self, uint32_t device, uint32_t prim_type, uint32_t start_vertex,
               uint32_t vertex_count);
  // The game's Present: ends the frame being recorded. The picture is shown
  // when the game's VdSwap reaches the ring skimmer.
  void (*present)(void* self, uint32_t device);

  // Resource creation, after the library's own function returned: kind 0
  // vertex shader, 1 pixel shader; container is the XDK shader container the
  // engine passed (CreateVertexShader / CreatePixelShader's pFunction),
  // object the shader the library returned (0 on failure).
  void (*shader_created)(void* self, uint32_t kind, uint32_t container, uint32_t object);
} NrApi;

#ifdef __cplusplus
}
#endif
