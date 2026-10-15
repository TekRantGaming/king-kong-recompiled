// GameRenderer: render targets, framebuffers, textures, samplers and buffers.

#include <algorithm>
#include <chrono>
#include <cstring>

#include "backend/game_renderer.h"
#include "backend/log.h"
#include "backend/texture_workers.h"
#include "backend/upload_batch.h"
#include "backend/resource_upload_workers.h"
#include "kknr/buffers.h"
#include "kknr/resident_snapshot.h"
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

// Adds the scope's time to a stats counter (milliseconds).
struct MsTimer {
  explicit MsTimer(double& total) : total(total) {}
  ~MsTimer() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
  double& total;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

}  // namespace

// ---------------------------------------------------------------- targets

uint64_t GameRenderer::TargetKeyOf(bool depth, uint32_t edram_base, uint32_t pitch, uint32_t format) {
  return TargetKey(depth, edram_base, pitch, format);
}

nvrhi::Format GameRenderer::ColorTargetFormat(uint32_t format) {
  // xenos::ColorRenderTargetFormat -> host (docs/formats.md, render targets;
  // the 16-bit fixed formats as floats: their -32..32 range does not fit a
  // normalized host format without the scaling the shaders do not do). The
  // table is the resources library's, which its resolve tests rely on.
  return kknr::ToNvrhi(kknr::EdramColorHostFormat(format));
}

GameRenderer::HostTarget* GameRenderer::FindTarget(bool depth, uint32_t edram_base, uint32_t pitch,
                                                   uint32_t format, int generation) {
  const bool out = upscale_ && (generation < 0 ? OutputGeneration() : generation == 1);
  auto it = targets_.find(TargetKey(depth, edram_base, pitch, format) | (out ? kOutputGeneration : 0));
  if (it == targets_.end() || !it->second || !RestoreTarget(*it->second)) return nullptr;
  // Upscaler generations use used_frame to mean GetTarget actually selected
  // the attachment, not a FindTarget probe for height or a stale resolve.
  if (!upscale_) it->second->used_frame = frame_;
  return it->second.get();
}

bool GameRenderer::RestoreTarget(HostTarget& t) {
  if (t.texture) return true;
  if (!t.clear_only || !current_cl_) return false;
  nvrhi::TextureDesc d;
  d.width = t.host_width;
  d.height = t.host_height;
  d.format = t.format;
  d.isRenderTarget = true;
  d.initialState = t.depth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
  d.keepInitialState = true;
  d.debugName = "Restored clear-only EDRAM target";
  auto texture = device_->createTexture(d);
  if (!texture) return false;
  if (t.depth)
    current_cl_->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, t.saved_depth, true, t.saved_stencil);
  else
    current_cl_->clearTextureFloat(texture, nvrhi::AllSubresources, t.saved_color);
  t.texture = texture;
  t.allocation_bytes = device_->getTextureMemoryRequirements(texture).size;
  stats_.edram_target_bytes += t.allocation_bytes;
  ++memory_restores_;
  ++frame_resource_creations_;
  return true;
}

GameRenderer::Scale GameRenderer::ScaleFor(bool depth, uint32_t pitch, uint32_t format, bool output_gen) {
  // Shadow maps: k_32_FLOAT colour targets that are not the frame's size, and the depth
  // buffers of that pitch (every attachment of a draw has one size).
  if (!depth && format == 14 && pitch != options_.guest_frame_width) shadow_pitches_.insert(pitch);
  if (options_.shadow_scale > 0.0f && shadow_pitches_.count(pitch)) return {options_.shadow_scale, options_.shadow_scale};
  // The soft shadows (docs/lighting.md): the screen-space shadow mask's targets, finer than the frame.
  if (!lighting_mask_pitches_.empty() && lighting_mask_pitches_.count(pitch))
    return {options_.render_scale_x * kLightingMaskScale, options_.render_scale_y * kLightingMaskScale};
  // --native_debug bit 32: only the frame-wide targets scale (to find a pass that breaks).
  if ((options_.debug & 32) && pitch != options_.guest_frame_width) return {1.0f, 1.0f};
  // The census identifies these pitches as the game's 320x180 and 640x360/480 effect
  // surfaces, including their matching depth attachments. Shadow maps were handled above.
  // Use one scale on both axes so a non-16:9 output cannot change an effect's aspect.
  if ((pitch == 320 || pitch == 640) && !shadow_pitches_.count(pitch) && (depth || format != 14)) {
    const float scale = options_.effect_buffers ? std::min(options_.render_scale_x, options_.render_scale_y) : 1.0f;
    return {scale, scale};
  }
  // Upscalers: the scene's targets are the input size before the upscale point; shadow maps keep the
  // output's scale (their resolution is not the upscaler's to recover).
  if (upscale_ && !output_gen && !shadow_pitches_.count(pitch)) return {upscale_->in_x, upscale_->in_y};
  return {options_.render_scale_x, options_.render_scale_y};
}

GameRenderer::HostTarget* GameRenderer::GetTarget(bool depth, uint32_t edram_base, uint32_t pitch,
                                                  uint32_t format, uint32_t height, int generation) {
  MsTimer timer(stats_.target_ms);
  const bool out = upscale_ && (generation < 0 ? OutputGeneration() : generation == 1);
  const uint64_t key = TargetKey(depth, edram_base, pitch, format) | (out ? kOutputGeneration : 0);
  auto& slot = targets_[key];
  if (slot && slot->height >= height) {
    if (slot->scale_x == ScaleFor(depth, pitch, format, !upscale_ || out).x) {
      if (!RestoreTarget(*slot)) return nullptr;
      if (!upscale_) slot->used_frame = frame_;
      if (upscale_) {
        // A target of the output generation first used after the upscale point gets its input
        // twin's picture of this frame (MigrateTarget does nothing otherwise).
        if (out && current_cl_ && upscale_->upscaled) MigrateTarget(current_cl_, slot.get());
        slot->used_frame = frame_;
      }
      return slot.get();
    }
  }
  // New, or taller than before (the old contents are lost; this happens once
  // per placement as the game's surfaces are first seen).
  auto t = std::make_unique<HostTarget>();
  t->key = key;
  t->depth = depth;
  t->width = pitch;
  t->height = std::max(height, slot ? slot->height : 0u);
  const Scale scale = ScaleFor(depth, pitch, format, !upscale_ || out);
  t->output_gen = out;
  t->scale_x = scale.x;
  t->scale_y = scale.y;
  t->host_width = uint32_t(std::max(ScaleCoord(int32_t(t->width), scale.x), 1));
  t->host_height = uint32_t(std::max(ScaleCoord(int32_t(t->height), scale.y), 1));
  // Depth: 24-bit integer depth is not a render target format everywhere
  // (RADV has no D24S8), so both 360 depth formats become D32S8.
  t->format = depth ? nvrhi::Format::D32S8 : ColorTargetFormat(format);
  nvrhi::TextureDesc desc;
  desc.width = t->host_width;
  desc.height = t->host_height;
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
  ++frame_resource_creations_;
  if (!t->texture) return nullptr;
  t->allocation_bytes = device_->getTextureMemoryRequirements(t->texture).size;
  if (memory_migration_ready_ && !depth) MemoryColorPipeline(t->format);
  stats_.edram_target_bytes += t->allocation_bytes;
  if (slot) stats_.edram_target_bytes -= slot->allocation_bytes;
  if (slot) {
    // Framebuffers holding the old texture are dropped with it.
    framebuffers_.clear();
    framebuffer_used_.clear();
    blit_sets_.erase(slot->texture.Get());
    RetireResource(slot->texture);
    if (upscale_) {
      upscale_->framebuffers.clear();
      upscale_->pass_sets.clear();
    }
  }
  if (!slot) ++stats_.edram_targets;
  slot = std::move(t);
  slot->used_frame = frame_;
  if (upscale_) {
    slot->used_frame = frame_;
    if (out && current_cl_ && upscale_->upscaled) MigrateTarget(current_cl_, slot.get());
  }
  return slot.get();
}

std::vector<GameRenderer::TargetInfo> GameRenderer::DebugTargets() const {
  std::vector<TargetInfo> out;
  for (const auto& [key, t] : targets_) {
    if (!t || !t->texture) continue;
    TargetInfo i;
    i.depth = t->depth;
    i.edram_base = uint32_t(key & 0xFFF);
    i.pitch = uint32_t((key >> 16) & 0x3FFF);
    i.format = uint32_t((key >> 40) & 0xFF);
    i.width = t->width;
    i.height = t->height;
    i.host_width = t->host_width;
    i.host_height = t->host_height;
    i.host_format = t->format;
    i.texture = t->texture;
    out.push_back(i);
  }
  return out;
}

uint64_t GameRenderer::FormatSignature(const std::array<HostTarget*, 4>& colors, const HostTarget* depth) {
  uint64_t h = 0x51ED27;
  for (const HostTarget* c : colors) h = Mix64(h, c ? uint64_t(c->format) + 1 : 0);
  return Mix64(h, depth ? uint64_t(depth->format) + 1 : 0);
}

nvrhi::IFramebuffer* GameRenderer::GetFramebuffer(const std::array<HostTarget*, 4>& colors,
                                                  HostTarget* depth) {
  // A bound attachment may be written. Conservative even for read-only draws,
  // failed pipeline creation, and partial clears.
  for (HostTarget* c : colors) if (c) c->clear_only = false;
  if (depth) depth->clear_only = false;
  MsTimer timer(stats_.target_ms);
  uint64_t key = 0xFB;
  for (const HostTarget* c : colors) key = Mix64(key, reinterpret_cast<uintptr_t>(c ? c->texture.Get() : nullptr));
  key = Mix64(key, reinterpret_cast<uintptr_t>(depth ? depth->texture.Get() : nullptr));
  framebuffer_used_[key] = frame_;
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
                                                           bool render_target, float scale_x, float scale_y,
                                                           kknr::ResourceWork* profile) {
  auto host = std::make_unique<HostTexture>();
  if (!render_target) scale_x = scale_y = 1.0f;  // only a resolve makes a scaled texture
  host->scale_x = scale_x;
  host->scale_y = scale_y;
  host->plan = plan;
  host->format = kknr::ToNvrhi(plan.format);
  host->render_target = render_target;
  if (host->format == nvrhi::Format::UNKNOWN) return nullptr;
  nvrhi::TextureDesc desc;
  desc.width = uint32_t(std::max(ScaleCoord(int32_t(std::max(plan.width, 1u)), scale_x), 1));
  desc.height = uint32_t(std::max(ScaleCoord(int32_t(std::max(plan.height, 1u)), scale_y), 1));
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
  {
    kknr::ResourceWorkTimer timer(profile ? profile : resource_profile_, kknr::ResourceWork::TextureCreate);
    host->texture = device_->createTexture(desc);
  }
  if (!host->texture) return nullptr;
  return host.release();
}

GameRenderer::HostTexture* GameRenderer::ResolveDestTexture(kknr::TextureCache::Entry& entry,
                                                            const kknr::HostTexturePlan& plan, bool drawable,
                                                            float scale_x, float scale_y) {
  if (!drawable) scale_x = scale_y = 1.0f;
  auto fits = [&](const HostTexture* h) {
    return h && h->render_target && h->plan.format == plan.format && h->plan.width == plan.width &&
           h->plan.height == plan.height && h->plan.levels == plan.levels && h->plan.layers == plan.layers &&
           h->plan.dimension == plan.dimension;
  };
  auto* host = static_cast<HostTexture*>(entry.host);
  if (fits(host) && host->scale_x == scale_x && host->scale_y == scale_y) return host;
  if (upscale_ && fits(host)) {
    // Only the scale differs (the scene copy before and after the upscale point): swap in the twin of
    // the other scale, or make it and keep this one as the twin.
    HostTexture* twin = host->twin;
    if (fits(twin) && twin->scale_x == scale_x && twin->scale_y == scale_y) {
      host->twin = nullptr;
      twin->twin = host;
      entry.host = twin;
      return twin;
    }
    if (twin) ReleaseHostTexture(twin);
    host->twin = nullptr;
    HostTexture* made = CreateHostTexture(plan, drawable, scale_x, scale_y);
    if (made) {
      made->twin = host;
      entry.host = made;
    }
    return made;
  }
  ReleaseHostTexture(host);
  host = CreateHostTexture(plan, drawable, scale_x, scale_y);
  entry.host = host;
  return host;
}

void GameRenderer::ReleaseHostTexture(HostTexture* host) {
  if (!host) return;
  pack_originals_.erase(host);
  if (pack_upload_.original == host) pack_upload_ = {};
  if (host->replacement) { ReleaseHostTexture(host->replacement); host->replacement = nullptr; }
  pack_bytes_ -= std::min(pack_bytes_, host->pack_allocation);
  descriptor_textures_.erase(host);
  if (host->twin) {
    HostTexture* twin = host->twin;
    host->twin = nullptr;
    ReleaseHostTexture(twin);
  }
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
  RetireResource(host->texture);
  delete host;
}

GameRenderer::HostTexture* GameRenderer::UploadTexture(nvrhi::ICommandList* cl,
                                                       kknr::TextureCache::Entry& entry,
                                                       const kknr::TextureFetch& fetch) {
  const auto start = std::chrono::steady_clock::now();
  struct Timer {
    std::chrono::steady_clock::time_point start;
    double& total;
    ~Timer() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
  } timer{start, stats_.texture_ms};
  kknr::HostTextureData data;
  std::string why;
  const auto options = TextureOptions();
  const auto ranges = kknr::GetTextureRanges(fetch, options);
  kknr::TextureConversionWorkers* workers = nullptr;
  if (uint64_t(ranges.base_bytes) + ranges.mip_bytes >= (256u << 10)) {
    // Only the CPU conversion is parallel. This thread retains command-list ownership and waits for
    // all slabs before writing any texture or letting the dependent draw bind it.
    if (!texture_conversion_workers_)
      texture_conversion_workers_ = std::make_unique<kknr::TextureConversionWorkers>(TextureWorkerCount());
    if (texture_conversion_workers_->WorkerCount()) workers = texture_conversion_workers_.get();
  }
  bool converted;
  {
    kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::TextureConvert);
    converted = kknr::ConvertTexture(fetch, physical_, data, &why, options, workers);
  }
  if (!converted) {
    texture_bind_failure_ = "conversion: " + why;
    ++stats_.texture_failures;
    if (stats_.texture_failures < 50) {
      Logf(LogLevel::kWarning, "rexgpu-native: texture %08X %08X %08X not converted: %s", fetch.words[0],
           fetch.words[1], fetch.words[2], why.c_str());
    }
    return nullptr;
  }
  auto* host = static_cast<HostTexture*>(entry.host);
  const kknr::HostTexturePlan& p = data.plan;
  bool created = false;
  if (!host || host->plan.format != p.format || host->plan.width != p.width || host->plan.height != p.height ||
      host->plan.depth != p.depth || host->plan.layers != p.layers || host->plan.levels != p.levels ||
      host->plan.dimension != p.dimension || host->scale_x != 1.0f || host->scale_y != 1.0f) {
    ReleaseHostTexture(host);
    host = CreateHostTexture(p, false);
    created = true;
    entry.host = host;
    if (!host) {
      texture_bind_failure_ = "host texture creation failed";
      return nullptr;
    }
  }
  host->material_hash = 0;
  if (host->replacement) { ReleaseHostTexture(host->replacement); host->replacement = nullptr; }
  if (pack_upload_.original == host) pack_upload_ = {};
  host->pack_hash = kknr::TextureBaseHash(data);
  host->pack_unavailable = false;
  if (host->pack_hash) pack_originals_.insert(host);
  else pack_originals_.erase(host);
  if (texture_pack_stream_ && host->pack_hash) texture_pack_stream_->Request(host->pack_hash, host->plan);
  // Only a new resource can be moved ahead of this frame's earlier draws. The
  // upload list finishes in ShaderResource; explicitly start the draw list's
  // local tracking there (the new Vulkan image otherwise starts in Common).
  nvrhi::ICommandList* upload = created ? EarlyUpload(data.bytes.size()) : nullptr;
  if (upload) cl->beginTrackingTextureState(host->texture, nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::ShaderResource);
  else upload = cl;
  {
  kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::TextureStage);
  for (const kknr::HostSubresource& sub : data.subresources) {
    upload->writeTexture(host->texture, sub.layer, sub.level, data.bytes.data() + sub.offset, sub.row_pitch,
                     sub.depth_pitch);
  }
  upload->setTextureState(host->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  }
  host->alias_source = nullptr;
  textures_.OnUploaded(entry, physical_);
  ++stats_.texture_uploads;
  stats_.upload_bytes += data.bytes.size();
  return host;
}

uint32_t GameRenderer::TextureView(HostTexture* host, const kknr::TextureFetch& fetch,
                                   kkshaders::TextureDimension dimension, bool base_map) {
  descriptor_textures_.insert(host);
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

  const bool cube = host->plan.dimension == kknr::Dimension::kCube;
  const bool volume = host->plan.dimension == kknr::Dimension::k3D;
  const bool array = !cube && !volume && host->plan.layers > 1;
  Table table;
  nvrhi::TextureDimension view_dim;
  uint32_t flag = 0;
  switch (dimension) {
    case kkshaders::TextureDimension::Cube:
      if (!cube) {
        texture_bind_failure_ = "cube view of non-cube texture";
        return 0;
      }
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
        texture_bind_failure_ = "3D view of non-volume/non-array texture";
        return 0;
      }
      break;
    default:
      if (cube || volume) {
        texture_bind_failure_ = "2D view of cube/volume texture";
        return 0;
      }
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
  if (it != host->views.end()) {
    TouchSlot(table, it->second & 0x7FFFFFFFu);
    return it->second;
  }
  const uint32_t slot = AllocateSlot(table);
  if (!slot) {
    texture_bind_failure_ = "texture descriptor table full";
    return 0;
  }
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
  texture_bind_failure_.clear();
  bound_scale_x_ = bound_scale_y_ = 1.0f;
  if (fetch.Type() != 2) {
    texture_bind_failure_ = "unset/non-texture fetch constant";
    return 0;
  }
  textures_.options = TextureOptions();  // ranges are computed with it when an entry is made
  kknr::TextureCache::BindResult r = textures_.Bind(fetch, physical_, frame_);
  if (!r.entry) {
    texture_bind_failure_ = "cache bind returned no entry";
    return 0;
  }
  auto* host = static_cast<HostTexture*>(r.entry->host);
  if (!r.entry->gpu_written) {
    auto it = resolved_by_base_.find(fetch.BaseAddress());
    if (it != resolved_by_base_.end() && it->second != r.entry && it->second->gpu_written && it->second->host) {
      if (HostTexture* alias = AliasTexture(cl, *r.entry, fetch, static_cast<HostTexture*>(it->second->host))) {
        cl->setTextureState(alias->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        bound_scale_x_ = alias->scale_x;
        bound_scale_y_ = alias->scale_y;
        return TextureView(alias, fetch, dimension, base_map);
      }
    }
    if (host && host->alias_source) {
      // The resolve's memory was rewritten since: the guest's data again.
      r.action = kknr::BindAction::kReupload;
    }
  }
  if (r.action != kknr::BindAction::kUseExisting || !host) {
    if (r.entry->gpu_written && host && r.action == kknr::BindAction::kUseExisting) {
      // The resolve's copy is the truth.
    } else {
      if (!host) {
        TryPrepareResourceUploads(cl);
        host = static_cast<HostTexture*>(r.entry->host);
        if (host && !r.entry->dirty) r.action = kknr::BindAction::kUseExisting;
      }
      if (!host || r.entry->dirty || r.action == kknr::BindAction::kReupload)
        host = UploadTexture(cl, *r.entry, fetch);
      if (!host) return 0;
    }
  }
  cl->setTextureState(host->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  if (host->preloaded) {
    if (r.action == kknr::BindAction::kUseExisting && !r.entry->dirty) ++stats_.preload_hits;
    host->preloaded = false;
  }
  if (!host->alias_source) RememberResource(ResourcePreloadRecord::Tex(fetch), *r.entry);
  bound_scale_x_ = host->scale_x;
  bound_scale_y_ = host->scale_y;
  if (options_.texture_pack && !pack_pressure_ && !r.entry->gpu_written && !host->alias_source &&
      host->replacement && host->replacement->pack_ready)
    host = host->replacement;
  return TextureView(host, fetch, dimension, base_map);
}

GameRenderer::HostTexture* GameRenderer::AliasTexture(nvrhi::ICommandList* cl, kknr::TextureCache::Entry& entry,
                                                      const kknr::TextureFetch& fetch, const HostTexture* source) {
  auto* host = static_cast<HostTexture*>(entry.host);
  if (host && host->alias_source == source && host->alias_serial == source->serial) return host;
  kknr::HostTexturePlan plan;
  if (!kknr::PlanHostTexture(fetch, plan, nullptr, TextureOptions())) return nullptr;
  if (kknr::GetHostFormatInfo(plan.format).block_size != 1) return nullptr;  // not drawable
  if (!host || !host->render_target || host->plan.format != plan.format || host->plan.width != plan.width ||
      host->plan.height != plan.height || host->plan.levels != plan.levels || host->plan.layers != plan.layers ||
      host->plan.dimension != plan.dimension || host->scale_x != source->scale_x || host->scale_y != source->scale_y) {
    ReleaseHostTexture(host);
    host = CreateHostTexture(plan, true, source->scale_x, source->scale_y);
    entry.host = host;
    if (!host) return nullptr;
  }
  // In host pixels: the overlap of the two (the guest's size times the scale).
  const int32_t w = ScaleCoord(int32_t(std::min(plan.width, source->plan.width)), source->scale_x);
  const int32_t h = ScaleCoord(int32_t(std::min(plan.height, source->plan.height)), source->scale_y);
  const uint32_t identity[4] = {0, 1, 2, 3};
  bool ok;
  const bool four = host->format == nvrhi::Format::RGBA8_UNORM;
  if (source->depth && four) {
    // The 360's resolve wrote the D24S8 word (depth << 8 | stencil); read as
    // 8:8:8:8 its bytes are X stencil, Y-W the depth from the low byte up.
    ok = Blit(cl, source->texture, 0, 0, w, h, host->texture, 0, 0, 0, 0, identity, kBlitDepthBytes);
  } else if (!source->depth && source->format == nvrhi::Format::RGBA8_UNORM &&
             host->format == nvrhi::Format::R32_FLOAT &&
             (fetch.Format() == kknr::TextureFormat::k_24_8 || fetch.Format() == kknr::TextureFormat::k_24_8_FLOAT)) {
    // An 8:8:8:8 resolve read back as depth (the light shafts' downsampled
    // depth): the texture's 32-bit words, their top 24 bits.
    ok = Blit(cl, source->texture, 0, 0, w, h, host->texture, 0, 0, 0, 0, identity, kBlitTexelBytesToDepth);
    if (ok) host->depth = true;
  } else if (source->format == host->format && !source->depth) {
    nvrhi::TextureSlice slice;
    slice.setSize(uint32_t(w), uint32_t(h), 1);
    cl->copyTexture(host->texture, slice, source->texture, slice);
    ok = true;
  } else if (!source->depth) {
    ok = Blit(cl, source->texture, 0, 0, w, h, host->texture, 0, 0, 0, 0, identity);
  } else {
    ok = false;
  }
  if (!ok) {
    if (stats_.alias_failures++ < 20) {
      Logf(LogLevel::kWarning, "rexgpu-native: texture %08X %08X over a resolve's memory not converted (format %u)",
           fetch.words[0], fetch.words[1], uint32_t(host->format));
    }
    return nullptr;
  }
  host->alias_source = source;
  host->alias_serial = source->serial;
  ++stats_.aliases;
  return host;
}

// ---------------------------------------------------------------- samplers

uint32_t GameRenderer::BindSampler(const uint32_t words[6], const kkshaders::SamplerBinding& b, float extra_lod_bias) {
  const kknr::TextureFetch f = kknr::TextureFetch::FromWords(words);
  // A scene material may also read a shadow/depth/resolve texture. Never apply the upscaler's
  // material bias to those, even though this draw is jittered. UI and post draws already pass zero.
  if (extra_lod_bias != 0.0f) {
    const auto* entry = textures_.Find(f);
    const auto* host = entry ? static_cast<const HostTexture*>(entry->host) : nullptr;
    if (!host || host->render_target || !host->texture || host->texture->getDesc().mipLevels <= 1)
      extra_lod_bias = 0.0f;
  }
  const uint32_t mag = b.magFilter == 3 ? f.MagFilter() : b.magFilter;
  const uint32_t min = b.minFilter == 3 ? f.MinFilter() : b.minFilter;
  const uint32_t mip = b.mipFilter == 3 ? f.MipFilter() : b.mipFilter;
  uint32_t aniso = b.anisoFilter == 7 ? f.AnisoFilter() : b.anisoFilter;
  if (options_.anisotropic_override >= 0 && options_.anisotropic_override <= 5) aniso = uint32_t(options_.anisotropic_override);
  const uint32_t key = f.ClampX() | f.ClampY() << 3 | f.ClampZ() << 6 | (mag & 3) << 9 | (min & 3) << 11 |
                       (mip & 3) << 13 | (aniso & 7) << 15 | (f.BorderColor() & 3) << 18 |
                       (uint32_t(f.LodBiasFixed()) & 0x3FF) << 20;
  // The upscalers' bias, in 1/32 steps (as the fetch constant's), above the 32 bits of the rest.
  const int32_t extra = int32_t(std::lround(extra_lod_bias * 32.0f));
  const uint64_t key64 = uint64_t(key) | (uint64_t(uint16_t(int16_t(extra))) << 32);
  auto it = samplers_.find(key64);
  if (it != samplers_.end()) {
    it->second.last_used = frame_;
    TouchSlot(kSamplers, it->second.second);
    return it->second.second;
  }
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
  sd.mipBias = float(f.LodBiasFixed()) / 32.0f + float(extra) / 32.0f;
  sd.borderColor = f.BorderColor() == 1 ? nvrhi::Color(1.0f) : nvrhi::Color(0.0f);
  nvrhi::SamplerHandle sampler = device_->createSampler(sd);
  uint32_t slot = 0;
  if (sampler) {
    slot = AllocateSlot(kSamplers);
    if (slot) WriteSlot(kSamplers, nvrhi::BindingSetItem::Sampler(slot, sampler));
  }
  // A failed allocation is retryable after retirement, never cached as dummy.
  if (slot) samplers_[key64] = {sampler, slot, frame_};
  return slot;
}

// ---------------------------------------------------------------- buffers

nvrhi::ICommandList* GameRenderer::EarlyUpload(uint64_t bytes) {
  if (!options_.upload_submit) return nullptr;
  if (!upload_batch_) upload_batch_ = std::make_unique<UploadBatch>(device_, options_.upload_submit);
  return upload_batch_->Begin(bytes);
}

void GameRenderer::FlushUploads() {
  if (!upload_batch_) return;
  kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::Submit);
  upload_batch_->Flush();
  stats_.early_upload_bytes = upload_batch_->bytes();
  stats_.early_upload_batches = upload_batch_->batches();
}

bool GameRenderer::IsDynamicVertexBuffer(uint32_t physical, uint32_t bytes) {
  // The same test as BindVertexBuffer's dynamic path (no hashing, no watch, nothing changes).
  const kknr::BufferCache::Entry* known = bytes ? vertex_buffers_.Find(physical, bytes, 0) : nullptr;
  const auto* host = known ? static_cast<const HostBuffer*>(known->host) : nullptr;
  return host && host->dynamic && host->buffer && host->slot;
}

GameRenderer::HostBuffer* GameRenderer::BindVertexBuffer(nvrhi::ICommandList* cl, uint32_t physical,
                                                         uint32_t bytes, uint32_t used_begin, uint32_t used_end) {
  if (!bytes) return nullptr;
  MsTimer timer(stats_.buffer_ms);
  const uint8_t* data = physical_.At(physical, bytes);
  if (!data) return nullptr;
  if (options_.dynamic_vertex_buffers) {
    // A dynamic buffer: no watch, no hash; this draw's vertices go up every time.
    if (const kknr::BufferCache::Entry* known = vertex_buffers_.Find(physical, bytes, 0)) {
      auto* host = static_cast<HostBuffer*>(known->host);
      if (host && host->dynamic && host->buffer && !host->slot) {
        host->slot = AllocateSlot(kBuffers);
        if (host->slot) WriteSlot(kBuffers, nvrhi::BindingSetItem::RawBuffer_SRV(host->slot, host->buffer));
      }
      if (host && host->dynamic && host->buffer && host->slot) {
        TouchSlot(kBuffers, host->slot);
        vertex_buffers_.Touch(physical, bytes, 0, frame_);
        const uint32_t begin = std::min(used_begin, bytes) & ~3u;
        const uint32_t end = std::min((uint64_t(std::min(used_end, bytes)) + 3) & ~uint64_t(3), uint64_t(host->bytes + 3) & ~uint64_t(3));
        if (end > begin) {
          std::vector<uint8_t> tail;
          const uint8_t* src = data + begin;
          if (begin + (end - begin) > bytes) {  // the last word runs past the guest buffer
            tail.assign(src, data + bytes);
            tail.resize(end - begin, 0);
            src = tail.data();
          }
          {
            kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::BufferStage);
            cl->writeBuffer(host->buffer, src, end - begin, begin);
          }
          stats_.dynamic_vertex_bytes += end - begin;
          stats_.upload_bytes += end - begin;
        }
        cl->setBufferState(host->buffer, nvrhi::ResourceStates::ShaderResource);
        return host;
      }
    }
  }
  kknr::BufferCache::BindResult r = vertex_buffers_.Bind(physical, bytes, 0, physical_, frame_);
  auto* host = static_cast<HostBuffer*>(r.entry->host);
  if (!host) {
    TryPrepareResourceUploads(cl);
    host = static_cast<HostBuffer*>(r.entry->host);
    if (host && !r.entry->dirty) r.action = kknr::BindAction::kUseExisting;
  }
  const bool created = !host;
  if (!host) {
    host = new HostBuffer();
    nvrhi::BufferDesc d;
    d.byteSize = (uint64_t(bytes) + 15) & ~uint64_t(15);
    d.canHaveRawViews = true;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = "Guest vertex data";
    {
      kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::BufferCreate);
      host->buffer = device_->createBuffer(d);
    }
    host->bytes = bytes;
    if (host->buffer) {
      host->slot = AllocateSlot(kBuffers);
      if (host->slot) WriteSlot(kBuffers, nvrhi::BindingSetItem::RawBuffer_SRV(host->slot, host->buffer));
    }
    r.entry->host = host;
    r.action = kknr::BindAction::kCreateAndUpload;
  }
  descriptor_buffers_.insert(host);
  if (host->buffer && !host->slot) {
    host->slot = AllocateSlot(kBuffers);
    if (host->slot) WriteSlot(kBuffers, nvrhi::BindingSetItem::RawBuffer_SRV(host->slot, host->buffer));
  }
  if (!host->buffer || !host->slot) return nullptr;
  TouchSlot(kBuffers, host->slot);
  if (r.action != kknr::BindAction::kUseExisting) {
    if (r.action == kknr::BindAction::kReupload && options_.dynamic_vertex_buffers) {
      // Uploaded again within four frames (rings of three buffers are common): three times running makes it dynamic (the
      // watch is not armed again; the next draws upload what they read).
      host->upload_streak = host->last_upload_frame + 4 >= frame_ ? host->upload_streak + 1 : 0;
      host->last_upload_frame = frame_;
      if (host->upload_streak >= 3) {
        host->dynamic = true;
        ++stats_.dynamic_vertex_buffers;
      }
    } else {
      host->last_upload_frame = frame_;
    }
    nvrhi::ICommandList* upload = created ? EarlyUpload((uint64_t(bytes) + 3) & ~uint64_t(3)) : nullptr;
    if (!upload) upload = cl;
    {
      kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::BufferStage);
      upload->writeBuffer(host->buffer, data, (uint64_t(bytes) + 3) & ~uint64_t(3));
    }
    upload->setBufferState(host->buffer, nvrhi::ResourceStates::ShaderResource);
    stats_.upload_bytes += (uint64_t(bytes) + 3) & ~uint64_t(3);
    if (!host->dynamic) vertex_buffers_.OnUploaded(*r.entry, physical_);
    ++stats_.buffer_uploads;
  }
  cl->setBufferState(host->buffer, nvrhi::ResourceStates::ShaderResource);
  if (host->preloaded) {
    if (r.action == kknr::BindAction::kUseExisting) ++stats_.preload_hits;
    host->preloaded = false;
  }
  if (!host->dynamic) RememberResource(ResourcePreloadRecord::Buffer(physical, bytes, false, false), *r.entry);
  return host;
}

GameRenderer::HostBuffer* GameRenderer::BindIndexBuffer(nvrhi::ICommandList* cl, uint32_t physical,
                                                        uint32_t bytes, bool index32) {
  if (!bytes) return nullptr;
  MsTimer timer(stats_.buffer_ms);
  const uint8_t* data = physical_.At(physical, bytes);
  if (!data) return nullptr;
  kknr::BufferCache::BindResult r =
      index_buffers_.Bind(physical, bytes, kknr::IndexConversionId(index32), physical_, frame_);
  auto* host = static_cast<HostBuffer*>(r.entry->host);
  if (!host) {
    TryPrepareResourceUploads(cl);
    host = static_cast<HostBuffer*>(r.entry->host);
    if (host && !r.entry->dirty) r.action = kknr::BindAction::kUseExisting;
  }
  const bool created = !host;
  if (!host) {
    host = new HostBuffer();
    nvrhi::BufferDesc d;
    d.byteSize = (uint64_t(bytes) + 15) & ~uint64_t(15);
    d.isIndexBuffer = true;
    d.initialState = nvrhi::ResourceStates::IndexBuffer;
    d.keepInitialState = true;
    d.debugName = "Guest indices";
    {
      kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::BufferCreate);
      host->buffer = device_->createBuffer(d);
    }
    host->bytes = bytes;
    r.entry->host = host;
    r.action = kknr::BindAction::kCreateAndUpload;
  }
  if (!host->buffer) return nullptr;
  if (r.action != kknr::BindAction::kUseExisting) {
    auto& converted = scratch_index_upload_;
    {
      kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::BufferConvert);
      kknr::ConvertIndexBuffer(data, converted, bytes, index32);
    }
    nvrhi::ICommandList* upload = created ? EarlyUpload(converted.size()) : nullptr;
    if (!upload) upload = cl;
    {
      kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::BufferStage);
      upload->writeBuffer(host->buffer, converted.data(), converted.size());
    }
    upload->setBufferState(host->buffer, nvrhi::ResourceStates::IndexBuffer);
    stats_.upload_bytes += converted.size();
    index_buffers_.OnUploaded(*r.entry, physical_);
    ++stats_.buffer_uploads;
  }
  if (host->preloaded) {
    if (r.action == kknr::BindAction::kUseExisting) ++stats_.preload_hits;
    host->preloaded = false;
  }
  RememberResource(ResourcePreloadRecord::Buffer(physical, bytes, true, index32), *r.entry);
  return host;
}

void GameRenderer::TryPrepareResourceUploads(nvrhi::ICommandList* cl) {
  if (!resource_prepare_call_ || resource_prepare_tried_) return;
  resource_prepare_tried_ = true;
  PrepareResourceUploads(cl, *resource_prepare_call_, *resource_prepare_device_,
                         *resource_prepare_vs_, resource_prepare_ps_);
}

void GameRenderer::PrepareResourceUploads(nvrhi::ICommandList* cl, const DrawCall& call,
                                          const dev::DeviceView& d, const GameShader& vs, const GameShader* ps) {
  std::vector<ResourcePreloadRecord> requests;
  auto texture = [&](const GameShader& shader) {
    for (const auto& use : shader.bindings.textures) {
      uint32_t words[6];
      d.fetch_constant(use.slot & 31, words);
      requests.push_back(ResourcePreloadRecord::Tex(kknr::TextureFetch::FromWords(words)));
    }
  };
  texture(vs);
  if (ps) texture(*ps);
  auto& elements = scratch_preparation_declaration_;
  elements.clear();
  if (const uint8_t* decl = call.vertex_declaration ? memory_.Virtual(call.vertex_declaration) : nullptr)
    kknr::DecodeVertexDeclaration(decl, elements);
  for (const auto& binding : vs.bindings.vertexBindings) {
    uint32_t stream = 16;
    if (binding.raw) {
      if (binding.fetchConstant >= 80 && binding.fetchConstant <= 95) stream = 95 - binding.fetchConstant;
    } else {
      for (const auto& e : elements) if (e.usage == uint8_t(binding.usage) && e.usage_index == binding.usageIndex) {
        stream = e.stream; break;
      }
    }
    if (stream >= 16) continue;
    const uint32_t object = d.u32(dev::kStreamBuffer + 8 * stream);
    VertexBufferInfo info;
    if (object && DecodeVertexBuffer(memory_.Virtual(object), info))
      requests.push_back(ResourcePreloadRecord::Buffer(info.physical, info.size_bytes, false, false));
  }
  if (call.indexed && call.primitive != GuestPrimitive::kTriangleFan && call.primitive != GuestPrimitive::kQuadList)
    requests.push_back(ResourcePreloadRecord::Buffer(call.index_info.physical, call.index_info.size_bytes,
                                                     true, call.index_info.index32));
  PrepareResourceRequests(cl, requests);
}

void GameRenderer::PrepareResourceRequests(nvrhi::ICommandList* cl,
    const std::vector<ResourcePreloadRecord>& requests, bool preload) {
  if (!options_.upload_submit || !TextureWorkerCount()) return;
  struct Job {
    kknr::TextureCache::Entry* texture = nullptr;
    kknr::BufferCache::Entry* buffer = nullptr;
    bool index = false, index32 = false;
    kknr::TextureFetch fetch;
    kknr::ResidentSnapshot snapshot;
    uint64_t upload_bytes = 0, hash = 0;
    std::unique_ptr<HostTexture> host_texture;
    std::unique_ptr<HostBuffer> host_buffer;
    kknr::ResourceWork work;
  };
  // A draw cannot stage more than the existing early-upload allowance. Storage
  // snapshots have their own 32 MiB bound; an expanded texture can be larger.
  std::vector<Job> jobs;

  uint64_t upload_bytes = 0, snapshot_bytes = 0;
  const auto options = TextureOptions();
  const uint64_t revision = resource_revision_.load(std::memory_order_acquire);
  auto fits = [&](uint64_t input, uint64_t output) {
    if (jobs.size() >= 32 || output < UploadBatch::kMinBytes ||
        input > UploadBatch::kFrameBytes - snapshot_bytes ||
        output > UploadBatch::kFrameBytes - upload_bytes) return false;
    if (preload && output + upload_bytes > kChapterPreloadBytes - preload_reserved_bytes_) return false;
    if (!upload_batch_) upload_batch_ = std::make_unique<UploadBatch>(device_, options_.upload_submit);
    return upload_batch_->CanPrepare(upload_bytes + output,
        std::min(unsigned(jobs.size()) + 1, TextureWorkerCount() + 1));
  };
  auto capture = [&](Job& job, const kknr::GuestRange ranges[2], uint64_t expected_hash) {
    if (!resource_upload_workers_)
      resource_upload_workers_ = std::make_unique<ResourceUploadWorkers>(device_, TextureWorkerCount());
    kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::Snapshot);
    auto& scratch = resource_upload_workers_->scratch(jobs.size());
    if (!job.snapshot.Capture(ranges, physical_, scratch.source, UploadBatch::kFrameBytes - snapshot_bytes))
      return false;
    if (preload) {
      kknr::ResourceWorkTimer hash_timer(resource_profile_, kknr::ResourceWork::Hash);
      job.hash = job.snapshot.Hash();
      if (job.hash != expected_hash) { ++stats_.preload_hash_misses; return false; }
    }
    snapshot_bytes += scratch.source.size();
    upload_bytes += job.upload_bytes;
    jobs.push_back(std::move(job));
    return true;
  };
  auto texture = [&](const ResourcePreloadRecord& request) {
      const auto fetch = kknr::TextureFetch::FromWords(request.words.data());
      if (fetch.Type() != 2) return;
      // A resolve/alias may own the picture. Its usual bind decides this; never
      // prepare guest bytes over it, even through a different fetch description.
      if (auto it = resolved_by_base_.find(fetch.BaseAddress());
          it != resolved_by_base_.end() && it->second->gpu_written) return;
      auto* known = textures_.Find(fetch);
      if (known && (known->host || known->gpu_written)) return;
      kknr::HostTexturePlan plan;
      if (!kknr::PlanHostTexture(fetch, plan, nullptr, options)) return;
      const auto ranges = kknr::GetTextureRanges(fetch, options);
      const auto& format = kknr::GetHostFormatInfo(plan.format);
      uint64_t size = 0;
      for (uint32_t level = plan.min_level; level < plan.levels; ++level) {
        const uint64_t w = std::max(plan.width >> level, 1u), h = std::max(plan.height >> level, 1u);
        size += ((w + format.block_size - 1) / format.block_size) *
            ((h + format.block_size - 1) / format.block_size) * format.bytes_per_block *
            std::max(plan.depth >> level, 1u) * plan.layers;
      }
      const uint64_t input = ((uint64_t(ranges.base_bytes) + 4095) & ~uint64_t(4095)) +
                             ((uint64_t(ranges.mip_bytes) + 4095) & ~uint64_t(4095));
      if (!fits(input, size)) return;
      textures_.options = options;
      auto* entry = textures_.Bind(fetch, physical_, frame_).entry;
      if (!entry || entry->host || std::any_of(jobs.begin(), jobs.end(),
          [&](const Job& j) { return j.texture == entry; })) return;
      // Protect before copying. A concurrent write changes resource_revision_;
      // the joined results are then discarded and the ordinary bind retries.
      bool readable = true;
      for (const auto& r : entry->ranges) if (r.bytes && physical_.Available(r.start) <
          ((uint64_t(r.bytes) + 3) & ~uint64_t(3))) readable = false;
      if (!readable) return;
      textures_.EnsureWatched(*entry);
      Job job; job.texture = entry; job.fetch = fetch; job.upload_bytes = size;
      capture(job, entry->ranges, request.hash);
  };
  auto buffer = [&](uint32_t physical, uint32_t bytes, bool index, bool index32, uint64_t expected_hash) {
    if (!bytes || !physical_.At(physical, bytes)) return;
    auto& cache = index ? index_buffers_ : vertex_buffers_;
    const uint64_t conversion = index ? kknr::IndexConversionId(index32) : 0;
    auto* known = cache.Find(physical, bytes, conversion);
    if (known && known->host) return;
    const uint64_t size = (uint64_t(bytes) + 3) & ~uint64_t(3);
    if (!fits((uint64_t(bytes) + 4095) & ~uint64_t(4095), size)) return;
    auto* entry = cache.Bind(physical, bytes, conversion, physical_, frame_).entry;
    if (!entry || entry->host || std::any_of(jobs.begin(), jobs.end(),
        [&](const Job& j) { return j.buffer == entry; })) return;
    cache.EnsureWatched(*entry);
    Job job; job.buffer = entry; job.index = index; job.index32 = index32; job.upload_bytes = size;
    capture(job, entry->ranges, expected_hash);
  };
  for (const auto& request : requests) {
    if (request.kind == ResourcePreloadRecord::Texture) texture(request);
    else if (request.kind <= ResourcePreloadRecord::Index32)
      buffer(request.words[0], request.words[1], request.kind != ResourcePreloadRecord::Vertex,
             request.kind == ResourcePreloadRecord::Index32, request.hash);
  }
  if (jobs.empty()) {
    if (resource_upload_workers_) resource_upload_workers_->TrimScratch();
    return;
  }
  if (preload) preload_reserved_bytes_ += upload_bytes;  // includes stale/failed attempts

  MsTimer total(stats_.resource_prepare_ms);
  kknr::ResourceWorkTimer wall(resource_profile_, kknr::ResourceWork::PrepareWall);
  std::vector<ResourceUploadWorkers::Task> tasks(jobs.size());
  // Retain the existing slab parallelism when this draw has only one large
  // texture. The batch pool has no other participants in this case.
  kknr::TextureConversionWorkers* slabs = nullptr;
  if (jobs.size() == 1 && jobs[0].texture) {
    if (!texture_conversion_workers_)
      texture_conversion_workers_ = std::make_unique<kknr::TextureConversionWorkers>(TextureWorkerCount());
    slabs = texture_conversion_workers_.get();
  }
  for (size_t i = 0; i < jobs.size(); ++i) {
    tasks[i].bytes = jobs[i].upload_bytes;
    tasks[i].prepare = [&, i] {
      Job& job = jobs[i];
      auto* profile = resource_profile_ ? &job.work : nullptr;
      if (!preload) {
        kknr::ResourceWorkTimer timer(profile, kknr::ResourceWork::Hash);
        job.hash = job.snapshot.Hash();
      }
      auto& scratch = resource_upload_workers_->scratch(i);
      if (job.texture) {
        {
          kknr::ResourceWorkTimer timer(profile, kknr::ResourceWork::TextureConvert);
          // The snapshot's addresses are compact; format, layout and texel data
          // are identical. Workers read no live guest memory. Slabs only run
          // for a single job on the caller, with the batch workers idle.
          if (!kknr::ConvertTexture(job.snapshot.Relocate(job.fetch), job.snapshot.memory,
                                   scratch.texture, nullptr, options, slabs)) return false;
        }
        job.upload_bytes = scratch.texture.bytes.size();
        tasks[i].bytes = job.upload_bytes;
        job.host_texture.reset(CreateHostTexture(scratch.texture.plan, false, 1.0f, 1.0f, profile));
        if (job.host_texture) job.host_texture->pack_hash = kknr::TextureBaseHash(scratch.texture);
        return bool(job.host_texture);
      }
      job.host_buffer = std::make_unique<HostBuffer>();
      nvrhi::BufferDesc desc;
      desc.byteSize = (uint64_t(job.buffer->bytes) + 15) & ~uint64_t(15);
      desc.canHaveRawViews = !job.index;
      desc.isIndexBuffer = job.index;
      desc.initialState = job.index ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::ShaderResource;
      desc.keepInitialState = true;
      desc.debugName = job.index ? "Guest indices" : "Guest vertex data";
      {
        kknr::ResourceWorkTimer timer(profile, kknr::ResourceWork::BufferCreate);
        job.host_buffer->buffer = device_->createBuffer(desc);
      }
      job.host_buffer->bytes = job.buffer->bytes;
      job.host_buffer->last_upload_frame = frame_;
      job.host_buffer->preloaded = preload;
      if (job.index) {
        kknr::ResourceWorkTimer timer(profile, kknr::ResourceWork::BufferConvert);
        kknr::ConvertIndexBuffer(scratch.source.data(), scratch.indices, job.buffer->bytes, job.index32);
      }
      return bool(job.host_buffer->buffer);
    };
    tasks[i].record = [&, i](nvrhi::ICommandList* upload) {
      Job& job = jobs[i];
      auto& scratch = resource_upload_workers_->scratch(i);
      auto* profile = resource_profile_ ? &job.work : nullptr;
      if (job.texture) {
        kknr::ResourceWorkTimer timer(profile, kknr::ResourceWork::TextureStage);
        for (const auto& sub : scratch.texture.subresources)
          upload->writeTexture(job.host_texture->texture, sub.layer, sub.level,
              scratch.texture.bytes.data() + sub.offset, sub.row_pitch, sub.depth_pitch);
        upload->setTextureState(job.host_texture->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
      } else {
        kknr::ResourceWorkTimer timer(profile, kknr::ResourceWork::BufferStage);
        const auto& data = job.index ? scratch.indices : scratch.source;
        upload->writeBuffer(job.host_buffer->buffer, data.data(), job.upload_bytes);
        upload->setBufferState(job.host_buffer->buffer,
            job.index ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::ShaderResource);
      }
    };
  }
  resource_upload_workers_->Run(tasks);
  {
    kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::Submit);
    resource_upload_workers_->SubmitAll([&](auto* list, uint64_t bytes) { upload_batch_->SubmitPrepared(list, bytes); });
  }
  std::vector<bool> stale(jobs.size(), false);
  const bool changed = resource_revision_.load(std::memory_order_acquire) != revision;
  if (changed) {
    // Loading writes many unrelated assets at once. Only a write overlapping
    // this immutable snapshot invalidates its result. No cache mutation or
    // worker/list operation is performed inside the fault handler.
    std::lock_guard lock(invalidation_mutex_);
    for (size_t i = 0; i < jobs.size(); ++i) {
      stale[i] = !preload;
      if (preload) for (const auto& [start, bytes] : invalidations_)
        for (const auto& range : jobs[i].snapshot.original)
          if (range.Overlaps(start, bytes)) stale[i] = true;
    }
  }
  // Leave loading notifications queued until the next service/draw. In
  // particular, never consume a write arriving after the overlap check and
  // then erase its dirty flag while publishing an unaffected snapshot.
  if (changed && !preload) ProcessInvalidations();
  for (size_t i = 0; i < jobs.size(); ++i) {
    auto& job = jobs[i];
    if (resource_profile_) for (unsigned stage = 0; stage < kknr::ResourceWork::Count; ++stage) {
      resource_work_.ms[stage] += job.work.ms[stage];
      resource_work_.calls[stage] += job.work.calls[stage];
    }
    if (!tasks[i].ready || stale[i]) { ++stats_.prepared_discarded; continue; }
    if (job.texture) {
      job.host_texture->preloaded = preload;
      if (job.host_texture->pack_hash) {
        pack_originals_.insert(job.host_texture.get());
        if (texture_pack_stream_) texture_pack_stream_->Request(job.host_texture->pack_hash, job.host_texture->plan);
      }
      cl->beginTrackingTextureState(job.host_texture->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
      job.texture->host = job.host_texture.release();
      textures_.OnUploaded(*job.texture, job.hash);
      ++stats_.texture_uploads;
    } else {
      if (!job.index) {
        job.host_buffer->slot = AllocateSlot(kBuffers);
        if (job.host_buffer->slot) WriteSlot(kBuffers,
            nvrhi::BindingSetItem::RawBuffer_SRV(job.host_buffer->slot, job.host_buffer->buffer));
      }
      job.buffer->host = job.host_buffer.release();
      (job.index ? index_buffers_ : vertex_buffers_).OnUploaded(*job.buffer, job.hash);
      ++stats_.buffer_uploads;
    }
    stats_.upload_bytes += job.upload_bytes;
    ++stats_.prepared_resources;
    if (preload) { ++stats_.preloaded_resources; stats_.preload_bytes += job.upload_bytes; }
  }
  resource_upload_workers_->TrimScratch();
  stats_.early_upload_bytes = upload_batch_->bytes();
  stats_.early_upload_batches = upload_batch_->batches();
}

void GameRenderer::ProfileResources() {
  if (!resource_profile_file_) return;
  std::fprintf(resource_profile_file_, "%llu,%.6f", static_cast<unsigned long long>(frame_),
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
  for (double ms : resource_work_.ms) std::fprintf(resource_profile_file_, ",%.6f", ms);
  std::fprintf(resource_profile_file_, ",%llu,%llu,%.6f,%llu,%llu,%llu,%llu\n",
      static_cast<unsigned long long>(stats_.prepared_resources),
      static_cast<unsigned long long>(stats_.prepared_discarded), stats_.resource_prepare_ms,
      static_cast<unsigned long long>(stats_.preloaded_resources), static_cast<unsigned long long>(stats_.preload_hits),
      static_cast<unsigned long long>(stats_.preload_hash_misses), static_cast<unsigned long long>(stats_.preload_bytes));
  resource_work_ = {};
  if (frame_ % 300 == 0) std::fflush(resource_profile_file_);
}

}  // namespace nr
