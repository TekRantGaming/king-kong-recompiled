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

void GameRenderer::ServiceTexturePacks(nvrhi::ICommandList* cl) {
  if (pack_frame_ == frame_) return;
  pack_frame_ = frame_;
  if (options_.texture_pack_folder != texture_pack_folder_) {
    DropTexturePacks();
    for (auto* original : pack_originals_) original->pack_unavailable=false;
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
  // Prepare even when comparing Off, so switching On is just a descriptor choice.
  // The stream limits pending requests, decoded bytes and worker concurrency.
  for (auto* original : pack_originals_) if (!original->render_target && !original->alias_source &&
      !original->replacement && !original->pack_unavailable && original->pack_hash) {
    texture_pack_stream_->Request(original->pack_hash, original->plan);
    if (pack_upload_.original || pack_bytes_ >= pack_limit_) continue;
    auto data = texture_pack_stream_->Take(original->pack_hash, original->plan, pack_limit_-pack_bytes_);
    if (!data) continue;
    const auto support=device_->queryFormatSupport(kknr::ToNvrhi(data->plan.format));
    if ((support & nvrhi::FormatSupport::Texture) == nvrhi::FormatSupport::None ||
        (support & nvrhi::FormatSupport::ShaderSample) == nvrhi::FormatSupport::None) {
      original->pack_unavailable=true; continue;
    }
    auto* replacement = CreateHostTexture(data->plan, false);
    if (!replacement) { original->pack_unavailable=true; continue; }
    replacement->pack_allocation = device_->getTextureMemoryRequirements(replacement->texture).size;
    if (replacement->pack_allocation > pack_limit_-pack_bytes_) {
      replacement->pack_allocation=0; ReleaseHostTexture(replacement); original->pack_unavailable=true; continue;
    }
    pack_bytes_ += replacement->pack_allocation;
    original->replacement = replacement;
    pack_upload_ = {original,std::move(data),0,0};
  }
  AdvanceTexturePack(cl);
  if (frame_%300==0) Logf(LogLevel::kInfo,
      "rexgpu-native: texture pack frame %llu enabled=%u pressure=%u resident_bytes=%llu limit=%llu status=%s",
      (unsigned long long)frame_,unsigned(options_.texture_pack),unsigned(pack_pressure_),
      (unsigned long long)pack_bytes_,(unsigned long long)pack_limit_,texture_pack_stream_->Status().c_str());
}

void GameRenderer::AdvanceTexturePack(nvrhi::ICommandList* cl) {
  if (!pack_upload_.original) return;
  auto& pending = pack_upload_;
  auto* replacement = pending.original->replacement;
  if (!replacement) { pending={}; return; }
  const auto& data = *pending.data;
  // At most one 2 MiB strip per gameplay frame, or 16 MiB while loading.
  // Small mip tails go straight to their destination. A replacement is never
  // visible until every level has been copied and transitioned.
  const uint32_t budget = resource_loading_ ? 16u << 20 : 2u << 20;
  const auto& sub = data.subresources[pending.subresource];
  const auto& format = kknr::GetHostFormatInfo(data.plan.format);
  const uint32_t bs = format.block_size;
  const uint32_t block_rows = (sub.height+bs-1)/bs;
  if (!pending.row && sub.size <= budget) {
    cl->writeTexture(replacement->texture,0,sub.level,data.bytes.data()+sub.offset,sub.row_pitch,sub.depth_pitch);
    ++pending.subresource;
  } else {
    // The temporary strip has a legal BC base size, including tiny edge mips.
    // Copies whose BC extent isn't block-aligned must reach both image edges;
    // those uncommon NPOT mips use one whole-subresource write (<=16 MiB).
    if (bs==4 && ((sub.width&3) || (sub.height&3))) {
      cl->writeTexture(replacement->texture,0,sub.level,data.bytes.data()+sub.offset,sub.row_pitch,sub.depth_pitch);
      ++pending.subresource;
    } else {
      const uint32_t rows = std::min(block_rows-pending.row,std::max(1u,budget/sub.row_pitch));
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
      pending.row+=rows;
      if (pending.row==block_rows) { pending.row=0; ++pending.subresource; }
    }
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
