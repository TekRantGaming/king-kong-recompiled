// GameRenderer: clears, resolves, Present and the blits behind them.

#include <algorithm>
#include <cstring>

#include "backend/game_renderer.h"
#include "backend/guest_layout.h"
#include "backend/log.h"
#include "kknr/nvrhi_format.h"
#include "kknr/render_targets.h"

namespace nr {

namespace {

inline uint64_t Mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 29);
}

struct BlitConstants {
  int32_t src_rect[4];
  uint32_t channels[4];
  uint32_t flags[4];
};
static_assert(sizeof(BlitConstants) == 48);

struct ClearConstants {
  float color[4];
  float depth;
  float pad[3];
};
static_assert(sizeof(ClearConstants) == 32);

// The texture object's fetch constant (object + 16) as the GPU would see it:
// the base and mip addresses are CPU addresses in the object, physical in the
// device copy SetTexture makes (d3d-structs.md).
kknr::TextureFetch FetchFromObject(const uint8_t* object) {
  uint32_t w[6];
  for (int i = 0; i < 6; ++i) w[i] = LoadBE32(object + 16 + 4 * i);
  if (w[1] & 0xFFFFF000u) w[1] = (w[1] & 0xFFFu) | (CpuToPhysical(w[1] & 0xFFFFF000u) & 0xFFFFF000u);
  if (w[5] & 0xFFFFF000u) w[5] = (w[5] & 0xFFFu) | (CpuToPhysical(w[5] & 0xFFFFF000u) & 0xFFFFF000u);
  return kknr::TextureFetch::FromWords(w);
}

}  // namespace

// ---------------------------------------------------------------- blits

bool GameRenderer::Blit(nvrhi::ICommandList* cl, nvrhi::ITexture* source, int32_t sx, int32_t sy, int32_t w,
                        int32_t h, nvrhi::ITexture* dest, uint32_t dest_level, uint32_t dest_slice,
                        int32_t dx, int32_t dy, const uint32_t channels[4], bool gamma_ramp) {
  if (w <= 0 || h <= 0) return false;
  nvrhi::FramebufferDesc fd;
  fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(dest).setMipLevel(dest_level).setArraySlice(dest_slice));
  uint64_t fb_key = Mix64(Mix64(0xB117, reinterpret_cast<uintptr_t>(dest)), (uint64_t(dest_level) << 32) | dest_slice);
  nvrhi::IFramebuffer* fb = nullptr;
  if (auto it = framebuffers_.find(fb_key); it != framebuffers_.end()) {
    fb = it->second;
  } else {
    nvrhi::FramebufferHandle h2 = device_->createFramebuffer(fd);
    if (!h2) return false;
    framebuffers_[fb_key] = h2;
    fb = h2;
  }
  const nvrhi::Format format = dest->getDesc().format;
  const uint64_t pipe_key = Mix64(0xB1170000, uint64_t(format));
  nvrhi::GraphicsPipelineDesc pd;
  pd.primType = nvrhi::PrimitiveType::TriangleList;
  pd.VS = blit_vs_;
  pd.PS = blit_ps_;
  pd.bindingLayouts = {blit_layout_};
  pd.renderState.depthStencilState.depthTestEnable = false;
  pd.renderState.depthStencilState.depthWriteEnable = false;
  pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  pd.renderState.rasterState.scissorEnable = true;
  nvrhi::IGraphicsPipeline* pipeline = GetPipeline(pd, fb, pipe_key);
  if (!pipeline) return false;
  nvrhi::IBindingSet* set = nullptr;
  if (auto it = blit_sets_.find(source); it != blit_sets_.end()) {
    set = it->second;
  } else {
    nvrhi::BindingSetDesc bs;
    bs.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(BlitConstants)),
                   nvrhi::BindingSetItem::Texture_SRV(0, source, nvrhi::Format::UNKNOWN,
                                                      nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   nvrhi::BindingSetItem::Texture_SRV(1, gamma_ramp_)};
    nvrhi::BindingSetHandle handle = device_->createBindingSet(bs, blit_layout_);
    if (!handle) return false;
    blit_sets_[source] = handle;
    set = handle;
  }
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {set};
  gs.viewport.addViewport(nvrhi::Viewport(float(dx), float(dx + w), float(dy), float(dy + h), 0.0f, 1.0f));
  gs.viewport.addScissorRect(nvrhi::Rect(dx, dx + w, dy, dy + h));
  cl->setGraphicsState(gs);
  BlitConstants c = {{sx, sy, w, h}, {channels[0], channels[1], channels[2], channels[3]}, {gamma_ramp ? 1u : 0u, 0, 0, 0}};
  cl->setPushConstants(&c, sizeof(c));
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
  return true;
}

void GameRenderer::ClearRect(nvrhi::ICommandList* cl, HostTarget* color, HostTarget* depth,
                             const nvrhi::Rect& rect_in, const float rgba[4], bool clear_color,
                             bool clear_depth, bool clear_stencil, float z, uint32_t stencil) {
  if (!color) clear_color = false;
  if (!depth) clear_depth = clear_stencil = false;
  if (!clear_color && !clear_depth && !clear_stencil) return;
  const HostTarget* size_from = color ? color : depth;
  nvrhi::Rect rect = rect_in;
  rect.minX = std::clamp(rect.minX, 0, int(size_from->width));
  rect.maxX = std::clamp(rect.maxX, rect.minX, int(size_from->width));
  rect.minY = std::clamp(rect.minY, 0, int(size_from->height));
  rect.maxY = std::clamp(rect.maxY, rect.minY, int(size_from->height));
  if (rect.minX == rect.maxX || rect.minY == rect.maxY) return;
  ++stats_.clears;
  const bool whole = rect.minX == 0 && rect.minY == 0 && rect.maxX == int(size_from->width) &&
                     rect.maxY == int(size_from->height);
  if (whole) {
    if (clear_color)
      cl->clearTextureFloat(color->texture, nvrhi::AllSubresources, nvrhi::Color(rgba[0], rgba[1], rgba[2], rgba[3]));
    if (clear_depth || clear_stencil)
      cl->clearDepthStencilTexture(depth->texture, nvrhi::AllSubresources, clear_depth, std::clamp(z, 0.0f, 1.0f),
                                   clear_stencil, uint8_t(stencil));
    return;
  }
  // Part of the target: a triangle at the clear depth under the scissor.
  std::array<HostTarget*, 4> colors{};
  colors[0] = color;
  nvrhi::IFramebuffer* fb = GetFramebuffer(colors, depth);
  if (!fb) return;
  nvrhi::GraphicsPipelineDesc pd;
  pd.primType = nvrhi::PrimitiveType::TriangleList;
  pd.VS = clear_vs_;
  pd.PS = clear_ps_;
  pd.bindingLayouts = {clear_layout_};
  pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  pd.renderState.rasterState.scissorEnable = true;
  pd.renderState.rasterState.depthClipEnable = false;
  pd.renderState.blendState.targets[0].colorWriteMask = clear_color ? nvrhi::ColorMask::All : nvrhi::ColorMask(0);
  auto& ds = pd.renderState.depthStencilState;
  ds.depthTestEnable = clear_depth;
  ds.depthWriteEnable = clear_depth;
  ds.depthFunc = nvrhi::ComparisonFunc::Always;
  ds.stencilEnable = clear_stencil;
  ds.stencilWriteMask = 0xFF;
  ds.dynamicStencilRef = true;
  ds.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Always;
  ds.frontFaceStencil.passOp = nvrhi::StencilOp::Replace;
  ds.backFaceStencil = ds.frontFaceStencil;
  uint64_t key = Mix64(FormatSignature(colors, depth), 0xC1EA);
  key = Mix64(key, (clear_color ? 1 : 0) | (clear_depth ? 2 : 0) | (clear_stencil ? 4 : 0));
  nvrhi::IGraphicsPipeline* pipeline = GetPipeline(pd, fb, key);
  if (!pipeline) return;
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {clear_set_};
  gs.viewport.addViewport(nvrhi::Viewport(float(size_from->width), float(size_from->height)));
  gs.viewport.addScissorRect(rect);
  gs.dynamicStencilRefValue = uint8_t(stencil);
  cl->setGraphicsState(gs);
  ClearConstants c = {{rgba[0], rgba[1], rgba[2], rgba[3]}, std::clamp(z, 0.0f, 1.0f), {0, 0, 0}};
  cl->setPushConstants(&c, sizeof(c));
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
}

// ---------------------------------------------------------------- clear

void GameRenderer::Clear(nvrhi::ICommandList* cl, const ClearCall& call) {
  dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
  if (!d.valid() || !(call.color || call.depth || call.stencil)) return;
  ProcessInvalidations();
  const uint32_t pitch = d.u32(dev::kRbSurfaceInfo) & 0x3FFF;
  if (!pitch) return;
  uint32_t height = 0;
  HostTarget* colors[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(call.flags & (1u << i))) continue;
    const uint32_t surface = d.u32(dev::kRenderTargets + 4 * i);
    dev::SurfaceInfo s;
    if (!surface || !dev::DecodeSurface(memory_.Virtual(surface), false, s)) continue;
    const uint32_t info = d.u32(i == 0 ? dev::kRbColorInfo : dev::kRbColor1Info + 4 * (i - 1));
    HostTarget* existing = FindTarget(false, info & 0xFFF, pitch, (info >> 16) & 0xF);
    colors[i] = GetTarget(false, info & 0xFFF, pitch, (info >> 16) & 0xF,
                          std::max(s.height, existing ? existing->height : 0u));
    if (colors[i]) height = std::max(height, colors[i]->height);
  }
  HostTarget* depth = nullptr;
  if ((call.depth || call.stencil) && call.depth_stencil) {
    dev::SurfaceInfo s;
    if (dev::DecodeSurface(memory_.Virtual(call.depth_stencil), true, s)) {
      const uint32_t info = d.u32(dev::kRbDepthInfo);
      HostTarget* existing = FindTarget(true, info & 0xFFF, pitch, (info >> 16) & 1);
      depth = GetTarget(true, info & 0xFFF, pitch, (info >> 16) & 1,
                        std::max(s.height, existing ? existing->height : 0u));
    }
  }
  // The rectangles, or the viewport (the device's, after its clamp).
  std::vector<nvrhi::Rect> rects;
  for (uint32_t r = 0; r < call.rect_count; ++r)
    rects.emplace_back(call.rects[r][0], call.rects[r][2], call.rects[r][1], call.rects[r][3]);
  if (rects.empty()) {
    const int32_t x = int32_t(d.u32(dev::kViewport)), y = int32_t(d.u32(dev::kViewport + 4));
    const int32_t w = int32_t(d.u32(dev::kViewport + 8)), h = int32_t(d.u32(dev::kViewport + 12));
    rects.emplace_back(x, x + w, y, y + h);
  }
  float rgba[4] = {call.rgba[0], call.rgba[1], call.rgba[2], call.rgba[3]};
  if (options_.debug & 1) {
    rgba[0] = rgba[2] = 0.0f;
    rgba[1] = rgba[3] = 1.0f;
  }
  for (const nvrhi::Rect& rect : rects) {
    bool depth_done = false;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!colors[i]) continue;
      ClearRect(cl, colors[i], depth_done ? nullptr : depth, rect, rgba, true, call.depth && !depth_done,
                call.stencil && !depth_done, call.z, call.stencil_value);
      depth_done = depth != nullptr;
    }
    if (!depth_done && depth)
      ClearRect(cl, nullptr, depth, rect, call.rgba, false, call.depth, call.stencil, call.z, call.stencil_value);
  }
  if (Dump(call.frame)) {
    Logf(LogLevel::kInfo, "rexgpu-native: clear flags %X rgba %.3f %.3f %.3f %.3f z %.3f rects %zu", call.flags,
         call.rgba[0], call.rgba[1], call.rgba[2], call.rgba[3], call.z, rects.size());
  }
  (void)height;
}

// ---------------------------------------------------------------- resolve

void GameRenderer::Resolve(nvrhi::ICommandList* cl, const ResolveCall& call) {
  dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
  if (!d.valid()) return;
  ProcessInvalidations();
  ++stats_.resolves;
  const kknr::ResolveFlags flags{call.flags};
  const uint32_t pitch = d.u32(dev::kRbSurfaceInfo) & 0x3FFF;
  const bool from_depth = flags.DepthStencil();
  const uint32_t index = flags.RenderTargetIndex();
  const uint32_t surface = from_depth ? call.depth_stencil : call.render_targets[index];
  dev::SurfaceInfo s;
  if (!pitch || !surface || !dev::DecodeSurface(memory_.Virtual(surface), from_depth, s)) {
    ++stats_.resolve_failures;
    return;
  }
  const uint32_t info = from_depth ? d.u32(dev::kRbDepthInfo)
                                   : d.u32(index == 0 ? dev::kRbColorInfo : dev::kRbColor1Info + 4 * (index - 1));
  const uint32_t base = info & 0xFFF, format = from_depth ? (info >> 16) & 1 : (info >> 16) & 0xF;
  HostTarget* source = FindTarget(from_depth, base, pitch, format);
  if (!source) {
    // Never drawn: the 360 would copy whatever EDRAM held. Make it (cleared).
    source = GetTarget(from_depth, base, pitch, format, s.height);
    if (!source) {
      ++stats_.resolve_failures;
      return;
    }
  }
  int32_t sx = 0, sy = 0, w = int32_t(s.width), h = int32_t(s.height);
  if (call.has_rect) {
    sx = call.rect[0];
    sy = call.rect[1];
    w = call.rect[2] - call.rect[0];
    h = call.rect[3] - call.rect[1];
  }
  sx = std::clamp(sx, 0, int32_t(source->width));
  sy = std::clamp(sy, 0, int32_t(source->height));
  w = std::clamp(w, 0, int32_t(source->width) - sx);
  h = std::clamp(h, 0, int32_t(source->height) - sy);

  const uint8_t* dest_object = call.dest_texture ? memory_.Virtual(call.dest_texture) : nullptr;
  bool copied = false;
  if (dest_object && w > 0 && h > 0) {
    const kknr::TextureFetch fetch = FetchFromObject(dest_object);
    kknr::HostTexturePlan plan;
    if (fetch.Type() == 2 && kknr::PlanHostTexture(fetch, plan)) {
      kknr::TextureCache::Entry& entry = textures_.MarkGpuWritten(fetch, frame_);
      auto* host = static_cast<HostTexture*>(entry.host);
      if (!host || !host->render_target || host->plan.format != plan.format || host->plan.width != plan.width ||
          host->plan.height != plan.height || host->plan.levels != plan.levels || host->plan.layers != plan.layers ||
          host->plan.dimension != plan.dimension) {
        ReleaseHostTexture(host);
        host = CreateHostTexture(plan, kknr::GetHostFormatInfo(plan.format).block_size == 1);
        entry.host = host;
      }
      if (host) {
        const int32_t dx = call.has_point ? call.point[0] : 0, dy = call.has_point ? call.point[1] : 0;
        const uint32_t level = call.dest_level, slice = call.dest_slice;
        const int32_t dw = int32_t(std::max(plan.width >> level, 1u)), dh = int32_t(std::max(plan.height >> level, 1u));
        const int32_t cw = std::min(w, dw - dx), ch = std::min(h, dh - dy);
        // The render target holds the shader's red in R; a destination read
        // as A8R8G8B8 (X = blue) needs R and B exchanged.
        const uint16_t swizzle = fetch.Swizzle();
        const kknr::TextureFormat df = fetch.Format();
        const bool four = df == kknr::TextureFormat::k_8_8_8_8 || df == kknr::TextureFormat::k_8_8_8_8_A ||
                          df == kknr::TextureFormat::k_8_8_8_8_AS_16_16_16_16 ||
                          df == kknr::TextureFormat::k_2_10_10_10 ||
                          df == kknr::TextureFormat::k_2_10_10_10_AS_16_16_16_16 ||
                          df == kknr::TextureFormat::k_16_16_16_16 || df == kknr::TextureFormat::k_16_16_16_16_FLOAT;
        const bool swap = !from_depth && four && kknr::SwizzleComponent(swizzle, 0) == kknr::kSwzZ &&
                          kknr::SwizzleComponent(swizzle, 2) == kknr::kSwzX;
        if (cw > 0 && ch > 0 && slice < plan.layers && level < plan.levels) {
          if (!from_depth && !swap && host->format == source->format) {
            nvrhi::TextureSlice src_slice, dst_slice;
            src_slice.setOrigin(uint32_t(sx), uint32_t(sy)).setSize(uint32_t(cw), uint32_t(ch), 1);
            dst_slice.setOrigin(uint32_t(dx), uint32_t(dy)).setSize(uint32_t(cw), uint32_t(ch), 1)
                .setMipLevel(level)
                .setArraySlice(slice);
            cl->copyTexture(host->texture, dst_slice, source->texture, src_slice);
            copied = true;
          } else if (host->render_target) {
            uint32_t channels[4] = {0, 1, 2, 3};
            if (from_depth) {
              channels[1] = channels[2] = 4;
              channels[3] = 5;
            } else if (swap) {
              channels[0] = 2;
              channels[2] = 0;
            }
            copied = Blit(cl, source->texture, sx, sy, cw, ch, host->texture, level, slice, dx, dy, channels);
          }
          cl->setTextureState(host->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        }
      }
    }
  }
  if (!copied && dest_object) ++stats_.resolve_failures;

  // The resolve's clears, over the source rectangle.
  if (flags.ClearRenderTarget() || flags.ClearDepthStencil()) {
    const nvrhi::Rect rect(sx, sx + w, sy, sy + h);
    if (flags.ClearRenderTarget() && !from_depth) {
      const float rgba[4] = {call.has_clear_color ? call.clear_color[0] : 0.0f,
                             call.has_clear_color ? call.clear_color[1] : 0.0f,
                             call.has_clear_color ? call.clear_color[2] : 0.0f,
                             call.has_clear_color ? call.clear_color[3] : 0.0f};
      ClearRect(cl, source, nullptr, rect, rgba, true, false, false, 1.0f, 0);
    }
    if (flags.ClearDepthStencil() && call.depth_stencil) {
      dev::SurfaceInfo ds;
      if (dev::DecodeSurface(memory_.Virtual(call.depth_stencil), true, ds)) {
        const uint32_t dinfo = d.u32(dev::kRbDepthInfo);
        HostTarget* existing = FindTarget(true, dinfo & 0xFFF, pitch, (dinfo >> 16) & 1);
        HostTarget* depth = GetTarget(true, dinfo & 0xFFF, pitch, (dinfo >> 16) & 1,
                                      std::max(ds.height, existing ? existing->height : 0u));
        const float zero[4] = {0, 0, 0, 0};
        if (depth) ClearRect(cl, nullptr, depth, rect, zero, false, true, true, call.clear_z, 0);
      }
    }
  }
  if (Dump(call.frame)) {
    Logf(LogLevel::kInfo,
         "rexgpu-native: resolve flags %X %s base %u fmt %u pitch %u rect %d,%d %dx%d -> %08X %s", call.flags,
         from_depth ? "depth" : "colour", base, format, pitch, sx, sy, w, h, call.dest_texture,
         copied ? "copied" : "not copied");
  }
}

// ---------------------------------------------------------------- present

bool GameRenderer::Present(nvrhi::ICommandList* cl, uint32_t device, nvrhi::ITexture* target) {
  dev::DeviceView d(device ? memory_.Virtual(device) : nullptr);
  if (!d.valid()) return false;
  const uint32_t back_buffer = d.u32(dev::kBackBuffer);
  const uint8_t* object = back_buffer ? memory_.Virtual(back_buffer) : nullptr;
  dev::SurfaceInfo s;
  if (!object || !dev::DecodeSurface(object, false, s)) return false;
  const uint32_t pitch = s.pitch ? s.pitch : s.width;
  HostTarget* source = FindTarget(false, s.edram_base, pitch, s.format);
  if (!source) {
    ++stats_.presents_missing;
    if (stats_.presents_missing < 10) {
      Logf(LogLevel::kWarning, "rexgpu-native: Present: no host target for the back buffer %08X (base %u pitch %u format %u %ux%u)",
           back_buffer, s.edram_base, pitch, s.format, s.width, s.height);
    }
    return false;
  }
  const nvrhi::TextureDesc& td = target->getDesc();
  const uint32_t channels[4] = {0, 1, 2, 3};
  if (options_.debug & 2) cl->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(1.0f, 0.0f, 1.0f, 1.0f));
  // The display gamma ramp, when the game changed it.
  std::array<uint16_t, 768> ramp;
  for (uint32_t i = 0; i < 768; ++i) ramp[i] = LoadBE16(d.at(dev::kGammaRamp + i * 2));
  bool identity = true;
  for (uint32_t i = 0; i < 256 && identity; ++i)
    identity = ramp[i] == i * 257 && ramp[256 + i] == i * 257 && ramp[512 + i] == i * 257;
  bool all_zero = true;
  for (uint16_t v : ramp) all_zero = all_zero && v == 0;
  const bool use_ramp = !identity && !all_zero && !(options_.debug & 4);
  if (use_ramp && (!gamma_ramp_uploaded_ || ramp != gamma_ramp_values_)) {
    std::array<uint16_t, 1024> texels;
    for (uint32_t i = 0; i < 256; ++i) {
      texels[i * 4 + 0] = ramp[i];
      texels[i * 4 + 1] = ramp[256 + i];
      texels[i * 4 + 2] = ramp[512 + i];
      texels[i * 4 + 3] = 0xFFFF;
    }
    cl->writeTexture(gamma_ramp_, 0, 0, texels.data(), 256 * 8);
    cl->setTextureState(gamma_ramp_, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    gamma_ramp_values_ = ramp;
    gamma_ramp_uploaded_ = true;
    Logf(LogLevel::kInfo, "rexgpu-native: gamma ramp %u %u %u %u %u (red 0, 64, 128, 192, 255)", ramp[0], ramp[64],
         ramp[128], ramp[192], ramp[255]);
  }
  return Blit(cl, source->texture, 0, 0, int32_t(std::min(s.width, td.width)), int32_t(std::min(s.height, td.height)),
              target, 0, 0, 0, 0, channels, use_ramp);
}

}  // namespace nr
