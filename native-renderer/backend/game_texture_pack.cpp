// Player-owned replacements: asynchronous file work, bounded incremental GPU
// staging, and immediate original/pack selection without reuploading originals.
#include "backend/game_renderer.h"
#include "backend/texture_workers.h"
#include "backend/log.h"
#include "kknr/nvrhi_format.h"

namespace nr {
void GameRenderer::ReleasePackReplacement(HostTexture* original) {
  HostTexture* replacement = original->replacement;
  if (!replacement) return;
  original->replacement = nullptr;
  if (replacement->pack_refs > 1) { --replacement->pack_refs; return; }
  pack_shared_.erase(replacement->pack_key);
  ReleaseHostTexture(replacement);  // retired: GPU lifetime + per-frame release budget (task 174)
}

void GameRenderer::DropTexturePacks() {
  pack_upload_ = {};
  for (auto* original : pack_originals_) ReleasePackReplacement(original);
  pack_shared_.clear();
}

// The original stays: its replacement does not fit the current limit. Only the
// count, sizes and frame are kept; the decoded bytes are already freed.
void GameRenderer::CapTexturePack(HostTexture* original, uint64_t bytes) {
  if (!pack_capped_) Logf(LogLevel::kWarning,
      "rexgpu-native: texture pack limit reached, keeping originals: resident_bytes=%llu limit=%llu next=%llu",
      (unsigned long long)pack_bytes_,(unsigned long long)pack_limit_,(unsigned long long)bytes);
  Logf(LogLevel::kInfo,"rexgpu-native: texture pack capped %016llX bytes=%llu",
      (unsigned long long)original->pack_hash,(unsigned long long)bytes);
  original->pack_capped=true; original->pack_capped_bytes=bytes; ++pack_capped_;
  pack_capped_min_=std::min(pack_capped_min_,bytes); pack_capped_frame_=frame_;
}

void GameRenderer::UncapTexturePack(HostTexture* original) {
  if (!original->pack_capped) return;
  original->pack_capped=false; original->pack_capped_bytes=0;
  pack_capped_ -= std::min<uint64_t>(pack_capped_, 1);
}

void GameRenderer::TexturePackLoadStarted() {
  pack_load_frame_ = frame_;
  pack_sweep_pending_ = true;
  pack_sweep_frame_ = UINT64_MAX;
}

void GameRenderer::TexturePackLoadEnded() {
  if (pack_sweep_pending_) pack_sweep_frame_ = frame_ + kPackSweepDelayFrames;
}

// A chapter change: the old chapter's replacements otherwise stay until the
// texture cache trims their originals 20-60 s later (task 178), holding the
// limit the new chapter needs. Replacements whose identity no original has used
// since the load started are released (retired over frames); their originals
// stay and ask again only when bound. Capped originals of the new chapter then
// retry at once.
void GameRenderer::SweepTexturePacks() {
  pack_sweep_pending_ = false;
  pack_sweep_frame_ = UINT64_MAX;
  if (texture_pack_folder_.empty() && pack_shared_.empty()) return;  // no pack: nothing to free
  std::unordered_set<PackKey, PackKeyHash> used;
  for (auto* original : pack_originals_)
    if (original->pack_used_frame >= pack_load_frame_) used.insert(PackKeyOf(*original));
  const uint64_t before = pack_bytes_, shared_before = pack_shared_.size();
  uint64_t parked = 0;
  for (auto* original : pack_originals_) {
    if (original->pack_used_frame >= pack_load_frame_ || original->pack_swept) continue;
    if (used.contains(PackKeyOf(*original))) continue;
    ReleasePackReplacement(original);
    UncapTexturePack(original);
    if (texture_pack_stream_ && !original->pack_unavailable) texture_pack_stream_->Cancel(original->pack_hash, original->plan);
    original->pack_swept = true;
    ++parked;
  }
  pack_capped_min_ = UINT64_MAX;
  for (auto* original : pack_originals_)
    if (original->pack_capped) pack_capped_min_ = std::min(pack_capped_min_, original->pack_capped_bytes);
  pack_capped_frame_ = 0;  // what now fits retries this frame
  Logf(LogLevel::kInfo,
      "rexgpu-native: texture pack chapter change: released %llu replacements (%llu bytes), kept %zu, %llu unused originals parked, capped=%llu (frame %llu)",
      (unsigned long long)(shared_before - pack_shared_.size()),(unsigned long long)(before - pack_bytes_),
      pack_shared_.size(),(unsigned long long)parked,(unsigned long long)pack_capped_,(unsigned long long)frame_);
}

void GameRenderer::ServiceTexturePacks(nvrhi::ICommandList* cl) {
  if (pack_frame_ == frame_) return;
  pack_frame_ = frame_;
  if (options_.texture_pack_folder != texture_pack_folder_) {
    DropTexturePacks();
    for (auto* original : pack_originals_) {
      original->pack_unavailable=original->pack_capped=original->pack_swept=false;
      original->pack_capped_bytes=0;
    }
    pack_capped_=0; pack_capped_min_=UINT64_MAX;
    texture_pack_folder_ = options_.texture_pack_folder;
    if (!texture_pack_stream_) {
      try { texture_pack_stream_ = std::make_unique<kknr::TexturePackStream>(TextureWorkerCount()); }
      catch (const std::exception& e) {
        Logf(LogLevel::kWarning,"rexgpu-native: texture pack worker unavailable: %s",e.what()); return;
      }
    }
    texture_pack_stream_->Configure(texture_pack_folder_);
    Logf(LogLevel::kInfo,"rexgpu-native: texture pack folder changed: %s",texture_pack_folder_.c_str());
  }
  if (pack_sweep_pending_ && frame_ >= pack_sweep_frame_) SweepTexturePacks();
  if (!texture_pack_stream_ || texture_pack_folder_.empty() || pack_pressure_) return;
  // Capped originals ask again once room returns (replacements freed, or the
  // limit grew), but only as many as the free room holds, so a full limit never
  // re-reads files that cannot fit. At most every 30 frames.
  if (pack_capped_ && pack_limit_ > pack_bytes_ && pack_limit_-pack_bytes_ >= pack_capped_min_ &&
      frame_ >= pack_capped_frame_+30) {
    uint64_t room = pack_limit_-pack_bytes_;
    pack_capped_min_ = UINT64_MAX;
    for (auto* original : pack_originals_) if (original->pack_capped) {
      if (original->pack_capped_bytes <= room) { room -= original->pack_capped_bytes; UncapTexturePack(original); }
      else pack_capped_min_ = std::min(pack_capped_min_, original->pack_capped_bytes);
    }
    pack_capped_frame_ = frame_;
  }
  // Prepare even when comparing Off, so switching On is just a descriptor choice.
  // The stream limits pending requests, decoded bytes and worker concurrency.
  for (auto* original : pack_originals_) if (!original->render_target && !original->alias_source &&
      !original->replacement && !original->pack_unavailable && !original->pack_capped && !original->pack_swept &&
      original->pack_hash) {
    const PackKey key = PackKeyOf(*original);
    // Another original with the same identity already has it: share that copy.
    if (auto it = pack_shared_.find(key); it != pack_shared_.end()) {
      if (kknr::TexturePackEligible(original->plan)) {
        original->replacement = it->second; ++it->second->pack_refs; ++pack_shared_attached_;
        Logf(LogLevel::kInfo,"rexgpu-native: texture pack shared %016llX refs=%u bytes=%llu",
            (unsigned long long)original->pack_hash,it->second->pack_refs,(unsigned long long)it->second->pack_allocation);
      }
      continue;
    }
    texture_pack_stream_->Request(original->pack_hash, original->plan);
    if (pack_upload_.replacement) continue;
    const uint64_t room = pack_limit_ > pack_bytes_ ? pack_limit_-pack_bytes_ : 0;
    uint64_t too_large = 0;
    auto data = texture_pack_stream_->Take(original->pack_hash, original->plan, room, &too_large);
    if (too_large) { CapTexturePack(original, too_large); continue; }
    if (!data) continue;
    const auto support=device_->queryFormatSupport(kknr::ToNvrhi(data->plan.format));
    if ((support & nvrhi::FormatSupport::Texture) == nvrhi::FormatSupport::None ||
        (support & nvrhi::FormatSupport::ShaderSample) == nvrhi::FormatSupport::None) {
      original->pack_unavailable=true; continue;
    }
    auto* replacement = CreateHostTexture(data->plan, false);
    if (!replacement) { original->pack_unavailable=true; continue; }
    replacement->pack_allocation = device_->getTextureMemoryRequirements(replacement->texture).size;
    if (replacement->pack_allocation > room) {
      const uint64_t bytes=replacement->pack_allocation;
      replacement->pack_allocation=0; ReleaseHostTexture(replacement); CapTexturePack(original, bytes); continue;
    }
    pack_bytes_ += replacement->pack_allocation;
    replacement->pack_hash = original->pack_hash;  // for the log; replacements are never pack originals
    replacement->pack_key = key; replacement->pack_refs = 1;
    pack_shared_.emplace(key, replacement);
    original->replacement = replacement;
    pack_upload_ = {replacement,std::move(data),0,0};
  }
  AdvanceTexturePack(cl);
  if (frame_%300==0) Logf(LogLevel::kInfo,
      "rexgpu-native: texture pack frame %llu enabled=%u pressure=%u resident_bytes=%llu limit=%llu capped=%llu shared=%llu replacements=%zu status=%s",
      (unsigned long long)frame_,unsigned(options_.texture_pack),unsigned(pack_pressure_),
      (unsigned long long)pack_bytes_,(unsigned long long)pack_limit_,(unsigned long long)pack_capped_,
      (unsigned long long)pack_shared_attached_,pack_shared_.size(),texture_pack_stream_->Status().c_str());
}

void GameRenderer::AdvanceTexturePack(nvrhi::ICommandList* cl) {
  if (!pack_upload_.replacement) return;
  auto& pending = pack_upload_;
  auto* replacement = pending.replacement;
  const auto& data = *pending.data;
  // At most 2 MiB per gameplay frame, or 16 MiB while loading: whole small
  // levels share one frame's budget (a 4x pack's mip tails no longer cost a
  // frame each), and larger levels go in strips of the remaining budget. A
  // replacement is never visible until every level has been copied and
  // transitioned.
  const uint32_t budget = resource_loading_ ? 16u << 20 : 2u << 20;
  const auto& format = kknr::GetHostFormatInfo(data.plan.format);
  const uint32_t bs = format.block_size;
  uint64_t spent = 0;
  while (pending.subresource < data.subresources.size() && spent < budget) {
    const auto& sub = data.subresources[pending.subresource];
    const uint32_t block_rows = (sub.height+bs-1)/bs;
    const uint64_t left = budget-spent;
    if (!pending.row && sub.size <= left) {
      cl->writeTexture(replacement->texture,0,sub.level,data.bytes.data()+sub.offset,sub.row_pitch,sub.depth_pitch);
      ++pending.subresource; spent += sub.size;
      stats_.pack_upload_bytes += sub.size;
      continue;
    }
    if (!pending.row && sub.size <= budget) break;  // whole in the next frame, no strip
    // The temporary strip has a legal BC base size, including tiny edge mips.
    // Copies whose BC extent isn't block-aligned must reach both image edges;
    // those uncommon NPOT mips use one whole-subresource write (<=16 MiB),
    // alone in its frame.
    if (bs==4 && ((sub.width&3) || (sub.height&3))) {
      if (spent) break;
      cl->writeTexture(replacement->texture,0,sub.level,data.bytes.data()+sub.offset,sub.row_pitch,sub.depth_pitch);
      ++pending.subresource;
      stats_.pack_upload_bytes += sub.size;
      break;
    }
    if (left < sub.row_pitch && spent) break;
    const uint32_t rows = std::min<uint32_t>(block_rows-pending.row,std::max<uint64_t>(1,left/sub.row_pitch));
    nvrhi::TextureDesc desc;
    desc.width=sub.width; desc.height=rows*bs;
    desc.format=replacement->format; desc.initialState=nvrhi::ResourceStates::CopySource;
    desc.keepInitialState=true; desc.debugName="Texture pack upload strip";
    auto strip=device_->createTexture(desc);
    if (!strip) {
      // Every original sharing it falls back to its own original.
      for (auto* original : pack_originals_) if (original->replacement == replacement) {
        original->replacement = nullptr; original->pack_unavailable = true;
      }
      pack_shared_.erase(replacement->pack_key);
      pending={}; ReleaseHostTexture(replacement); return;
    }
    const auto* pixels=data.bytes.data()+sub.offset+size_t(pending.row)*sub.row_pitch;
    cl->writeTexture(strip,0,0,pixels,sub.row_pitch,sub.row_pitch*rows);
    nvrhi::TextureSlice dst,src;
    dst.mipLevel=sub.level; dst.y=pending.row*bs; dst.width=src.width=sub.width;
    dst.height=src.height=rows*bs;
    cl->copyTexture(replacement->texture,dst,strip,src);
    RetireResource(strip);
    spent += uint64_t(rows)*sub.row_pitch;
    stats_.pack_upload_bytes += uint64_t(rows)*sub.row_pitch;
    pending.row+=rows;
    if (pending.row==block_rows) { pending.row=0; ++pending.subresource; }
  }
  if (pending.subresource == data.subresources.size()) {
    cl->setTextureState(replacement->texture,nvrhi::AllSubresources,nvrhi::ResourceStates::ShaderResource);
    replacement->pack_ready=true;
    ++stats_.pack_textures_ready;
    Logf(LogLevel::kInfo,"rexgpu-native: texture pack ready %016llX %ux%u %s %u mips bytes=%llu",
        (unsigned long long)replacement->pack_hash,data.plan.width,data.plan.height,
        kknr::GetHostFormatInfo(data.plan.format).name,data.plan.levels,(unsigned long long)replacement->pack_allocation);
    pending={};
  }
}
}  // namespace nr
