#include "backend/draw_state.h"

#include <algorithm>

namespace nr {

void DrawTracker::SetRenderTarget(uint32_t index, uint32_t surface) {
  if (index < render_targets_.size()) {
    render_targets_[index] = surface;
  }
}

void DrawTracker::SetDepthStencilSurface(uint32_t surface) { depth_stencil_ = surface; }

void DrawTracker::SetViewport(uint32_t viewport_guest) {
  if (const uint8_t* p = memory_.Virtual(viewport_guest)) {
    viewport_ = DecodeViewport(p);
  }
}

void DrawTracker::SetTexture(uint32_t sampler, uint32_t texture) {
  if (sampler < kMaxSamplers) {
    textures_[sampler] = texture;
  }
}

void DrawTracker::SetIndices(uint32_t index_buffer) { index_buffer_ = index_buffer; }

void DrawTracker::SetStreamSource(uint32_t stream, uint32_t vertex_buffer, uint32_t offset,
                                uint32_t stride) {
  if (stream < kMaxStreams) {
    streams_[stream] = Stream{vertex_buffer, offset, stride};
  }
}

void DrawTracker::SetPixelShader(uint32_t pixel_shader) { pixel_shader_ = pixel_shader; }
void DrawTracker::SetVertexShader(uint32_t vertex_shader) { vertex_shader_ = vertex_shader; }
void DrawTracker::SetVertexDeclaration(uint32_t declaration) { vertex_declaration_ = declaration; }

void DrawTracker::CopyFloatConstants(std::array<float, 4>* dest, uint32_t dest_count, uint32_t reg,
                                   uint32_t data_guest, uint32_t count) {
  if (reg >= dest_count) {
    return;
  }
  count = std::min(count, dest_count - reg);
  const uint8_t* p = memory_.Virtual(data_guest);
  if (!p) {
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t c = 0; c < 4; ++c) {
      dest[reg + i][c] = LoadBEFloat(p + (i * 4 + c) * 4);
    }
  }
}

void DrawTracker::SetVsConstantsF(uint32_t reg, uint32_t data_guest, uint32_t count) {
  CopyFloatConstants(vs_f_.data(), kVsFloatConstants, reg, data_guest, count);
}

void DrawTracker::SetPsConstantsF(uint32_t reg, uint32_t data_guest, uint32_t count) {
  CopyFloatConstants(ps_f_.data(), kPsFloatConstants, reg, data_guest, count);
}

void DrawTracker::SetVsConstantsI(uint32_t reg, uint32_t data_guest, uint32_t count) {
  // int4s; the library packs x, y, z into the bytes of the loop constant
  // (count, start, step).
  const uint8_t* p = memory_.Virtual(data_guest);
  if (!p || reg >= kIntConstants) {
    return;
  }
  count = std::min(count, kIntConstants - reg);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t x = LoadBE32(p + i * 16), y = LoadBE32(p + i * 16 + 4), z = LoadBE32(p + i * 16 + 8);
    vs_i_[reg + i] = (x & 0xFF) | ((y & 0xFF) << 8) | ((z & 0xFF) << 16);
  }
}

void DrawTracker::SetBlendControl(uint32_t render_target, uint32_t value) {
  if (render_target < blend_control_.size()) {
    blend_control_[render_target] = value;
  }
}

void DrawTracker::SetRenderState(uint32_t state, uint32_t value) {
  if (state >= rs::kCount || (state & 3)) {
    return;
  }
  render_state_values_[state / 4] = value;
  switch (state) {
    case rs::kZEnable: states_.z_enable = value; break;
    case rs::kZFunc: states_.z_func = value; break;
    case rs::kZWriteEnable: states_.z_write_enable = value; break;
    case rs::kCullMode: states_.cull_mode = value; break;
    case rs::kAlphaTestEnable: states_.alpha_test_enable = value; break;
    case rs::kAlphaRef: states_.alpha_ref = value; break;
    case rs::kAlphaFunc: states_.alpha_func = value; break;
    case rs::kStencilEnable: states_.stencil_enable = value; break;
    case rs::kStencilRef: states_.stencil_ref = value; break;
    case rs::kColorWriteEnable: states_.color_write_enable = value; break;
    default: break;
  }
}

uint32_t DrawTracker::render_state(uint32_t state) const {
  return state < rs::kCount && !(state & 3) ? render_state_values_[state / 4] : 0;
}

void DrawTracker::SetSamplerState(uint32_t sampler, uint32_t type, uint32_t value) {
  if (sampler < kMaxSamplers && type < ss::kCount && !(type & 3)) {
    sampler_state_values_[sampler][type / 4] = value;
  }
}

uint32_t DrawTracker::sampler_state(uint32_t sampler, uint32_t type) const {
  if (sampler < kMaxSamplers && type < ss::kCount && !(type & 3)) {
    return sampler_state_values_[sampler][type / 4];
  }
  return 0;
}

void DrawTracker::BeginConditionalRendering(uint32_t id) { conditional_stack_.push_back(id); }

void DrawTracker::EndConditionalRendering() {
  if (!conditional_stack_.empty()) {
    conditional_stack_.pop_back();
  }
}

void DrawTracker::Clear(uint32_t flags, uint32_t color_argb, float z, uint32_t stencil) {
  ++stats_.clears;
  if (!sink_) {
    return;
  }
  ClearCall call;
  call.frame = frame_;
  call.flags = flags;
  call.color = (flags & clear_flags::kTargetMask) != 0;
  call.depth = (flags & clear_flags::kZBuffer) != 0;
  call.stencil = (flags & clear_flags::kStencil) != 0;
  call.rgba[0] = float((color_argb >> 16) & 0xFF) / 255.0f;
  call.rgba[1] = float((color_argb >> 8) & 0xFF) / 255.0f;
  call.rgba[2] = float(color_argb & 0xFF) / 255.0f;
  call.rgba[3] = float(color_argb >> 24) / 255.0f;
  call.z = z;
  call.stencil_value = stencil;
  call.render_target0 = render_targets_[0];
  call.depth_stencil = depth_stencil_;
  call.viewport = viewport_;
  sink_->OnClear(call);
}

void DrawTracker::Resolve(uint32_t flags, uint32_t dest_texture) {
  ++stats_.resolves;
  if (sink_) {
    sink_->OnResolve(ResolveCall{frame_, flags, dest_texture, render_targets_[0]});
  }
}

bool DrawTracker::FillCommon(DrawCall& call) {
  call.frame = frame_;
  call.index_in_frame = draws_in_frame_++;

  const Stream& s0 = streams_[0];
  call.vertex_buffer = s0.vertex_buffer;
  call.stride = s0.stride;
  const uint8_t* vb_object = s0.vertex_buffer ? memory_.Virtual(s0.vertex_buffer) : nullptr;
  if (!vb_object || !DecodeVertexBuffer(vb_object, call.vertex_info)) {
    return false;
  }
  if (s0.offset >= call.vertex_info.size_bytes) {
    return false;
  }
  const uint8_t* vb_data = memory_.Physical(call.vertex_info.physical);
  if (!vb_data) {
    return false;
  }
  call.vertex_data = vb_data + s0.offset;
  call.vertex_data_size = call.vertex_info.size_bytes - s0.offset;

  DeclElement position;
  if (vertex_declaration_ &&
      FindPositionElement(memory_.Virtual(vertex_declaration_), 0, position)) {
    call.position_offset = position.offset;
    call.position_from_declaration = true;
  }

  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) {
      call.vs_c0_c3[r * 4 + c] = vs_f_[r][c];
    }
  }
  call.vertex_shader = vertex_shader_;
  call.pixel_shader = pixel_shader_;
  call.vertex_declaration = vertex_declaration_;
  call.render_targets = render_targets_;
  call.depth_stencil = depth_stencil_;
  call.viewport = viewport_;
  call.blend_control = blend_control_;
  for (uint32_t i = 0; i < call.textures.size(); ++i) {
    call.textures[i] = textures_[i];
  }
  call.states = states_;
  call.conditional_id = conditional_id();
  return true;
}

void DrawTracker::DrawIndexed(uint32_t primitive, int32_t base_vertex, uint32_t start_index,
                            uint32_t index_count) {
  ++stats_.draws;
  ++stats_.indexed_draws;
  DrawCall call;
  call.indexed = true;
  call.primitive = GuestPrimitive(primitive);
  call.base_vertex = base_vertex;
  call.start = start_index;
  call.count = index_count;
  call.index_buffer = index_buffer_;
  const uint8_t* ib_object = index_buffer_ ? memory_.Virtual(index_buffer_) : nullptr;
  bool ok = ib_object && DecodeIndexBuffer(ib_object, call.index_info);
  if (ok) {
    call.index_data = memory_.Physical(call.index_info.physical);
    uint32_t index_size = call.index_info.index32 ? 4 : 2;
    ok = call.index_data &&
         uint64_t(start_index + uint64_t(index_count)) * index_size <= call.index_info.size_bytes;
  }
  ok = FillCommon(call) && ok;
  if (!ok) {
    ++stats_.dropped_draws;
    return;
  }
  if (sink_) {
    sink_->OnDraw(call);
  }
}

void DrawTracker::Draw(uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count) {
  ++stats_.draws;
  DrawCall call;
  call.indexed = false;
  call.primitive = GuestPrimitive(primitive);
  call.start = start_vertex;
  call.count = vertex_count;
  if (!FillCommon(call)) {
    ++stats_.dropped_draws;
    return;
  }
  if (sink_) {
    sink_->OnDraw(call);
  }
}

void DrawTracker::Present() {
  ++stats_.presents;
  if (sink_) {
    sink_->OnPresent(frame_, render_targets_[0]);
  }
  ++frame_;
  draws_in_frame_ = 0;
}

}  // namespace nr
