// Player-owned replacements: asynchronous file work, bounded incremental GPU
// staging, and immediate original/pack selection without reuploading originals.
#include "backend/game_renderer.h"
#include "backend/texture_workers.h"
#include "backend/log.h"
#include "kknr/nvrhi_format.h"

namespace nr {
void GameRenderer::DropTexturePacks() {
  pack_upload_ = {};
  for (auto* original : pack_originals_) if (original->replacement) {
    ReleaseHostTexture(original->replacement);
    original->replacement = nullptr;
  }
}

// The original stays: its replacement does not fit the current limit. Only the
// count, smallest size and frame are kept; the decoded bytes are already freed.
void GameRenderer::CapTexturePack(HostTexture* original, uint64_t bytes) {
  if (!pack_capped_) Logf(LogLevel::kWarning,
      "rexgpu-native: texture pack limit reached, keeping originals: resident_bytes=%llu limit=%llu next=%llu",
      (unsigned long long)pack_bytes_,(unsigned long long)pack_limit_,(unsigned long long)bytes);
  original->pack_capped=true; ++pack_capped_;
  pack_capped_min_=std::min(pack_capped_min_,bytes); pack_capped_frame_=frame_;
}

void GameRenderer::ServiceTexturePacks(nvrhi::ICommandList* cl) {
  if (pack_frame_ == frame_) return;
  pack_frame_ = frame_;
  if (options_.texture_pack_folder != texture_pack_folder_) {
    DropTexturePacks();
    for (auto* original : pack_originals_) original->pack_unavailable=original->pack_capped=false;
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
  if (!texture_pack_stream_ || texture_pack_folder_.empty() || pack_pressure_) return;
  // Capped originals ask again only when the smallest capped replacement fits
  // (the limit grew or replacements were freed), at most every 600 frames.
  if (pack_capped_ && pack_limit_ > pack_bytes_ && pack_limit_-pack_bytes_ >= pack_capped_min_ &&
      frame_ >= pack_capped_frame_+600) {
    for (auto* original : pack_originals_) original->pack_capped=false;
    pack_capped_=0; pack_capped_min_=UINT64_MAX;
  }
  // Prepare even when comparing Off, so switching On is just a descriptor choice.
  // The stream limits pending requests, decoded bytes and worker concurrency.
  for (auto* original : pack_originals_) if (!original->render_target && !original->alias_source &&
      !original->replacement && !original->pack_unavailable && !original->pack_capped && original->pack_hash) {
    texture_pack_stream_->Request(original->pack_hash, original->plan);
    if (pack_upload_.original) continue;
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
    original->replacement = replacement;
    pack_upload_ = {original,std::move(data),0,0};
  }
  AdvanceTexturePack(cl);
  if (frame_%300==0) Logf(LogLevel::kInfo,
      "rexgpu-native: texture pack frame %llu enabled=%u pressure=%u resident_bytes=%llu limit=%llu capped=%llu status=%s",
      (unsigned long long)frame_,unsigned(options_.texture_pack),unsigned(pack_pressure_),
      (unsigned long long)pack_bytes_,(unsigned long long)pack_limit_,(unsigned long long)pack_capped_,
      texture_pack_stream_->Status().c_str());
}

void GameRenderer::AdvanceTexturePack(nvrhi::ICommandList* cl) {
  if (!pack_upload_.original) return;
  auto& pending = pack_upload_;
  auto* replacement = pending.original->replacement;
  if (!replacement) { pending={}; return; }
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
      break;
    }
    if (left < sub.row_pitch && spent) break;
    const uint32_t rows = std::min<uint32_t>(block_rows-pending.row,std::max<uint64_t>(1,left/sub.row_pitch));
    nvrhi::TextureDesc desc;
    desc.width=sub.width; desc.height=rows*bs;
    desc.format=replacement->format; desc.initialState=nvrhi::ResourceStates::CopySource;
    desc.keepInitialState=true; desc.debugName="Texture pack upload strip";
    auto strip=device_->createTexture(desc);
    if (!strip) { ReleaseHostTexture(replacement); pending.original->replacement=nullptr; pending={}; return; }
    const auto* pixels=data.bytes.data()+sub.offset+size_t(pending.row)*sub.row_pitch;
    cl->writeTexture(strip,0,0,pixels,sub.row_pitch,sub.row_pitch*rows);
    nvrhi::TextureSlice dst,src;
    dst.mipLevel=sub.level; dst.y=pending.row*bs; dst.width=src.width=sub.width;
    dst.height=src.height=rows*bs;
    cl->copyTexture(replacement->texture,dst,strip,src);
    RetireResource(strip);
    spent += uint64_t(rows)*sub.row_pitch;
    pending.row+=rows;
    if (pending.row==block_rows) { pending.row=0; ++pending.subresource; }
  }
  if (pending.subresource == data.subresources.size()) {
    cl->setTextureState(replacement->texture,nvrhi::AllSubresources,nvrhi::ResourceStates::ShaderResource);
    replacement->pack_ready=true;
    Logf(LogLevel::kInfo,"rexgpu-native: texture pack ready %016llX %ux%u %s %u mips bytes=%llu",
        (unsigned long long)pending.original->pack_hash,data.plan.width,data.plan.height,
        kknr::GetHostFormatInfo(data.plan.format).name,data.plan.levels,(unsigned long long)replacement->pack_allocation);
    pending={};
  }
}
}  // namespace nr
