#include "backend/renderer.h"

#include <algorithm>
#include <cstring>

#include "backend/primitive.h"
#include "backend/shaders.h"

namespace nr {

Renderer::~Renderer() { Shutdown(); }

void Renderer::Shutdown() {
  frame_command_list_ = nullptr;
  frame_open_ = false;
  framebuffers_.clear();
  placeholder_set_ = nullptr;
  placeholder_pipeline_ = nullptr;
  triangle_pipeline_ = nullptr;
  placeholder_layout_ = nullptr;
  vertex_ring_ = nullptr;
  index_ring_ = nullptr;
  frames_ = {};
  triangle_vs_ = triangle_ps_ = placeholder_vs_ = placeholder_ps_ = nullptr;
}

bool Renderer::Initialize(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;
  const nvrhi::GraphicsAPI api = device_->getGraphicsAPI();

  for (size_t i = 0; i < frames_.size(); ++i) {
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = kFrameFormat;
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.keepInitialState = true;
    desc.clearValue = nvrhi::Color(0.0f);
    desc.useClearValue = true;
    desc.debugName = i == 0 ? "Native frame 0" : "Native frame 1";
    frames_[i] = device_->createTexture(desc);
    if (!frames_[i]) return false;
  }

  auto make_shader = [&](ShaderId id, nvrhi::ShaderType type) -> nvrhi::ShaderHandle {
    ShaderBytecode code = GetShaderBytecode(api, id);
    if (!code.data) return nullptr;
    nvrhi::ShaderDesc desc;
    desc.shaderType = type;
    desc.entryName = type == nvrhi::ShaderType::Vertex ? "vs_main" : "ps_main";
    return device_->createShader(desc, code.data, code.size);
  };
  triangle_vs_ = make_shader(ShaderId::kTriangleVs, nvrhi::ShaderType::Vertex);
  triangle_ps_ = make_shader(ShaderId::kTrianglePs, nvrhi::ShaderType::Pixel);
  placeholder_vs_ = make_shader(ShaderId::kPlaceholderVs, nvrhi::ShaderType::Vertex);
  placeholder_ps_ = make_shader(ShaderId::kPlaceholderPs, nvrhi::ShaderType::Pixel);
  if (!triangle_vs_ || !triangle_ps_ || !placeholder_vs_ || !placeholder_ps_) return false;

  nvrhi::BufferDesc vb;
  vb.byteSize = kVertexRingBytes;
  vb.canHaveRawViews = true;
  vb.debugName = "Native vertex ring";
  vb.initialState = nvrhi::ResourceStates::ShaderResource;
  vb.keepInitialState = true;
  vertex_ring_ = device_->createBuffer(vb);
  nvrhi::BufferDesc ib;
  ib.byteSize = kIndexRingBytes;
  ib.isIndexBuffer = true;
  ib.debugName = "Native index ring";
  ib.initialState = nvrhi::ResourceStates::IndexBuffer;
  ib.keepInitialState = true;
  index_ring_ = device_->createBuffer(ib);
  if (!vertex_ring_ || !index_ring_) return false;

  nvrhi::BindingLayoutDesc layout;
  layout.visibility = nvrhi::ShaderType::All;
  layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PlaceholderConstants)),
                     nvrhi::BindingLayoutItem::RawBuffer_SRV(0)};
  placeholder_layout_ = device_->createBindingLayout(layout);
  if (!placeholder_layout_) return false;
  nvrhi::BindingSetDesc set;
  set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(PlaceholderConstants)),
                  nvrhi::BindingSetItem::RawBuffer_SRV(0, vertex_ring_)};
  placeholder_set_ = device_->createBindingSet(set, placeholder_layout_);
  if (!placeholder_set_) return false;

  nvrhi::IFramebuffer* fb = FramebufferFor(frames_[0]);
  if (!fb) return false;

  nvrhi::GraphicsPipelineDesc tri;
  tri.primType = nvrhi::PrimitiveType::TriangleList;
  tri.VS = triangle_vs_;
  tri.PS = triangle_ps_;
  tri.renderState.depthStencilState.depthTestEnable = false;
  tri.renderState.depthStencilState.depthWriteEnable = false;
  tri.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  triangle_pipeline_ = device_->createGraphicsPipeline(tri, fb->getFramebufferInfo());
  if (!triangle_pipeline_) return false;

  nvrhi::GraphicsPipelineDesc ph = tri;
  ph.VS = placeholder_vs_;
  ph.PS = placeholder_ps_;
  ph.bindingLayouts = {placeholder_layout_};
  placeholder_pipeline_ = device_->createGraphicsPipeline(ph, fb->getFramebufferInfo());
  if (!placeholder_pipeline_) return false;

  frame_command_list_ = device_->createCommandList();
  return frame_command_list_ != nullptr;
}

nvrhi::IFramebuffer* Renderer::FramebufferFor(nvrhi::ITexture* target) {
  auto it = framebuffers_.find(target);
  if (it != framebuffers_.end()) return it->second;
  nvrhi::FramebufferDesc desc;
  desc.addColorAttachment(target);
  nvrhi::FramebufferHandle fb = device_->createFramebuffer(desc);
  if (!fb) return nullptr;
  // Keep a few: the frame images plus test targets.
  if (framebuffers_.size() > 16) framebuffers_.clear();
  framebuffers_[target] = fb;
  return fb;
}

void Renderer::RecordClear(nvrhi::ICommandList* command_list, nvrhi::ITexture* target,
                           const nvrhi::Color& color) {
  command_list->clearTextureFloat(target, nvrhi::AllSubresources, color);
}

void Renderer::RecordTestTriangle(nvrhi::ICommandList* command_list, nvrhi::ITexture* target) {
  nvrhi::IFramebuffer* fb = FramebufferFor(target);
  if (!fb) return;
  const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
  nvrhi::GraphicsState state;
  state.pipeline = triangle_pipeline_;
  state.framebuffer = fb;
  state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(info.width), float(info.height)));
  command_list->setGraphicsState(state);
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  command_list->draw(args);
}

nvrhi::ICommandList* Renderer::FrameCommandList() {
  if (!frame_open_) {
    frame_command_list_->open();
    frame_open_ = true;
  }
  return frame_command_list_;
}

bool Renderer::OnMainSurface(uint32_t render_target0) const {
  return !options_.only_main_surface || main_surface_ == 0 || render_target0 == main_surface_;
}

nvrhi::Viewport Renderer::ViewportFor(const Viewport& v) const {
  if (v.width == 0 || v.height == 0) {
    return nvrhi::Viewport(float(width_), float(height_));
  }
  float x0 = std::min(float(v.x), float(width_));
  float y0 = std::min(float(v.y), float(height_));
  float x1 = std::min(float(v.x + v.width), float(width_));
  float y1 = std::min(float(v.y + v.height), float(height_));
  nvrhi::Viewport out(x0, std::max(x0, x1), y0, std::max(y0, y1), v.min_z, v.max_z);
  if (out.maxZ < out.minZ) std::swap(out.minZ, out.maxZ);
  return out;
}

nvrhi::Color Renderer::DrawColor(uint32_t vertex_shader, uint32_t pixel_shader) {
  uint32_t h = vertex_shader * 0x9E3779B1u ^ (pixel_shader + 0x7F4A7C15u) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  // Bright, saturated-ish colours: each channel in [0.25, 1].
  auto channel = [](uint32_t bits) { return 0.25f + 0.75f * float(bits & 0xFF) / 255.0f; };
  return nvrhi::Color(channel(h), channel(h >> 8), channel(h >> 16), 1.0f);
}

nvrhi::Color Renderer::TestClearColor(uint32_t frame) {
  // 120 frames per cycle; each channel a phase-shifted triangle wave.
  auto wave = [](float t) {
    t -= float(int(t));
    return t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t;
  };
  float t = float(frame % 120) / 120.0f;
  return nvrhi::Color(wave(t), wave(t + 1.0f / 3.0f), wave(t + 2.0f / 3.0f), 1.0f);
}

void Renderer::OnClear(const ClearCall& call) {
  if (!call.color || !OnMainSurface(call.render_target0)) return;
  nvrhi::Color color(call.rgba[0], call.rgba[1], call.rgba[2], call.rgba[3]);
  RecordClear(FrameCommandList(), frames_[recording_], color);
  ++stats_.clears_recorded;
  if (observer_) observer_->OnClearRecorded(call, color);
}

void Renderer::OnDraw(const DrawCall& call) {
  if (!OnMainSurface(call.render_targets[0])) {
    ++stats_.draws_skipped_target;
    return;
  }
  PrimitiveInput in;
  in.primitive = call.primitive;
  in.index_data = call.indexed ? call.index_data : nullptr;
  in.index32 = call.index_info.index32;
  in.start = call.start;
  in.count = call.count;
  in.base_vertex = call.indexed ? call.base_vertex : 0;
  if (!BuildTriangleList(in, scratch_indices_)) {
    ++stats_.draws_skipped_primitive;
    return;
  }
  if (scratch_indices_.empty() || call.stride == 0) return;

  // The vertex range the draw uses; the indices are rebased onto it.
  auto [min_it, max_it] = std::minmax_element(scratch_indices_.begin(), scratch_indices_.end());
  const uint32_t first = *min_it, last = *max_it;
  const uint64_t begin = uint64_t(first) * call.stride;
  const uint64_t end = uint64_t(last) * call.stride + call.position_offset + 12;
  if (end > call.vertex_data_size) {
    ++stats_.draws_skipped_range;
    return;
  }
  const uint64_t vertex_bytes = (end - begin + 3) & ~uint64_t(3);
  const uint64_t index_bytes = scratch_indices_.size() * sizeof(uint32_t);
  if (vertex_ring_used_ + vertex_bytes > kVertexRingBytes ||
      index_ring_used_ + index_bytes > kIndexRingBytes) {
    ++stats_.draws_skipped_range;
    return;
  }
  for (uint32_t& i : scratch_indices_) i -= first;

  nvrhi::ICommandList* cl = FrameCommandList();
  // The guest bytes may not be a multiple of 4 at the end: copy what exists.
  std::vector<uint8_t> vertex_copy(vertex_bytes, 0);
  std::memcpy(vertex_copy.data(), call.vertex_data + begin,
              size_t(std::min<uint64_t>(vertex_bytes, call.vertex_data_size - begin)));
  cl->writeBuffer(vertex_ring_, vertex_copy.data(), vertex_bytes, vertex_ring_used_);
  cl->writeBuffer(index_ring_, scratch_indices_.data(), index_bytes, index_ring_used_);

  PlaceholderConstants constants = {};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      constants.wvp[r * 4 + c] =
          options_.transpose_wvp ? call.vs_c0_c3[c * 4 + r] : call.vs_c0_c3[r * 4 + c];
    }
  }
  nvrhi::Color color = DrawColor(call.vertex_shader, call.pixel_shader);
  constants.color[0] = color.r;
  constants.color[1] = color.g;
  constants.color[2] = color.b;
  constants.color[3] = 1.0f;
  constants.stride = call.stride;
  constants.pos_offset = call.position_offset;
  constants.vertex_base = uint32_t(vertex_ring_used_);

  nvrhi::GraphicsState state;
  state.pipeline = placeholder_pipeline_;
  state.framebuffer = FramebufferFor(frames_[recording_]);
  state.bindings = {placeholder_set_};
  state.indexBuffer = nvrhi::IndexBufferBinding()
                          .setBuffer(index_ring_)
                          .setFormat(nvrhi::Format::R32_UINT)
                          .setOffset(uint32_t(index_ring_used_));
  state.viewport.addViewportAndScissorRect(ViewportFor(call.viewport));
  cl->setGraphicsState(state);
  cl->setPushConstants(&constants, sizeof(constants));
  nvrhi::DrawArguments args;
  args.vertexCount = uint32_t(scratch_indices_.size());
  cl->drawIndexed(args);

  vertex_ring_used_ += (vertex_bytes + 255) & ~uint64_t(255);
  index_ring_used_ += (index_bytes + 255) & ~uint64_t(255);
  ++stats_.draws_recorded;
  if (observer_) observer_->OnDrawRecorded(call, state, args, constants, scratch_indices_);
}

void Renderer::OnResolve(const ResolveCall& /*call*/) {
  // Render-to-texture is not modelled yet: resolves (and their clears) wait
  // for the render-target pool.
}

void Renderer::OnPresent(uint32_t frame, uint32_t render_target0) {
  main_surface_ = render_target0;
  vertex_ring_used_ = 0;
  index_ring_used_ = 0;
  if (!frame_open_) return;
  frame_command_list_->close();
  frame_open_ = false;
  if (submit_) submit_(frame_command_list_);
  ++stats_.frames_submitted;
  presented_ = recording_;
  recording_ ^= 1;
  if (observer_) observer_->OnFrameSubmitted(frame, frames_[presented_]);
}

}  // namespace nr
