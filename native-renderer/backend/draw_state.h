// The draw-state tracker: the D3D-level state the engine sets through the
// game's Direct3D library calls (docs/d3d-api-map.md), kept on our side so
// each draw can be issued with everything it needs. Fed by the NrApi calls
// (plugin/native_api.h, bound in api_binding.cpp); emits one record per
// clear / draw / resolve / present to a DrawSink.
//
// Guest data passed by pointer (constants, viewports) is copied at call time:
// the engine may pass stack buffers. Guest objects (buffers, declarations) are
// read at draw time, as the GPU would.
//
// Single-threaded, like the game's device (Acquire / ReleaseThreadOwnership).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "backend/draw_sink.h"
#include "backend/guest_memory.h"

namespace nr {

// True when rows[0..3] (4 floats each) look like a perspective projection
// times a view and world transform: the z row is a multiple (0.8 to 1.25) of
// the w row in x, y, z, the w row is not (0, 0, 0, *) as in an affine matrix,
// and the x and y rows are not zero.
bool LooksLikePerspectiveRows(const float* rows);

class DrawTracker {
 public:
  static constexpr uint32_t kMaxStreams = 16;
  static constexpr uint32_t kMaxSamplers = 32;
  static constexpr uint32_t kVsFloatConstants = 256;
  static constexpr uint32_t kPsFloatConstants = 256;
  static constexpr uint32_t kIntConstants = 32;

  struct Stream {
    uint32_t vertex_buffer = 0;
    uint32_t offset = 0;
    uint32_t stride = 0;
  };

  struct Stats {
    uint64_t draws = 0;
    uint64_t indexed_draws = 0;
    uint64_t clears = 0;
    uint64_t resolves = 0;
    uint64_t presents = 0;
    uint64_t dropped_draws = 0;  // missing index / vertex buffer or unmapped memory
  };

  DrawTracker(const GuestMemory& memory, DrawSink* sink) : memory_(memory), sink_(sink) {}

  void set_sink(DrawSink* sink) { sink_ = sink; }

  // State.
  void SetRenderTarget(uint32_t index, uint32_t surface);
  void SetDepthStencilSurface(uint32_t surface);
  void SetViewport(uint32_t viewport_guest);
  void SetTexture(uint32_t sampler, uint32_t texture);
  void SetIndices(uint32_t index_buffer);
  void SetStreamSource(uint32_t stream, uint32_t vertex_buffer, uint32_t offset, uint32_t stride);
  void SetPixelShader(uint32_t pixel_shader);
  void SetVertexShader(uint32_t vertex_shader);
  void SetVertexDeclaration(uint32_t declaration);
  void SetVsConstantsF(uint32_t reg, uint32_t data_guest, uint32_t count);
  void SetPsConstantsF(uint32_t reg, uint32_t data_guest, uint32_t count);
  void SetVsConstantsI(uint32_t reg, uint32_t data_guest, uint32_t count);
  void SetBlendControl(uint32_t render_target, uint32_t value);
  void SetRenderState(uint32_t state, uint32_t value);
  void SetSamplerState(uint32_t sampler, uint32_t type, uint32_t value);
  void BeginConditionalRendering(uint32_t id);
  void EndConditionalRendering();

  // Draw and frame.
  void Clear(uint32_t flags, uint32_t color_argb, float z, uint32_t stencil) {
    Clear(0, 0, 0, flags, color_argb, z, stencil);
  }
  void Clear(uint32_t device, uint32_t rect_count, uint32_t rects_guest, uint32_t flags,
             uint32_t color_argb, float z, uint32_t stencil);
  void Resolve(uint32_t flags, uint32_t dest_texture) {
    Resolve(0, flags, 0, dest_texture, 0, 0, 0, 0, 1.0f);
  }
  void Resolve(uint32_t device, uint32_t flags, uint32_t source_rect_guest, uint32_t dest_texture,
               uint32_t dest_point_guest, uint32_t dest_level, uint32_t dest_slice,
               uint32_t clear_color_guest, float clear_z);
  void DrawIndexed(uint32_t primitive, int32_t base_vertex, uint32_t start_index,
                   uint32_t index_count) {
    DrawIndexed(0, primitive, base_vertex, start_index, index_count);
  }
  void DrawIndexed(uint32_t device, uint32_t primitive, int32_t base_vertex, uint32_t start_index,
                   uint32_t index_count);
  void Draw(uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count) {
    Draw(0, primitive, start_vertex, vertex_count);
  }
  void Draw(uint32_t device, uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count);
  void Present(uint32_t device = 0);
  // Forwarded to the sink as is (any thread; touches no tracker state).
  void ShaderCreated(uint32_t kind, uint32_t container, uint32_t object) {
    if (sink_) sink_->OnShaderCreated(kind, container, object);
  }
  // Search the vertex constants for a perspective matrix on every draw (the
  // placeholder pipeline needs it; the real shaders do not).
  void set_find_wvp(bool on) { find_wvp_ = on; }

  // Read access (tests, logging).
  uint32_t frame() const { return frame_; }
  const Stats& stats() const { return stats_; }
  const RenderStates& render_states() const { return states_; }
  uint32_t render_state(uint32_t state) const;
  uint32_t sampler_state(uint32_t sampler, uint32_t type) const;
  const Viewport& viewport() const { return viewport_; }
  const Stream& stream(uint32_t i) const { return streams_[i]; }
  uint32_t index_buffer() const { return index_buffer_; }
  uint32_t texture(uint32_t sampler) const { return textures_[sampler]; }
  const float* vs_constant(uint32_t reg) const { return vs_f_[reg].data(); }
  const float* ps_constant(uint32_t reg) const { return ps_f_[reg].data(); }
  uint32_t vs_int_constant(uint32_t reg) const { return vs_i_[reg]; }
  uint32_t render_target(uint32_t i) const { return render_targets_[i]; }
  uint32_t depth_stencil() const { return depth_stencil_; }
  int32_t conditional_id() const {
    return conditional_stack_.empty() ? -1 : int32_t(conditional_stack_.back());
  }

 private:
  void CopyFloatConstants(std::array<float, 4>* dest, uint32_t dest_count, uint32_t reg,
                          uint32_t data_guest, uint32_t count);
  bool FillCommon(DrawCall& call);

  const GuestMemory& memory_;
  DrawSink* sink_;

  std::array<uint32_t, 4> render_targets_ = {};
  uint32_t depth_stencil_ = 0;
  Viewport viewport_;
  std::array<uint32_t, kMaxSamplers> textures_ = {};
  uint32_t index_buffer_ = 0;
  std::array<Stream, kMaxStreams> streams_ = {};
  uint32_t vertex_shader_ = 0;
  uint32_t pixel_shader_ = 0;
  uint32_t vertex_declaration_ = 0;
  std::array<std::array<float, 4>, kVsFloatConstants> vs_f_ = {};
  std::array<std::array<float, 4>, kPsFloatConstants> ps_f_ = {};
  std::array<uint32_t, kIntConstants> vs_i_ = {};
  std::array<uint32_t, 4> blend_control_ = {};
  std::array<uint32_t, 97> render_state_values_ = {};
  std::array<std::array<uint32_t, 20>, kMaxSamplers> sampler_state_values_ = {};
  RenderStates states_;
  std::vector<uint32_t> conditional_stack_;

  uint32_t frame_ = 0;
  uint32_t draws_in_frame_ = 0;
  bool find_wvp_ = true;
  Stats stats_;
};

}  // namespace nr
