#include "backend/game_renderer.h"
#include "kkshaders/tessellation.h"
#include <cstring>
#include "backend/guest_layout.h"

namespace nr {
uint64_t GameRenderer::GeometryContentHash(const DrawCall& call) const {
  if (!call.indexed || !call.index_data || !call.stride || !call.vertex_data) return 0;
  const uint32_t bytes = call.index_info.index32 ? 4 : 2;
  if ((uint64_t(call.start)+call.count)*bytes > call.index_info.size_bytes) return 0;
  uint64_t hash = kkshaders::hashBytes(call.index_data+size_t(call.start)*bytes,size_t(call.count)*bytes);
  for (uint32_t i=0;i<call.count;++i) {
    const auto* p=call.index_data+size_t(call.start+i)*bytes;
    const uint32_t index=bytes==4 ? LoadBE32(p) : LoadBE16(p);
    if (index==(bytes==4 ? UINT32_MAX : UINT16_MAX)) continue;
    const int64_t vertex=int64_t(index)+call.base_vertex;
    if (vertex<0 || (uint64_t(vertex)+1)*call.stride>call.vertex_data_size) return 0;
    const uint64_t v=kkshaders::hashBytes(call.vertex_data+size_t(vertex)*call.stride,call.stride);
    hash ^= v+0x9E3779B97F4A7C15ull+(hash<<6)+(hash>>2);
    hash *= 0xFF51AFD7ED558CCDull;
  }
  return hash;
}

bool GameRenderer::AnnMainTessellation(const DrawCall& call, const dev::DeviceView& d,
    const GameShader& vs, const GameShader* ps, kkshaders::DrawConstants& dc,
    PipelineRecord& record, nvrhi::IGraphicsPipeline*& pipeline) {
  if (!options_.tessellation_supported || (!tessellation_ && !tessellation_previous_flags_ && !TessellationBenchRequested())) return false;
  const uint64_t h=vs.ucode_hash;
  const bool depth=h==0xD710A475A7840F37ull || h==0xC66476D2889954ECull;
  const bool first=h==0x7149CE3656F92235ull || h==0x91632BD5704802BDull;
  const bool second=h==0xA6E7AF900E6F2741ull || h==0x448D8A65A65954D8ull;
  if (!depth && !first && !second) return false;
  const uint64_t mesh=GeometryContentHash(call);
  const bool clothing=mesh==0x40022003D57903FBull && call.count==5298;
  const bool skin=(mesh==0x4602AF0699809BC4ull && call.count==3024) ||
                  (mesh==0x5E97B95CF5FEFC4Eull && call.count==2901);
  const bool face=mesh==0xBE2A0AED8846FA65ull && call.count==7779;
  if (!clothing && !skin && !face) return false;
  const uint64_t expected=depth ? (face?0xC66476D2889954ECull:0xD710A475A7840F37ull) :
      first ? (face?0x91632BD5704802BDull:0x7149CE3656F92235ull) :
              (face?0x448D8A65A65954D8ull:0xA6E7AF900E6F2741ull);
  if (h!=expected || record.topology!=uint32_t(nvrhi::PrimitiveType::TriangleList) || !record.depth_format) return false;
  if (depth && (ps || (record.depth_control&6)!=6)) return false;
  if (!depth) {
    const uint64_t pixel=first ? (clothing?0x01D62D86BB133315ull:0x3A076C4DF061DFA7ull) :
                                (clothing?0xBBD239D3BEB254D9ull:0x07BA5284CF0C43AEull);
    if (!ps || ps->ucode_hash!=pixel || !AnnTessellationMaterial(d,vs,ps)) return false;
  }
  auto& learned=ann_normals_[mesh];
  if (!depth) {
    // Read the very same declaration element/format the lit binding used.
    const kknr::VertexElement* normal=nullptr;
    for (const auto& e:scratch_declaration_) if(e.usage==uint8_t(kkshaders::DeclUsage::Normal) && e.usage_index==0) normal=&e;
    if (!normal || normal->stream>=16 || !(dc.vertexFetch[1][3]&63)) return false;
    if (!learned.noted && TessellationBenchRequested())
      Logf(LogLevel::kInfo,"rexgpu-native: ANN NORMAL mesh %016llX stream %u offset %u type %08X stride %u format %08X",
           (unsigned long long)mesh,normal->stream,normal->offset,normal->type.value,dc.vertexFetch[1][2],dc.vertexFetch[1][3]);
    learned.noted=true;
    // All admitted meshes keep integer UBYTE4 bone indices in the hashed stream.
    // Compare only referenced rows: unused palette registers legitimately contain
    // another pass's stale lighting/projection constants.
    const kknr::VertexElement* indices=nullptr;
    for (const auto& e:scratch_declaration_) if(e.usage==uint8_t(kkshaders::DeclUsage::BlendIndices) && e.usage_index==0) indices=&e;
    if (!indices || indices->stream!=0 || indices->type.Format()!=kknr::VertexFormat::k_8_8_8_8 ||
        !indices->type.Integer() || indices->offset+4>call.stride) return false;
    VertexBufferInfo normal_buffer;
    const uint32_t object=d.u32(dev::kStreamBuffer+8*normal->stream);
    if(!object || !DecodeVertexBuffer(memory_.Virtual(object),normal_buffer)) return false;
    const auto* normal_data=memory_.Physical(normal_buffer.physical);
    if(!normal_data || !normal_buffer.size_bytes) return false;
    learned.buffer=normal_buffer;
    learned.offset=d.u32(dev::kStreamOffset+8*normal->stream);
    learned.normal_content=kkshaders::hashBytes(normal_data,normal_buffer.size_bytes);
    uint64_t bones=0;
    const unsigned ib=call.index_info.index32?4:2;
    for(uint32_t i=0;i<call.count;++i) {
      const auto* ip=call.index_data+size_t(call.start+i)*ib;
      const int64_t vertex=int64_t(ib==4?LoadBE32(ip):LoadBE16(ip))+call.base_vertex;
      if(vertex<0 || (uint64_t(vertex)+1)*call.stride>call.vertex_data_size) return false;
      const auto* bi=call.vertex_data+size_t(vertex)*call.stride+indices->offset;
      for(unsigned j=0;j<4;++j) { if(bi[j]>=(face?12:48)) return false; bones|=1ull<<bi[j]; }
    }
    if(!bones) return false;
    learned.bones=bones;
    const unsigned pass=second?1:0;
    learned.element=*normal;
    learned.stride=dc.vertexFetch[1][2];
    learned.format=dc.vertexFetch[1][3];
    learned.lit[pass]=record;
    learned.seen|=1u<<pass;
    learned.learned=call.frame;
  }
  uint64_t palette=0;
  for(unsigned bone=0;bone<48;++bone) if(learned.bones&(1ull<<bone)) {
    const uint64_t v=kkshaders::hashBytes(d.at(dev::kVsConstants+bone*3*16),3*16);
    palette ^= v+0x9E3779B97F4A7C15ull+(palette<<6)+(palette>>2);
  }
  if (depth) {
    learned.previous_flags=learned.decision_frame+1==call.frame && learned.admitted ? learned.current_flags : 0;
    learned.current_flags=tessellation_ ? TessellationFlags() : 0;
    learned.decision_frame=call.frame;
    learned.admitted=false;
    learned.skin=palette;
    if (!(learned.seen&1) || learned.learned>=call.frame || !learned.stride ||
        dc.vertexFetch[15][2]!=learned.stride || dc.vertexFetch[15][3]!=learned.format) return true;
    const auto* normal_data=memory_.Physical(learned.buffer.physical);
    if(!normal_data || kkshaders::hashBytes(normal_data,learned.buffer.size_bytes)!=learned.normal_content) return true;
    auto tess=record; tess.flags|=kRecordTessellation;
    auto* prepared=FindPreparedPipeline(PipelineKey(tess));
    if (!prepared) return true;
    for (unsigned pass=0;pass<2;++pass) {
      if(!(learned.seen&(1u<<pass))) continue;
      auto lit=learned.lit[pass];
      lit.flags|=kRecordTessellation;
      if (!FindPreparedPipeline(PipelineKey(lit))) return true;
    }
    if (upscale_ && !FindPreparedPipeline(PipelineKey(MotionPipelineRecord(tess,uint32_t(nvrhi::Format::RG16_FLOAT))))) return true;
    learned.admitted=true;
    if (tessellation_ || learned.previous_flags) { record=tess; pipeline=prepared; dc.reserved=tessellation_ ? TessellationFlags() : (learned.previous_flags&255u); }
  } else if ((tessellation_ || learned.previous_flags) && learned.decision_frame==call.frame && learned.admitted && learned.skin==palette) {
    auto tess=record; tess.flags|=kRecordTessellation;
    if (auto* prepared=FindPreparedPipeline(PipelineKey(tess))) { record=tess; pipeline=prepared; dc.reserved=tessellation_ ? TessellationFlags() : (learned.previous_flags&255u); }
  }
  return true;
}

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
  const bool mainFirst = vs.ucode_hash==0x7149CE3656F92235ull || vs.ucode_hash==0x91632BD5704802BDull;
  const bool mainSecond = vs.ucode_hash==0xA6E7AF900E6F2741ull || vs.ucode_hash==0x448D8A65A65954D8ull;
  const bool clothing = ps->ucode_hash==0x01D62D86BB133315ull || ps->ucode_hash==0xBBD239D3BEB254D9ull;
  if (!first && !second && !mainFirst && !mainSecond) return false;
  // Canonical base-level bytes. Host entries are invalidated/reuploaded by the
  // ordinary texture cache; an upload clears this identity. No address is an
  // ID.
  const uint64_t wanted[] = {clothing?0x88F33E0AAFA4C35Cull:0x4B3484FFDEF93A7Eull, clothing?0x3CB7C020D642C006ull:0x7D106E09208BE9F1ull};
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
    const uint32_t size = slot && !clothing ? 512u : 1024u;
    const auto format =
        slot ? kknr::HostFormat::BC5_UNORM : clothing ? kknr::HostFormat::BC1_UNORM : kknr::HostFormat::BC3_UNORM;
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
