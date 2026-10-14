// Budget-driven target resizing. All old contents survive until every new
// target has been allocated and every migration has been recorded successfully.
#include "backend/game_renderer.h"
#include "backend/log.h"
#include "backend/shaders.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace nr {
namespace {
uint64_t MemoryBlitKey(nvrhi::Format format) {
  uint64_t h = 0xB1170000, v = uint64_t(format);
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 29);
}
}

nvrhi::IGraphicsPipeline* GameRenderer::MemoryColorPipeline(nvrhi::Format format) {
  nvrhi::GraphicsPipelineDesc d;
  d.primType = nvrhi::PrimitiveType::TriangleList;
  d.VS = blit_vs_; d.PS = blit_ps_;
  d.bindingLayouts = {blit_layout_};
  d.renderState.depthStencilState.depthTestEnable = false;
  d.renderState.depthStencilState.depthWriteEnable = false;
  d.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  d.renderState.rasterState.scissorEnable = true;
  nvrhi::FramebufferInfo info;
  info.colorFormats.push_back(format);
  return GetPipeline(d, info, MemoryBlitKey(format), true, true);
}

bool GameRenderer::InitializeMemoryMigration() {
  auto code = GetShaderBytecode(device_->getGraphicsAPI(), ShaderId::kMemoryDepthPs);
  nvrhi::ShaderDesc sd;
  sd.shaderType = nvrhi::ShaderType::Pixel;
  sd.entryName = "ps_main";
  memory_depth_ps_ = device_->createShader(sd, code.data, code.size);
  nvrhi::BindingLayoutDesc layout;
  layout.visibility = nvrhi::ShaderType::All;
  layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, 16),
                     nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1)};
  memory_depth_layout_ = device_->createBindingLayout(layout);
  if (!memory_depth_ps_ || !memory_depth_layout_) return false;
  nvrhi::FramebufferInfo info;
  info.depthFormat = nvrhi::Format::D32S8;
  for (unsigned pass = 0; pass < 9; ++pass) {
    nvrhi::GraphicsPipelineDesc d;
    d.primType = nvrhi::PrimitiveType::TriangleList;
    d.VS = blit_vs_;
    d.PS = memory_depth_ps_;
    d.bindingLayouts = {memory_depth_layout_};
    d.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    auto& ds = d.renderState.depthStencilState;
    ds.depthTestEnable = ds.depthWriteEnable = pass == 0;
    ds.depthFunc = nvrhi::ComparisonFunc::Always;
    ds.stencilEnable = pass != 0;
    ds.stencilWriteMask = pass ? uint8_t(1u << (pass - 1)) : 0;
    ds.dynamicStencilRef = true;
    ds.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Always;
    ds.frontFaceStencil.passOp = nvrhi::StencilOp::Replace;
    ds.backFaceStencil = ds.frontFaceStencil;
    // All nine pipelines are made at initialization, never during pressure.
    memory_depth_pipelines_[pass] = CreatePipeline(d, info);
    if (!memory_depth_pipelines_[pass]) return false;
  }
  return true;
}

bool GameRenderer::MigrateMemoryDepth(nvrhi::ICommandList* cl, nvrhi::ITexture* from, nvrhi::ITexture* to,
                                      nvrhi::IFramebuffer* fb, nvrhi::IBindingSet* set) {
  if (!fb || !set) return false;
  const auto& a = from->getDesc();
  const auto& b = to->getDesc();
  struct Params { float ratio[2]; uint32_t mask, pad; } p{{float(a.width) / b.width, float(a.height) / b.height}, 0, 0};
  cl->clearDepthStencilTexture(to, nvrhi::AllSubresources, true, 1.0f, true, 0);
  for (unsigned pass = 0; pass < 9; ++pass) {
    nvrhi::GraphicsState state;
    state.pipeline = memory_depth_pipelines_[pass];
    state.framebuffer = fb;
    state.bindings = {set};
    state.dynamicStencilRefValue = 0xFF;
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(b.width), float(b.height)));
    cl->setGraphicsState(state);
    p.mask = pass ? 1u << (pass - 1) : 0;
    cl->setPushConstants(&p, sizeof(p));
    nvrhi::DrawArguments draw;
    draw.vertexCount = 3;
    cl->draw(draw);
  }
  return true;
}

void GameRenderer::ApplyMemoryScale(nvrhi::ICommandList* cl) {
  const auto started = std::chrono::steady_clock::now();
  const float factor = pending_memory_scale_;
  pending_memory_scale_ = 0;
  if (!memory_migration_ready_ || upscale_) { memory_staged_.clear(); return; }
  const auto live = options_.memory_budget ? options_.memory_budget() : MemoryBudget{};
  uint64_t cap = 0;
  if (const char* v = std::getenv("KK_DEV_VRAM_BUDGET_GB")) {
    if (std::strcmp(v, "4") == 0) cap = 4ull << 30;
    else if (std::strcmp(v, "6") == 0) cap = 6ull << 30;
    else if (std::strcmp(v, "8") == 0) cap = 8ull << 30;
  }
  const auto limit = CapMemoryBudget(live, cap);
  if (!limit.valid || limit.usage >= limit.budget) { memory_staged_.clear(); return; }
  if (memory_staged_.empty()) {
    if (factor <= 0 || factor >= 1) return;
    for (auto& [key, t] : targets_)
      if (t && t->texture && !t->depth && !MemoryColorPipeline(t->format)) return;
    const std::array<float, 3> old{options_.render_scale_x, options_.render_scale_y, options_.shadow_scale};
    float step = factor;
    bool fits = false;
    for (unsigned attempt = 0; attempt < 9; ++attempt) {
      memory_staged_.clear();
      options_.render_scale_x = ReducedMemoryScale(old[0], memory_floor_x_, step);
      options_.render_scale_y = ReducedMemoryScale(old[1], memory_floor_y_, step);
      options_.shadow_scale = old[2] > 1 ? std::max(1.0f, old[2] * step) : old[2];
      memory_staged_options_ = {options_.render_scale_x, options_.render_scale_y, options_.shadow_scale};
      uint64_t estimate = 0;
      for (auto& [key, t] : targets_) {
        if (!t) continue;
        MemoryReplacement r;
        r.key = key; r.source = t->texture; r.old_width = t->host_width; r.old_height = t->host_height;
        r.scale = ScaleFor(t->depth, t->width, uint32_t((key >> 40) & 0xFF));
        r.width = uint32_t(std::max(ScaleCoord(int32_t(t->width), r.scale.x), 1));
        r.height = uint32_t(std::max(ScaleCoord(int32_t(t->height), r.scale.y), 1));
        if (r.source && (r.width != r.old_width || r.height != r.old_height))
          estimate += uint64_t(double(t->allocation_bytes) * double(r.width) * r.height /
                               (double(r.old_width) * r.old_height)) + (64ull << 10);
        memory_staged_.push_back(std::move(r));
      }
      options_.render_scale_x = old[0]; options_.render_scale_y = old[1]; options_.shadow_scale = old[2];
      if (estimate + (64ull << 20) <= limit.budget - limit.usage) { fits = true; break; }
      step *= 0.75f;  // A larger reduction may fit when the first step cannot.
    }
    if (!fits || memory_staged_.empty()) {
      memory_staged_.clear();
      Logf(LogLevel::kWarning, "rexgpu-native: memory scale deferred: temporary migration headroom unavailable");
      return;
    }
    memory_staged_index_ = 0;
    memory_staged_target_count_ = targets_.size();
    memory_staged_first_frame_ = frame_;
  }
  // No HostTarget pointer survives across frames. Reject a changed cache or
  // replaced source before using any candidate; new work keeps the old scale.
  size_t present = 0;
  for (auto& [key, t] : targets_) present += bool(t);
  if (targets_.size() != memory_staged_target_count_ || present != memory_staged_.size()) {
    memory_staged_.clear(); return;
  }
  for (const auto& r : memory_staged_) {
    auto it = targets_.find(r.key);
    if (it == targets_.end() || !it->second || it->second->texture.Get() != r.source.Get() ||
        it->second->host_width != r.old_width || it->second->host_height != r.old_height) {
      memory_staged_.clear(); return;
    }
  }
  // At most one texture, framebuffer and binding set are allocated per frame.
  // Until all candidates are ready, every draw continues using the old targets.
  while (memory_staged_index_ < memory_staged_.size()) {
    auto& r = memory_staged_[memory_staged_index_++];
    if (!r.source || (r.width == r.old_width && r.height == r.old_height)) continue;
    auto& t = *targets_.at(r.key);
    auto d = r.source->getDesc(); d.width = r.width; d.height = r.height;
    r.texture = device_->createTexture(d);
    if (!r.texture) { memory_staged_.clear(); return; }
    ++frame_resource_creations_;
    r.bytes = device_->getTextureMemoryRequirements(r.texture).size;
    nvrhi::FramebufferDesc fd;
    if (t.depth) fd.setDepthAttachment(r.texture); else fd.addColorAttachment(r.texture);
    r.framebuffer = device_->createFramebuffer(fd);
    nvrhi::BindingSetDesc bd;
    if (t.depth) {
      bd.bindings = {nvrhi::BindingSetItem::PushConstants(0, 16),
                     nvrhi::BindingSetItem::Texture_SRV(0, r.source),
                     nvrhi::BindingSetItem::Texture_SRV(1, r.source, nvrhi::Format::X32G8_UINT)};
    } else {
      bd.bindings = {nvrhi::BindingSetItem::PushConstants(0, 48),
                     nvrhi::BindingSetItem::Texture_SRV(0, r.source, nvrhi::Format::UNKNOWN,
                                                      nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                     nvrhi::BindingSetItem::Texture_SRV(1, gamma_ramp_)};
    }
    r.bindings = device_->createBindingSet(bd, t.depth ? memory_depth_layout_ : blit_layout_);
    if (!r.framebuffer || !r.bindings) { memory_staged_.clear(); return; }
    return;
  }
  const std::array<float, 3> old{options_.render_scale_x, options_.render_scale_y, options_.shadow_scale};
  options_.render_scale_x = memory_staged_options_[0]; options_.render_scale_y = memory_staged_options_[1];
  options_.shadow_scale = memory_staged_options_[2];
  for (const auto& r : memory_staged_) {
    const auto& t = *targets_.at(r.key);
    const auto expected = ScaleFor(t.depth, t.width, uint32_t((r.key >> 40) & 0xFF));
    if (expected.x != r.scale.x || expected.y != r.scale.y) {
      options_.render_scale_x = old[0]; options_.render_scale_y = old[1]; options_.shadow_scale = old[2];
      memory_staged_.clear(); return;
    }
  }
  const uint32_t channels[4] = {0, 1, 2, 3};
  for (auto& r : memory_staged_) {
    auto& t = *targets_.at(r.key);
    if (!r.texture) continue;
    bool copied = true;
    // AO can write outside GetFramebuffer, leaving an old clear hint behind.
    // Its targets must migrate the actual image, exactly as for eviction.
    if (t.clear_only && !scene_ao_) {
      if (t.depth) cl->clearDepthStencilTexture(r.texture, nvrhi::AllSubresources, true, t.saved_depth, true, t.saved_stencil);
      else cl->clearTextureFloat(r.texture, nvrhi::AllSubresources, t.saved_color);
    } else {
      copied = t.depth ? MigrateMemoryDepth(cl, t.texture, r.texture, r.framebuffer, r.bindings) :
          Blit(cl, t.texture, 0, 0, int32_t(t.host_width), int32_t(t.host_height), r.texture,
               0, 0, 0, 0, channels, 0, 0, int32_t(r.width), int32_t(r.height), r.framebuffer, r.bindings);
    }
    if (!copied) {
      options_.render_scale_x = old[0]; options_.render_scale_y = old[1]; options_.shadow_scale = old[2];
      for (auto& candidate : memory_staged_) {
        RetireResource(candidate.texture); RetireResource(candidate.framebuffer); RetireResource(candidate.bindings);
      }
      memory_staged_.clear(); return;
    }
  }
  for (auto& [key, fb] : framebuffers_) RetireResource(fb);
  framebuffers_.clear(); framebuffer_used_.clear();
  for (auto& r : memory_staged_) {
    auto& t = *targets_.at(r.key);
    if (r.texture) {
      if (auto it = blit_sets_.find(t.texture.Get()); it != blit_sets_.end()) {
        RetireResource(it->second); blit_sets_.erase(it);
      }
      RetireResource(t.texture); RetireResource(r.framebuffer); RetireResource(r.bindings);
      stats_.edram_target_bytes -= t.allocation_bytes;
      t.texture = r.texture; t.allocation_bytes = r.bytes; stats_.edram_target_bytes += r.bytes;
    }
    t.scale_x = r.scale.x; t.scale_y = r.scale.y; t.host_width = r.width; t.host_height = r.height;
  }
  memory_staged_.clear();
  if (scene_ao_) {
    scene_ao_->ForgetTargets();
    // Keep old bindless descriptors valid for pending frames, but force a new
    // slot on the next AO image instead of comparing potentially reused pointers.
    RetireSlot(kTex2D, ao_slot_); ao_slot_ = 0; ao_slot_texture_ = nullptr;
    ao_constants_valid_ = false;
  }
  ++memory_scale_steps_;
  Logf(LogLevel::kWarning, "rexgpu-native: GPU budget pressure: render scale %.3f x %.3f -> %.3f x %.3f; contents migrated at frame %llu (%.3f ms CPU), staged from frame %llu",
       old[0], old[1], options_.render_scale_x, options_.render_scale_y, static_cast<unsigned long long>(frame_),
       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
       static_cast<unsigned long long>(memory_staged_first_frame_));
}
}  // namespace nr
