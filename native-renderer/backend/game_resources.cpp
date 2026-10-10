// GameRenderer: render targets, framebuffers, textures, samplers and buffers.

#include <algorithm>
#include <cstring>

#include "backend/game_renderer.h"
#include "backend/log.h"
#include "kknr/buffers.h"
#include "kknr/nvrhi_format.h"
#include "kknr/resolve.h"

namespace nr {

namespace {

inline uint64_t Mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 29);
}

uint64_t TargetKey(bool depth, uint32_t base, uint32_t pitch, uint32_t format) {
  return (uint64_t(depth) << 63) | (uint64_t(format & 0xFF) << 40) | (uint64_t(pitch & 0x3FFF) << 16) |
         (base & 0xFFF);
}

}  // namespace

// ---------------------------------------------------------------- targets

nvrhi::Format GameRenderer::ColorTargetFormat(uint32_t format) {
  // xenos::ColorRenderTargetFormat -> host (docs/formats.md, render targets;
  // the 16-bit fixed formats as floats: their -32..32 range does not fit a
  // normalized host format without the scaling the shaders do not do). The
  // table is the resources library's, which its resolve tests rely on.
  return kknr::ToNvrhi(kknr::EdramColorHostFormat(format));
}

GameRenderer::HostTarget* GameRenderer::FindTarget(bool depth, uint32_t edram_base, uint32_t pitch,
                                                   uint32_t format) {
  auto it = targets_.find(TargetKey(depth, edram_base, pitch, format));
  return it == targets_.end() ? nullptr : it->second.get();
}

GameRenderer::HostTarget* GameRenderer::GetTarget(bool depth, uint32_t edram_base, uint32_t pitch,
                                                  uint32_t format, uint32_t height) {
  const uint64_t key = TargetKey(depth, edram_base, pitch, format);
  auto& slot = targets_[key];
  if (slot && slot->height >= height) return slot.get();
  // New, or taller than before (the old contents are lost; this happens once
  // per placement as the game's surfaces are first seen).
  auto t = std::make_unique<HostTarget>();
  t->key = key;
  t->depth = depth;
  t->width = pitch;
  t->height = std::max(height, slot ? slot->height : 0u);
  // Depth: 24-bit integer depth is not a render target format everywhere
  // (RADV has no D24S8), so both 360 depth formats become D32S8.
  t->format = depth ? nvrhi::Format::D32S8 : ColorTargetFormat(format);
  nvrhi::TextureDesc desc;
  desc.width = t->width;
  desc.height = t->height;
  desc.format = t->format;
  desc.isRenderTarget = true;
  desc.initialState = depth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
  desc.keepInitialState = true;
  desc.useClearValue = true;
  desc.clearValue = depth ? nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f) : nvrhi::Color(0.0f);
  char name[64];
  std::snprintf(name, sizeof(name), "EDRAM %s base %u pitch %u fmt %u", depth ? "depth" : "colour",
                edram_base, pitch, format);
  desc.debugName = name;
  t->texture = device_->createTexture(desc);
  if (!t->texture) return nullptr;
  if (slot) {
    // Framebuffers holding the old texture are dropped with it.
    framebuffers_.clear();
    blit_sets_.erase(slot->texture.Get());
  }
  slot = std::move(t);
  return slot.get();
}

uint64_t GameRenderer::FormatSignature(const std::array<HostTarget*, 4>& colors, const HostTarget* depth) {
  uint64_t h = 0x51ED27;
  for (const HostTarget* c : colors) h = Mix64(h, c ? uint64_t(c->format) + 1 : 0);
  return Mix64(h, depth ? uint64_t(depth->format) + 1 : 0);
}

nvrhi::IFramebuffer* GameRenderer::GetFramebuffer(const std::array<HostTarget*, 4>& colors,
                                                  HostTarget* depth) {
  uint64_t key = 0xFB;
  for (const HostTarget* c : colors) key = Mix64(key, reinterpret_cast<uintptr_t>(c ? c->texture.Get() : nullptr));
  key = Mix64(key, reinterpret_cast<uintptr_t>(depth ? depth->texture.Get() : nullptr));
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) return it->second;
  nvrhi::FramebufferDesc desc;
  for (const HostTarget* c : colors)
    if (c) desc.addColorAttachment(c->texture);
  if (depth) desc.setDepthAttachment(depth->texture);
  nvrhi::FramebufferHandle fb = device_->createFramebuffer(desc);
  if (!fb) return nullptr;
  framebuffers_[key] = fb;
  return fb;
}

// ---------------------------------------------------------------- textures

GameRenderer::HostTexture* GameRenderer::CreateHostTexture(const kknr::HostTexturePlan& plan,
                                                           bool render_target) {
  auto host = std::make_unique<HostTexture>();
  host->plan = plan;
  host->format = kknr::ToNvrhi(plan.format);
  host->render_target = render_target;
  if (host->format == nvrhi::Format::UNKNOWN) return nullptr;
  nvrhi::TextureDesc desc;
  desc.width = std::max(plan.width, 1u);
  desc.height = std::max(plan.height, 1u);
  desc.depth = std::max(plan.depth, 1u);
  desc.arraySize = std::max(plan.layers, 1u);
  desc.mipLevels = std::max(plan.levels, 1u);
  desc.format = host->format;
  switch (plan.dimension) {
    case kknr::Dimension::k3D:
      desc.dimension = nvrhi::TextureDimension::Texture3D;
      break;
    case kknr::Dimension::kCube:
      desc.dimension = nvrhi::TextureDimension::TextureCube;
      desc.arraySize = 6;
      break;
    default:
      desc.dimension = plan.layers > 1 ? nvrhi::TextureDimension::Texture2DArray
                                       : nvrhi::TextureDimension::Texture2D;
      break;
  }
  desc.isRenderTarget = render_target;
  desc.initialState = nvrhi::ResourceStates::ShaderResource;
  desc.keepInitialState = true;
  desc.debugName = render_target ? "Resolved texture" : "Guest texture";
  host->texture = device_->createTexture(desc);
  if (!host->texture) return nullptr;
  return host.release();
}

void GameRenderer::ReleaseHostTexture(HostTexture* host) {
  if (!host) return;
  for (const auto& [view, index] : host->views) {
    const uint32_t slot = index & 0x7FFFFFFF;
    const uint32_t dim = (view >> 12) & 3;
    Table table = kTex2D;
    if (index & 0x80000000u) table = kTex2DArray;
    else if (dim == uint32_t(kkshaders::TextureDimension::Cube)) table = kTexCube;
    else if (dim == uint32_t(kkshaders::TextureDimension::Tex3D)) table = kTex3D;
    RetireSlot(table, slot);
  }
  blit_sets_.erase(host->texture.Get());
  delete host;
}

GameRenderer::HostTexture* GameRenderer::UploadTexture(nvrhi::ICommandList* cl,
                                                       kknr::TextureCache::Entry& entry,
                                                       const kknr::TextureFetch& fetch) {
  kknr::HostTextureData data;
  std::string why;
  if (!kknr::ConvertTexture(fetch, physical_, data, &why, TextureOptions())) {
    ++stats_.texture_failures;
    if (stats_.texture_failures < 50) {
      Logf(LogLevel::kWarning, "rexgpu-native: texture %08X %08X %08X not converted: %s", fetch.words[0],
           fetch.words[1], fetch.words[2], why.c_str());
    }
    return nullptr;
  }
  auto* host = static_cast<HostTexture*>(entry.host);
  const kknr::HostTexturePlan& p = data.plan;
  if (!host || host->plan.format != p.format || host->plan.width != p.width || host->plan.height != p.height ||
      host->plan.depth != p.depth || host->plan.layers != p.layers || host->plan.levels != p.levels ||
      host->plan.dimension != p.dimension) {
    ReleaseHostTexture(host);
    host = CreateHostTexture(p, false);
    entry.host = host;
    if (!host) return nullptr;
  }
  for (const kknr::HostSubresource& sub : data.subresources) {
    cl->writeTexture(host->texture, sub.layer, sub.level, data.bytes.data() + sub.offset, sub.row_pitch,
                     sub.depth_pitch);
  }
  cl->setTextureState(host->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  textures_.OnUploaded(entry, physical_);
  ++stats_.texture_uploads;
  return host;
}

uint32_t GameRenderer::TextureView(HostTexture* host, const kknr::TextureFetch& fetch,
                                   kkshaders::TextureDimension dimension, bool base_map) {
  kknr::HostTexturePlan plan;
  if (!kknr::PlanHostTexture(fetch, plan, nullptr, TextureOptions())) plan = host->plan;
  const uint32_t levels = std::max(host->plan.levels, 1u);
  uint32_t lo = std::max(fetch.MipMinLevel(), host->plan.min_level);
  uint32_t hi = std::min(fetch.MipMaxLevel(), levels - 1);
  lo = std::min(lo, levels - 1);
  if (hi < lo || base_map) hi = lo;
  const uint32_t key = uint32_t(plan.view_swizzle & 0xFFF) | (uint32_t(dimension) & 3) << 12 | (lo & 15) << 14 |
                       (hi & 15) << 18;
  auto it = host->views.find(key);
  if (it != host->views.end()) return it->second;

  const bool cube = host->plan.dimension == kknr::Dimension::kCube;
  const bool volume = host->plan.dimension == kknr::Dimension::k3D;
  const bool array = !cube && !volume && host->plan.layers > 1;
  Table table;
  nvrhi::TextureDimension view_dim;
  uint32_t flag = 0;
  switch (dimension) {
    case kkshaders::TextureDimension::Cube:
      if (!cube) return 0;
      table = kTexCube;
      view_dim = nvrhi::TextureDimension::TextureCube;
      break;
    case kkshaders::TextureDimension::Tex3D:
      if (volume) {
        table = kTex3D;
        view_dim = nvrhi::TextureDimension::Texture3D;
      } else if (array) {
        table = kTex2DArray;
        view_dim = nvrhi::TextureDimension::Texture2DArray;
        flag = 0x80000000u;
      } else {
        return 0;
      }
      break;
    default:
      if (cube || volume) return 0;
      if (array) {
        table = kTex2DArray;
        view_dim = nvrhi::TextureDimension::Texture2DArray;
        flag = 0x80000000u;
      } else {
        table = kTex2D;
        view_dim = nvrhi::TextureDimension::Texture2D;
      }
      break;
  }
  const uint32_t slot = AllocateSlot(table);
  if (!slot) return 0;
  nvrhi::TextureSubresourceSet subresources(lo, hi - lo + 1, 0, nvrhi::TextureSubresourceSet::AllArraySlices);
  WriteSlot(table, nvrhi::BindingSetItem::Texture_SRV(slot, host->texture, nvrhi::Format::UNKNOWN, subresources,
                                                      view_dim, kknr::ToNvrhiMapping(plan.view_swizzle)));
  const uint32_t index = slot | flag;
  host->views[key] = index;
  return index;
}

uint32_t GameRenderer::BindTexture(nvrhi::ICommandList* cl, const uint32_t words[6],
                                   kkshaders::TextureDimension dimension, bool base_map) {
  const kknr::TextureFetch fetch = kknr::TextureFetch::FromWords(words);
  if (fetch.Type() != 2) return 0;
  textures_.options = TextureOptions();  // ranges are computed with it when an entry is made
  kknr::TextureCache::BindResult r = textures_.Bind(fetch, physical_, frame_);
  if (!r.entry) return 0;
  auto* host = static_cast<HostTexture*>(r.entry->host);
  if (r.action != kknr::BindAction::kUseExisting || !host) {
    if (r.entry->gpu_written && host && r.action == kknr::BindAction::kUseExisting) {
      // The resolve's copy is the truth.
    } else {
      host = UploadTexture(cl, *r.entry, fetch);
      if (!host) return 0;
    }
  }
  cl->setTextureState(host->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  return TextureView(host, fetch, dimension, base_map);
}

// ---------------------------------------------------------------- samplers

uint32_t GameRenderer::BindSampler(const uint32_t words[6], const kkshaders::SamplerBinding& b) {
  const kknr::TextureFetch f = kknr::TextureFetch::FromWords(words);
  const uint32_t mag = b.magFilter == 3 ? f.MagFilter() : b.magFilter;
  const uint32_t min = b.minFilter == 3 ? f.MinFilter() : b.minFilter;
  const uint32_t mip = b.mipFilter == 3 ? f.MipFilter() : b.mipFilter;
  const uint32_t aniso = b.anisoFilter == 7 ? f.AnisoFilter() : b.anisoFilter;
  const uint32_t key = f.ClampX() | f.ClampY() << 3 | f.ClampZ() << 6 | (mag & 3) << 9 | (min & 3) << 11 |
                       (mip & 3) << 13 | (aniso & 7) << 15 | (f.BorderColor() & 3) << 18 |
                       (uint32_t(f.LodBiasFixed()) & 0x3FF) << 20;
  auto it = samplers_.find(key);
  if (it != samplers_.end()) return it->second.second;
  auto address = [](uint32_t clamp) {
    switch (clamp) {
      case 0: return nvrhi::SamplerAddressMode::Wrap;
      case 1: return nvrhi::SamplerAddressMode::Mirror;
      case 2: return nvrhi::SamplerAddressMode::Clamp;
      case 3:
      case 5:
      case 7: return nvrhi::SamplerAddressMode::MirrorOnce;
      case 4: return nvrhi::SamplerAddressMode::Clamp;  // clamp half way to the border
      default: return nvrhi::SamplerAddressMode::Border;
    }
  };
  nvrhi::SamplerDesc sd;
  sd.addressU = address(f.ClampX());
  sd.addressV = address(f.ClampY());
  sd.addressW = address(f.ClampZ());
  sd.magFilter = (mag & 1) != 0;
  sd.minFilter = (min & 1) != 0;
  sd.mipFilter = (mip & 3) == 1;
  if (aniso >= 2 && aniso <= 5 && sd.minFilter) sd.maxAnisotropy = float(1u << (aniso - 1));
  sd.mipBias = float(f.LodBiasFixed()) / 32.0f;
  sd.borderColor = f.BorderColor() == 1 ? nvrhi::Color(1.0f) : nvrhi::Color(0.0f);
  nvrhi::SamplerHandle sampler = device_->createSampler(sd);
  uint32_t slot = 0;
  if (sampler) {
    slot = AllocateSlot(kSamplers);
    if (slot) WriteSlot(kSamplers, nvrhi::BindingSetItem::Sampler(slot, sampler));
  }
  samplers_[key] = {sampler, slot};
  return slot;
}

// ---------------------------------------------------------------- buffers

GameRenderer::HostBuffer* GameRenderer::BindVertexBuffer(nvrhi::ICommandList* cl, uint32_t physical,
                                                         uint32_t bytes) {
  if (!bytes) return nullptr;
  const uint8_t* data = physical_.At(physical, bytes);
  if (!data) return nullptr;
  kknr::BufferCache::BindResult r = vertex_buffers_.Bind(physical, bytes, 0, physical_, frame_);
  auto* host = static_cast<HostBuffer*>(r.entry->host);
  if (!host) {
    host = new HostBuffer();
    nvrhi::BufferDesc d;
    d.byteSize = (uint64_t(bytes) + 15) & ~uint64_t(15);
    d.canHaveRawViews = true;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = "Guest vertex data";
    host->buffer = device_->createBuffer(d);
    host->bytes = bytes;
    if (host->buffer) {
      host->slot = AllocateSlot(kBuffers);
      if (host->slot) WriteSlot(kBuffers, nvrhi::BindingSetItem::RawBuffer_SRV(host->slot, host->buffer));
    }
    r.entry->host = host;
    r.action = kknr::BindAction::kCreateAndUpload;
  }
  if (!host->buffer || !host->slot) return nullptr;
  if (r.action != kknr::BindAction::kUseExisting) {
    cl->writeBuffer(host->buffer, data, (uint64_t(bytes) + 3) & ~uint64_t(3));
    vertex_buffers_.OnUploaded(*r.entry, physical_);
    ++stats_.buffer_uploads;
  }
  cl->setBufferState(host->buffer, nvrhi::ResourceStates::ShaderResource);
  return host;
}

GameRenderer::HostBuffer* GameRenderer::BindIndexBuffer(nvrhi::ICommandList* cl, uint32_t physical,
                                                        uint32_t bytes, bool index32) {
  if (!bytes) return nullptr;
  const uint8_t* data = physical_.At(physical, bytes);
  if (!data) return nullptr;
  kknr::BufferCache::BindResult r =
      index_buffers_.Bind(physical, bytes, kknr::IndexConversionId(index32), physical_, frame_);
  auto* host = static_cast<HostBuffer*>(r.entry->host);
  if (!host) {
    host = new HostBuffer();
    nvrhi::BufferDesc d;
    d.byteSize = (uint64_t(bytes) + 15) & ~uint64_t(15);
    d.isIndexBuffer = true;
    d.initialState = nvrhi::ResourceStates::IndexBuffer;
    d.keepInitialState = true;
    d.debugName = "Guest indices";
    host->buffer = device_->createBuffer(d);
    host->bytes = bytes;
    r.entry->host = host;
    r.action = kknr::BindAction::kCreateAndUpload;
  }
  if (!host->buffer) return nullptr;
  if (r.action != kknr::BindAction::kUseExisting) {
    const uint32_t size = index32 ? 4 : 2;
    const uint32_t count = bytes / size;
    std::vector<uint8_t> converted((size_t(bytes) + 3) & ~size_t(3), 0);
    kknr::ConvertIndices(data, converted.data(), count, index32);
    cl->writeBuffer(host->buffer, converted.data(), converted.size());
    index_buffers_.OnUploaded(*r.entry, physical_);
    ++stats_.buffer_uploads;
  }
  return host;
}

}  // namespace nr
