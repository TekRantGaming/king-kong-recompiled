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

bool GameRenderer::MigrateMemoryDepth(nvrhi::ICommandList* cl, nvrhi::ITexture* from, nvrhi::ITexture* to) {
  nvrhi::FramebufferDesc fd;
  fd.setDepthAttachment(to);
  auto fb = device_->createFramebuffer(fd);
  nvrhi::BindingSetDesc bd;
  bd.bindings = {nvrhi::BindingSetItem::PushConstants(0, 16),
                 nvrhi::BindingSetItem::Texture_SRV(0, from),
                 nvrhi::BindingSetItem::Texture_SRV(1, from, nvrhi::Format::X32G8_UINT)};
  auto set = device_->createBindingSet(bd, memory_depth_layout_);
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
  RetireResource(fb);
  RetireResource(set);
  return true;
}

void GameRenderer::ApplyMemoryScale(nvrhi::ICommandList* cl) {
  const auto started = std::chrono::steady_clock::now();
  const float factor = pending_memory_scale_;
  pending_memory_scale_ = 0;
  if (!memory_migration_ready_ || upscale_ || factor <= 0 || factor >= 1) return;
  const auto live = options_.memory_budget ? options_.memory_budget() : MemoryBudget{};
  uint64_t cap = 0;
  if (const char* v = std::getenv("KK_DEV_VRAM_BUDGET_GB")) {
    if (std::strcmp(v, "4") == 0) cap = 4ull << 30;
    else if (std::strcmp(v, "6") == 0) cap = 6ull << 30;
    else if (std::strcmp(v, "8") == 0) cap = 8ull << 30;
  }
  const auto limit = CapMemoryBudget(live, cap);
  if (!limit.valid || limit.usage >= limit.budget) return;
  for (auto& [key, t] : targets_)
    if (t && t->texture && !t->depth && !MemoryColorPipeline(t->format)) return;
  const float old_x = options_.render_scale_x, old_y = options_.render_scale_y, old_shadow = options_.shadow_scale;
  options_.render_scale_x = ReducedMemoryScale(options_.render_scale_x, memory_floor_x_, factor);
  options_.render_scale_y = ReducedMemoryScale(options_.render_scale_y, memory_floor_y_, factor);
  if (options_.shadow_scale > 1) options_.shadow_scale = std::max(1.0f, options_.shadow_scale * factor);
  auto rollback = [&] {
    options_.render_scale_x = old_x; options_.render_scale_y = old_y; options_.shadow_scale = old_shadow;
    // A failed colour migration may already have cached its temporary FB.
    for (auto& [key, fb] : framebuffers_) RetireResource(fb);
    framebuffers_.clear(); framebuffer_used_.clear();
  };
  struct Replacement { HostTarget* target; Scale scale; uint32_t width, height; nvrhi::TextureHandle texture; uint64_t bytes = 0; };
  std::vector<Replacement> replacements;
  uint64_t estimate = 0;
  for (auto& [key, t] : targets_) {
    if (!t) continue;
    const auto scale = ScaleFor(t->depth, t->width, uint32_t((key >> 40) & 0xFF));
    const uint32_t w = uint32_t(std::max(ScaleCoord(int32_t(t->width), scale.x), 1));
    const uint32_t h = uint32_t(std::max(ScaleCoord(int32_t(t->height), scale.y), 1));
    if (w == t->host_width && h == t->host_height) continue;
    replacements.push_back({t.get(), scale, w, h, {}});
    if (t->texture) estimate += uint64_t(double(t->allocation_bytes) * double(w) * h /
                                       (double(t->host_width) * t->host_height)) + (64ull << 10);
  }
  // Use actual driver headroom for the temporary overlap, including in fault
  // injection runs. Never flush/wait for memory, nor destroy old pictures first.
  if (estimate + (64ull << 20) > limit.budget - limit.usage) {
    rollback();
    Logf(LogLevel::kWarning, "rexgpu-native: memory scale deferred: temporary migration headroom unavailable");
    return;
  }
  for (auto& r : replacements) {
    auto& t = *r.target;
    if (!t.texture) continue;  // clear-only metadata stays virtual at its new size
    auto d = t.texture->getDesc();
    d.width = r.width; d.height = r.height;
    r.texture = device_->createTexture(d);
    if (!r.texture) { rollback(); return; }
    ++frame_resource_creations_;
    r.bytes = device_->getTextureMemoryRequirements(r.texture).size;
  }
  const uint32_t channels[4] = {0, 1, 2, 3};
  for (auto& r : replacements) {
    auto& t = *r.target;
    if (!r.texture) continue;
    if (t.clear_only) {
      if (t.depth) cl->clearDepthStencilTexture(r.texture, nvrhi::AllSubresources, true, t.saved_depth, true, t.saved_stencil);
      else cl->clearTextureFloat(r.texture, nvrhi::AllSubresources, t.saved_color);
    } else {
      const bool copied = t.depth ? MigrateMemoryDepth(cl, t.texture, r.texture) :
          Blit(cl, t.texture, 0, 0, int32_t(t.host_width), int32_t(t.host_height), r.texture,
               0, 0, 0, 0, channels, 0, 0, int32_t(r.width), int32_t(r.height));
      if (!copied) { rollback(); return; }
    }
  }
  // Commit only after all migrations are recorded. The same queue orders old
  // writes, these reads/copies, then this frame's draws into the new targets.
  for (auto& [key, fb] : framebuffers_) RetireResource(fb);
  framebuffers_.clear(); framebuffer_used_.clear();
  for (auto& r : replacements) {
    auto& t = *r.target;
    if (t.texture) {
      if (auto it = blit_sets_.find(t.texture.Get()); it != blit_sets_.end()) {
        RetireResource(it->second); blit_sets_.erase(it);
      }
      RetireResource(t.texture);
      stats_.edram_target_bytes -= t.allocation_bytes;
      t.texture = r.texture;
      t.allocation_bytes = r.bytes;
      stats_.edram_target_bytes += r.bytes;
    }
    t.scale_x = r.scale.x; t.scale_y = r.scale.y;
    t.host_width = r.width; t.host_height = r.height;
  }
  if (scene_ao_) scene_ao_->ForgetTargets();
  ++memory_scale_steps_;
  Logf(LogLevel::kWarning, "rexgpu-native: GPU budget pressure: render scale %.3f x %.3f -> %.3f x %.3f; contents migrated at frame %llu (%.3f ms CPU)",
       old_x, old_y, options_.render_scale_x, options_.render_scale_y, static_cast<unsigned long long>(frame_),
       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}
}  // namespace nr
