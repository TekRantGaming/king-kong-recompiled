// What the draw-state tracker hands to a renderer: one self-contained record
// per clear, draw, resolve and frame end. Pointers into guest memory are valid
// only during the call (the renderer copies what it needs).
#pragma once

#include <array>
#include <cstdint>

#include "backend/guest_layout.h"

namespace nr {

// Render states the tracker keeps, as the setters receive them. The 360's
// values are the Xenos ones, not D3D9's (d3d-api-map.md): compare functions
// 0 never .. 3 less-equal, 4 greater, 6 greater-equal, 7 always; cull 0 none,
// 2 CW, 6 CCW. Defaults are the D3D defaults in that numbering.
struct RenderStates {
  uint32_t z_enable = 1;
  uint32_t z_func = 3;  // less-equal
  uint32_t z_write_enable = 1;
  uint32_t cull_mode = 0;
  uint32_t alpha_test_enable = 0;
  uint32_t alpha_ref = 0;
  uint32_t alpha_func = 7;  // always
  uint32_t stencil_enable = 0;
  uint32_t stencil_ref = 0;
  uint32_t color_write_enable = 0xF;
  bool operator==(const RenderStates&) const = default;
};

struct ClearCall {
  uint32_t frame = 0;
  uint32_t flags = 0;
  bool color = false, depth = false, stencil = false;
  float rgba[4] = {};  // from the D3DCOLOR (ARGB)
  float z = 1.0f;
  uint32_t stencil_value = 0;
  uint32_t render_target0 = 0;
  uint32_t depth_stencil = 0;
  Viewport viewport;
};

struct DrawCall {
  uint32_t frame = 0;
  uint32_t index_in_frame = 0;  // draws (indexed and not) since the frame began
  bool indexed = false;
  GuestPrimitive primitive = GuestPrimitive::kTriangleList;
  int32_t base_vertex = 0;
  uint32_t start = 0;  // first index (indexed) or first vertex
  uint32_t count = 0;  // index count or vertex count

  // Index buffer (indexed draws): host pointer to the first index of the
  // buffer (not of the draw), the guest object and its decoded fields.
  uint32_t index_buffer = 0;
  IndexBufferInfo index_info;
  const uint8_t* index_data = nullptr;

  // Stream 0: the guest vertex buffer from its offset, its stride, and where
  // the position is in each vertex (from the declaration, else 0).
  uint32_t vertex_buffer = 0;
  VertexBufferInfo vertex_info;
  const uint8_t* vertex_data = nullptr;  // vertex 0 of the stream (buffer + offset)
  uint32_t vertex_data_size = 0;         // bytes available from vertex_data
  uint32_t stride = 0;
  uint32_t position_offset = 0;
  bool position_from_declaration = false;
  // The position element's D3DDECLTYPE (0 without a declaration); the
  // placeholder reads only 32-bit float positions (DeclTypeFormat 57 or 38).
  uint32_t position_type = 0;

  // Vertex shader float constants c0..c3 (the world-view-projection matrix in
  // the per-draw sequence), as the guest stored them.
  std::array<float, 16> vs_c0_c3 = {};
  // The first four consecutive vertex float constants (from c0) that look like
  // a perspective world-view-projection matrix (FindPerspectiveRows), and their
  // first register; -1 when none does. c0..c3 for the static world, elsewhere
  // (or nowhere: post effects, HUD) for other shaders, e.g. skinned meshes
  // keep bone or world rows in c0..c3.
  int32_t wvp_register = -1;
  std::array<float, 16> wvp = {};

  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t vertex_declaration = 0;
  std::array<uint32_t, 4> render_targets = {};
  uint32_t depth_stencil = 0;
  Viewport viewport;
  std::array<uint32_t, 4> blend_control = {};
  std::array<uint32_t, 8> textures = {};  // samplers 0-7 (0-6 seen)
  RenderStates states;
  int32_t conditional_id = -1;  // innermost Begin/EndConditionalRendering, -1 when none
};

struct ResolveCall {
  uint32_t frame = 0;
  uint32_t flags = 0;
  uint32_t dest_texture = 0;
  uint32_t render_target0 = 0;
};

class DrawSink {
 public:
  virtual ~DrawSink() = default;
  virtual void OnClear(const ClearCall& call) = 0;
  virtual void OnDraw(const DrawCall& call) = 0;
  virtual void OnResolve(const ResolveCall& call) = 0;
  // The game's Present: the frame being recorded is complete.
  // render_target0 is the surface bound at that point (the frame's main
  // surface, which the library resolves to the frontbuffer).
  virtual void OnPresent(uint32_t frame, uint32_t render_target0) = 0;
};

}  // namespace nr
