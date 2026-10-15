#include "backend/game_renderer.h"
#include "kkshaders/tessellation.h"
#include <cstring>

namespace nr {
uint32_t GameRenderer::TessellationFlags() const {
  return std::clamp(tessellation_detail_, 1u, 16u) |
         (uint32_t(std::lround(
              std::clamp(tessellation_smoothness_, 0.0f, 1.0f) * 65535.0f))
          << 8);
}

bool GameRenderer::AnnTessellationMaterial(const dev::DeviceView &d,
                                           const GameShader &vs,
                                           const GameShader *ps) {
  if (!options_.tessellation_supported || !ps)
    return false;
  const bool first = vs.ucode_hash == 0x5E779DBA373D121Cull &&
                     ps->ucode_hash == 0x7DC46286A4EA2E2Eull;
  const bool second = vs.ucode_hash == 0x3390D930CD168AC0ull &&
                      ps->ucode_hash == 0xF5BCD47A4145F958ull;
  if (!first && !second)
    return false;
  // Canonical base-level bytes. Host entries are invalidated/reuploaded by the
  // ordinary texture cache; an upload clears this identity. No address is an
  // ID.
  const uint64_t wanted[] = {0x4B3484FFDEF93A7Eull, 0x7D106E09208BE9F1ull};
  for (uint32_t slot = 0; slot < 2; slot++) {
    uint32_t words[6];
    d.fetch_constant(slot, words);
    const auto fetch = kknr::TextureFetch::FromWords(words);
    if (fetch.Type() != 2)
      return false;
    auto *entry = textures_.Find(fetch);
    if (!entry || !entry->host || entry->dirty || entry->gpu_written)
      return false;
    auto *host = static_cast<HostTexture *>(entry->host);
    const uint32_t size = slot ? 512u : 1024u;
    const auto format =
        slot ? kknr::HostFormat::BC5_UNORM : kknr::HostFormat::BC3_UNORM;
    if (host->render_target || host->alias_source || host->plan.width != size ||
        host->plan.height != size || host->plan.depth != 1 ||
        host->plan.layers != 1 ||
        host->plan.dimension != kknr::Dimension::k2D ||
        host->plan.format != format)
      return false;
    if (!host->material_hash) {
      kknr::HostTextureData data;
      if (!kknr::ConvertTexture(fetch, physical_, data, nullptr,
                                TextureOptions()))
        return false;
      const auto *base = data.Find(0, 0);
      if (!base || !base->size)
        return false;
      host->material_hash =
          kkshaders::hashBytes(data.bytes.data() + base->offset, base->size);
    }
    if (host->material_hash != wanted[slot])
      return false;
  }
  return true;
}

nvrhi::IGraphicsPipeline *GameRenderer::FindPreparedPipeline(uint64_t key) {
  std::lock_guard lock(pipeline_mutex_);
  for (auto &[k, p] : pipelines_done_) {
    pipelines_[k] = p;
    pipelines_pending_.erase(k);
    if (p) {
      ++stats_.pipelines;
      ++stats_.draw_pipelines;
    }
  }
  pipelines_done_.clear();
  const auto it = pipelines_.find(key);
  return it == pipelines_.end() ? nullptr : it->second.Get();
}

nvrhi::IGraphicsPipeline *
GameRenderer::AnnTessellationPipeline(PipelineRecord &record) {
  if (record.topology != uint32_t(nvrhi::PrimitiveType::TriangleList) ||
      record.color_count != 1)
    return nullptr;
  auto tess = record;
  tess.flags |= kRecordTessellation;
  auto *pipeline = FindPreparedPipeline(PipelineKey(tess));
  if (!pipeline)
    return nullptr;
  // Both lighting passes of the same hair mesh must be prepared. The verified
  // pair changes only its shaders, depth write and blend; reject any new state.
  const bool first = record.vs_hash == 0x5E779DBA373D121Cull;
  if (record.color_control[0] != (first ? 0x07060706u : 0x01060106u))
    return nullptr;
  bool partner_ready = false;
  std::lock_guard records_lock(cache_mutex_);
  for (const auto &candidate : tessellation_records_) {
    if (candidate.flags & kRecordMotion)
      continue;
    auto expected = tess;
    expected.vs_hash = first ? 0x3390D930CD168AC0ull : 0x5E779DBA373D121Cull;
    expected.ps_hash = first ? 0xF5BCD47A4145F958ull : 0x7DC46286A4EA2E2Eull;
    expected.vs_input = candidate.vs_input;
    expected.ps_input = candidate.ps_input;
    expected.depth_control ^= 4;
    expected.color_control[0] = first ? 0x01060106u : 0x07060706u;
    if (expected != candidate || !FindPreparedPipeline(PipelineKey(candidate)))
      continue;
    // A temporal upscaler must also have the first (depth-writing) pass's
    // tessellated motion pipeline ready before either colour pass can switch.
    if (upscale_) {
      const auto &depth = first ? tess : candidate;
      const auto motion =
          MotionPipelineRecord(depth, uint32_t(nvrhi::Format::RG16_FLOAT));
      if (!FindPreparedPipeline(PipelineKey(motion)))
        continue;
    }
    partner_ready = true;
    break;
  }
  if (!partner_ready)
    return nullptr;
  record = tess;
  return pipeline;
}
} // namespace nr
