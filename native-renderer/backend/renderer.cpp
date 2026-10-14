#include "backend/renderer.h"
#include "backend/cpu_profile.h"
#include "backend/dev_gpu_stats.h"

#include <mutex>

#include <chrono>

#include <algorithm>
#include <cstring>
#include <cstdio>

#include "backend/primitive.h"
#if NR_GAME_RENDERER
#include "backend/game_renderer.h"
#else
namespace nr {

// Without the game renderer: never created, never called.
class GameRenderer {
 public:
  template <typename... A> void Draw(A&&...) {}
  template <typename... A> void Clear(A&&...) {}
  template <typename... A> void Resolve(A&&...) {}
  template <typename... A> bool Present(A&&...) { return false; }
  template <typename... A> void VertexShaderPatched(A&&...) {}
  void UploadDummies(nvrhi::ICommandList*) {}
  void EndFrame() {}
  struct Options { uint32_t debug = 0; };
  const Options& options() const { static const Options options; return options; }
};
}  // namespace nr
#endif
#include "backend/shaders.h"
#include "backend/log.h"

namespace nr {

// Every pipeline of the renderer is made here. On D3D12 NVRHI's root signature cache had no lock, so this made
// one pipeline at a time; the vendored NVRHI now locks the cache (thirdparty/NOTES.md, Phase 2c) and the
// workers make D3D12 pipelines in parallel as on Vulkan.
nvrhi::GraphicsPipelineHandle CreateGraphicsPipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc,
                                                     const nvrhi::FramebufferInfo& framebuffer) {
  return device->createGraphicsPipeline(desc, framebuffer);
}


Renderer::Renderer(nvrhi::IDevice* device) : device_(device) {}

Renderer::~Renderer() { Shutdown(); }

void Renderer::EnableGame(std::unique_ptr<GameRenderer> game) { game_ = std::move(game); }

void Renderer::Shutdown() {
  frame_gpu_timer_.reset();
  debug_open_pass_ = nullptr;
  debug_pass_timers_.clear();
  game_.reset();
  frame_command_list_ = nullptr;
  frame_open_ = false;
  framebuffers_.clear();
  placeholder_pipelines_.clear();
  depth_.clear();
  placeholder_set_ = nullptr;
  placeholder_pipeline_ = nullptr;
  triangle_pipeline_ = nullptr;
  placeholder_layout_ = nullptr;
  vertex_ring_ = nullptr;
  index_ring_ = nullptr;
  frames_ = {};
  frame_queries_ = {};
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
    frame_queries_[i] = device_->createEventQuery();
    if (!frame_queries_[i]) return false;
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

  placeholder_desc_ = tri;
  placeholder_desc_.VS = placeholder_vs_;
  placeholder_desc_.PS = placeholder_ps_;
  placeholder_desc_.bindingLayouts = {placeholder_layout_};
  placeholder_pipeline_ = device_->createGraphicsPipeline(placeholder_desc_, fb->getFramebufferInfo());
  if (!placeholder_pipeline_) return false;

  for (auto& list : frame_lists_) {
    list = device_->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
    if (!list) return false;
  }
  frame_command_list_ = frame_lists_[0];
  return true;
}

nvrhi::ITexture* Renderer::DepthFor(nvrhi::ITexture* target) {
  auto it = depth_.find(target);
  if (it != depth_.end()) return it->second;
  const nvrhi::TextureDesc& color = target->getDesc();
  nvrhi::TextureDesc desc;
  desc.width = color.width;
  desc.height = color.height;
  desc.format = kDepthFormat;
  desc.isRenderTarget = true;
  desc.initialState = nvrhi::ResourceStates::DepthWrite;
  desc.keepInitialState = true;
  desc.clearValue = nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f);
  desc.useClearValue = true;
  desc.debugName = "Native depth";
  nvrhi::TextureHandle depth = device_->createTexture(desc);
  if (!depth) return nullptr;
  if (depth_.size() > 16) depth_.clear();
  depth_[target] = depth;
  return depth;
}

nvrhi::IGraphicsPipeline* Renderer::PlaceholderPipeline(const RenderStates& states) {
  const bool test = states.z_enable != 0;
  const bool write = test && states.z_write_enable != 0;
  const uint32_t func = test ? (states.z_func & 7) : 7;
  const uint32_t key = (test ? 16u : 0u) | (write ? 8u : 0u) | func;
  auto it = placeholder_pipelines_.find(key);
  if (it != placeholder_pipelines_.end()) return it->second;
  nvrhi::GraphicsPipelineDesc desc = placeholder_desc_;
  desc.renderState.depthStencilState.depthTestEnable = test;
  desc.renderState.depthStencilState.depthWriteEnable = write;
  // Xenos 0 never .. 7 always is NVRHI's Never (1) .. Always (8).
  desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc(func + 1);
  nvrhi::IFramebuffer* fb = FramebufferFor(frames_[0]);
  if (!fb) return nullptr;
  nvrhi::GraphicsPipelineHandle pipeline = CreateGraphicsPipeline(device_, desc, fb->getFramebufferInfo());
  if (!pipeline) return nullptr;
  placeholder_pipelines_[key] = pipeline;
  return pipeline;
}

nvrhi::IFramebuffer* Renderer::FramebufferFor(nvrhi::ITexture* target) {
  auto it = framebuffers_.find(target);
  if (it != framebuffers_.end()) return it->second;
  nvrhi::ITexture* depth = DepthFor(target);
  if (!depth) return nullptr;
  nvrhi::FramebufferDesc desc;
  desc.addColorAttachment(target);
  desc.setDepthAttachment(depth);
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

void Renderer::WaitForFrameImage() {
  // Submission alone does not retire volatile constant versions or upload pages. Before frame N
  // reaches the GPU (or, with frame_wait_at_open, before it is recorded), frame N-2, which used
  // the same image and completion query, must have completed: the GPU never holds more than two
  // frames, and N-2's resources are free again.
  if (frame_wait_ && stats_.frames_submitted >= frame_queries_.size()) {
    const auto start = std::chrono::steady_clock::now();
    frame_wait_(frame_queries_[recording_]);
    stats_.gpu_reuse_wait_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
  }
}

nvrhi::ICommandList* Renderer::FrameCommandList() {
  if (!frame_open_) {
    if (options_.frame_wait_at_open) WaitForFrameImage();
    frame_command_list_->open();
    if (!frame_gpu_timer_) {
      frame_gpu_timer_ = std::make_unique<AoGpuTimer>(device_, "whole frame", "frame GPU time");
#if NR_GAME_RENDERER
      frame_gpu_timer_->set_quiet(!(game_ && (game_->options().debug & 32768)));
#else
      frame_gpu_timer_->set_quiet(true);
#endif
    }
    frame_gpu_timer_->Begin(frame_command_list_);
    frame_open_ = true;
    if (game_) game_->UploadDummies(frame_command_list_);
    // A defined depth at the start of every frame (the game's own depth clear
    // comes later, if at all).
    if (nvrhi::ITexture* depth = game_ ? nullptr : DepthFor(frames_[recording_])) {
      frame_command_list_->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 1.0f,
                                                    false, 0);
    }
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

void Renderer::DebugGpuPass(nvrhi::ICommandList* cl, uint64_t key, const char* label) {
  // Sample every fourth frame to bound query usage and perturbation. Batches of
  // draws sharing a target are one measured pass, including their guest uploads.
  if (!game_ || !(game_->options().debug & 524288) || stats_.frames_submitted % 4 != 0) return;
  auto it = debug_pass_timers_.find(key);
  if (it == debug_pass_timers_.end()) {
    if (debug_pass_timers_.size() >= 64) return;
    it = debug_pass_timers_.emplace(key, std::make_unique<TimedPass>(device_, label)).first;
  }
  TimedPass* pass = it->second.get();
  if (pass == debug_open_pass_) return;
  if (debug_open_pass_) debug_open_pass_->timer.End(cl);
  debug_open_pass_ = pass;
  pass->timer.Begin(cl);
}

void Renderer::OnClear(const ClearCall& call) {
  if (game_) {
    auto* cl = FrameCommandList();
    if (game_->options().debug & 524288) DebugGpuPass(cl, UINT64_MAX, "clears");
    game_->Clear(cl, call);
    return;
  }
  if (!(call.color || call.depth) || !OnMainSurface(call.render_target0)) return;
  if (!call.color) {
    if (nvrhi::ITexture* depth = DepthFor(frames_[recording_])) {
      FrameCommandList()->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true,
                                                   std::clamp(call.z, 0.0f, 1.0f), false, 0);
    }
    return;
  }
  nvrhi::Color color(call.rgba[0], call.rgba[1], call.rgba[2], call.rgba[3]);
  RecordClear(FrameCommandList(), frames_[recording_], color);
  if (call.depth) {
    if (nvrhi::ITexture* depth = DepthFor(frames_[recording_])) {
      FrameCommandList()->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true,
                                                   std::clamp(call.z, 0.0f, 1.0f), false, 0);
    }
  }
  ++stats_.clears_recorded;
  if (observer_) observer_->OnClearRecorded(call, color);
}

void Renderer::OnDraw(const DrawCall& call) {
  if (game_) {
    auto* cl = FrameCommandList();
    if ((game_->options().debug & 524288) && stats_.frames_submitted % 4 == 0) {
      char label[96];
      std::snprintf(label, sizeof(label), "draw RT0 %08X depth %08X (uploads included)",
                    call.render_targets[0], call.depth_stencil);
      DebugGpuPass(cl, uint64_t(call.render_targets[0]) << 32 | call.depth_stencil, label);
    }
    game_->Draw(cl, call);
    return;
  }
  if (!OnMainSurface(call.render_targets[0])) {
    ++stats_.draws_skipped_target;
    return;
  }
  // Opaque: RB_BLENDCONTROL0 colour part = source ONE, ADD, destination ZERO.
  const bool opaque = (call.blend_control[0] & 0x1FFF) == 0x0001;
  if (options_.only_depth_tested &&
      (!call.states.z_enable || (call.states.z_func & 7) == 7 ||
       (!call.states.z_write_enable && !opaque))) {
    ++stats_.draws_skipped_overlay;
    return;
  }
  if (call.position_from_declaration && DeclTypeFormat(call.position_type) != 57 &&
      DeclTypeFormat(call.position_type) != 38) {
    ++stats_.draws_skipped_format;
    return;
  }
  if (call.wvp_register < 0 && !options_.transpose_wvp) {
    ++stats_.draws_skipped_no_wvp;
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
          options_.transpose_wvp ? call.vs_c0_c3[c * 4 + r] : call.wvp[r * 4 + c];
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
  state.pipeline = PlaceholderPipeline(call.states);
  if (!state.pipeline) state.pipeline = placeholder_pipeline_;
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

void Renderer::OnResolve(const ResolveCall& call) {
  // The placeholder does not model render-to-texture.
  if (game_) {
    auto* cl = FrameCommandList();
    if (game_->options().debug & 524288) DebugGpuPass(cl, UINT64_MAX - 1, "resolves and scene-copy work");
    game_->Resolve(cl, call);
  }
}

void Renderer::OnVertexShaderPatched(uint32_t shader, uint32_t microcode, uint32_t declaration) {
  if (game_) game_->VertexShaderPatched(shader, microcode, declaration);
}

void Renderer::OnPresent(uint32_t frame, uint32_t render_target0, uint32_t device) {
  using clock = std::chrono::steady_clock;
  auto us = [](clock::time_point a, clock::time_point b) {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
  };
  main_surface_ = render_target0;
  vertex_ring_used_ = 0;
  index_ring_used_ = 0;
  const auto t0 = clock::now();
  if (game_) {
    // The game's back buffer into the frame image.
    auto* cl = FrameCommandList();
    if (game_->options().debug & 524288) DebugGpuPass(cl, UINT64_MAX - 2, "game Present copy");
    game_->Present(cl, device, frames_[recording_]);
  }
  const auto t1 = clock::now();
  stats_.present_copy_us += us(t0, t1);
  if (!frame_open_) return;
  if (debug_open_pass_) debug_open_pass_->timer.End(frame_command_list_);
  debug_open_pass_ = nullptr;
  if (frame_gpu_timer_) frame_gpu_timer_->End(frame_command_list_);
  frame_command_list_->close();
  frame_open_ = false;
  const auto t2 = clock::now();
  // The image the Present copy just wrote, and the volatile versions this frame took, belong to
  // frame N-2 on the GPU until it completes (the GPU runs the lists in order).
  if (!options_.frame_wait_at_open) WaitForFrameImage();
  const auto t2w = clock::now();
  if (submit_) submit_(frame_command_list_);
  frame_list_index_ = (frame_list_index_ + 1) % uint32_t(frame_lists_.size());
  frame_command_list_ = frame_lists_[frame_list_index_];
  const auto t3 = clock::now();
  ++stats_.frames_submitted;
  if (game_ && (game_->options().debug & 32768) && stats_.frames_submitted % 300 == 0) {
    const auto counters = GpuCommandStats();
    Logf(LogLevel::kInfo,
         "rexgpu-native: emitted frame %llu (300 frames): %llu pipeline binds, %llu viewports, %llu scissors, "
         "%llu blends, %llu stencil refs, %llu root CBV updates",
         static_cast<unsigned long long>(stats_.frames_submitted),
         static_cast<unsigned long long>(counters[0] - debug_gpu_api_previous_[0]),
         static_cast<unsigned long long>(counters[1] - debug_gpu_api_previous_[1]),
         static_cast<unsigned long long>(counters[2] - debug_gpu_api_previous_[2]),
         static_cast<unsigned long long>(counters[3] - debug_gpu_api_previous_[3]),
         static_cast<unsigned long long>(counters[4] - debug_gpu_api_previous_[4]),
         static_cast<unsigned long long>(counters[5] - debug_gpu_api_previous_[5]));
    debug_gpu_api_previous_ = counters;
    Logf(LogLevel::kInfo, "rexgpu-native: render CPU frame %llu: %llu us execution (cumulative)",
         static_cast<unsigned long long>(stats_.frames_submitted),
         static_cast<unsigned long long>(CurrentThreadCpuMicroseconds()));
  }
  presented_ = recording_;
  recording_ ^= 1;
  if (observer_) observer_->OnFrameSubmitted(frame, frames_[presented_]);
  if (game_) game_->EndFrame();
  if (frame_gpu_timer_) frame_gpu_timer_->FrameEnd();
  // The frame timing summary: this thread's CPU time and the interval between Presents.
  stats_.render_cpu_us = CurrentThreadCpuMicroseconds();
  if (last_present_time_.time_since_epoch().count() != 0 && frame_intervals_.size() < 8192)
    frame_intervals_.push_back(uint32_t(std::min<uint64_t>(us(last_present_time_, t3), UINT32_MAX)));
  last_present_time_ = t3;
  for (auto& [key, pass] : debug_pass_timers_) pass->timer.FrameEnd();
  const auto t4 = clock::now();
  stats_.present_close_us += us(t1, t2);
  stats_.present_submit_us += us(t2w, t3);
  stats_.present_end_us += us(t3, t4);
}

}  // namespace nr
