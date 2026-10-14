#include "backend/game_renderer.h"
#include "kkshaders/tessellation.h"

#include <algorithm>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>

#include "backend/guest_layout.h"
#include "backend/log.h"
#include "backend/pipeline_workers.h"
#include "backend/renderer.h"
#include "backend/shaders.h"
#include "backend/fallback_resources.h"
#include "kknr/buffers.h"
#include "kknr/nvrhi_format.h"

namespace nr {

namespace {

// XXH3-64 of the microcode of the title's two AfterEffects fog pixel shaders.
constexpr uint64_t kAoFogShaderA = UINT64_C(0x1B65AC7429B67FE9);
constexpr uint64_t kAoFogShaderB = UINT64_C(0x9F699E763A41FE07);

inline uint64_t Mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 29);
}

// Xenos BlendFactor -> NVRHI (CONSTANT_ALPHA has no NVRHI name: the
// constant colour stands in, exact when the blend colour is grey).
nvrhi::BlendFactor BlendFactorFrom(uint32_t f) {
  using B = nvrhi::BlendFactor;
  switch (f) {
    case 0: return B::Zero;
    case 1: return B::One;
    case 4: return B::SrcColor;
    case 5: return B::InvSrcColor;
    case 6: return B::SrcAlpha;
    case 7: return B::InvSrcAlpha;
    case 8: return B::DstColor;
    case 9: return B::InvDstColor;
    case 10: return B::DstAlpha;
    case 11: return B::InvDstAlpha;
    case 12: return B::ConstantColor;
    case 13: return B::InvConstantColor;
    case 14: return B::ConstantColor;
    case 15: return B::InvConstantColor;
    case 16: return B::SrcAlphaSaturate;
    default: return B::One;
  }
}

nvrhi::BlendOp BlendOpFrom(uint32_t op) {
  using O = nvrhi::BlendOp;
  switch (op) {
    case 0: return O::Add;
    case 1: return O::Subtract;
    case 2: return O::Min;
    case 3: return O::Max;
    case 4: return O::ReverseSubtract;
    default: return O::Add;
  }
}

nvrhi::BlendState::RenderTarget BlendTargetFrom(uint32_t control, uint32_t mask) {
  nvrhi::BlendState::RenderTarget t;
  const uint32_t cs = control & 0x1F, cop = (control >> 5) & 7, cd = (control >> 8) & 0x1F;
  const uint32_t as = (control >> 16) & 0x1F, aop = (control >> 21) & 7, ad = (control >> 24) & 0x1F;
  const bool color_off = cs == 1 && cd == 0 && cop == 0;
  const bool alpha_off = as == 1 && ad == 0 && aop == 0;
  t.blendEnable = !(color_off && alpha_off);
  t.srcBlend = BlendFactorFrom(cs);
  t.destBlend = BlendFactorFrom(cd);
  t.blendOp = BlendOpFrom(cop);
  t.srcBlendAlpha = BlendFactorFrom(as);
  t.destBlendAlpha = BlendFactorFrom(ad);
  t.blendOpAlpha = BlendOpFrom(aop);
  // The alpha factors cannot be colour factors in the alpha channel on every
  // API: the colour ones there mean the same as the alpha ones.
  auto to_alpha = [](nvrhi::BlendFactor f) {
    switch (f) {
      case nvrhi::BlendFactor::SrcColor: return nvrhi::BlendFactor::SrcAlpha;
      case nvrhi::BlendFactor::InvSrcColor: return nvrhi::BlendFactor::InvSrcAlpha;
      case nvrhi::BlendFactor::DstColor: return nvrhi::BlendFactor::DstAlpha;
      case nvrhi::BlendFactor::InvDstColor: return nvrhi::BlendFactor::InvDstAlpha;
      default: return f;
    }
  };
  t.srcBlendAlpha = to_alpha(t.srcBlendAlpha);
  t.destBlendAlpha = to_alpha(t.destBlendAlpha);
  t.colorWriteMask = nvrhi::ColorMask(mask & 0xF);
  return t;
}

nvrhi::StencilOp StencilOpFrom(uint32_t op) { return nvrhi::StencilOp((op & 7) + 1); }
nvrhi::ComparisonFunc CompareFrom(uint32_t f) { return nvrhi::ComparisonFunc((f & 7) + 1); }

}  // namespace

GameRenderer::GameRenderer(nvrhi::IDevice* device, const GuestMemory& memory,
                           kknr::GuestMemory physical, ShaderLibrary* shaders)
    : device_(device), memory_(memory), physical_(physical), shaders_(shaders) {
  textures_.release_host = [this](kknr::TextureCache::Entry& e) {
    for (auto it = resolved_by_base_.begin(); it != resolved_by_base_.end();) {
      it = it->second == &e ? resolved_by_base_.erase(it) : std::next(it);
    }
    ReleaseHostTexture(static_cast<HostTexture*>(e.host));
    e.host = nullptr;
  };
  auto release_buffer = [this](kknr::BufferCache::Entry& e) {
    auto* h = static_cast<HostBuffer*>(e.host);
    if (h && h->slot) RetireSlot(kBuffers, h->slot);
    if (h) RetireResource(h->buffer);
    delete h;
    e.host = nullptr;
  };
  vertex_buffers_.release_host = release_buffer;
  index_buffers_.release_host = release_buffer;
}

GameRenderer::~GameRenderer() {
  if (gi_build_.valid()) gi_build_.wait();
  if (lighting_build_.valid()) lighting_build_.wait();
  if (upscale_build_.valid()) upscale_build_.wait();
  if (dlss_warm_.valid()) dlss_warm_.wait();
  if (frame_trace_) std::fclose(frame_trace_);
  if (!pipeline_threads_.empty()) {
    {
      std::lock_guard lock(pipeline_mutex_);
      pipeline_stop_ = true;
    }
    pipeline_cv_.notify_all();
    for (std::thread& t : pipeline_threads_) t.join();
  }
  // What this run added to the driver's pipeline cache (the workers are stopped).
  if (driver_cache_ && driver_cache_new_.load() > 0) SaveDriverPipelineCache();
  textures_.Trim(UINT64_MAX / 2, 0);
  vertex_buffers_.Trim(UINT64_MAX / 2, 0);
  index_buffers_.Trim(UINT64_MAX / 2, 0);
}

namespace {
// Adds [start, start + bytes) to a map of disjoint ranges (start -> end), merged with the ranges it
// overlaps (not the ones it only touches: neighbours stay apart).
void AddRange(std::map<uint32_t, uint32_t>& ranges, uint32_t physical, uint32_t bytes) {
  uint32_t start = physical, end = physical + bytes;
  auto it = ranges.upper_bound(start);
  if (it != ranges.begin() && std::prev(it)->second > start) --it;
  while (it != ranges.end() && it->first < end) {
    start = std::min(start, it->first);
    end = std::max(end, it->second);
    it = ranges.erase(it);
  }
  ranges[start] = end;
}
// The first range of the map that overlaps [start, end), or end().
std::map<uint32_t, uint32_t>::iterator FirstOverlap(std::map<uint32_t, uint32_t>& ranges, uint32_t start,
                                                    uint32_t end) {
  auto it = ranges.upper_bound(start);
  if (it != ranges.begin() && std::prev(it)->second > start) --it;
  return it != ranges.end() && it->first < end ? it : ranges.end();
}
}  // namespace

void GameRenderer::set_watch(std::function<void(uint32_t, uint32_t)> watch) {
  watch_ = watch;
  // --native_debug bit 64: arm the watches at every check, upload and resolve (debugging).
  textures_.watch_once = vertex_buffers_.watch_once = index_buffers_.watch_once = !(options_.debug & 64);
  if (watch) {
    // Buffers: their ranges are unwatched whole on a write (OnGuestWrite). Textures: never
    // widened, and a widening that would reach one is not made: a texture a resolve wrote holds
    // the GPU's picture, which guest memory does not have, and invalidating it for a write
    // beside it would replace the picture with stale memory.
    vertex_buffers_.watch_range = index_buffers_.watch_range = [this, watch](uint32_t physical, uint32_t bytes) {
      if (!bytes) return;
      {
        std::lock_guard lock(watched_mutex_);
        AddRange(watched_buffers_, physical, bytes);
      }
      watch(physical, bytes);
    };
    textures_.watch_range = [this, watch](uint32_t physical, uint32_t bytes) {
      if (!bytes) return;
      {
        std::lock_guard lock(watched_mutex_);
        AddRange(watched_textures_, physical, bytes);
      }
      watch(physical, bytes);
    };
  } else {
    textures_.watch_range = vertex_buffers_.watch_range = index_buffers_.watch_range = nullptr;
  }
}

bool GameRenderer::Initialize() {
  const nvrhi::GraphicsAPI api = device_->getGraphicsAPI();
  // Live settings (game_effects.cpp): what the start asked for, which the changes are compared with.
  upscale_initial_ = upscale_requested_ = options_.upscale;
  lighting_initial_ = options_.lighting;
  gi_user_gpu_time_ = options_.gi.gpu_time;
  fxaa_wanted_ = options_.fxaa;
  // Upscalers (game_upscale.cpp): the upscaler and its input scale first (scaled() depends on it).
  if (options_.upscale.enabled() && !UpscaleSetup()) upscale_.reset();
  if (options_.effect_settings) vendor_ = UpscalerDeviceVendor(device_);
  if (options_.effect_settings && vendor_ == 0x10DE) {
    // NVIDIA: NGX's start (seconds) on a worker now, so a live switch to DLSS is quick (game_effects.cpp).
    UpscaleConfig probe;
    probe.kind = UpscalerKind::kDlss;
    dlss_warm_ = std::async(std::launch::async, [device = device_, probe] {
      std::string why;
      return CreateDlssUpscaler(device, probe, &why);
    });
  }
  shaders_->SetMotionPrewarm(bool(upscale_));

  // Set 0: the three constant buffers at b0-b2 (binding = register on Vulkan).
  nvrhi::BindingLayoutDesc constants;
  constants.visibility = nvrhi::ShaderType::All;
  constants.registerSpace = 0;
  constants.bindingOffsets = nvrhi::VulkanBindingOffsets()
                                 .setShaderResourceOffset(0)
                                 .setSamplerOffset(0)
                                 .setConstantBufferOffset(0)
                                 .setUnorderedAccessViewOffset(0);
  constants.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                        nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
                        nvrhi::BindingLayoutItem::VolatileConstantBuffer(2)};
  // b3: the render scale constants, only when scaling (the 1:1 layout is the one it always was).
  if (scaled()) constants.bindings.push_back(nvrhi::BindingLayoutItem::VolatileConstantBuffer(3));
  AoLayoutBindings(constants);  // AO hook (game_ao.cpp): b4 for the ambient-only shaders
  LightingLayoutBindings(constants);  // Lighting hook (game_lighting.cpp): b5 for the lighting options
  constants_layout_ = device_->createBindingLayout(constants);
  if (!constants_layout_) return false;

  auto make_cb = [&](uint32_t size, const char* name) {
    nvrhi::BufferDesc d;
    d.byteSize = size;
    d.isConstantBuffer = true;
    d.isVolatile = true;
    d.maxVersions = 8192;
    d.debugName = name;
    return device_->createBuffer(d);
  };
  vs_constants_ = make_cb(4096, "Game VS constants");
  ps_constants_ = make_cb(4096, "Game PS constants");
  draw_constants_ = make_cb(sizeof(kkshaders::DrawConstants), "Game draw constants");
  if (!vs_constants_ || !ps_constants_ || !draw_constants_) return false;
  nvrhi::BindingSetDesc cs;
  cs.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, vs_constants_),
                 nvrhi::BindingSetItem::ConstantBuffer(1, ps_constants_),
                 nvrhi::BindingSetItem::ConstantBuffer(2, draw_constants_)};
  if (scaled()) {
    scale_constants_ = make_cb(sizeof(kkshaders::ScaleConstants), "Game scale constants");
    if (!scale_constants_) return false;
    cs.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(3, scale_constants_));
  }
  if (!AoSetBindings(cs)) return false;  // AO hook (game_ao.cpp)
  if (!LightingSetBindings(cs)) return false;  // Lighting hook (game_lighting.cpp)
  constants_set_ = device_->createBindingSet(cs, constants_layout_);
  if (!constants_set_) return false;

  // Sets 1-6: the bindless tables. On D3D12 the item's slot is the register
  // space; on Vulkan each table is its own descriptor set (binding 0).
  struct Spec {
    Table table;
    nvrhi::BindingLayoutItem item;
    uint32_t capacity;
  };
  const Spec specs[] = {
      {kTex2D, nvrhi::BindingLayoutItem::Texture_SRV(1), 16384},
      {kTex3D, nvrhi::BindingLayoutItem::Texture_SRV(2), 256},
      {kTexCube, nvrhi::BindingLayoutItem::Texture_SRV(3), 2048},
      {kTex2DArray, nvrhi::BindingLayoutItem::Texture_SRV(4), 256},
      {kSamplers, nvrhi::BindingLayoutItem::Sampler(5), 1024},
      {kBuffers, nvrhi::BindingLayoutItem::RawBuffer_SRV(6), 16384},
  };
  pipeline_layouts_ = {constants_layout_};
  for (const Spec& s : specs) {
    nvrhi::BindlessLayoutDesc d;
    d.visibility = nvrhi::ShaderType::All;
    d.firstSlot = 0;
    d.maxCapacity = s.capacity;
    d.registerSpaces.push_back(s.item);
    DescriptorTable& t = tables_[s.table];
    t.layout = device_->createBindlessLayout(d);
    if (!t.layout) return false;
    t.table = device_->createDescriptorTable(t.layout);
    if (!t.table) return false;
    device_->resizeDescriptorTable(t.table, s.capacity, false);
    t.capacity = s.capacity;
    pipeline_layouts_.push_back(t.layout);
  }

  // Slot 0 of every table: a transparent black texture, a point sampler and
  // a small zero buffer, for unset or unusable fetch constants.
  auto dummy = [&](nvrhi::TextureDimension dim, uint32_t layers, const char* name) {
    nvrhi::TextureDesc d;
    d.width = d.height = 1;
    d.depth = 1;
    d.arraySize = layers;
    d.dimension = dim;
    d.format = nvrhi::Format::RGBA8_UNORM;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = name;
    return device_->createTexture(d);
  };
  dummy_2d_ = dummy(nvrhi::TextureDimension::Texture2D, 1, "Dummy 2D");
  dummy_3d_ = dummy(nvrhi::TextureDimension::Texture3D, 1, "Dummy 3D");
  dummy_cube_ = dummy(nvrhi::TextureDimension::TextureCube, 6, "Dummy cube");
  dummy_array_ = dummy(nvrhi::TextureDimension::Texture2DArray, 1, "Dummy array");
  nvrhi::SamplerDesc sd;
  sd.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
  dummy_sampler_ = device_->createSampler(sd);
  nvrhi::BufferDesc bd;
  bd.byteSize = 256;
  bd.canHaveRawViews = true;
  bd.initialState = nvrhi::ResourceStates::ShaderResource;
  bd.keepInitialState = true;
  bd.debugName = "Dummy vertex data";
  dummy_buffer_ = device_->createBuffer(bd);
  if (!dummy_2d_ || !dummy_3d_ || !dummy_cube_ || !dummy_array_ || !dummy_sampler_ || !dummy_buffer_)
    return false;
  WriteSlot(kTex2D, nvrhi::BindingSetItem::Texture_SRV(0, dummy_2d_));
  WriteSlot(kTex3D, nvrhi::BindingSetItem::Texture_SRV(0, dummy_3d_));
  WriteSlot(kTexCube, nvrhi::BindingSetItem::Texture_SRV(0, dummy_cube_, nvrhi::Format::UNKNOWN,
                                                          nvrhi::AllSubresources,
                                                          nvrhi::TextureDimension::TextureCube));
  WriteSlot(kTex2DArray, nvrhi::BindingSetItem::Texture_SRV(0, dummy_array_, nvrhi::Format::UNKNOWN,
                                                             nvrhi::AllSubresources,
                                                             nvrhi::TextureDimension::Texture2DArray));
  WriteSlot(kSamplers, nvrhi::BindingSetItem::Sampler(0, dummy_sampler_));
  WriteSlot(kBuffers, nvrhi::BindingSetItem::RawBuffer_SRV(0, dummy_buffer_));

  // Index patterns and the ring.
  auto make_ib = [&](uint64_t size, const char* name) {
    nvrhi::BufferDesc d;
    d.byteSize = size;
    d.isIndexBuffer = true;
    d.initialState = nvrhi::ResourceStates::IndexBuffer;
    d.keepInitialState = true;
    d.debugName = name;
    return device_->createBuffer(d);
  };
  quad_indices_ = make_ib(uint64_t(kPatternVertices / 4) * 6 * 4, "Quad list indices");
  fan_indices_ = make_ib(uint64_t(kPatternVertices) * 3 * 4, "Fan indices");
  index_ring_ = make_ib(kIndexRingBytes, "Converted index ring");
  if (!quad_indices_ || !fan_indices_ || !index_ring_) return false;

  // Built-in passes.
  auto make_shader = [&](ShaderId id, nvrhi::ShaderType type) -> nvrhi::ShaderHandle {
    ShaderBytecode code = GetShaderBytecode(api, id);
    if (!code.data) return nullptr;
    nvrhi::ShaderDesc desc;
    desc.shaderType = type;
    desc.entryName = type == nvrhi::ShaderType::Vertex ? "vs_main" : "ps_main";
    return device_->createShader(desc, code.data, code.size);
  };
  blit_vs_ = make_shader(ShaderId::kBlitVs, nvrhi::ShaderType::Vertex);
  blit_ps_ = make_shader(ShaderId::kBlitPs, nvrhi::ShaderType::Pixel);
  clear_vs_ = make_shader(ShaderId::kClearVs, nvrhi::ShaderType::Vertex);
  clear_ps_ = make_shader(ShaderId::kClearPs, nvrhi::ShaderType::Pixel);
  if (!blit_vs_ || !blit_ps_ || !clear_vs_ || !clear_ps_) return false;
  nvrhi::BindingLayoutDesc bl;
  bl.visibility = nvrhi::ShaderType::All;
  bl.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, 48), nvrhi::BindingLayoutItem::Texture_SRV(0),
                 nvrhi::BindingLayoutItem::Texture_SRV(1)};
  nvrhi::TextureDesc rd;
  rd.width = 256;
  rd.height = 1;
  rd.format = nvrhi::Format::RGBA16_UNORM;
  rd.initialState = nvrhi::ResourceStates::ShaderResource;
  rd.keepInitialState = true;
  rd.debugName = "Gamma ramp";
  gamma_ramp_ = device_->createTexture(rd);
  if (!gamma_ramp_) return false;
  blit_layout_ = device_->createBindingLayout(bl);
  nvrhi::BindingLayoutDesc cl;
  cl.visibility = nvrhi::ShaderType::All;
  cl.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, 32)};
  clear_layout_ = device_->createBindingLayout(cl);
  if (!blit_layout_ || !clear_layout_) return false;
  nvrhi::BindingSetDesc cset;
  cset.bindings = {nvrhi::BindingSetItem::PushConstants(0, 32)};
  clear_set_ = device_->createBindingSet(cset, clear_layout_);
  if (!clear_set_) return false;
  // Ambient occlusion: optional, never a reason to refuse to start.
  ao_ = std::make_unique<AmbientOcclusion>(device_);
  if (!ao_->Initialize()) ao_.reset();
  AoInitialize();  // AO hook (game_ao.cpp): the GTAO method
  GiInitialize();  // GI hook (game_gi.cpp): the screen-space GI
  // Upscalers: their passes. Without them the renderer runs as without an upscaler (logged).
  if (upscale_ && !UpscaleInitialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: upscaler: the passes could not be made; no upscaler");
    upscale_.reset();
    shaders_->SetMotionPrewarm(false);
  }
  LightingInitialize();  // Lighting hook (game_lighting.cpp)
  return true;
}

void GameRenderer::UploadDummies(nvrhi::ICommandList* cl) {
  // Frame start and first-use paths share one initialization flag and must upload
  // the same contents, including the identity gamma ramp and committed barriers.
  EnsureFallbackResources(cl);
}

// ---------------------------------------------------------------- tables

void GameRenderer::EnsureFallbackResources(nvrhi::ICommandList* cl) {
  if (dummies_uploaded_) return;
  UploadFallbackResources(cl, dummy_2d_.Get(), dummy_3d_.Get(), dummy_cube_.Get(),
                          dummy_array_.Get(), dummy_buffer_.Get(), gamma_ramp_.Get());
  dummies_uploaded_ = true;
}

uint32_t GameRenderer::AllocateSlot(Table table) {
  DescriptorTable& t = tables_[table];
  if (!t.free.empty()) {
    uint32_t s = t.free.back();
    t.free.pop_back();
    return s;
  }
  if (t.next < t.capacity) return t.next++;
  return 0;  // full: the dummy
}

void GameRenderer::RetireResource(nvrhi::IResource* resource) {
  if (resource) retired_resources_.emplace_back(frame_, resource);
}

void GameRenderer::RetireSlot(Table table, uint32_t slot) {
  if (slot) tables_[table].retired.emplace_back(frame_, slot);
}

void GameRenderer::WriteSlot(Table table, const nvrhi::BindingSetItem& item) {
  device_->writeDescriptorTable(tables_[table].table, item);
  ++stats_.descriptor_updates;
}

std::pair<uint32_t, uint32_t> GameRenderer::OnGuestWrite(uint32_t physical, uint32_t bytes) {
  if (options_.debug & 128) {  // debugging: the written range only
    InvalidateRange(physical, bytes);
    return {physical, bytes};
  }
  const uint32_t write_end = physical + std::max(bytes, 1u);
  uint32_t start = physical, end = write_end;
  {
    std::lock_guard lock(watched_mutex_);
    // The buffer ranges the write touches, whole.
    for (auto it = FirstOverlap(watched_buffers_, physical, write_end); it != watched_buffers_.end() &&
                                                                         it->first < write_end;
         ++it) {
      start = std::min(start, it->first);
      end = std::max(end, it->second);
    }
    // Not over a texture beyond what the write itself touched.
    const bool widened = start < physical || end > write_end;
    const bool over_texture = widened && ((start < physical && FirstOverlap(watched_textures_, start, physical) !=
                                                                   watched_textures_.end()) ||
                                          (end > write_end && FirstOverlap(watched_textures_, write_end, end) !=
                                                                  watched_textures_.end()));
    if (over_texture) {
      start = physical;
      end = write_end;
    }
    // What is unwatched now is watched no more.
    for (auto* ranges : {&watched_buffers_, &watched_textures_}) {
      for (auto it = FirstOverlap(*ranges, start, end); it != ranges->end() && it->first < end;) {
        // A range partly outside stays (the SDK keeps the rest of its pages watched).
        if (it->first >= start && it->second <= end) {
          it = ranges->erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  InvalidateRange(start, end - start);
  return {start, end - start};
}

void GameRenderer::InvalidateRange(uint32_t physical, uint32_t bytes) {
  std::lock_guard lock(invalidation_mutex_);
  invalidations_.emplace_back(physical, bytes);
}

void GameRenderer::ProcessInvalidations() {
  std::vector<std::pair<uint32_t, uint32_t>> list;
  {
    std::lock_guard lock(invalidation_mutex_);
    list.swap(invalidations_);
  }
  for (const auto& [start, bytes] : list) {
    textures_.InvalidateRange(start, bytes);
    vertex_buffers_.InvalidateRange(start, bytes);
    index_buffers_.InvalidateRange(start, bytes);
    if (options_.debug & 8) {
      ++invalidation_histogram_[start & ~0xFFFu];
      invalidation_bytes_ += bytes;
    }
  }
  stats_.invalidations += list.size();
}

void GameRenderer::LogInvalidationHistogram() {
  // --native_debug=8: the pages CPU writes invalidated most since the last report.
  std::vector<std::pair<uint64_t, uint32_t>> top;
  for (const auto& [page, count] : invalidation_histogram_) top.emplace_back(count, page);
  std::sort(top.begin(), top.end(), std::greater<>());
  std::string text;
  for (size_t i = 0; i < top.size() && i < 12; ++i) {
    // Which caches own the page: textures, vertex and index buffers (entries, their uploads).
    uint64_t tu = 0, vu = 0, iu = 0;
    const size_t t = textures_.CountOverlapping(top[i].second, 4096, &tu);
    const size_t v = vertex_buffers_.CountOverlapping(top[i].second, 4096, &vu);
    const size_t n = index_buffers_.CountOverlapping(top[i].second, 4096, &iu);
    char item[128];
    std::snprintf(item, sizeof(item), " %08X x%llu [t%zu/%llu v%zu/%llu i%zu/%llu]", top[i].second,
                  static_cast<unsigned long long>(top[i].first), t, static_cast<unsigned long long>(tu), v,
                  static_cast<unsigned long long>(vu), n, static_cast<unsigned long long>(iu));
    text += item;
  }
  Logf(LogLevel::kInfo, "rexgpu-native: invalidations by page (%zu pages, %llu bytes):%s", top.size(),
       static_cast<unsigned long long>(invalidation_bytes_), text.c_str());
  invalidation_histogram_.clear();
  invalidation_bytes_ = 0;
}

void GameRenderer::NoteFrameWait(double ms) {
  frame_waited_ = true;
  frame_stutter_wait_ms_ += ms;
}

GameRenderer::StutterStats GameRenderer::stutter() const {
  std::lock_guard lock(stutter_mutex_);
  return stutter_;
}

void GameRenderer::TrimClearTargets() {
  // A cache entry owns an attachment reference, not the contents of EDRAM.
  // Drop references only after recording ends; the ordinary eight-frame
  // retirement queue outlives the two-frame GPU completion bound. NVRHI also
  // retains resources referenced by submitted command lists until completion.
  for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
    auto used = framebuffer_used_.find(it->first);
    if (used != framebuffer_used_.end() && frame_ > used->second && frame_ - used->second > 120) {
      RetireResource(it->second);
      framebuffer_used_.erase(used);
      it = framebuffers_.erase(it);
    } else ++it;
  }
  // Upscaler and GTAO keep independent attachment views and can write targets
  // outside GetFramebuffer. Until those writes participate in clear tracking,
  // retain their targets rather than incorrectly restore an old clear.
  if (upscale_ || scene_ao_) return;
  for (auto& [key, t] : targets_) {
    if (!t || !t->texture || !CanEvictClearTarget(t->clear_only, bool(upscale_ || scene_ao_),
                                                t->used_frame, frame_)) continue;
    if (auto it = blit_sets_.find(t->texture.Get()); it != blit_sets_.end()) {
      RetireResource(it->second);
      blit_sets_.erase(it);
    }
    RetireResource(t->texture);
    t->texture = nullptr;
    stats_.edram_target_bytes -= t->allocation_bytes;
    t->allocation_bytes = 0;
    ++memory_evictions_;
  }
}

void GameRenderer::CheckMemoryBudget() {
  const auto now = std::chrono::steady_clock::now();
  if (memory_sample_ != std::chrono::steady_clock::time_point{} &&
      now - memory_sample_ < std::chrono::seconds(1)) return;
  const bool first = memory_sample_ == std::chrono::steady_clock::time_point{};
  memory_sample_ = now;
  const MemoryBudget live = options_.memory_budget ? options_.memory_budget() : MemoryBudget{};
  if (!live.valid) {
    if (first) Logf(LogLevel::kWarning, "rexgpu-native: live GPU memory budget unavailable; adaptation disabled");
    memory_pressure_samples_ = 0;
    return;
  }
  uint64_t cap = 0;
  if (const char* value = std::getenv("KK_DEV_VRAM_BUDGET_GB")) {
    if (std::strcmp(value, "4") == 0) cap = 4ull << 30;
    else if (std::strcmp(value, "6") == 0) cap = 6ull << 30;
    else if (std::strcmp(value, "8") == 0) cap = 8ull << 30;
    else if (first) Logf(LogLevel::kWarning, "rexgpu-native: KK_DEV_VRAM_BUDGET_GB must be 4, 6 or 8; ignored");
  }
  const MemoryBudget effective = CapMemoryBudget(live, cap);
  MemoryBudget tested = effective;
  // Developer fault injection: exercise pressure handling on a large GPU
  // without allocating gigabytes or disturbing other applications. The live
  // values remain separately logged. Requires an explicit synthetic cap.
  const char* force = std::getenv("KK_DEV_VRAM_PRESSURE");
  const bool simulated = cap && force && std::strcmp(force, "1") == 0;
  if (simulated) tested.usage = std::max(tested.usage, tested.budget);
  const bool pressure = MemoryPressure(tested);
  const char* control = std::getenv("KK_DEV_VRAM_CONTROL");
  const bool enabled = !control || std::strcmp(control, "0") != 0;
  if (enabled && pressure) {
    // Reclaim first. Wait five one-second samples before reducing GTAO; never
    // wait for the GPU, flush the queue, or restore quality in a pressure loop.
    if (memory_pressure_samples_ % 5 == 0) TrimClearTargets();
    if (++memory_pressure_samples_ >= 5 && scene_ao_ && !memory_half_ao_) {
      scene_ao_->SetMemoryPressure(true);
      memory_half_ao_ = true;
      Logf(LogLevel::kWarning, "rexgpu-native: GPU budget pressure: GTAO reduced to half resolution");
    }
  } else {
    memory_pressure_samples_ = 0;
  }
  Logf(LogLevel::kInfo,
       "rexgpu-native: memory budget frame %llu: usage=%llu budget=%llu effective=%llu pressure=%u "
       "control=%u targets=%llu target_bytes=%llu framebuffers=%zu retired=%zu evictions=%llu restores=%llu half_ao=%u simulated=%u tested_usage=%llu",
       static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(live.usage),
       static_cast<unsigned long long>(live.budget), static_cast<unsigned long long>(effective.budget),
       unsigned(pressure), unsigned(enabled), static_cast<unsigned long long>(stats_.edram_targets),
       static_cast<unsigned long long>(stats_.edram_target_bytes), framebuffers_.size(), retired_resources_.size(),
       static_cast<unsigned long long>(memory_evictions_), static_cast<unsigned long long>(memory_restores_),
       unsigned(memory_half_ao_), unsigned(simulated), static_cast<unsigned long long>(tested.usage));
}

void GameRenderer::EndFrame() {
  if (tessellation_timer_) tessellation_timer_->FrameEnd();
  PublishEffectTimes();
  if (TessellationBenchRequested() && frame_%300==0)
    Logf(LogLevel::kInfo,"rexgpu-native: ANN TESS status frame %llu enabled %d detail %u draws %u prepared %zu",(unsigned long long)frame_,int(tessellation_),tessellation_detail_,tessellation_draws_this_frame_,tessellation_records_.size());
  tessellation_previous_flags_ = tessellation_drawn_this_frame_ ? TessellationFlags() : 0;
  tessellation_drawn_this_frame_ = false;
  tessellation_draws_this_frame_ = 0;
  if (!frame_trace_checked_) {
    frame_trace_checked_ = true;
    if (const char* path = std::getenv("KK_DEV_DLSS_LOG"); path && *path) {
      frame_trace_ = std::fopen(path, "w");
      if (frame_trace_) std::fprintf(frame_trace_, "frame,wall_s,interval_ms,pipeline_wait_ms,motion_cpu_ms,sdk_cpu_ms,target_cpu_ms,resource_creations,skipped_draws\n");
    }
  }
  if (frame_trace_) {
    const auto now = std::chrono::steady_clock::now();
    const double interval = frame_trace_last_ == std::chrono::steady_clock::time_point{} ? 0 :
        std::chrono::duration<double, std::milli>(now - frame_trace_last_).count();
    std::fprintf(frame_trace_, "%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%llu\n",
        static_cast<unsigned long long>(frame_), std::chrono::duration<double>(now.time_since_epoch()).count(),
        interval, frame_stutter_wait_ms_, frame_motion_ms_, frame_sdk_ms_, stats_.target_ms - frame_target_ms_last_,
        static_cast<unsigned long long>(frame_resource_creations_), static_cast<unsigned long long>(frame_skips_));
    frame_trace_last_ = now;
    if (frame_ % 300 == 0) std::fflush(frame_trace_);
  }
  frame_motion_ms_ = frame_sdk_ms_ = 0;
  frame_resource_creations_ = 0;
  frame_target_ms_last_ = stats_.target_ms;
  {
    std::lock_guard lock(stutter_mutex_);
    ++stutter_.frames;
    if (frame_waited_) {
      ++stutter_.frames_waited;
      stutter_.wait_ms += frame_stutter_wait_ms_;
      if (frame_stutter_wait_ms_ > stutter_.worst_wait_ms) {
        stutter_.worst_wait_ms = frame_stutter_wait_ms_;
        stutter_.worst_wait_frame = frame_;
      }
    }
    if (frame_skips_) {
      ++stutter_.frames_skipped;
      stutter_.draws_skipped += frame_skips_;
    }
    if (frame_waited_ || frame_skips_) ++stutter_.stutter_frames;
  }
  frame_waited_ = false;
  frame_stutter_wait_ms_ = 0;
  frame_skips_ = 0;
  if (upscale_) UpscaleEndFrame();
  ++frame_;
  current_cl_ = nullptr;
  index_ring_used_ = 0;
  for (DescriptorTable& t : tables_) {
    while (!t.retired.empty() && t.retired.front().first + 8 < frame_) {
      t.free.push_back(t.retired.front().second);
      t.retired.pop_front();
    }
  }
  while (!retired_resources_.empty() && retired_resources_.front().first + 8 < frame_) retired_resources_.pop_front();
  CheckMemoryBudget();
  stats_.edram_framebuffers = framebuffers_.size();
  if (frame_ % 300 == 0) {
    if (options_.debug & 32768) {
      Logf(LogLevel::kInfo,
           "rexgpu-native: EDRAM cache: %llu targets, %llu target allocation bytes, %llu framebuffers "
           "(framebuffers share target storage; excludes retired resources and upscaler auxiliary caches)",
           static_cast<unsigned long long>(stats_.edram_targets),
           static_cast<unsigned long long>(stats_.edram_target_bytes),
           static_cast<unsigned long long>(stats_.edram_framebuffers));
      size_t slots = 0;
      for (const DescriptorTable& t : tables_) slots += t.next - 1 - t.free.size() - t.retired.size();
      Logf(LogLevel::kInfo,
           "rexgpu-native: residency frame %llu: textures %zu, vertex buffers %zu, index buffers %zu, "
           "descriptor slots %zu, core binding sets %zu, retired resources %zu; per frame: %.1f draws, "
           "%.1f clears, %.1f resolves, %.1f guest upload bytes, %.3f ms textures, %.3f ms buffers; pipelines %llu",
           static_cast<unsigned long long>(frame_), textures_.size(), vertex_buffers_.size(), index_buffers_.size(),
           slots, blit_sets_.size() + bool(constants_set_) + bool(clear_set_), retired_resources_.size(),
           double(stats_.draws - debug_previous_.draws) / 300,
           double(stats_.clears - debug_previous_.clears) / 300,
           double(stats_.resolves - debug_previous_.resolves) / 300,
           double(stats_.upload_bytes - debug_previous_.upload_bytes) / 300,
           (stats_.texture_ms - debug_previous_.texture_ms) / 300,
           (stats_.buffer_ms - debug_previous_.buffer_ms) / 300,
           static_cast<unsigned long long>(stats_.pipelines));
      Logf(LogLevel::kInfo,
           "rexgpu-native: perf frame %llu: scale writes %llu, identical %llu; vertex binds %llu, repeated %llu; "
           "descriptor updates %llu, graphics states %llu, game pipeline transitions %llu (300 frames)",
           static_cast<unsigned long long>(frame_),
           static_cast<unsigned long long>(stats_.scale_uploads - debug_previous_.scale_uploads),
           static_cast<unsigned long long>(stats_.scale_identical - debug_previous_.scale_identical),
           static_cast<unsigned long long>(stats_.vertex_binds - debug_previous_.vertex_binds),
           static_cast<unsigned long long>(stats_.vertex_repeated - debug_previous_.vertex_repeated),
           static_cast<unsigned long long>(stats_.descriptor_updates - debug_previous_.descriptor_updates),
           static_cast<unsigned long long>(stats_.graphics_states - debug_previous_.graphics_states),
           static_cast<unsigned long long>(stats_.pipeline_changes - debug_previous_.pipeline_changes));
      debug_previous_ = stats_;
    }
    if (options_.debug & 8) LogInvalidationHistogram();
    textures_.Trim(frame_, 1800);
    vertex_buffers_.Trim(frame_, 1800);
    index_buffers_.Trim(frame_, 1800);
  }
  RetryDeferred();
  stats_.driver_cache_saves = driver_cache_saves_.load(std::memory_order_relaxed);
  stats_.driver_cache_bytes = driver_cache_bytes_.load(std::memory_order_relaxed);
  std::lock_guard lock(cache_mutex_);
  cache_file_.Flush();
}

void GameRenderer::PipelineWorker() {
  for (;;) {
    PipelineJob job;
    {
      std::unique_lock lock(pipeline_mutex_);
      pipeline_cv_.wait(lock, [&] {
        // Reserve one compiler for demand work while prewarm has a backlog.
        const unsigned background = std::max(1u, unsigned(pipeline_threads_.size()) - 1);
        return pipeline_stop_ || (!pipeline_jobs_.empty() &&
            (pipeline_jobs_.front().urgent || unsigned(pipelines_active_) < background));
      });
      if (pipeline_stop_) return;
      job = std::move(pipeline_jobs_.front());
      pipeline_jobs_.pop_front();
      ++pipelines_active_;
    }
    const auto start = std::chrono::steady_clock::now();
    nvrhi::GraphicsPipelineHandle p = CreatePipeline(job.desc, job.framebuffer);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    bool idle = false;
    {
      std::lock_guard lock(pipeline_mutex_);
      pipeline_worker_ms_ += ms;
      pipelines_done_.emplace_back(job.key, p);
      ++pipelines_made_;
      // The last job of a burst: this worker stays counted while it saves the driver's cache, so
      // WaitForPrewarm includes the save.
      idle = pipeline_jobs_.empty() && pipelines_active_ == 1;
      if (!idle) --pipelines_active_;
    }
    pipeline_done_cv_.notify_all();
    pipeline_cv_.notify_all();
    if (idle) {
      MaybeSaveDriverCache();
      {
        std::lock_guard lock(pipeline_mutex_);
        --pipelines_active_;
      }
      pipeline_done_cv_.notify_all();
      pipeline_cv_.notify_all();
    }
  }
}

nvrhi::GraphicsPipelineHandle GameRenderer::CreatePipeline(const nvrhi::GraphicsPipelineDesc& desc,
                                                           const nvrhi::FramebufferInfo& framebuffer) {
  std::shared_lock lock(creation_mutex_);
  nvrhi::GraphicsPipelineHandle p = CreateGraphicsPipeline(device_, desc, framebuffer);
  if (p && driver_cache_) driver_cache_new_.fetch_add(1, std::memory_order_relaxed);
  return p;
}

bool GameRenderer::IsPending(uint64_t key) {
  std::lock_guard lock(pipeline_mutex_);
  return pipelines_pending_.count(key) != 0;
}

nvrhi::IGraphicsPipeline* GameRenderer::GetPipeline(const nvrhi::GraphicsPipelineDesc& desc,
                                                    const nvrhi::FramebufferInfo& framebuffer, uint64_t key,
                                                    bool async, bool optional) {
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  // optional (the upscalers' motion pass): made on the workers, never waited for, not counted as a draw
  // that waited or was skipped.
  const bool for_draws = async && !optional;
  async = async && (optional || options_.async_pipelines);
  if (async && optional) {
    std::unique_lock lock(pipeline_mutex_);
    for (auto& [k, p] : pipelines_done_) {
      pipelines_[k] = p;
      pipelines_pending_.erase(k);
      if (p) {
        ++stats_.pipelines;
        ++stats_.draw_pipelines;
      }
    }
    pipelines_done_.clear();
    if (auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;
    if (!pipelines_pending_.count(key)) {
      EnsureWorkers();
      pipelines_pending_[key] = true;
      pipeline_jobs_.push_back({key, desc, framebuffer});
      pipeline_cv_.notify_one();
    }
    return nullptr;
  }
  if (async) {
    // Finished pipelines (the draws' and the prewarm's) join the map.
    auto drain = [&] {
      for (auto& [k, p] : pipelines_done_) {
        pipelines_[k] = p;
        pipelines_pending_.erase(k);
        if (p) {
          ++stats_.pipelines;
          ++stats_.draw_pipelines;
        }
      }
      pipelines_done_.clear();
      stats_.pipeline_ms = pipeline_worker_ms_;
    };
    std::unique_lock lock(pipeline_mutex_);
    drain();
    if (auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;
    if (!pipelines_pending_.count(key)) {
      EnsureWorkers();
      pipelines_pending_[key] = true;
      pipeline_jobs_.push_front({key, desc, framebuffer, true});
      pipeline_cv_.notify_all();
    } else {
      // Promote an unstarted prewarm job too. An already active job needs no duplicate.
      if (PromotePipelineJob(pipeline_jobs_, key)) pipeline_cv_.notify_all();
    }
    if (options_.pipeline_wait) {
      // Wait for the worker that has this pipeline (the draw's own job, or the prewarm's).
      const auto start = std::chrono::steady_clock::now();
      pipeline_done_cv_.wait(lock, [&] {
        drain();
        return pipelines_.count(key) != 0 || pipeline_stop_;
      });
      ++stats_.pipeline_waits;
      const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      stats_.pipeline_wait_ms += waited;
      NoteFrameWait(waited);
      auto found = pipelines_.find(key);
      return found != pipelines_.end() ? found->second.Get() : nullptr;
    }
    if (for_draws && options_.pipeline_wait_budget_ms > 0) {
      // Wait within what is left of this frame's budget.
      if (wait_budget_frame_ != frame_) {
        wait_budget_frame_ = frame_;
        frame_wait_ms_ = 0;
      }
      const double left = options_.pipeline_wait_budget_ms - frame_wait_ms_;
      if (left > 0) {
        const auto start = std::chrono::steady_clock::now();
        pipeline_done_cv_.wait_for(lock, std::chrono::duration<double, std::milli>(left), [&] {
          drain();
          return pipelines_.count(key) != 0 || pipeline_stop_;
        });
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        frame_wait_ms_ += ms;
        stats_.pipeline_wait_ms += ms;
        NoteFrameWait(ms);
        if (auto found = pipelines_.find(key); found != pipelines_.end()) {
          ++stats_.pipeline_budget_waits;
          return found->second;
        }
      }
    }
    ++stats_.skipped_pending;
    ++frame_skips_;
    return nullptr;
  }
  const auto start = std::chrono::steady_clock::now();
  nvrhi::GraphicsPipelineHandle p = CreatePipeline(desc, framebuffer);
  const double made_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  stats_.pipeline_ms += made_ms;
  if (for_draws) NoteFrameWait(made_ms);  // the draw waited for the driver on its own thread
  pipelines_[key] = p;  // a failure is remembered too (null)
  if (p) {
    ++stats_.pipelines;
    if (for_draws) ++stats_.draw_pipelines;
  }
  return p;
}

// ------------------------------------------------------- the pipeline cache

void GameRenderer::EnsureWorkers() {
  // With pipeline_mutex_ held. A few workers: drivers compile pipelines in parallel.
  if (!pipeline_threads_.empty()) return;
  const unsigned n = ProcessPipelineWorkerCount();
  Logf(LogLevel::kInfo, "rexgpu-native: pipeline compiler workers %u (process affinity)", n);
  for (unsigned i = 0; i < n; ++i) pipeline_threads_.emplace_back([this] { PipelineWorker(); });
}

void GameRenderer::NoteRecord(const PipelineRecord& record, uint64_t key) {
  if (!cache_recording_.load(std::memory_order_relaxed)) return;
  if (!noted_keys_.insert(key).second) return;  // the render thread's own set: no lock for a known pipeline
  std::lock_guard lock(cache_mutex_);
  if (cache_file_.is_open() && cache_file_.Append(record)) ++stats_.cache_recorded;
}

bool GameRenderer::QueuePrewarm(const PipelineRecord& record) {
  PipelineRecord adapted = record;
  if (!(record.flags & kRecordMotion))
    adapted.flags = (record.flags & ~kRecordScaled) | (RecordFlags() & kRecordScaled);
  const kkshaders::ShaderKind vk = kkshaders::ShaderKind::Vertex, pk = kkshaders::ShaderKind::Pixel;
  std::shared_ptr<const GameShader> vs = shaders_->GetByHash(vk, record.vs_hash, record.vs_input);
  std::shared_ptr<const GameShader> ps =
      record.ps_hash ? shaders_->GetForPrewarm(pk, record.ps_hash, record.ps_input,
                                              record.flags == adapted.flags) : nullptr;
  if (!vs || (record.ps_hash && !ps)) return false;
  if (ps) adapted.ps_input = ps->input_hash;
  const bool motion = (record.flags & kRecordMotion) != 0;
  const bool tess = (record.flags & kRecordTessellation) != 0;
  if (tess && (!options_.tessellation_supported || !shaders_->AnnTessellationStages(vs->ucode_hash,vs->input_hash,motion))) return false;
  nvrhi::IShader* vertex = vs->handle;
  if (motion && !tess) {
    // Prepared motion shaders are keyed by the original translation and need no guest container.
    if (!upscale_) return false;
    vertex = shaders_->MotionVariant(*vs);
    if (!vertex) return false;
  }
  nvrhi::GraphicsPipelineDesc desc;
  nvrhi::FramebufferInfo framebuffer;
  BuildPipelineDesc(adapted, vertex, motion ? upscale_->object_ps.Get() : (ps ? ps->handle.Get() : nullptr), desc, framebuffer);
  if (motion && !desc.bindingLayouts.empty()) desc.bindingLayouts[0] = upscale_->motion_layout;
  if (tess && std::find(tessellation_records_.begin(),tessellation_records_.end(),adapted)==tessellation_records_.end())
    tessellation_records_.push_back(adapted);
  const uint64_t key = PipelineKey(adapted);
  std::lock_guard lock(pipeline_mutex_);
  if (pipelines_.count(key) || pipelines_pending_.count(key)) return true;
  EnsureWorkers();
  pipelines_pending_[key] = true;
  pipeline_jobs_.push_back({key, std::move(desc), framebuffer});
  pipeline_cv_.notify_one();
  ++stats_.prewarm_queued;
  return true;
}

uint32_t GameRenderer::RecordFlags() const {
  return (options_.flip_front_face ? kRecordFlipFrontFace : 0u) | (scaled() ? kRecordScaled : 0u);
}

void GameRenderer::QueueRecordVariants(const PipelineRecord& record) {
  QueueOrDefer(record);
  if (upscale_ && !(record.flags & kRecordMotion) && record.depth_format && (record.depth_control & 6) == 6)
    QueueOrDefer(MotionPipelineRecord(record, uint32_t(nvrhi::Format::RG16_FLOAT)));
  if (options_.tessellation_supported && kkshaders::annTessellationShader(record.vs_hash) && !(record.flags & kRecordTessellation)) {
    auto tess=record; tess.flags |= kRecordTessellation;
    QueueOrDefer(tess);
    if (upscale_ && !(record.flags & kRecordMotion) && record.depth_format && (record.depth_control&6)==6)
      QueueOrDefer(MotionPipelineRecord(tess,uint32_t(nvrhi::Format::RG16_FLOAT)));
  }
}

void GameRenderer::QueueOrDefer(const PipelineRecord& record) {
  // With cache_mutex_ held. Each description once, from whichever source (the cache file, the
  // shipped list) names it first.
  if (!prewarm_seen_.insert(PipelineKey(record)).second) return;
  // Scale variants are adapted by QueuePrewarm; flipped faces and unknown flags stay isolated.
  const bool motion = (record.flags & kRecordMotion) != 0;
  const uint32_t wanted = (motion ? ((RecordFlags() & kRecordFlipFrontFace) | kRecordMotion) : RecordFlags()) |
                          (record.flags & kRecordTessellation);
  if ((record.flags & ~kRecordScaled) != (wanted & ~kRecordScaled) || (motion && !upscale_)) {
    ++stats_.prewarm_other_kind;
    return;
  }
  if (!QueuePrewarm(record)) {
    deferred_.push_back(record);
    ++stats_.prewarm_deferred;
  }
}

bool GameRenderer::SetPipelineCache(const std::string& path) {
  std::lock_guard lock(cache_mutex_);
  const bool opened = cache_file_.Open(path);
  cache_recording_.store(opened, std::memory_order_relaxed);
  if (!cache_file_.note().empty()) {
    Logf(LogLevel::kWarning, "rexgpu-native: pipeline cache %s ignored and started again: %s", path.c_str(),
         cache_file_.note().c_str());
  }
  const bool can_prewarm = options_.async_pipelines;
  if (can_prewarm) {
    for (const PipelineRecord& record : cache_file_.loaded()) {
      QueueRecordVariants(record);
    }
    deferred_shader_count_ = shaders_->created_count();
  }
  Logf(LogLevel::kInfo, "rexgpu-native: pipeline cache %s: %zu descriptions, %llu queued, %llu waiting for their shaders%s",
       path.c_str(), cache_file_.loaded().size(), static_cast<unsigned long long>(stats_.prewarm_queued),
       static_cast<unsigned long long>(stats_.prewarm_deferred), can_prewarm ? "" : " (no prewarm: needs async pipelines)");
  return opened;
}

bool GameRenderer::LoadPipelineList(const std::string& path) {
  std::vector<PipelineRecord> records;
  std::string note;
  size_t torn = 0;
  if (!ReadPipelineList(path, records, &note, &torn)) {
    Logf(note.empty() ? LogLevel::kInfo : LogLevel::kWarning, "rexgpu-native: pipeline list %s not used: %s",
         path.c_str(), note.empty() ? "absent" : note.c_str());
    return false;
  }
  std::lock_guard lock(cache_mutex_);
  const uint64_t queued = stats_.prewarm_queued, deferred = stats_.prewarm_deferred,
                 other = stats_.prewarm_other_kind;
  const bool can_prewarm = options_.async_pipelines;
  if (can_prewarm) {
    for (const PipelineRecord& record : records) {
      QueueRecordVariants(record);
    }
    deferred_shader_count_ = shaders_->created_count();
  }
  stats_.list_records += records.size();
  Logf(LogLevel::kInfo,
       "rexgpu-native: pipeline list %s: %zu descriptions, %llu queued, %llu waiting for their shaders, %llu of "
       "another render scale%s%s",
       path.c_str(), records.size(), static_cast<unsigned long long>(stats_.prewarm_queued - queued),
       static_cast<unsigned long long>(stats_.prewarm_deferred - deferred),
       static_cast<unsigned long long>(stats_.prewarm_other_kind - other), torn ? " (an incomplete last record)" : "",
       can_prewarm ? "" : " (no prewarm: needs async pipelines)");
  return true;
}

void GameRenderer::RetryDeferred() {
  std::lock_guard lock(cache_mutex_);
  if (deferred_.empty()) return;
  const uint64_t created = shaders_->created_count();
  const auto motion = shaders_->motion_stats();
  const uint64_t completed = motion.made + motion.failed;
  if (created == deferred_shader_count_ && completed == deferred_motion_count_) return;
  deferred_shader_count_ = created;
  deferred_motion_count_ = completed;
  std::vector<PipelineRecord> still;
  for (const PipelineRecord& record : deferred_) {
    if (!QueuePrewarm(record)) still.push_back(record);
  }
  deferred_ = std::move(still);
}

DriverPipelineCache::Load GameRenderer::SetDriverPipelineCache(DriverPipelineCache* cache, const std::string& path) {
  std::unique_lock lock(creation_mutex_);  // no pipeline is created while the file is read
  std::lock_guard save_lock(driver_cache_save_mutex_);
  driver_cache_ = cache;
  driver_cache_path_ = path;
  std::string note;
  const DriverPipelineCache::Load load = cache->LoadFile(path, &note);
  stats_.driver_cache_load = int32_t(load);
  driver_cache_new_.store(0);
  driver_cache_saved_at_ = std::chrono::steady_clock::now();
  if (load == DriverPipelineCache::Load::kLoaded) {
    Logf(LogLevel::kInfo, "rexgpu-native: driver pipeline cache %s: loaded (%s)", path.c_str(), note.c_str());
  } else if (load == DriverPipelineCache::Load::kAbsent) {
    // The first run: saved as soon as the workers are first idle, not after the usual count.
    driver_cache_rebuild_.store(true);
    Logf(LogLevel::kInfo, "rexgpu-native: driver pipeline cache %s: %s, made by this run", path.c_str(), note.c_str());
  } else {
    driver_cache_rebuild_.store(true);
    Logf(LogLevel::kWarning,
         "rexgpu-native: driver pipeline cache %s: %s (%s): started empty, rebuilt in the background by the "
         "prewarm and saved over the old file when the workers are done",
         path.c_str(), LoadResultName(load), note.c_str());
  }
  return load;
}

void GameRenderer::MaybeSaveDriverCache() {
  if (!driver_cache_) return;
  const uint64_t fresh = driver_cache_new_.load(std::memory_order_relaxed);
  if (fresh == 0) return;
  std::unique_lock lock(driver_cache_save_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return;  // another worker is saving
  const double since =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - driver_cache_saved_at_).count();
  if (!driver_cache_rebuild_.load() && fresh < options_.driver_cache_save_every &&
      since < options_.driver_cache_save_seconds) {
    return;
  }
  SaveDriverCacheLocked();
}

bool GameRenderer::SaveDriverPipelineCache() {
  if (!driver_cache_) return false;
  std::lock_guard lock(driver_cache_save_mutex_);
  return SaveDriverCacheLocked();
}

bool GameRenderer::SaveDriverCacheLocked() {
  const uint64_t fresh = driver_cache_new_.exchange(0);
  const auto start = std::chrono::steady_clock::now();
  size_t bytes = 0;
  std::string note;
  const bool ok = driver_cache_->SaveFile(driver_cache_path_, &bytes, &note);
  driver_cache_saved_at_ = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(driver_cache_saved_at_ - start).count();
  if (!ok) {
    driver_cache_new_.fetch_add(fresh);  // still to be saved
    Logf(LogLevel::kWarning, "rexgpu-native: driver pipeline cache %s not saved: %s", driver_cache_path_.c_str(),
         note.c_str());
    return false;
  }
  driver_cache_rebuild_.store(false);
  driver_cache_saves_.fetch_add(1);
  driver_cache_bytes_.store(bytes);
  Logf(LogLevel::kInfo, "rexgpu-native: driver pipeline cache %s saved: %s, %llu pipelines new since the last save, %.0f ms",
       driver_cache_path_.c_str(), note.c_str(), static_cast<unsigned long long>(fresh), ms);
  return true;
}

GameRenderer::WorkerProgress GameRenderer::worker_progress() {
  std::lock_guard lock(pipeline_mutex_);
  return {stats_.prewarm_queued, pipelines_made_, uint64_t(pipeline_jobs_.size()), uint64_t(pipelines_active_)};
}

void GameRenderer::WaitForPrewarm() {
  std::unique_lock lock(pipeline_mutex_);
  pipeline_done_cv_.wait(lock, [&] { return pipeline_stop_ || (pipeline_jobs_.empty() && pipelines_active_ == 0); });
}

// ---------------------------------------------------------------- viewport

GameRenderer::ViewportSetup GameRenderer::ComputeViewport(const dev::DeviceView& d, uint32_t width,
                                                          uint32_t height, float scale_x, float scale_y) const {
  // As the Xenos plugin does (src/graphics/util/draw.cpp GetHostViewportInfo):
  // the host viewport is the whole target and the vertex shader applies the
  // guest's viewport transform (or none: screen-space draws give pixels), the
  // window offset and the half-pixel offset. Y: the translated shaders keep
  // D3D's clip space (+Y up, Vulkan gets -fvk-invert-y), so the pixel row
  // grows downwards as the NDC Y goes down.
  ViewportSetup v;
  const uint32_t vte = d.u32(dev::kPaClVteCntl);
  const uint32_t clip = d.u32(dev::kPaClClipCntl);
  const uint32_t mode = d.u32(dev::kPaSuScModeCntl);
  const uint32_t vtx = d.u32(dev::kPaSuVtxCntl);
  const float sx = (vte & 1) ? d.f32(dev::kPaClVport + 0) : 1.0f;
  const float ox = (vte & 2) ? d.f32(dev::kPaClVport + 4) : 0.0f;
  const float sy = (vte & 4) ? d.f32(dev::kPaClVport + 8) : 1.0f;
  const float oy = (vte & 8) ? d.f32(dev::kPaClVport + 12) : 0.0f;
  const float sz = (vte & 16) ? d.f32(dev::kPaClVport + 16) : 1.0f;
  const float oz = (vte & 32) ? d.f32(dev::kPaClVport + 20) : 0.0f;
  float add_x = 0.0f, add_y = 0.0f;
  const uint32_t window_offset = d.u32(dev::kPaScWindowOffset);
  const int32_t wx = int32_t(window_offset << 17) >> 17;
  const int32_t wy = int32_t((window_offset >> 16) << 17) >> 17;
  if (mode & (1u << 16)) {  // vtx_window_offset_enable
    add_x += float(wx);
    add_y += float(wy);
  }
  if ((vtx & 1) == 0) {  // pix_center: D3D9's integer pixel centres
    add_x += 0.5f;
    add_y += 0.5f;
  }
  const float w = float(std::max(width, 1u)), h = float(std::max(height, 1u));
  v.ndc_scale[0] = sx * 2.0f / w;
  v.ndc_offset[0] = (ox + add_x) * 2.0f / w - 1.0f;
  v.ndc_scale[1] = -sy * 2.0f / h;
  v.ndc_offset[1] = 1.0f - (oy + add_y) * 2.0f / h;
  if (device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN) {
    // NVRHI flips Vulkan viewports to D3D's +Y up, and the translated vertex
    // shaders are compiled with -fvk-invert-y as well: undo one of the two.
    v.ndc_scale[1] = -v.ndc_scale[1];
    v.ndc_offset[1] = -v.ndc_offset[1];
  }
  // The clip space above is in guest pixels; the host viewport is the whole (scaled) target.
  v.viewport = nvrhi::Viewport(0.0f, float(std::max(ScaleCoord(int32_t(width), scale_x), 1)), 0.0f,
                               float(std::max(ScaleCoord(int32_t(height), scale_y), 1)), 0.0f, 1.0f);
  if (clip & (1u << 16)) {  // clip_disable: depth from the shader as is
    v.ndc_scale[2] = sz;
    v.ndc_offset[2] = oz;
  } else {
    float z0 = std::clamp(oz, 0.0f, 1.0f), z1 = std::clamp(oz + sz, 0.0f, 1.0f);
    if (z0 > z1) {
      std::swap(z0, z1);
      v.ndc_scale[2] = -1.0f;
      v.ndc_offset[2] = 1.0f;
    }
    v.viewport.minZ = z0;
    v.viewport.maxZ = z1;
  }
  // The window scissor (the library sets it to the viewport rectangle).
  const uint32_t tl = d.u32(dev::kPaScWindowScissorTl), br = d.u32(dev::kPaScWindowScissorBr);
  int32_t x0 = int32_t(tl & 0x7FFF), y0 = int32_t((tl >> 16) & 0x7FFF);
  int32_t x1 = int32_t(br & 0x7FFF), y1 = int32_t((br >> 16) & 0x7FFF);
  if (!(tl & 0x80000000u)) {  // window_offset_disable clear: the offset applies
    x0 += wx;
    x1 += wx;
    y0 += wy;
    y1 += wy;
  }
  x0 = std::clamp(x0, 0, int32_t(width));
  x1 = std::clamp(x1, 0, int32_t(width));
  y0 = std::clamp(y0, 0, int32_t(height));
  y1 = std::clamp(y1, 0, int32_t(height));
  v.scissor = ScaleRect(nvrhi::Rect(x0, std::max(x0, x1), y0, std::max(y0, y1)), scale_x, scale_y);
  return v;
}

// ---------------------------------------------------------------- draw

// The pipeline description for a record (pipeline_cache.h): the same in a draw and in the prewarm.
void GameRenderer::BuildPipelineDesc(const PipelineRecord& r, nvrhi::IShader* vs, nvrhi::IShader* ps,
                                     nvrhi::GraphicsPipelineDesc& pd, nvrhi::FramebufferInfo& fb) const {
  pd.primType = nvrhi::PrimitiveType(r.topology);
  pd.VS = vs;
  pd.PS = ps;
  if (r.flags & kRecordTessellation) {
    const auto stages=shaders_->AnnTessellationStages(r.vs_hash,r.vs_input,(r.flags&kRecordMotion)!=0);
    pd.VS=stages.vs; pd.HS=stages.hs; pd.DS=stages.ds;
    pd.primType=nvrhi::PrimitiveType::PatchList; pd.patchControlPoints=3;
  }
  for (const nvrhi::BindingLayoutHandle& l : pipeline_layouts_) pd.bindingLayouts.push_back(l);
  for (uint32_t i = 0; i < r.color_count && i < 4; ++i) {
    pd.renderState.blendState.targets[i] = BlendTargetFrom(r.color_control[i], r.color_mask[i]);
    fb.colorFormats.push_back(nvrhi::Format(r.color_format[i]));
  }
  const uint32_t depth_control = r.depth_control, ref_mask = r.ref_mask, mode = r.mode_control;
  nvrhi::DepthStencilState& ds = pd.renderState.depthStencilState;
  const bool depth = r.depth_format != 0;
  if (depth) {
    fb.depthFormat = nvrhi::Format(r.depth_format);
    ds.depthTestEnable = (depth_control & 2) != 0;
    ds.depthWriteEnable = ds.depthTestEnable && (depth_control & 4) != 0;
    ds.depthFunc = CompareFrom(depth_control >> 4);
    ds.stencilEnable = (depth_control & 1) != 0;
    ds.stencilReadMask = uint8_t(ref_mask >> 8);
    ds.stencilWriteMask = uint8_t(ref_mask >> 16);
    ds.dynamicStencilRef = true;
    ds.frontFaceStencil.stencilFunc = CompareFrom(depth_control >> 8);
    ds.frontFaceStencil.failOp = StencilOpFrom(depth_control >> 11);
    ds.frontFaceStencil.passOp = StencilOpFrom(depth_control >> 14);
    ds.frontFaceStencil.depthFailOp = StencilOpFrom(depth_control >> 17);
    if (depth_control & 0x80) {
      ds.backFaceStencil.stencilFunc = CompareFrom(depth_control >> 20);
      ds.backFaceStencil.failOp = StencilOpFrom(depth_control >> 23);
      ds.backFaceStencil.passOp = StencilOpFrom(depth_control >> 26);
      ds.backFaceStencil.depthFailOp = StencilOpFrom(depth_control >> 29);
    } else {
      ds.backFaceStencil = ds.frontFaceStencil;
    }
  } else {
    ds.depthTestEnable = false;
    ds.depthWriteEnable = false;
    ds.stencilEnable = false;
  }
  nvrhi::RasterState& rs = pd.renderState.rasterState;
  const bool cull_front = mode & 1, cull_back = mode & 2;
  rs.cullMode = cull_front ? nvrhi::RasterCullMode::Front
                           : (cull_back ? nvrhi::RasterCullMode::Back : nvrhi::RasterCullMode::None);
  // face = 0: counter-clockwise is the front face.
  rs.frontCounterClockwise = ((mode >> 2) & 1) == 0;
  if (r.flags & kRecordFlipFrontFace) rs.frontCounterClockwise = !rs.frontCounterClockwise;
  const uint32_t poly_mode = (mode >> 3) & 3;
  rs.fillMode = (poly_mode != 0 && ((mode >> 5) & 7) == 1) ? nvrhi::RasterFillMode::Wireframe
                                                            : nvrhi::RasterFillMode::Solid;
  rs.scissorEnable = true;
  rs.depthClipEnable = (r.clip_control & (1u << 16)) == 0;
  if (depth && (mode & (1u << 11))) {  // poly_offset_front_enable
    float scale, offset;
    std::memcpy(&scale, &r.poly_scale, 4);
    std::memcpy(&offset, &r.poly_offset, 4);
    rs.slopeScaledDepthBias = scale / 16.0f;
    rs.depthBias = int(offset * 16777216.0f);
  }
  fb.sampleCount = 1;
}

void GameRenderer::Draw(nvrhi::ICommandList* cl, const DrawCall& call) {
  UpdateEffects();
  if (options_.debug & 512) return;  // debugging: no draws
  if (options_.debug & 8192) {       // debugging: no draws, but 10 us of the game thread's time each
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(10);
    while (std::chrono::steady_clock::now() < until) {
    }
    return;
  }
  const uint32_t stop_at = (options_.debug >> 16) & 7;  // debugging: end the draw at a stage
  const bool dump = Dump(call.frame);
  dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
  if (!d.valid()) {
    ++stats_.skipped_device;
    return;
  }
  ProcessInvalidations();
  EnsureFallbackResources(cl);

  std::shared_ptr<const GameShader> vs = shaders_->Get(kkshaders::ShaderKind::Vertex, call.vertex_shader);
  std::shared_ptr<const GameShader> ps;
  if (call.pixel_shader) ps = shaders_->Get(kkshaders::ShaderKind::Pixel, call.pixel_shader);
  if (scene_lighting_) scene_lighting_->BeforeDraw(cl, call.frame, LightFamilyOf(ps ? ps->ucode_hash : 0));
  if (!vs || (call.pixel_shader && !ps)) {
    ++stats_.skipped_shader;
    if (dump) {
      Logf(LogLevel::kInfo, "rexgpu-native: draw %u skipped: shader vs %08X (%s) ps %08X (%s)",
           call.index_in_frame, call.vertex_shader, vs ? "ok" : "missing", call.pixel_shader,
           call.pixel_shader ? (ps ? "ok" : "missing") : "none");
    }
    return;
  }

  if (stop_at == 1) return;
  // The game's fog pass: its two AfterEffects fog pixel shaders (microcode
  // hashes as the Xenos plugin's patch 0007 matches them). Their constants c0
  // (colour), c1 (g_vFogParams) and c2 (g_vFogNormalizing) tell the ambient
  // occlusion how much fog covers each pixel.
  if (ao_ && ps && (ps->ucode_hash == kAoFogShaderA || ps->ucode_hash == kAoFogShaderB)) {
    float fog[16];
    for (uint32_t i = 0; i < 16; ++i) fog[i] = LoadBEFloat(d.at(dev::kPsConstants + i * 4));
    ao_->NoteFog(fog, call.frame);
  }

  // Primitive.
  enum class Expand { kNone, kQuads, kFan } expand = Expand::kNone;
  nvrhi::PrimitiveType topology;
  switch (call.primitive) {
    case GuestPrimitive::kTriangleList: topology = nvrhi::PrimitiveType::TriangleList; break;
    case GuestPrimitive::kTriangleStrip: topology = nvrhi::PrimitiveType::TriangleStrip; break;
    case GuestPrimitive::kTriangleFan:
      topology = nvrhi::PrimitiveType::TriangleList;
      expand = Expand::kFan;
      break;
    case GuestPrimitive::kQuadList:
      topology = nvrhi::PrimitiveType::TriangleList;
      expand = Expand::kQuads;
      break;
    case GuestPrimitive::kPointList: topology = nvrhi::PrimitiveType::PointList; break;
    case GuestPrimitive::kLineList: topology = nvrhi::PrimitiveType::LineList; break;
    case GuestPrimitive::kLineStrip: topology = nvrhi::PrimitiveType::LineStrip; break;
    default:
      ++stats_.skipped_primitive;
      return;
  }
  if (call.count == 0) return;

  // Targets: the surfaces bound, placed by the register images.
  uint32_t pitch = d.u32(dev::kRbSurfaceInfo) & 0x3FFF;
  uint32_t height = 0;
  struct Want {
    bool used = false;
    uint32_t base = 0, format = 0;
  };
  std::array<Want, 4> want_color{};
  Want want_depth;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!call.render_targets[i]) continue;
    dev::SurfaceInfo s;
    if (!dev::DecodeSurface(memory_.Virtual(call.render_targets[i]), false, s)) continue;
    const uint32_t info = d.u32(i == 0 ? dev::kRbColorInfo : dev::kRbColor1Info + 4 * (i - 1));
    want_color[i] = {true, info & 0xFFF, (info >> 16) & 0xF};
    height = std::max(height, s.height);
    if (!pitch) pitch = s.pitch ? s.pitch : s.width;
  }
  if (call.depth_stencil) {
    dev::SurfaceInfo s;
    if (dev::DecodeSurface(memory_.Virtual(call.depth_stencil), true, s)) {
      const uint32_t info = d.u32(dev::kRbDepthInfo);
      want_depth = {true, info & 0xFFF, (info >> 16) & 1};
      height = std::max(height, s.height);
      if (!pitch) pitch = s.pitch ? s.pitch : s.width;
    }
  }
  if (!pitch || !height) {
    ++stats_.skipped_target;
    return;
  }
  // Lighting hook (game_lighting.cpp): the soft shadows draw the screen-space shadow mask finer.
  if (ps && options_.lighting.shadows_on()) LightingNoteShadowMask(*ps, pitch);
  // All attachments the same size: grow to the largest existing one.
  for (uint32_t i = 0; i < 4; ++i) {
    if (!want_color[i].used) continue;
    if (HostTarget* t = FindTarget(false, want_color[i].base, pitch, want_color[i].format))
      height = std::max(height, t->height);
  }
  if (want_depth.used) {
    if (HostTarget* t = FindTarget(true, want_depth.base, pitch, want_depth.format))
      height = std::max(height, t->height);
  }
  current_cl_ = cl;
  // Upscalers (game_upscale.cpp): the upscale point may come before this draw (it changes which
  // targets the draw gets), and the draw may be jittered and replayed by the motion pass.
  UpscaleDraw up;
  if (upscale_) {
    bool writes_color = false;
    const uint32_t mask_all = d.u32(dev::kRbColorMask);
    for (uint32_t i = 0; i < 4; ++i) {
      if (want_color[i].used && ((mask_all >> (4 * i)) & 0xF) && ps && (ps->bindings.pixelOutputs & (1u << i)))
        writes_color = true;
    }
    const uint64_t color_key = want_color[0].used ? TargetKeyOf(false, want_color[0].base, pitch, want_color[0].format) : 0;
    const uint64_t depth_key = want_depth.used ? TargetKeyOf(true, want_depth.base, pitch, want_depth.format) : 0;
    up = UpscaleBeginDraw(cl, call, d, pitch, height, color_key, depth_key, writes_color, *vs, ps.get());
  }
  std::array<HostTarget*, 4> colors{};
  HostTarget* depth = nullptr;
  for (uint32_t i = 0; i < 4; ++i) {
    if (want_color[i].used) colors[i] = GetTarget(false, want_color[i].base, pitch, want_color[i].format, height);
  }
  if (want_depth.used) depth = GetTarget(true, want_depth.base, pitch, want_depth.format, height);
  bool any = depth != nullptr;
  for (HostTarget* c : colors) any = any || c;
  if (!any) {
    ++stats_.skipped_target;
    return;
  }
  nvrhi::IFramebuffer* framebuffer = GetFramebuffer(colors, depth);
  if (!framebuffer) {
    ++stats_.skipped_target;
    return;
  }

  if (stop_at == 6) return;
  // Pipeline state from the register images: a record of the inputs (pipeline_cache.h), which
  // builds the description here and in the prewarm.
  const uint32_t color_mask = d.u32(dev::kRbColorMask);
  const uint32_t depth_control = d.u32(dev::kRbDepthControl);
  const uint32_t mode = d.u32(dev::kPaSuScModeCntl);
  const uint32_t clip_cntl = d.u32(dev::kPaClClipCntl);
  const uint32_t ref_mask = d.u32(dev::kRbStencilRefMask);
  if ((mode & 1) && (mode & 2)) return;  // everything culled
  PipelineRecord record;
  record.vs_hash = vs->ucode_hash;
  record.ps_hash = ps ? ps->ucode_hash : 0;
  record.vs_input = vs->input_hash;
  record.ps_input = ps ? ps->input_hash : 0;
  record.topology = uint32_t(topology);
  for (uint32_t i = 0; i < 4; ++i) {
    if (!colors[i]) continue;
    uint32_t mask = (color_mask >> (4 * i)) & 0xF;
    if (!ps || !(ps->bindings.pixelOutputs & (1u << i))) mask = 0;
    const uint32_t n = record.color_count++;
    record.color_format[n] = uint32_t(colors[i]->format);
    record.color_control[n] = d.u32(i == 0 ? dev::kRbBlendControl0 : dev::kRbBlendControl1 + 4 * (i - 1));
    record.color_mask[n] = mask;
    record.color_index[n] = i;
  }
  if (depth) {
    record.depth_format = uint32_t(depth->format);
    record.depth_control = depth_control;
    record.ref_mask = ref_mask;
  }
  record.mode_control = mode;
  record.clip_control = clip_cntl;
  if (depth && (mode & (1u << 11))) {  // poly_offset_front_enable
    const float scale = d.f32(dev::kPaSuPolyOffset + 0);
    const float offset = d.f32(dev::kPaSuPolyOffset + 4);
    std::memcpy(&record.poly_scale, &scale, 4);
    std::memcpy(&record.poly_offset, &offset, 4);
  }
  record.flags = RecordFlags();
  const uint64_t key = PipelineKey(record);
  NoteRecord(record, key);
  // AO hook (game_ao.cpp): classify the draw; at the end of the opaque pass the occlusion goes into
  // the scene before this draw.
  if (scene_ao_ || ao_constants_) AoDrawHook(cl, call, d, colors, depth, record, *vs, ps.get());
  // GI hook (game_gi.cpp): at the end of the opaque pass the bounce light goes in (after the AO's occlusion).
  if (scene_gi_) GiDrawHook(cl, call, d, colors, depth, record, *vs, ps.get());
  // Lighting hook (game_lighting.cpp): the lighting options' moments and the census.
  // With live settings b5 is always bound (the layout cannot change): written every draw list.
  if (options_.lighting.active() || options_.effect_settings) LightingDrawHook(cl, call, d, colors, depth, record, *vs, ps.get());
  if (up.snapshot) UpscaleTakeSnapshot(cl);
  if (frame_log_ && frame_log_->enabled()) LogDraw(call, d, *vs, ps.get());
  if (dump) DumpVertexShaderHashes(call, d, *vs);
  if (stop_at == 7) return;
  // The description only when the pipeline is not made yet (most draws find it).
  nvrhi::IGraphicsPipeline* pipeline = nullptr;
  if (auto known = pipelines_.find(key); known != pipelines_.end()) {
    pipeline = known->second;
  } else {
    nvrhi::GraphicsPipelineDesc pd;
    nvrhi::FramebufferInfo framebuffer_info;
    BuildPipelineDesc(record, vs->handle, ps ? ps->handle.Get() : nullptr, pd, framebuffer_info);
    pipeline = GetPipeline(pd, framebuffer_info, key, true);
  }
  if (!pipeline) {
    const bool pending = IsPending(key);
    if (!pending) ++stats_.skipped_pipeline;
    if (frame_log_ && frame_log_->active()) NoteDrawNotDrawn(pending);
    return;
  }

  // Prepare the variant without waiting. Until its pipeline is ready this real draw uses
  // its original shader and pipeline, so a live change cannot hide surfaces.
  if (ps) {
    if (auto variant = LiveLightingShader(*ps)) {
      auto variant_record = record;
      variant_record.ps_input = variant->input_hash;
      const uint64_t variant_key = PipelineKey(variant_record);
      nvrhi::GraphicsPipelineDesc pd;
      nvrhi::FramebufferInfo info;
      BuildPipelineDesc(variant_record, vs->handle, variant->handle, pd, info);
      const bool water = LightFamilyOf(variant->ucode_hash) != LightFamily::kShadowQuad;
      if (auto* prepared = GetPipeline(pd, info, variant_key, true, true)) {
        pipeline = prepared;
        ps = std::move(variant);
        ++(water ? live_draws_.water : live_draws_.shadows);
      } else {
        ++(water ? live_draws_.water_waiting : live_draws_.shadows_waiting);
      }
    }
  }

  if (stop_at == 2) return;
  // Draw constants: textures, samplers, vertex fetch, clip planes, viewport,
  // alpha test.
  kkshaders::DrawConstants dc = {};
  for (uint32_t i = 0; i < 8; ++i) dc.boolConstants[i] = d.u32(dev::kBoolConstants + 4 * i);
  for (uint32_t i = 0; i < 32; ++i) dc.loopConstants[i] = d.u32(dev::kLoopConstants + 4 * i);
  // A base-map mip filter on any sampler of a slot keeps its view at one level.
  uint32_t base_map_slots = 0;
  auto note_base_map = [&](const kkshaders::SamplerBinding& sb) {
    uint32_t words[6];
    d.fetch_constant(sb.slot & 31, words);
    const uint32_t mip = sb.mipFilter == 3 ? (words[3] >> 23) & 3 : sb.mipFilter;
    if (mip == 2) base_map_slots |= 1u << (sb.slot & 31);
  };
  for (const auto& sb : vs->bindings.samplers) note_base_map(sb);
  if (ps)
    for (const auto& sb : ps->bindings.samplers) note_base_map(sb);
  kkshaders::ScaleConstants sc = {};
  struct PlaceholderUse {
    uint64_t warning_key;
    uint32_t slot, dimension;
    bool vertex;
    kknr::TextureKey key;
    std::string reason;
  };
  constexpr size_t kPlaceholderWarningLimit = 256;
  std::vector<PlaceholderUse> placeholder_uses;
  for (auto& v : sc.texInvScale) v[0] = v[1] = v[2] = v[3] = 1.0f;
  auto bind_textures = [&](const GameShader& shader) {
    for (const kkshaders::TextureUse& t : shader.bindings.textures) {
      uint32_t words[6];
      d.fetch_constant(t.slot & 31, words);
      dc.textureIndex[t.slot & 31] =
          BindTexture(cl, words, t.dimension, (base_map_slots >> (t.slot & 31)) & 1);
      if (!dc.textureIndex[t.slot & 31] && placeholder_warned_.size() <= kPlaceholderWarningLimit) {
        uint64_t warn_key = 1469598103934665603ull;
        auto mix = [&warn_key](uint64_t v) { warn_key = (warn_key ^ v) * 1099511628211ull; };
        mix(vs->ucode_hash);
        mix(ps ? ps->ucode_hash : 0);
        mix((uint64_t(&shader == vs.get()) << 32) | t.slot);
        for (char c : texture_bind_failure_) mix(uint8_t(c));
        // Known warnings need neither a vector allocation nor a reason-string
        // copy on every draw. New warnings are still emitted only after the
        // final binding is known and this real draw is actually going to run.
        if (!placeholder_warned_.contains(warn_key))
          placeholder_uses.push_back({warn_key, t.slot, uint32_t(t.dimension), &shader == vs.get(),
                                     kknr::MakeTextureKey(kknr::TextureFetch::FromWords(words)),
                                     texture_bind_failure_});
      }
      if (scaled()) {
        sc.texInvScale[t.slot & 31][0] = 1.0f / bound_scale_x_;
        sc.texInvScale[t.slot & 31][1] = 1.0f / bound_scale_y_;
      }
      if (dump) {
        Logf(LogLevel::kInfo, "rexgpu-native:   %s texture slot %u dim %u fetch %08X %08X %08X %08X %08X %08X -> %08X",
             &shader == vs.get() ? "vs" : "ps", t.slot, uint32_t(t.dimension), words[0], words[1], words[2],
             words[3], words[4], words[5], dc.textureIndex[t.slot & 31]);
      }
    }
  };
  bind_textures(*vs);
  if (ps) bind_textures(*ps);
  // Upscalers: the scene's textures sharper by the input / output ratio (the SDKs' mip bias).
  const float lod_bias = up.jitter && upscale_ ? upscale_->mip_bias : 0.0f;
  for (size_t i = 0; i < vs->bindings.samplers.size() && i < 32; ++i) {
    uint32_t words[6];
    d.fetch_constant(vs->bindings.samplers[i].slot & 31, words);
    dc.vertexSamplers[i] = BindSampler(words, vs->bindings.samplers[i], lod_bias);
  }
  if (ps) {
    for (size_t i = 0; i < ps->bindings.samplers.size() && i < 32; ++i) {
      uint32_t words[6];
      d.fetch_constant(ps->bindings.samplers[i].slot & 31, words);
      dc.pixelSamplers[i] = BindSampler(words, ps->bindings.samplers[i], lod_bias);
    }
  }

  if (stop_at == 3) return;
  // The vertices this draw reads (for dynamic vertex buffers): the index range, or the vertex range.
  // Only a dynamic buffer uploads just those (the others go up whole), so an indexed draw's indices
  // are scanned at its first dynamic buffer, not at every draw (task 085: the scan of every draw's
  // indices took about a quarter of the render thread in chapter 8 on the ROG Ally).
  uint32_t first_vertex = 0, last_vertex = ~0u;
  bool vertex_range_known = !options_.dynamic_vertex_buffers;
  auto find_vertex_range = [&] {
    if (vertex_range_known) return;
    vertex_range_known = true;
    if (!call.indexed) {
      first_vertex = call.start;
      last_vertex = call.start + call.count;
    } else if (call.index_data) {
      uint32_t lo = ~0u, hi = 0;
      for (uint32_t i = 0; i < call.count; ++i) {
        const uint32_t v = call.index_info.index32 ? LoadBE32(call.index_data + size_t(call.start + i) * 4)
                                                   : LoadBE16(call.index_data + size_t(call.start + i) * 2);
        if (v == (call.index_info.index32 ? 0xFFFFFFFFu : 0xFFFFu)) continue;  // a strip's restart
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      if (lo <= hi) {
        const int64_t base = call.base_vertex;
        first_vertex = uint32_t(std::max<int64_t>(0, int64_t(lo) + base));
        last_vertex = uint32_t(std::max<int64_t>(0, int64_t(hi) + base + 1));
      }
    }
  };
  if (!call.indexed) find_vertex_range();  // no scan: the same for every buffer

  // Vertex fetch: each binding's declaration element, from its stream.
  auto& elements = scratch_declaration_;
  elements.clear();
  struct StreamBind { uint32_t physical = 0, bytes = 0, begin = 0, end = 0; HostBuffer* host = nullptr; };
  std::array<StreamBind, 16> stream_binds = {};
  if (const uint8_t* decl = call.vertex_declaration ? memory_.Virtual(call.vertex_declaration) : nullptr)
    kknr::DecodeVertexDeclaration(decl, elements);
  const bool ann_depth = vs->ucode_hash == 0xD710A475A7840F37ull || vs->ucode_hash == 0xC66476D2889954ECull;
  auto normal_it = ann_depth && (tessellation_ || tessellation_previous_flags_) ? ann_normals_.find(GeometryContentHash(call)) : ann_normals_.end();
  if(normal_it!=ann_normals_.end() && !normal_it->second.learned) normal_it=ann_normals_.end();
  for (size_t b = 0; b < (normal_it != ann_normals_.end() ? 16 : vs->bindings.vertexBindings.size()) && b < 16; ++b) {
    const bool extra_normal = b == 15 && normal_it != ann_normals_.end();
    if (!extra_normal && b >= vs->bindings.vertexBindings.size()) continue;
    const kkshaders::VertexBinding vb = extra_normal ? kkshaders::VertexBinding{false, kkshaders::DeclUsage::Normal} : vs->bindings.vertexBindings[b];
    uint32_t stream = 0;
    const kknr::VertexElement* element = nullptr;
    if (vb.raw) {
      if (vb.fetchConstant < 80) continue;
      stream = 95 - vb.fetchConstant;
    } else {
      if (extra_normal) element = &normal_it->second.element;
      for (const kknr::VertexElement& e : elements) {
        if (!extra_normal && e.usage == uint8_t(vb.usage) && e.usage_index == vb.usageIndex) {
          element = &e;
          break;
        }
      }
      if (!element) continue;  // format 0: reads (0, 0, 0, 1)
      stream = element->stream;
    }
    if (stream >= 16) continue;
    const uint32_t vb_object = d.u32(dev::kStreamBuffer + 8 * stream);
    const uint32_t offset = extra_normal ? normal_it->second.offset : d.u32(dev::kStreamOffset + 8 * stream);
    const uint32_t stride = extra_normal ? normal_it->second.stride : uint32_t(*d.at(12688 + stream)) * 4;
    VertexBufferInfo info;
    if (extra_normal) info = normal_it->second.buffer;
    else if (!vb_object || !DecodeVertexBuffer(memory_.Virtual(vb_object), info)) continue;
    uint32_t used_begin = 0, used_end = ~0u;
    if (!vb.raw && stride && !vertex_range_known && IsDynamicVertexBuffer(info.physical, info.size_bytes))
      find_vertex_range();
    if (!vb.raw && stride && last_vertex != ~0u) {
      used_begin = uint32_t(std::min<uint64_t>(uint64_t(offset) + uint64_t(first_vertex) * stride, info.size_bytes));
      used_end = uint32_t(std::min<uint64_t>(uint64_t(offset) + uint64_t(last_vertex) * stride, info.size_bytes));
    }
    // Fetches of position, normal and UV often share one stream and range.
    // Reuse only inside this draw: dynamic data may change before the next draw.
    StreamBind& previous = stream_binds[stream];
    const bool repeated = previous.host && previous.physical == info.physical &&
        previous.bytes == info.size_bytes && previous.begin == used_begin && previous.end == used_end;
    HostBuffer* host = repeated ? previous.host :
        BindVertexBuffer(cl, info.physical, info.size_bytes, used_begin, used_end);
    if (options_.debug & 32768) {
      ++stats_.vertex_binds;
      if (repeated) ++stats_.vertex_repeated;
    }
    previous = {info.physical, info.size_bytes, used_begin, used_end, host};
    if (!host) continue;
    if (vb.raw) {
      dc.vertexFetch[b][0] = host->slot;
      dc.vertexFetch[b][1] = offset;
      dc.vertexFetch[b][2] = info.size_bytes > offset ? info.size_bytes - offset : 0;
      dc.vertexFetch[b][3] = info.endian;
    } else {
      const kknr::DeclType type = element->type;
      const uint32_t endian = options_.element_endian ? uint32_t(type.EndianMode()) : info.endian;
      uint32_t word = uint32_t(type.Format()) | (type.Signed() ? kkshaders::vertex_format::kSigned : 0) |
                      (type.Integer() ? kkshaders::vertex_format::kInteger : 0) |
                      (endian << kkshaders::vertex_format::kEndianShift) |
                      (uint32_t(type.Swizzle()) << kkshaders::vertex_format::kSwizzleShift);
      dc.vertexFetch[b][0] = host->slot;
      dc.vertexFetch[b][1] = offset + element->offset;
      dc.vertexFetch[b][2] = stride;
      dc.vertexFetch[b][3] = word;
    }
  }

  if (stop_at == 4) return;
  // Clip planes (PA_CL_CLIP_CNTL ucp_ena, unless clipping is off).
  if (!(clip_cntl & (1u << 16))) dc.clipPlaneMask = clip_cntl & 0x3F;
  for (uint32_t p = 0; p < 6; ++p)
    for (uint32_t c = 0; c < 4; ++c) dc.clipPlanes[p][c] = d.f32(dev::kClipPlanes + p * 16 + c * 4);
  const HostTarget* size_from = colors[0] ? colors[0] : (depth ? depth : nullptr);
  for (HostTarget* c : colors)
    if (!size_from && c) size_from = c;
  const ViewportSetup vp = ComputeViewport(d, size_from->width, size_from->height, size_from->scale_x, size_from->scale_y);
  for (uint32_t c = 0; c < 3; ++c) {
    dc.ndcScale[c] = vp.ndc_scale[c];
    dc.ndcOffset[c] = vp.ndc_offset[c];
  }
  if (up.jitter && upscale_) {
    // Upscalers: the picture moved by this frame's jitter (input pixels over the target's host size).
    float jx = 0, jy = 0;
    JitterNdcOffset(upscale_->jitter, uint32_t(vp.viewport.width()), uint32_t(vp.viewport.height()),
                    device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN, jx, jy);
    dc.ndcOffset[0] += jx;
    dc.ndcOffset[1] += jy;
  }
  const uint32_t color_control = d.u32(dev::kRbColorControl);
  if (color_control & 8) {
    dc.alphaFunc = (color_control & 7) + 1;
    dc.alphaRef = d.f32(dev::kRbAlphaRef);
  }

  // Constants: the device's shadows, byte-swapped. Compared as the guest's bytes first, so a
  // draw whose constants did not change converts nothing.
  // Volatile buffers must be written in every command list that binds them
  // (one per frame; the same object is reopened, hence the frame number).
  const bool new_list = !constants_valid_ || constants_list_ != cl || constants_frame_ != frame_;
  // Compared and converted in blocks of 16 constants: a draw usually changes a few of the 256
  // registers (a matrix), and `last` always holds `last_raw` converted (task 085).
  auto upload = [&](uint32_t base, std::array<uint8_t, 4096>& last_raw, std::array<float, 1024>& last,
                    const nvrhi::BufferHandle& buffer) {
    const uint8_t* raw = d.at(base);
    bool changed = false;
    for (uint32_t block = 0; block < 4096; block += 64) {
      if (SameBytes64(raw + block, last_raw.data() + block)) continue;
      changed = true;
      std::memcpy(last_raw.data() + block, raw + block, 64);
      for (uint32_t i = block / 4; i < block / 4 + 16; ++i) last[i] = LoadBEFloat(raw + i * 4);
    }
    if (!changed && !new_list) return;
    cl->writeBuffer(buffer, last.data(), sizeof(last));
    stats_.upload_bytes += sizeof(last);
  };
  upload(dev::kVsConstants, last_vs_raw_, last_vs_, vs_constants_);
  upload(dev::kPsConstants, last_ps_raw_, last_ps_, ps_constants_);
  constants_valid_ = true;
  constants_list_ = cl;
  constants_frame_ = frame_;
  // Only prepared, complete material/pass pairs can use the experimental geometry.
  // Missing pipelines keep the whole pair original; this never queues a compile in play.
  bool ann_material = (vs->ucode_hash==0x5E779DBA373D121Cull || vs->ucode_hash==0x3390D930CD168AC0ull) && (dc.vertexFetch[1][3]&0x3Fu) && (tessellation_ || TessellationBenchRequested()) &&
                            AnnTessellationMaterial(d,*vs,ps.get());
  bool tessellated=false;
  if (ann_material && tessellation_) {
    if (auto* prepared=AnnTessellationPipeline(record)) {
      pipeline=prepared; tessellated=true; dc.reserved=TessellationFlags();
    }
  }
  if (AnnMainTessellation(call,d,*vs,ps.get(),dc,record,pipeline)) {
    ann_material = true;
    tessellated = (record.flags & kRecordTessellation)!=0;
  }
  cl->writeBuffer(draw_constants_, &dc, sizeof(dc));
  stats_.upload_bytes += sizeof(dc);
  if (scaled()) {
    sc.renderScale[0] = size_from->scale_x;
    sc.renderScale[1] = size_from->scale_y;
    sc.renderScale[2] = 1.0f / size_from->scale_x;
    sc.renderScale[3] = 1.0f / size_from->scale_y;
    // Like VS/PS constants, a volatile scale buffer needs one write per list
    // and a new version only when its complete contents change between draws.
    if (new_list || std::memcmp(&sc, &last_scale_, sizeof(sc)) != 0) {
      cl->writeBuffer(scale_constants_, &sc, sizeof(sc));
      stats_.upload_bytes += sizeof(sc);
      ++stats_.scale_uploads;
      last_scale_ = sc;
    } else {
      ++stats_.scale_identical;
    }
  }

  // State and draw.
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = framebuffer;
  gs.viewport.addViewport(vp.viewport);
  gs.viewport.addScissorRect(vp.scissor);
  gs.bindings.push_back(constants_set_);
  for (DescriptorTable& t : tables_) gs.bindings.push_back(t.table);
  gs.blendConstantColor = nvrhi::Color(d.f32(dev::kRbBlendRgba), d.f32(dev::kRbBlendRgba + 4),
                                       d.f32(dev::kRbBlendRgba + 8), d.f32(dev::kRbBlendRgba + 12));
  gs.dynamicStencilRefValue = uint8_t(ref_mask & 0xFF);

  nvrhi::DrawArguments args;
  bool indexed_draw = false;
  if (call.indexed) {
    const IndexBufferInfo& ib = call.index_info;
    if (expand == Expand::kNone) {
      HostBuffer* host = BindIndexBuffer(cl, ib.physical, ib.size_bytes, ib.index32);
      if (!host) return;
      gs.indexBuffer = nvrhi::IndexBufferBinding()
                           .setBuffer(host->buffer)
                           .setFormat(ib.index32 ? nvrhi::Format::R32_UINT : nvrhi::Format::R16_UINT)
                           .setOffset(0);
      args.vertexCount = call.count;
      args.startIndexLocation = call.start;
    } else {
      // Expand the guest's indices (base vertex left to the draw).
      scratch_indices_.clear();
      auto index = [&](uint32_t i) -> uint32_t {
        return ib.index32 ? LoadBE32(call.index_data + size_t(call.start + i) * 4)
                          : LoadBE16(call.index_data + size_t(call.start + i) * 2);
      };
      if (expand == Expand::kQuads) {
        for (uint32_t i = 0; i + 3 < call.count; i += 4) {
          uint32_t q[4] = {index(i), index(i + 1), index(i + 2), index(i + 3)};
          scratch_indices_.insert(scratch_indices_.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
        }
      } else {
        for (uint32_t i = 1; i + 1 < call.count; ++i)
          scratch_indices_.insert(scratch_indices_.end(), {index(0), index(i), index(i + 1)});
      }
      const uint64_t bytes = scratch_indices_.size() * 4;
      if (!bytes || index_ring_used_ + bytes > kIndexRingBytes) return;
      cl->writeBuffer(index_ring_, scratch_indices_.data(), bytes, index_ring_used_);
      stats_.upload_bytes += bytes;
      gs.indexBuffer = nvrhi::IndexBufferBinding()
                           .setBuffer(index_ring_)
                           .setFormat(nvrhi::Format::R32_UINT)
                           .setOffset(uint32_t(index_ring_used_));
      index_ring_used_ += (bytes + 255) & ~uint64_t(255);
      args.vertexCount = uint32_t(scratch_indices_.size());
    }
    args.startVertexLocation = uint32_t(call.base_vertex);
    indexed_draw = true;
  } else if (expand != Expand::kNone) {
    if (!patterns_uploaded_) {
      std::vector<uint32_t> quads, fans;
      quads.reserve(kPatternVertices / 4 * 6);
      for (uint32_t q = 0; q < kPatternVertices / 4; ++q) {
        const uint32_t v = q * 4;
        quads.insert(quads.end(), {v, v + 1, v + 2, v, v + 2, v + 3});
      }
      fans.reserve(size_t(kPatternVertices) * 3);
      for (uint32_t i = 1; i < kPatternVertices; ++i) fans.insert(fans.end(), {0u, i, i + 1});
      cl->writeBuffer(quad_indices_, quads.data(), quads.size() * 4);
      cl->writeBuffer(fan_indices_, fans.data(), std::min(fans.size(), size_t(kPatternVertices) * 3) * 4);
      stats_.upload_bytes += (quads.size() + std::min(fans.size(), size_t(kPatternVertices) * 3)) * 4;
      patterns_uploaded_ = true;
    }
    if (call.count > kPatternVertices) return;
    gs.indexBuffer = nvrhi::IndexBufferBinding()
                         .setBuffer(expand == Expand::kQuads ? quad_indices_ : fan_indices_)
                         .setFormat(nvrhi::Format::R32_UINT)
                         .setOffset(0);
    args.vertexCount = expand == Expand::kQuads ? call.count / 4 * 6 : (call.count >= 3 ? (call.count - 2) * 3 : 0);
    args.startVertexLocation = call.start;
    indexed_draw = true;
  } else {
    args.vertexCount = call.count;
    args.startVertexLocation = call.start;
  }
  if (!args.vertexCount) return;

  if (stop_at == 5) return;
  for (const auto& use : placeholder_uses) {
    // Both shader stages share the slot's final draw constant. A later bind may have succeeded.
    if (dc.textureIndex[use.slot & 31]) continue;
    // Once per distinct (vs, ps, stage and slot, reason): the same draw repeats every frame.
    if (placeholder_warned_.size() > kPlaceholderWarningLimit) continue;
    if (!placeholder_warned_.insert(use.warning_key).second) continue;
    if (placeholder_warned_.size() > kPlaceholderWarningLimit) {
      Logf(LogLevel::kWarning, "rexgpu-native: texture placeholder warnings limit reached; further ones are not logged");
      continue;
    }
    const auto& w = use.key.words;
    Logf(LogLevel::kWarning,
         "rexgpu-native: texture placeholder real draw frame %llu draw %u %s slot %u dim %u "
         "vs %016llX ps %016llX key %08X %08X %08X %08X %08X %08X reason %s",
         static_cast<unsigned long long>(frame_), call.index_in_frame, use.vertex ? "vs" : "ps",
         use.slot, use.dimension, static_cast<unsigned long long>(vs->ucode_hash),
         static_cast<unsigned long long>(ps ? ps->ucode_hash : 0),
         w[0], w[1], w[2], w[3], w[4], w[5], use.reason.c_str());
  }
  // Upscalers: a draw of the scene's depth, replayed by the motion pass at the upscale point.
  if (up.motion) UpscaleRecordMotion(call, d, vs, dc, record, gs, args, indexed_draw);
  if (scene_ao_) AoRecordMotionHook(call, d, *vs);
  cl->setGraphicsState(gs);
  ++stats_.graphics_states;
  if (debug_last_pipeline_ != pipeline) ++stats_.pipeline_changes;
  debug_last_pipeline_ = pipeline;
  if (indexed_draw) {
    if (ann_material && tessellation_timer_) tessellation_timer_->Begin(cl);
    cl->drawIndexed(args);
    if (ann_material && tessellation_timer_) tessellation_timer_->End(cl);
  } else {
    cl->draw(args);
  }
  ++stats_.draws;
  if (tessellated) {
    tessellation_drawn_this_frame_=true;
    ++tessellation_draws_this_frame_;
  }
  if (tessellated && frame_log_ && frame_log_->active())
    Logf(LogLevel::kWarning,"rexgpu-native: ANN TESS frame %llu draw %u detail %u smoothness %.4f",(unsigned long long)call.frame,call.index_in_frame,tessellation_detail_,tessellation_smoothness_);
  if (dump) {
    for (size_t b = 0; b < vs->bindings.vertexBindings.size() && b < 16; ++b) {
      Logf(LogLevel::kInfo, "rexgpu-native:   fetch %zu usage %u/%u -> %u %u %u %08X", b,
           uint32_t(vs->bindings.vertexBindings[b].usage), uint32_t(vs->bindings.vertexBindings[b].usageIndex),
           dc.vertexFetch[b][0], dc.vertexFetch[b][1], dc.vertexFetch[b][2], dc.vertexFetch[b][3]);
    }
    Logf(LogLevel::kInfo, "rexgpu-native:   mode %08X colorctl %08X alpha %u %.3f decl %zu elements, indexed %d base %d start %u",
         mode, color_control, dc.alphaFunc, dc.alphaRef, elements.size(), int(call.indexed), call.base_vertex, call.start);
    Logf(LogLevel::kInfo,
         "rexgpu-native: draw %u vs %016llX ps %016llX prim %u count %u rt %ux%u fmt %u/%u "
         "depthctl %08X blend %08X mask %X vte %08X clip %08X ndc %.4g %.4g %.4g / %.4g %.4g %.4g "
         "scissor %d,%d-%d,%d",
         call.index_in_frame, static_cast<unsigned long long>(vs->ucode_hash),
         static_cast<unsigned long long>(ps ? ps->ucode_hash : 0), uint32_t(call.primitive), call.count,
         size_from->width, size_from->height, colors[0] ? uint32_t(colors[0]->format) : 0,
         depth ? uint32_t(depth->format) : 0, depth_control, d.u32(dev::kRbBlendControl0), color_mask,
         d.u32(dev::kPaClVteCntl), clip_cntl, vp.ndc_scale[0], vp.ndc_scale[1], vp.ndc_scale[2],
         vp.ndc_offset[0], vp.ndc_offset[1], vp.ndc_offset[2], vp.scissor.minX, vp.scissor.minY,
         vp.scissor.maxX, vp.scissor.maxY);
  }
}

}  // namespace nr
