#include "backend/game_renderer.h"
#if NR_HAS_VULKAN
#include <nvrhi/vulkan.h>
#endif
#include "plugin/native_benchmark_stats.h"
#include "backend/gpu_loss.h"
#include "backend/shader_constants.h"
#include "backend/resource_upload_workers.h"
#include "backend/upload_batch.h"
#include "kkshaders/tessellation.h"

#include <algorithm>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>

#include "backend/draw_tags.h"
#include "backend/guest_layout.h"
#include "backend/log.h"
#include "backend/pipeline_exit_gate.h"
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
  // Developer aids (task 177): KK_DEV_SLOW_PRELOAD_SAVE_MS=<ms> delays every learned-list write as a
  // slow disk would; KK_DEV_SYNC_FILE_WRITES=1 writes on the render thread again (the old path).
  if (const char* value = std::getenv("KK_DEV_SLOW_PRELOAD_SAVE_MS"); value && *value) {
    file_writer_.set_delay_ms(uint32_t(std::strtoul(value, nullptr, 10)));
    Logf(LogLevel::kInfo, "rexgpu-native: learned resource list writes delayed by %s ms (developer aid)", value);
  }
  if (const char* value = std::getenv("KK_DEV_SYNC_FILE_WRITES"); value && *value == '1') {
    file_writer_.set_synchronous(true);
    Logf(LogLevel::kInfo, "rexgpu-native: file writes on the render thread (developer A/B, the old path)");
  }
  // Task 179: the runtime hard-exits without ~GameRenderer, so the driver's pipeline cache is saved from
  // the game's exit flush (nr_flush_file_writes, bounded): the next session starts warm.
  file_writer_.set_exit_task([this] {
    if (!driver_cache_) return;
    const uint64_t fresh = driver_cache_new_.load();
    if (fresh == 0) {
      Logf(LogLevel::kInfo, "rexgpu-native: driver pipeline cache: nothing new to save at exit");
      return;
    }
    Logf(LogLevel::kInfo, "rexgpu-native: driver pipeline cache: saving %llu new pipelines before exit",
         static_cast<unsigned long long>(fresh));
    SaveDriverPipelineCache();
  });
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
    // Task 309: a pooled range goes back to its pool kRetireFrames later (buffer_pool.h).
    if (h && h->pool) h->pool->Retire(h->range, frame_);
    else if (h) RetireResource(h->buffer);
    descriptor_buffers_.erase(h);
    delete h;
    e.host = nullptr;
  };
  vertex_buffers_.release_host = release_buffer;
  index_buffers_.release_host = release_buffer;
}

GameRenderer::~GameRenderer() {
  if (pipeline_library_ && shaders_) shaders_->SetBuiltCallback({});
  file_writer_.set_exit_task({});  // this renderer is going: FlushAll must not reach it
  SaveResourcePreload();
  LogPipelineMisses(true);
  LogPipelineLibrary(true);
  if (resource_profile_file_) std::fclose(resource_profile_file_);
  if (gi_build_.valid()) gi_build_.wait();
  if (cel_build_.valid()) cel_build_.wait();
  if (lighting_build_.valid()) lighting_build_.wait();
  if (upscale_build_.valid()) upscale_build_.wait();
  if (dlss_warm_.valid()) dlss_warm_.wait();
  if (frame_trace_) std::fclose(frame_trace_);
  if (pipeline_log_) std::fclose(pipeline_log_);
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
  // Every queued file write is finished before the renderer (and its cache folder's owner) goes;
  // cache_file_'s own Close then writes records still waiting.
  file_writer_.Stop();
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
  pipeline_library_ = device_->queryFeatureSupport(nvrhi::Feature::GraphicsPipelineLibrary);
  // Dev (task 183, the linked pipelines' GPU cost): every game pipeline stays linked, none is compiled whole.
  if (const char* v = std::getenv("KK_DEV_GPL_LINK_ONLY"); pipeline_library_ && v && *v && *v != '0') {
    pipeline_library_link_only_ = true;
    Logf(LogLevel::kInfo, "rexgpu-native: KK_DEV_GPL_LINK_ONLY: game pipelines stay linked (never compiled whole)");
  }
  if (const char* v = std::getenv("KK_DEV_GPL_NO_SHADER_PARTS"); pipeline_library_ && shaders_ && !(v && *v && *v != '0')) {
    shaders_->SetBuiltCallback([this](kkshaders::ShaderKind kind, const std::shared_ptr<const GameShader>& shader) {
      if (shader) QueueShaderParts(kind == kkshaders::ShaderKind::Vertex, shader->handle);
    });
  }
  // Dev (task 267): a slow PC's cold compiles on a fast one (A/B runs; the cap: --native_pipeline_must_wait_ms).
  if (const char* v = std::getenv("KK_DEV_PIPELINE_DELAY_MS"); v && *v) {
    dev_compile_delay_ms_ = std::clamp(std::atoi(v), 0, 2000);
    Logf(LogLevel::kInfo, "rexgpu-native: KK_DEV_PIPELINE_DELAY_MS: each worker compile takes %d ms longer (dev)",
         dev_compile_delay_ms_);
  }
  // Keep the scale-aware shader ABI at the native-resolution floor too. It
  // was selected before shader creation for these initially scaled runs.
  memory_floor_x_ = std::min(options_.render_scale_x, 1.0f);
  memory_floor_y_ = std::min(options_.render_scale_y, 1.0f);
  memory_scale_aware_ = !(options_.debug & 16) &&
                       std::max({options_.render_scale_x, options_.render_scale_y, options_.shadow_scale}) > 1.0f;
  // Developer test (task 154): fewer volatile versions, so an ordinary frame shows the completion
  // path (KK_DEV_VOLATILE_VERSIONS=1024 completes every list of a ~1,500-draw scene part-way).
  if (const char* value = std::getenv("KK_DEV_VOLATILE_VERSIONS"); value && *value) {
    options_.volatile_versions = uint32_t(std::clamp(std::atoi(value), 64, 65536));
    Logf(LogLevel::kWarning, "rexgpu-native: KK_DEV_VOLATILE_VERSIONS: %u volatile versions", options_.volatile_versions);
  }
  if (const char* path = std::getenv("NR_RESOURCE_PROFILE"); path && *path) {
    resource_profile_file_ = std::fopen(path, "w");
    if (resource_profile_file_) {
      std::fprintf(resource_profile_file_, "frame,wall_s,texture_convert_ms,texture_create_ms,texture_stage_ms,buffer_create_ms,buffer_convert_ms,buffer_stage_ms,hash_ms,watch_ms,cache_bind_ms,invalidate_ms,descriptor_ms,submit_ms,snapshot_ms,prepare_wall_ms,prepared_resources,prepared_discarded,prepare_total_ms,preloaded_resources,preload_hits,preload_hash_misses,preload_bytes\n");
      resource_profile_ = &resource_work_;
      textures_.profile = vertex_buffers_.profile = index_buffers_.profile = resource_profile_;
    }
  }
  const nvrhi::GraphicsAPI api = device_->getGraphicsAPI();
  if (g_buffer_pool_enabled.load(std::memory_order_relaxed)) {
    // Task 309 (buffer_pool.h): guest vertex and index buffers from shared blocks (D3D12 and, task 313, Vulkan:
    // set by the host from --native_d3d12_buffer_pool / --native_vk_buffer_pool).
    auto block = [this](bool index) {
      return [this, index](uint64_t bytes) {
        nvrhi::BufferDesc d;
        d.byteSize = bytes;
        d.canHaveRawViews = !index;
        d.isIndexBuffer = index;
        d.initialState = index ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::ShaderResource;
        d.keepInitialState = true;
        d.debugName = index ? "Guest indices (pool)" : "Guest vertex data (pool)";
        nvrhi::BufferHandle b = device_->createBuffer(d);
        Logf(b ? LogLevel::kInfo : LogLevel::kWarning, "rexgpu-native: buffer pool: %s block of %llu MiB %s",
             index ? "index" : "vertex", static_cast<unsigned long long>(bytes >> 20), b ? "made" : "NOT made");
        return b;
      };
    };
    vertex_pool_ = std::make_unique<BufferPool>(block(false));
    index_pool_ = std::make_unique<BufferPool>(block(true));
  }
  if (options_.table_capacities != kNativeTableCapacities && options_.upscale.enabled()) {
    // Motion replay retains indices until the upscale point, beyond a possible
    // descriptor drain. Keep them disabled for prewarm and standalone users too.
    options_.upscale.kind = UpscalerKind::kOff;
    Logf(LogLevel::kWarning, "rexgpu-native: reduced Vulkan descriptor tables: temporal upscaling unavailable; using native render size");
  }
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
      std::string why, error;
      return BuildOrEmpty([&] { return CreateDlssUpscaler(device, probe, &why); }, &error);
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
  // What every game pipeline's layout is made of (set 0's constant buffers and the tables' capacities):
  // a pipeline prepared before play is the game's only when this matches (pipeline_prewarm.h).
  layout_signature_ = 1469598103934665603ull;
  auto mix = [this](uint64_t v) { layout_signature_ = (layout_signature_ ^ v) * 1099511628211ull; };
  for (const nvrhi::BindingLayoutItem& item : constants.bindings) mix((uint64_t(item.type) << 32) | item.slot);
  for (uint32_t capacity : options_.table_capacities) mix(capacity);
  mix(scaled() ? 1 : 0);
  if (!constants_layout_) return false;

  auto make_cb = [&](uint32_t size, const char* name) {
    nvrhi::BufferDesc d;
    d.byteSize = size;
    d.isConstantBuffer = true;
    d.isVolatile = true;
    d.maxVersions = options_.volatile_versions;
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
  };
  const Spec specs[] = {
      {kTex2D, nvrhi::BindingLayoutItem::Texture_SRV(1)},
      {kTex3D, nvrhi::BindingLayoutItem::Texture_SRV(2)},
      {kTexCube, nvrhi::BindingLayoutItem::Texture_SRV(3)},
      {kTex2DArray, nvrhi::BindingLayoutItem::Texture_SRV(4)},
      {kSamplers, nvrhi::BindingLayoutItem::Sampler(5)},
      {kBuffers, nvrhi::BindingLayoutItem::RawBuffer_SRV(6)},
  };
  pipeline_layouts_ = {constants_layout_};
  for (const Spec& s : specs) {
    nvrhi::BindlessLayoutDesc d;
    d.visibility = nvrhi::ShaderType::All;
    d.firstSlot = 0;
    const uint32_t capacity = options_.table_capacities[s.table];
    d.maxCapacity = capacity;
    d.registerSpaces.push_back(s.item);
    DescriptorTable& t = tables_[s.table];
    t.layout = device_->createBindlessLayout(d);
    if (!t.layout) return false;
    t.table = device_->createDescriptorTable(t.layout);
    if (!t.table) return false;
    device_->resizeDescriptorTable(t.table, capacity, false);
    t.capacity = capacity;
    t.last_used.resize(capacity);
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
  smaa_ = std::make_unique<Smaa>(device_);
  if (!smaa_->Initialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: SMAA initialization failed; spatial AA falls back to FXAA");
    smaa_.reset();
  }
  // Ambient occlusion: optional, never a reason to refuse to start.
  ao_ = std::make_unique<AmbientOcclusion>(device_);
  if (!ao_->Initialize()) ao_.reset();
  AoInitialize();  // AO hook (game_ao.cpp): the GTAO method
  GiInitialize();  // GI hook (game_gi.cpp): the screen-space GI
  CelInitialize();  // Cel hook (game_cel.cpp): the cel shading
  // Upscalers: their passes. Without them the renderer runs as without an upscaler (logged).
  if (upscale_ && !UpscaleInitialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: upscaler: the passes could not be made; no upscaler");
    upscale_.reset();
    shaders_->SetMotionPrewarm(false);
  }
  LightingInitialize();  // Lighting hook (game_lighting.cpp)
  if (memory_scale_aware_ && !upscale_) memory_migration_ready_ = InitializeMemoryMigration();
  return true;
}

void GameRenderer::UploadDummies(nvrhi::ICommandList* cl) {
  // Frame start and first-use paths share one initialization flag and must upload
  // the same contents, including the identity gamma ramp and committed barriers.
  EnsureFallbackResources(cl);
  if (pending_memory_scale_ > 0.0f || !memory_staged_.empty()) ApplyMemoryScale(cl);
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
  if (t.free.empty() && t.next >= t.capacity) RecycleDescriptors(table);
  if (!t.free.empty()) {
    uint32_t s = t.free.back();
    t.free.pop_back();
    TouchSlot(table, s);
    return s;
  }
  if (t.next < t.capacity) {
    const uint32_t s = t.next++;
    TouchSlot(table, s);
    return s;
  }
  Logf(LogLevel::kError, "rexgpu-native: descriptor table %u exhausted (%u slots still live); draw binding failed", unsigned(table), t.capacity);
  return 0;  // full: the dummy
}

void GameRenderer::TouchSlot(Table table, uint32_t slot) {
  if (slot) tables_[table].last_used[slot] = frame_;
}

void GameRenderer::PrepareDescriptorDraw(nvrhi::ICommandList* cl, const GameShader& vs, const GameShader* ps) {
  if (options_.table_capacities == kNativeTableCapacities) return;
  // Reserve before generating any draw indices. Count this draw's shader
  // uses rather than draining small tables for a hypothetical maximum draw.
  // Both stages share fetch constants and the base-map choice. Count unique
  // (fetch slot, dimension) views. 1D/2D/3D fetches may select an array.
  unsigned image_reserve[4] = {3, 0, 0, 0};  // AO and the two lighting views
  std::array<unsigned, 32> seen{};
  auto count_images = [&](const GameShader& shader) {
    for (const auto& use : shader.bindings.textures) {
      const unsigned dim = unsigned(use.dimension) & 3;
      const unsigned slot = use.slot & 31;
      if (seen[slot] & (1u << dim)) continue;
      seen[slot] |= 1u << dim;
      if (use.dimension == kkshaders::TextureDimension::Cube) ++image_reserve[kTexCube];
      else {
        ++image_reserve[kTex2DArray];
        ++image_reserve[use.dimension == kkshaders::TextureDimension::Tex3D ? kTex3D : kTex2D];
      }
    }
  };
  count_images(vs);
  if (ps) count_images(*ps);
  const unsigned samplers = 2 + unsigned(std::min(vs.bindings.samplers.size(), size_t(32)) +
      (ps ? std::min(ps->bindings.samplers.size(), size_t(32)) : 0));
  const unsigned buffers = unsigned(std::min(vs.bindings.vertexBindings.size(), size_t(16)));
  const unsigned reserve[kTableCount] = {image_reserve[0], image_reserve[1], image_reserve[2], image_reserve[3], samplers, buffers};
  bool pressure = false;
  for (unsigned i = 0; i < kTableCount; ++i) {
    auto& t = tables_[i];
    if (t.capacity - t.next + t.free.size() < reserve[i]) RecycleDescriptors(Table(i));
    pressure |= t.capacity - t.next + t.free.size() < reserve[i];
  }
  // Full-capacity mode can still have pending motion replay. Its saved draw
  // constants contain indices that must survive until that replay completes.
  if (!pressure || !options_.drain_descriptors || upscale_) return;
  const auto start = std::chrono::steady_clock::now();
  options_.drain_descriptors(cl);
  NoteFrameWait(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  // GPU completion, rather than an age heuristic, permits immediate reuse
  // of every non-dummy index while preserving all guest resource data.
  for (HostTexture* h : descriptor_textures_) h->views.clear();
  for (HostBuffer* h : descriptor_buffers_) h->slot = 0;
  for (auto& [key, sampler] : samplers_) RetireResource(sampler.first);
  samplers_.clear();
  for (auto& t : tables_) {
    t.next = 1;
    t.free.clear();
    t.retired.clear();
    std::fill(t.last_used.begin(), t.last_used.end(), frame_);
  }
  ao_slot_ = 0;
  ao_slot_texture_ = nullptr;
  constants_valid_ = ao_constants_valid_ = false;
  lighting_.sampler_slot = lighting_.wrap_sampler_slot = 0;
  lighting_.color_slot = lighting_.depth_slot = 0;
  lighting_.color_slot_texture = lighting_.depth_slot_texture = nullptr;
  lighting_.constants_valid = false;
  volatile_draws_ = volatile_motion_ = 0;  // every volatile version is free again too
  Logf(LogLevel::kInfo, "rexgpu-native: descriptor pressure: completed GPU work and recycled tables at frame %llu", static_cast<unsigned long long>(frame_));
}

bool GameRenderer::NoteVolatileDraw(nvrhi::ICommandList* cl, bool motion) {
  // Each draw writes at most one version of each per-draw volatile buffer (VS/PS/draw/scale, AO and
  // lighting; motion draws their own three), so the draws a list took bound the versions it holds.
  if (volatile_list_ != cl || volatile_frame_ != frame_) {
    volatile_list_ = cl;
    volatile_frame_ = frame_;
    volatile_draws_ = volatile_motion_ = 0;
  }
  uint32_t& count = motion ? volatile_motion_ : volatile_draws_;
  if (count >= volatile_budget()) {
    if (!options_.drain_descriptors) {
      ++stats_.volatile_skipped;
      if (!volatile_warned_) {
        volatile_warned_ = true;
        Logf(LogLevel::kWarning, "rexgpu-native: more than %u draws in one frame; extra draws are skipped",
             volatile_budget());
      }
      return false;
    }
    // Complete the GPU work so far (the versions of this list and the frames still on the GPU) and
    // continue in a fresh list. Only for a frame far heavier than the game's own.
    const auto start = std::chrono::steady_clock::now();
    options_.drain_descriptors(cl);
    NoteFrameWait(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    ++stats_.volatile_completions;
    // The reopened list must write every volatile buffer again before a draw binds it.
    constants_valid_ = ao_constants_valid_ = false;
    lighting_.constants_valid = false;
    volatile_draws_ = volatile_motion_ = 0;
    if (!volatile_warned_) {
      volatile_warned_ = true;
      Logf(LogLevel::kWarning, "rexgpu-native: more than %u draws in one frame (frame %llu); the frame's GPU work is "
           "completed in parts (shown once)", volatile_budget(), static_cast<unsigned long long>(frame_));
    }
  }
  const uint32_t n = ++count;
  uint32_t& peak = motion ? stats_.volatile_peak_motion : stats_.volatile_peak_draws;
  if (n > peak) {
    peak = n;
    // A new record above 2,048 (the game's own frames stay under about 1,600): noted every 512.
    if (n >= 2048 && n % 512 == 0)
      Logf(LogLevel::kInfo, "rexgpu-native: %u %sdraws in one list (frame %llu, budget %u)", n, motion ? "motion " : "",
           static_cast<unsigned long long>(frame_), volatile_budget());
  }
  std::atomic<uint32_t>& window = motion ? volatile_window_motion_ : volatile_window_draws_;
  if (n > window.load(std::memory_order_relaxed)) window.store(n, std::memory_order_relaxed);
  return true;
}

GameRenderer::VolatileWindow GameRenderer::TakeVolatileWindow() {
  VolatileWindow w;
  w.draws = volatile_window_draws_.exchange(0, std::memory_order_relaxed);
  w.motion = volatile_window_motion_.exchange(0, std::memory_order_relaxed);
  return w;
}

void GameRenderer::RecycleDescriptors(Table table) {
  // Only remove descriptor mappings unused for more than eight frames. Two GPU frames are
  // bounded by Renderer::FrameCommandList's completion wait. Current-draw
  // mappings have already been touched and cannot be evicted here. Texture
  // and buffer cache owners stay intact, including GPU-written resolve data.
  if (table <= kTex2DArray) {
    for (HostTexture* host : descriptor_textures_) {
      for (auto it = host->views.begin(); it != host->views.end();) {
        const uint32_t index = it->second;
        const uint32_t dim = (it->first >> 12) & 3;
        const Table owner = (index & 0x80000000u) ? kTex2DArray :
            dim == uint32_t(kkshaders::TextureDimension::Cube) ? kTexCube :
            dim == uint32_t(kkshaders::TextureDimension::Tex3D) ? kTex3D : kTex2D;
        const uint32_t slot = index & 0x7FFFFFFFu;
        if (owner == table && tables_[table].last_used[slot] + 8 < frame_) {
          RetireSlot(table, slot);
          it = host->views.erase(it);
        } else ++it;
      }
    }
  } else if (table == kBuffers) {
    for (HostBuffer* host : descriptor_buffers_) {
      if (host->slot && tables_[table].last_used[host->slot] + 8 < frame_) {
        RetireSlot(table, host->slot);
        host->slot = 0;
      }
    }
  }
  else {
    for (auto it = samplers_.begin(); it != samplers_.end();) {
      if (it->second.last_used + 8 < frame_) {
        RetireSlot(kSamplers, it->second.second);
        RetireResource(it->second.first);
        it = samplers_.erase(it);
      } else ++it;
    }
  }
}

void GameRenderer::RetireResource(nvrhi::IResource* resource) {
  if (resource) retired_resources_.emplace_back(frame_, resource);
}

void GameRenderer::ReleaseRetiredResources() {
  if (!retire_budget_checked_) {
    retire_budget_checked_ = true;
    // Developer A/B: KK_DEV_RETIRE_BUDGET_MS=<ms per frame>; 0 = every aged resource at once (the old path).
    if (const char* value = std::getenv("KK_DEV_RETIRE_BUDGET_MS"); value && *value) {
      retire_budget_.budget_ms = std::strtod(value, nullptr);
      Logf(LogLevel::kInfo, "rexgpu-native: retired resources released within %.2f ms a frame (0 = all at once)",
           retire_budget_.budget_ms);
    }
  }
  const auto start = std::chrono::steady_clock::now();
  if (vertex_pool_) vertex_pool_->Release(frame_);
  if (index_pool_) index_pool_->Release(frame_);
  // Under memory pressure the memory matters more than the frame: release all of them, as before.
  const bool all = pack_pressure_ || retire_budget_.budget_ms <= 0;
  const RetireRelease r = ReleaseRetired(retired_resources_, frame_, 8, all, retire_budget_);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (r.aged_left || retire_backlog_frames_ || r.released >= 64) {
    ++retire_backlog_frames_;
    retire_backlog_released_ += r.released;
    retire_backlog_ms_ = std::max(retire_backlog_ms_, ms);
    retire_backlog_budget_ms_ = std::max(retire_backlog_budget_ms_, all ? 0.0 : r.budget_ms);
    if (!r.aged_left) {
      // Task 185: the budget grows as the backlog ages (retire_budget.h); "budget up to" shows how far.
      Logf(LogLevel::kInfo,
           "rexgpu-native: retired resources: %llu released over %llu frames, longest %.2f ms, budget up to %.2f ms "
           "(frame %llu)",
           static_cast<unsigned long long>(retire_backlog_released_),
           static_cast<unsigned long long>(retire_backlog_frames_), retire_backlog_ms_, retire_backlog_budget_ms_,
           static_cast<unsigned long long>(frame_));
      retire_backlog_frames_ = retire_backlog_released_ = 0;
      retire_backlog_ms_ = retire_backlog_budget_ms_ = 0;
    }
  }
}

void GameRenderer::RetireSlot(Table table, uint32_t slot) {
  if (!slot) return;
  DescriptorTable& t = tables_[table];
  if (t.last_used[slot] + 8 < frame_) t.free.push_back(slot);
  else t.retired.emplace_back(frame_, slot);
}

void GameRenderer::WriteSlot(Table table, const nvrhi::BindingSetItem& item) {
  kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::Descriptor);
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
  resource_revision_.fetch_add(1, std::memory_order_release);
  invalidations_.emplace_back(physical, bytes);
}

void GameRenderer::ProcessInvalidations() {
  kknr::ResourceWorkTimer timer(resource_profile_, kknr::ResourceWork::Invalidate);
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
  // Scene effects and upscalers keep independent views and can write targets
  // outside GetFramebuffer. Until those writes participate in clear tracking,
  // retain their targets rather than incorrectly restore an old clear.
  // Drop GTAO's old target views once on entering pressure. No image or
  // pipeline is rebuilt here, and the next frame classifies its targets anew.
  if (memory_pressure_samples_ == 0) {
    if (scene_ao_) scene_ao_->ForgetTargets();
    if (scene_gi_) scene_gi_->ForgetTargets();
    if (gi_parked_) gi_parked_->ForgetTargets();
    if (scene_lighting_) scene_lighting_->ForgetTargets();
    if (lighting_parked_) lighting_parked_->ForgetTargets();
    if (scene_cel_) scene_cel_->ForgetTargets();
    if (cel_parked_) cel_parked_->ForgetTargets();
    if (scene_photo_) scene_photo_->ForgetTargets();
  }
  if (upscale_ || scene_ao_ || scene_gi_ || gi_parked_ || scene_lighting_ || lighting_parked_ || scene_cel_ || cel_parked_ ||
      scene_photo_) return;
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
  if (first) memory_start_ = now;
  memory_sample_ = now;
  const MemoryBudget live = options_.memory_budget ? options_.memory_budget() : MemoryBudget{};
  if (benchmark_stats_enabled.load(std::memory_order_relaxed)) {
    std::lock_guard lock(benchmark_stats_mutex);
    benchmark_stats.vram_used = live.usage;
    benchmark_stats.vram_budget = live.budget;
    benchmark_stats.memory_valid = live.valid;
  }
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
  // Let the chapter allocate and use its full-resolution AO first, so the
  // fault injection exercises a live resize rather than just startup sizing.
  const bool simulated = cap && force && std::strcmp(force, "1") == 0 &&
                         now - memory_start_ >= std::chrono::seconds(20);
  if (simulated) tested.usage = std::max(tested.usage, tested.budget);
  const bool pressure = MemoryPressure(tested);
  pack_limit_ = TexturePackLimit(tested, pack_bytes_);
  // Developer: a small card's pack limit without real memory pressure (MiB).
  if (const char* value = std::getenv("KK_DEV_TEXTURE_PACK_LIMIT_MIB"))
    pack_limit_ = std::min<uint64_t>(pack_limit_, std::strtoull(value, nullptr, 10) << 20);
  pack_pressure_ = pressure;
  if (pressure) DropTexturePacks();
  const char* control = std::getenv("KK_DEV_VRAM_CONTROL");
  const bool enabled = !control || std::strcmp(control, "0") != 0;
  if (enabled && pressure) {
    // Reclaim first. Wait five one-second samples before reducing GTAO; never
    // wait for the GPU, flush the queue, or restore quality in a pressure loop.
    if (memory_pressure_samples_ % 5 == 0) TrimClearTargets();
    if (++memory_pressure_samples_ >= 5 && scene_ao_ && !memory_half_ao_) {
      scene_ao_->SetMemoryPressure(true);
      memory_half_ao_ = true;
      Logf(LogLevel::kWarning, "rexgpu-native: GPU budget pressure: GTAO capped at half resolution when enabled");
    }
    if (memory_pressure_samples_ >= 10 && memory_pressure_samples_ % 5 == 0 &&
        memory_migration_ready_ && !upscale_ &&
        (options_.render_scale_x > memory_floor_x_ + 0.001f ||
         options_.render_scale_y > memory_floor_y_ + 0.001f || options_.shadow_scale > 1.001f))
      pending_memory_scale_ = 0.75f;
  } else {
    memory_pressure_samples_ = 0;
  }
  Logf(LogLevel::kInfo,
       "rexgpu-native: memory budget frame %llu: usage=%llu budget=%llu effective=%llu pressure=%u "
       "control=%u targets=%llu target_bytes=%llu framebuffers=%zu retired=%zu evictions=%llu restores=%llu half_ao=%u simulated=%u tested_usage=%llu scale_steps=%u scale_x_milli=%u scale_y_milli=%u",
       static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(live.usage),
       static_cast<unsigned long long>(live.budget), static_cast<unsigned long long>(effective.budget),
       unsigned(pressure), unsigned(enabled), static_cast<unsigned long long>(stats_.edram_targets),
       static_cast<unsigned long long>(stats_.edram_target_bytes), framebuffers_.size(), retired_resources_.size(),
       static_cast<unsigned long long>(memory_evictions_), static_cast<unsigned long long>(memory_restores_),
       unsigned(memory_half_ao_), unsigned(simulated), static_cast<unsigned long long>(tested.usage),
       memory_scale_steps_, unsigned(std::lround(options_.render_scale_x * 1000)),
       unsigned(std::lround(options_.render_scale_y * 1000)));
}

void GameRenderer::EndFrame() {
  // These Beta 5 passes bypass GetFramebuffer. Their writes invalidate saved
  // clears even if the player disables the effect before pressure next occurs.
  if (scene_gi_ || scene_lighting_ || scene_cel_)
    for (auto& [key, target] : targets_) if (target) target->clear_only = false;
  ProfileResources();
  EndStartUploadFrame();
  LogTexturePackFrame();
  FlushUploads();
  if (upload_batch_) upload_batch_->EndFrame();
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
  ReleaseRetiredResources();
  CheckMemoryBudget();
  stats_.edram_framebuffers = framebuffers_.size();
  if (pipeline_library_) {
    // Task 183: the whole pipelines that replace linked ones come in here (a draw finds its pipeline without
    // asking the workers), and the linked ones go two frames on (the lists hold their own references).
    std::lock_guard lock(pipeline_mutex_);
    DrainDonePipelinesLocked();
    std::erase_if(replaced_pipelines_, [&](const auto& r) { return r.first + 2 <= frame_; });
  }
  if (frame_ % 300 == 0) {
    LogPipelineMisses(false);
    LogPipelineLibrary(false);
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
    if (!resource_loading_) {
      // Drops what has been unused for 1800 frames; the host resources go to retired_resources_ and are
      // destroyed a few per frame (ReleaseRetiredResources).
      const auto trim_start = std::chrono::steady_clock::now();
      const size_t dt = textures_.Trim(frame_, 1800);
      const size_t dv = vertex_buffers_.Trim(frame_, 1800);
      const size_t di = index_buffers_.Trim(frame_, 1800);
      if (dt + dv + di)
        Logf(LogLevel::kInfo,
             "rexgpu-native: cache trim frame %llu: %zu textures, %zu vertex buffers, %zu index buffers unused for "
             "1800 frames dropped in %.2f ms (kept %zu/%zu/%zu); retired resources %zu",
             static_cast<unsigned long long>(frame_), dt, dv, di,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trim_start).count(),
             textures_.size(), vertex_buffers_.size(), index_buffers_.size(), retired_resources_.size());
    }
  }
  RetryDeferred();
  stats_.driver_cache_saves = driver_cache_saves_.load(std::memory_order_relaxed);
  stats_.driver_cache_bytes = driver_cache_bytes_.load(std::memory_order_relaxed);
  // New cache file records go to disk on the file writer (task 177), one queued write at a time.
  if (cache_file_.has_pending() && !pipeline_records_queued_.exchange(true)) {
    file_writer_.Post([this] {
      pipeline_records_queued_.store(false);
      cache_file_.Flush();
    });
  }
}

void GameRenderer::PipelineWorker() {
  bool urgent_priority = false;
  SchedulePipelineCompiler(false);
  for (;;) {
    PipelineJob job;
    {
      std::unique_lock lock(pipeline_mutex_);
      pipeline_cv_.wait(lock, [&] {
        // Reserve one compiler for demand work while prewarm has a backlog. Task 276: fewer while a level is
        // being played when --native_pipeline_play_workers says so (draws' jobs still take any worker).
        const unsigned background = pipelines_playing_ ? play_background_ : all_background_;
        return pipeline_stop_ || (!pipeline_jobs_.empty() &&
            (pipeline_jobs_.front().urgent || unsigned(pipelines_active_) < background));
      });
      if (pipeline_stop_) return;
      job = std::move(pipeline_jobs_.front());
      pipeline_jobs_.pop_front();
      ++pipelines_active_;
    }
    if (job.urgent != urgent_priority) {
      urgent_priority = job.urgent;
      SchedulePipelineCompiler(urgent_priority);
    }
    // Task 183: with the graphics pipeline library a draw's pipeline (a job a draw asked for: urgent) is linked
    // from cached parts, then compiled whole behind the other jobs; a listed record's parts are made with it.
    PipelineBuild build = job.build;
    if (job.urgent && build != PipelineBuild::kReplace && build != PipelineBuild::kParts)
      build = pipeline_library_ ? PipelineBuild::kLinked : PipelineBuild::kWhole;
    const auto start = std::chrono::steady_clock::now();
    // Dev (task 267): KK_DEV_PIPELINE_DELAY_MS=N adds N ms to each worker compile (not a link), as a slow PC's
    // cold driver compiles: a laptop's chapter-start backlog reproduced on a fast PC.
    if (dev_compile_delay_ms_ > 0 && build != PipelineBuild::kLinked)
      std::this_thread::sleep_for(std::chrono::milliseconds(dev_compile_delay_ms_));
    nvrhi::GraphicsPipelineHandle p;
    if (build == PipelineBuild::kParts) {
      const bool made = PrepareLibraryParts(job.desc, job.framebuffer);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      bool idle = false;
      {
        std::lock_guard lock(pipeline_mutex_);
        pipeline_worker_ms_ += ms;
        if (made) ++stats_.shader_parts;
        stats_.shader_parts_ms += ms;
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
      continue;
    }
    if (pipeline_library_ && (build == PipelineBuild::kLinked || build == PipelineBuild::kPrewarm)) {
      nvrhi::GraphicsPipelineDesc linked = job.desc;
      linked.vulkanLibraryLink = true;
      p = CreatePipeline(linked, job.framebuffer, true);
      if (build == PipelineBuild::kPrewarm && !pipeline_library_link_only_) p = nullptr;  // only its parts are kept
    }
    // Task 233: after the exit gate closed, a job is refused at once (nullptr, counted as failed: a setup run
    // the exit cut short is never marked prepared); a draw waiting for it then skips.
    if (!p) p = CreatePipeline(job.desc, job.framebuffer, true);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    bool idle = false;
    {
      std::lock_guard lock(pipeline_mutex_);
      pipeline_worker_ms_ += ms;
      pipelines_done_.push_back({job.key, p, build, ms, job.urgent});
      if (build == PipelineBuild::kLinked && p && !pipeline_stop_ && !pipeline_library_link_only_) {
        // The whole pipeline after everything queued (the prewarm included): it replaces the linked one.
        PipelineJob replace{job.key, std::move(job.desc), job.framebuffer};
        replace.build = PipelineBuild::kReplace;
        pipeline_jobs_.push_back(std::move(replace));
      }
      ++pipelines_made_;
      if (!p) ++pipelines_failed_;
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
                                                           const nvrhi::FramebufferInfo& framebuffer, bool background) {
  std::shared_lock lock(creation_mutex_);
  nvrhi::GraphicsPipelineHandle p = CreateGraphicsPipeline(device_, desc, framebuffer, background);
  if (p && driver_cache_) driver_cache_new_.fetch_add(1, std::memory_order_relaxed);
  return p;
}

bool GameRenderer::PrepareLibraryParts(const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& framebuffer) {
#if NR_HAS_VULKAN
  void* native = device_->getNativeObject(nvrhi::ObjectTypes::Nvrhi_VK_Device);
  if (!native) return false;
  std::shared_lock lock(creation_mutex_);
  PipelineExitGate::Pass pass(ExitGate(), true);  // a worker's job (task 233): refused once the exit started
  if (!pass) return false;
  const bool made = static_cast<nvrhi::vulkan::IDevice*>(native)->prepareGraphicsPipelineParts(desc, framebuffer);
  if (made && driver_cache_) driver_cache_new_.fetch_add(1, std::memory_order_relaxed);
  return made;
#else
  (void)desc;
  (void)framebuffer;
  return false;
#endif
}

void GameRenderer::QueueShaderParts(bool vertex, const nvrhi::ShaderHandle& shader) {
  // The game's thread, as it creates the shader. The part is keyed as a draw's pipeline will key it (BuildPipelineDesc:
  // this renderer's layouts, solid fill, one sample, no alpha to coverage).
  if (!shader) return;
  nvrhi::GraphicsPipelineDesc desc;
  (vertex ? desc.VS : desc.PS) = shader;
  for (const nvrhi::BindingLayoutHandle& l : pipeline_layouts_) desc.bindingLayouts.push_back(l);
  nvrhi::FramebufferInfo framebuffer;
  framebuffer.sampleCount = 1;
  std::lock_guard lock(pipeline_mutex_);
  if (pipeline_stop_ || !shader_parts_queued_.insert(shader.Get()).second) return;
  EnsureWorkers();
  PipelineJob job{0, std::move(desc), framebuffer};
  job.build = PipelineBuild::kParts;
  pipeline_jobs_.push_back(std::move(job));
  pipeline_cv_.notify_one();
}

bool GameRenderer::IsPending(uint64_t key) {
  std::lock_guard lock(pipeline_mutex_);
  return pipelines_pending_.count(key) != 0;
}

void GameRenderer::DrainDonePipelinesLocked() {
  if (pipelines_done_.empty()) return;
  const auto now = std::chrono::steady_clock::now();
  for (DonePipeline& done : pipelines_done_) {
    const uint64_t k = done.key;
    const nvrhi::GraphicsPipelineHandle& p = done.pipeline;
    if (done.build == PipelineBuild::kReplace) {
      // Task 183: the whole pipeline replaces the linked one; draws use it from now on.
      if (p) {
        nvrhi::GraphicsPipelineHandle& slot = pipelines_[k];
        if (slot) replaced_pipelines_.emplace_back(frame_, std::move(slot));
        slot = p;
        ++stats_.pipelines_replaced;
        stats_.replace_ms_total += done.ms;
      }
      if (pipeline_log_) LogPipelineDemand(k, "built:whole-replace", done.ms, bool(p));
      continue;
    }
    pipelines_[k] = p;
    pipelines_pending_.erase(k);
    if (p) {
      ++stats_.pipelines;
      ++stats_.draw_pipelines;
    }
    if (done.build == PipelineBuild::kLinked) {
      ++stats_.pipelines_linked;
      stats_.linked_ms_total += done.ms;
      stats_.linked_ms_max = std::max(stats_.linked_ms_max, done.ms);
    }
    if (pipeline_log_ && done.build != PipelineBuild::kPrewarm) {
      // Task 183: the worker's time for a pipeline made on demand (linked, or whole without the library).
      LogPipelineDemand(k, done.build == PipelineBuild::kLinked ? "built:linked" : (done.urgent ? "built:whole" : "built:optional"),
                        done.ms, bool(p));
    }
    if (auto miss = missing_pipelines_.find(k); miss != missing_pipelines_.end()) {
      // Task 179: a skipped draw's pipeline is ready; its draws are drawn from this frame on.
      const uint64_t frames = miss->second.frames;
      const double ms = std::chrono::duration<double, std::milli>(now - miss->second.first).count();
      ++stats_.pipelines_missed_ready;
      stats_.missed_frames_total += frames;
      stats_.missed_frames_max = std::max(stats_.missed_frames_max, frames);
      stats_.missed_ms_total += ms;
      stats_.missed_ms_max = std::max(stats_.missed_ms_max, ms);
      if (pipeline_log_) {
        char kind[32];
        std::snprintf(kind, sizeof(kind), "ready:%llu", static_cast<unsigned long long>(frames));
        LogPipelineDemand(k, kind, ms, bool(p));
      }
      missing_pipelines_.erase(miss);
    }
  }
  pipelines_done_.clear();
  stats_.pipeline_ms = pipeline_worker_ms_;
}

PipelineMissClass GameRenderer::DrawMissClass(const PipelineRecord& record) const {
  if (options_.pipeline_wait) return PipelineMissClass::kBlockAll;
  if (!options_.pipeline_wait_for_dependencies) return PipelineMissClass::kScene;
  if (resource_loading_) return PipelineMissClass::kLoading;
  return ClassifyPipelineMiss(record);
}

nvrhi::IGraphicsPipeline* GameRenderer::GetPipeline(const nvrhi::GraphicsPipelineDesc& desc,
                                                    const nvrhi::FramebufferInfo& framebuffer, uint64_t key,
                                                    PipelineDemand demand, PipelineMissClass miss_class) {
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  // optional (the upscalers' motion pass, lighting variants, memory blits): made on the workers, never
  // waited for, not counted as a draw that waited or was skipped. kSoon too, but queued with the draws' jobs
  // (task 247: else the AO hands redraw waited ~50 s behind a cold start's prewarm).
  const bool soon = demand == PipelineDemand::kSoon;
  const bool optional = demand == PipelineDemand::kOptional || soon;
  const bool for_draws = demand == PipelineDemand::kMustDraw || demand == PipelineDemand::kMaySkip;
  const bool async = optional || (for_draws && options_.async_pipelines);
  if (async) {
    std::unique_lock lock(pipeline_mutex_);
    DrainDonePipelinesLocked();
    if (auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;
    const bool was_pending = pipelines_pending_.count(key) != 0;
    const bool first = demand == PipelineDemand::kMustDraw;
    if (!was_pending) {
      EnsureWorkers();
      pipelines_pending_[key] = true;
      if (optional && !soon) {
        pipeline_jobs_.push_back({key, desc, framebuffer});
        pipeline_cv_.notify_one();
      } else {
        QueueDrawJob(pipeline_jobs_, PipelineJob{key, desc, framebuffer}, first);
        pipeline_cv_.notify_all();
      }
    } else if (!optional || soon) {
      // Promote an unstarted prewarm job too. An already active job needs no duplicate.
      if (PromoteDrawJob(pipeline_jobs_, key, first)) pipeline_cv_.notify_all();
    }
    if (optional) return nullptr;
    const char* demand_kind = was_pending ? "pending" : "new";
    if (demand == PipelineDemand::kMustDraw) {
      // Wait for the worker that has this pipeline (the draw's own job, at the front, or the prewarm's). Task 267:
      // within a hard cap per frame (all such draws together); after it the draw is skipped until it is ready.
      if (must_wait_frame_ != frame_) {
        must_wait_frame_ = frame_;
        must_wait_ms_ = 0;
      }
      const double cap = miss_class == PipelineMissClass::kBlockAll ? -1.0
                         : miss_class == PipelineMissClass::kLoading ? options_.pipeline_loading_wait_ms
                                                                     : options_.pipeline_must_wait_ms;
      const double left = MustDrawWaitLeft(cap, must_wait_ms_);
      auto ready = [&] {
        DrainDonePipelinesLocked();
        return pipelines_.count(key) != 0 || pipeline_stop_;
      };
      const auto start = std::chrono::steady_clock::now();
      if (left < 0) {
        pipeline_done_cv_.wait(lock, ready);
      } else if (left > 0) {
        pipeline_done_cv_.wait_for(lock, std::chrono::duration<double, std::milli>(left), ready);
      } else {
        ready();
      }
      const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      must_wait_ms_ += waited;
      stats_.pipeline_wait_ms += waited;
      auto found = pipelines_.find(key);
      if (found != pipelines_.end() || pipeline_stop_ || waited > 0.05) NoteFrameWait(waited);
      if (found != pipelines_.end() || pipeline_stop_) {
        ++stats_.pipeline_waits;
        ++stats_.pipeline_waits_by_class[size_t(miss_class)];
        if (pipeline_log_ || !pipeline_log_checked_)
          LogPipelineDemand(key, demand_kind, waited, found != pipelines_.end() && found->second, miss_class);
        return found != pipelines_.end() ? found->second.Get() : nullptr;
      }
      ++stats_.must_draw_skips;  // not ready within the cap: skipped below as a scene draw (its job stays first)
    } else if (options_.pipeline_wait_budget_ms > 0) {
      // Wait within what is left of this frame's budget.
      if (wait_budget_frame_ != frame_) {
        wait_budget_frame_ = frame_;
        frame_wait_ms_ = 0;
      }
      const double left = options_.pipeline_wait_budget_ms - frame_wait_ms_;
      if (left > 0) {
        const auto start = std::chrono::steady_clock::now();
        pipeline_done_cv_.wait_for(lock, std::chrono::duration<double, std::milli>(left), [&] {
          DrainDonePipelinesLocked();
          return pipelines_.count(key) != 0 || pipeline_stop_;
        });
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        frame_wait_ms_ += ms;
        stats_.pipeline_wait_ms += ms;
        NoteFrameWait(ms);
        if (auto found = pipelines_.find(key); found != pipelines_.end()) {
          ++stats_.pipeline_budget_waits;
          ++stats_.pipeline_waits_by_class[size_t(PipelineMissClass::kScene)];
          if (pipeline_log_ || !pipeline_log_checked_)
            LogPipelineDemand(key, demand_kind, ms, bool(found->second), miss_class);
          return found->second;
        }
      }
    }
    // Skipped this frame (task 179): drawn as soon as a worker has made it.
    auto [miss, added] =
        missing_pipelines_.try_emplace(key, MissingPipeline{frame_, frame_, 1, std::chrono::steady_clock::now()});
    if (added) {
      ++stats_.pipelines_missed;
      if (pipeline_log_ || !pipeline_log_checked_) LogPipelineDemand(key, demand_kind, 0, false, miss_class);
    } else if (miss->second.last_frame != frame_) {
      miss->second.last_frame = frame_;
      ++miss->second.frames;
    }
    ++stats_.skipped_pending;
    ++frame_skips_;
    return nullptr;
  }
  const auto start = std::chrono::steady_clock::now();
  nvrhi::GraphicsPipelineHandle p;
  const bool link = for_draws && pipeline_library_ && options_.link_draw_pipelines;
  if (link) {
    nvrhi::GraphicsPipelineDesc linked = desc;
    linked.vulkanLibraryLink = true;
    p = CreatePipeline(linked, framebuffer);
    if (p) ++stats_.pipelines_linked;
  }
  if (!p) p = CreatePipeline(desc, framebuffer);
  const double made_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  stats_.pipeline_ms += made_ms;
  if (for_draws) NoteFrameWait(made_ms);  // the draw waited for the driver on its own thread
  if (pipeline_log_ || !pipeline_log_checked_)
    LogPipelineDemand(key, for_draws ? "draw-thread" : "pass", made_ms, bool(p), miss_class);
  pipelines_[key] = p;  // a failure is remembered too (null)
  if (p) {
    ++stats_.pipelines;
    if (for_draws) ++stats_.draw_pipelines;
  }
  return p;
}

void GameRenderer::LogPipelineMisses(bool final) {
  // Task 179: the draws skipped while their pipelines were made, and the waits that remain (cumulative;
  // a line only when something changed, and one when the renderer goes).
  const Stats& s = stats_;
  const Stats& p = miss_log_previous_;
  if (!final && s.skipped_pending == p.skipped_pending && s.pipeline_waits == p.pipeline_waits &&
      s.pipeline_budget_waits == p.pipeline_budget_waits && s.pipelines_missed_ready == p.pipelines_missed_ready &&
      s.must_draw_skips == p.must_draw_skips)
    return;
  if (final && s.skipped_pending == 0 && s.pipeline_waits == 0 && s.pipeline_budget_waits == 0) return;
  miss_log_previous_ = stats_;
  uint64_t frames_skipped = 0;
  {
    std::lock_guard lock(stutter_mutex_);
    frames_skipped = stutter_.frames_skipped;
  }
  size_t queued = 0;
  {
    std::lock_guard lock(pipeline_mutex_);
    queued = pipeline_jobs_.size();
  }
  const auto& w = s.pipeline_waits_by_class;
  Logf(LogLevel::kInfo,
       "rexgpu-native: pipelines not ready %s frame %llu: %llu draws skipped in %llu frames, %llu pipelines missed "
       "(%llu ready after avg %.1f max %llu frames, max %.0f ms); waits %llu (depth-only %llu, stencil %llu, "
       "screen-space %llu, loading %llu, block-all %llu, budget %llu), %.0f ms; %llu skipped after the wait cap; "
       "%zu jobs queued",
       final ? "at the end," : "so far,", static_cast<unsigned long long>(frame_),
       static_cast<unsigned long long>(s.skipped_pending), static_cast<unsigned long long>(frames_skipped),
       static_cast<unsigned long long>(s.pipelines_missed), static_cast<unsigned long long>(s.pipelines_missed_ready),
       s.pipelines_missed_ready ? double(s.missed_frames_total) / double(s.pipelines_missed_ready) : 0.0,
       static_cast<unsigned long long>(s.missed_frames_max), s.missed_ms_max,
       static_cast<unsigned long long>(s.pipeline_waits + s.pipeline_budget_waits),
       static_cast<unsigned long long>(w[size_t(PipelineMissClass::kDepthOnly)]),
       static_cast<unsigned long long>(w[size_t(PipelineMissClass::kStencil)]),
       static_cast<unsigned long long>(w[size_t(PipelineMissClass::kScreenSpace)]),
       static_cast<unsigned long long>(w[size_t(PipelineMissClass::kLoading)]),
       static_cast<unsigned long long>(w[size_t(PipelineMissClass::kBlockAll)]),
       static_cast<unsigned long long>(w[size_t(PipelineMissClass::kScene)]), s.pipeline_wait_ms,
       static_cast<unsigned long long>(s.must_draw_skips), queued);
}

GameRenderer::PipelineLibraryStats GameRenderer::PipelineLibraryCounts() const {
  PipelineLibraryStats out;
#if NR_HAS_VULKAN
  // NVRHI's own Vulkan device, under its validation layer too (which passes the native object through).
  if (!pipeline_library_) return out;
  void* native = device_->getNativeObject(nvrhi::ObjectTypes::Nvrhi_VK_Device);
  if (!native) return out;
  const nvrhi::vulkan::IDevice::PipelineLibraryStats l =
      static_cast<nvrhi::vulkan::IDevice*>(native)->getPipelineLibraryStats();
  for (int i = 0; i < 4; ++i) {
    out.partsMade[i] = l.partsMade[i];
    out.partsReused[i] = l.partsReused[i];
  }
  out.partFailures = l.partFailures;
  out.partMs = l.partMs;
  out.links = l.links;
  out.linkFailures = l.linkFailures;
  out.linkMs = l.linkMs;
#endif
  return out;
}

void GameRenderer::LogPipelineLibrary(bool final) {
  // Task 183: the graphics pipeline library's parts and links (a line when something changed, and at the end).
  if (!pipeline_library_) return;
  const PipelineLibraryStats l = PipelineLibraryCounts();
  const uint64_t parts = l.partsMade[0] + l.partsMade[1] + l.partsMade[2] + l.partsMade[3];
  if (!final && l.links == library_logged_links_ && parts == library_logged_parts_) return;
  library_logged_links_ = l.links;
  library_logged_parts_ = parts;
  const Stats& s = stats_;
  Logf(LogLevel::kInfo,
       "rexgpu-native: pipeline library %s frame %llu: %llu draws' pipelines linked (avg %.2f max %.2f ms on a "
       "worker), %llu replaced by whole ones (avg %.1f ms); %llu game shaders' parts made ahead (%.0f ms); parts made %llu/%llu/%llu/%llu, reused "
       "%llu/%llu/%llu/%llu (vertex input/pre-rasterization/fragment shader/output), %.0f ms; %llu links %.1f ms, "
       "%llu part and %llu link failures",
       final ? "at the end," : "so far,", static_cast<unsigned long long>(frame_),
       static_cast<unsigned long long>(s.pipelines_linked),
       s.pipelines_linked ? s.linked_ms_total / double(s.pipelines_linked) : 0.0, s.linked_ms_max,
       static_cast<unsigned long long>(s.pipelines_replaced),
       s.pipelines_replaced ? s.replace_ms_total / double(s.pipelines_replaced) : 0.0,
       static_cast<unsigned long long>(s.shader_parts), s.shader_parts_ms,
       static_cast<unsigned long long>(l.partsMade[0]), static_cast<unsigned long long>(l.partsMade[1]),
       static_cast<unsigned long long>(l.partsMade[2]), static_cast<unsigned long long>(l.partsMade[3]),
       static_cast<unsigned long long>(l.partsReused[0]), static_cast<unsigned long long>(l.partsReused[1]),
       static_cast<unsigned long long>(l.partsReused[2]), static_cast<unsigned long long>(l.partsReused[3]),
       l.partMs, static_cast<unsigned long long>(l.links), l.linkMs,
       static_cast<unsigned long long>(l.partFailures), static_cast<unsigned long long>(l.linkFailures));
}

void GameRenderer::LogPipelineDemand(uint64_t key, const char* kind, double ms, bool made,
                                     PipelineMissClass miss_class) {
  if (!pipeline_log_checked_) {
    pipeline_log_checked_ = true;
    if (const char* path = std::getenv("NR_PIPELINE_LOG"); path && *path) {
      pipeline_log_ = std::fopen(path, "a");  // appended: a relaunched process (back to the launcher) keeps the rows
      if (pipeline_log_) std::fseek(pipeline_log_, 0, SEEK_END);
      if (pipeline_log_ && std::ftell(pipeline_log_) == 0)
        std::fprintf(pipeline_log_, "unix_s,frame,key,kind,wait_ms,made,vs,vs_input,ps,ps_input,flags,colors,depth,class\n");
    }
  }
  if (!pipeline_log_) return;
  const double unix_s =
      std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  const PipelineRecord* r = demand_record_;
  std::fprintf(pipeline_log_, "%.3f,%llu,%016llX,%s,%.3f,%d", unix_s, static_cast<unsigned long long>(frame_),
               static_cast<unsigned long long>(key), kind, ms, made ? 1 : 0);
  if (r && PipelineKey(*r) == key) {
    std::fprintf(pipeline_log_, ",%016llX,%016llX,%016llX,%016llX,%u,%u,%u",
                 static_cast<unsigned long long>(r->vs_hash), static_cast<unsigned long long>(r->vs_input),
                 static_cast<unsigned long long>(r->ps_hash), static_cast<unsigned long long>(r->ps_input), r->flags,
                 r->color_count, r->depth_format);
  } else {
    std::fprintf(pipeline_log_, ",,,,,,,");
  }
  std::fprintf(pipeline_log_, ",%s\n", PipelineMissClassName(miss_class));
  std::fflush(pipeline_log_);
}

// ------------------------------------------------------- the pipeline cache

void GameRenderer::EnsureWorkers() {
  // With pipeline_mutex_ held. A few workers: drivers compile pipelines in parallel.
  if (!pipeline_threads_.empty()) return;
  const unsigned n = ProcessPipelineWorkerCount();
  const ProcessCpus cpus = ProcessCpuCounts();
  all_background_ = BackgroundPipelineWorkers(n);
  play_background_ = PlayBackgroundPipelineWorkers(n, cpus.logical, cpus.cores, options_.pipeline_play_workers);
  Logf(LogLevel::kInfo,
       "rexgpu-native: pipeline compiler workers %u (process affinity; %u logical CPUs, %u cores); background jobs "
       "%u at once, %u while a level is played (native_pipeline_play_workers %d)",
       n, cpus.logical, cpus.cores, all_background_, play_background_, options_.pipeline_play_workers);
  for (unsigned i = 0; i < n; ++i) pipeline_threads_.emplace_back([this] { PipelineWorker(); });
}

// Task 276: a level's loading screen started. New records are tagged with the level from now on; with
// pipeline_level_first the prewarm's jobs of the records tagged with it go ahead of the rest of the list
// (behind the draws' jobs), and the background workers run at full speed until the level is played.
void GameRenderer::PipelineLevelLoading(uint32_t level) {
  record_level_tag_ = LevelTag(level);
  const auto start = std::chrono::steady_clock::now();
  size_t ahead = 0, queued = 0;
  {
    std::lock_guard lock(pipeline_mutex_);
    pipelines_playing_ = false;
    if (options_.pipeline_level_first) {
      const uint32_t tag = priority_level_tag_ = LevelTag(level);
      ahead = MoveJobsAhead(pipeline_jobs_, [tag](const PipelineJob& j) { return j.urgent || j.level == tag; });
      queued = pipeline_jobs_.size();
    }
  }
  pipeline_cv_.notify_all();
  if (options_.pipeline_level_first && queued) {
    Logf(LogLevel::kInfo, "rexgpu-native: pipelines of level %u first: %zu of %zu queued jobs ahead (in %.1f ms)",
         level, ahead, queued,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
}

void GameRenderer::PipelineLevelPlaying() {
  size_t queued = 0;
  {
    std::lock_guard lock(pipeline_mutex_);
    pipelines_playing_ = true;
    queued = pipeline_jobs_.size();
  }
  pipeline_cv_.notify_all();
  if (play_background_ < all_background_ && queued && !play_workers_logged_) {
    play_workers_logged_ = true;
    Logf(LogLevel::kInfo, "rexgpu-native: while a level is played, %u background pipeline jobs at once (of %u); %zu queued",
         play_background_, all_background_, queued);
  }
}

void GameRenderer::NoteRecord(const PipelineRecord& record, uint64_t key) {
  if (!cache_recording_.load(std::memory_order_relaxed)) return;
  if (!noted_keys_.insert(key).second) return;  // the render thread's own set: no lock for a known pipeline
  // Task 276: the level it is first drawn in (not known before the first loading screen).
  PipelineRecord tagged = record;
  tagged.reserved[0] = record_level_tag_;
  std::lock_guard lock(cache_mutex_);
  if (cache_file_.is_open() && cache_file_.Append(tagged)) ++stats_.cache_recorded;
}

bool GameRenderer::QueuePrewarm(const PipelineRecord& record) {
  PipelineRecord adapted = record;
  ClearRecordLevel(adapted);  // task 276: the tag orders the job, it is not part of the pipeline
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
  PipelineJob job{key, std::move(desc), framebuffer};
  job.build = PipelineBuild::kPrewarm;
  job.level = record.reserved[0];
  // Task 276: a record of the level being loaded or played (its shaders came late) joins its level's group.
  if (const uint32_t tag = priority_level_tag_; tag && job.level == tag)
    QueueJobBehind(pipeline_jobs_, std::move(job), [tag](const PipelineJob& j) { return j.urgent || j.level == tag; });
  else
    pipeline_jobs_.push_back(std::move(job));
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
  resource_cache_base_ = path;
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
  // Task 233: once the exit started, the exit flush saves it (the file writer's exit task), not an idle worker.
  if (!driver_cache_ || ExitGate().closed()) return;
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
  return {stats_.prewarm_queued, pipelines_made_, uint64_t(pipeline_jobs_.size()),
          uint64_t(pipelines_active_), pipelines_failed_};
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

namespace {
// Task 231 (draw_tags.h): does a HUD-tagged draw cover the whole guest width (a fade, a full-screen tint)?
// Reads up to 64 of its vertices' positions; true when they span the viewport's width as pixels, or the clip
// space through the c0-c3 rows (dp4 per row, as the game's 2D shaders do) spans -1..1. Logs the first distinct
// tagged draws when KK_ULTRAWIDE_HUD_LOG=1.
bool HudDrawCoversWidth(const DrawCall& c, const dev::DeviceView& d, bool* readable = nullptr) {
  if (readable) *readable = false;
  if (!c.vertex_data || c.stride == 0 || c.count == 0) return false;
  const uint32_t format = DeclTypeFormat(c.position_type);
  // Xenos formats: 37 = 32_32_FLOAT, 57 = 32_32_32_FLOAT, 38 = 32_32_32_32_FLOAT.
  const uint32_t comps = format == 38 ? 4 : format == 57 ? 3 : format == 37 ? 2 : (c.position_type ? 0 : 3);
  if (comps == 0) return false;
  float x0 = 1e30f, x1 = -1e30f, cx0 = 1e30f, cx1 = -1e30f;
  uint32_t n = 0;
  for (uint32_t i = 0; i < std::min(c.count, 64u); ++i) {
    int64_t v = int64_t(c.start) + i;
    if (c.indexed) {
      if (!c.index_data) return false;
      const uint64_t at = uint64_t(c.start) + i;
      if ((at + 1) * (c.index_info.index32 ? 4 : 2) > c.index_info.size_bytes) break;
      v = int64_t(c.index_info.index32 ? LoadBE32(c.index_data + at * 4) : LoadBE16(c.index_data + at * 2)) +
          c.base_vertex;
    }
    const uint64_t at = uint64_t(std::max<int64_t>(v, 0)) * c.stride + c.position_offset;
    if (v < 0 || at + comps * 4 > c.vertex_data_size) continue;
    float p[4] = {0, 0, 0, 1};
    for (uint32_t k = 0; k < comps; ++k) p[k] = LoadBEFloat(c.vertex_data + at + k * 4);
    const float* m = c.vs_c0_c3.data();
    const float cx = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3] * p[3];
    const float cw = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15] * p[3];
    x0 = std::min(x0, p[0]);
    x1 = std::max(x1, p[0]);
    if (std::fabs(cw) > 1e-6f) {
      cx0 = std::min(cx0, cx / cw);
      cx1 = std::max(cx1, cx / cw);
    }
    ++n;
  }
  if (n == 0) return false;
  if (readable) *readable = true;
  const uint32_t vte = d.u32(dev::kPaClVteCntl);
  const float vw = float(c.viewport.width ? c.viewport.width : 1280), vx = float(c.viewport.x);
  const bool pixels = (vte & 1) == 0 && x0 <= vx + 0.02f * vw && x1 >= vx + 0.98f * vw;
  // Clip space through c0-c3, or an untextured quad given straight in clip space (a fade: x -1..1).
  const bool clip = (cx0 <= -0.98f && cx1 >= 0.98f) ||
      ((vte & 1) && c.textures[0] == 0 && x0 <= -0.98f && x1 >= 0.98f);
  static const bool log = [] {
    const char* e = std::getenv("KK_ULTRAWIDE_HUD_LOG");
    return e && *e == '1';
  }();
  if (log) {
    static std::unordered_set<uint64_t> seen;
    const uint64_t key = (uint64_t(c.tags) << 40) ^ (uint64_t(c.count) << 20) ^ (c.vertex_shader & 0xFFFFF) ^
        (uint64_t(pixels || clip) << 63);
    if (seen.size() < 300 && seen.insert(key).second)
      Logf(LogLevel::kInfo,
           "KK: ultrawide HUD draw leaf %u prim %u count %u vs %08X tex0 %08X vte %X vp %u+%u x %.4g..%.4g clip "
           "%.4g..%.4g full %d",
           (c.tags >> kDrawTagLeafShift) & 0xFF, uint32_t(c.primitive), c.count, c.vertex_shader, c.textures[0],
           vte, c.viewport.x, c.viewport.width, x0, x1, cx0, cx1, int(pixels || clip));
  }
  return pixels || clip;
}
}  // namespace

void GameRenderer::Draw(nvrhi::ICommandList* cl, const DrawCall& call) {
  struct UploadGuard {
    GameRenderer* game;
    ~UploadGuard() { game->resource_prepare_call_ = nullptr; game->FlushUploads(); }
  } uploads{this};
  PollResourcePreload(cl);
  UpdateEffects();
  ServiceTexturePacks(cl);
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
  if (PhotoHidesDraw(call, d, *vs, ps.get())) return;  // photo mode (game_photo.cpp): arms, weapon, pause cover
  // Hide HUD / F10 (task 243, draw_tags.h): the HUD pass's textured 2D (the ammo counter, subtitles) is left
  // out. Fades stay: untextured draws, full-width draws, and draws whose positions can't be read are kept.
  if (call.tags & kDrawTagHideAll) return;  // task 330: the game's menus under the launcher (draw_tags.h)
  if ((call.tags & kDrawTagHideHud) && call.textures[0]) {
    bool readable = false;
    if (!HudDrawCoversWidth(call, d, &readable) && readable) return;
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
  PrepareDescriptorDraw(cl, *vs, ps.get());
  if (!NoteVolatileDraw(cl, false)) return;
  if (scene_ao_ || ao_constants_) AoDrawHook(cl, call, d, colors, depth, record, *vs, ps.get());
  if (scene_ao_) {  // task 244: the pre-pass's depth-only ALWAYS geometry (the first-person hands)
    bool writes_color = false;
    for (uint32_t i = 0; i < record.color_count; ++i) writes_color = writes_color || record.color_mask[i];
    AoNoteHandsDepth(call, record, writes_color);
  }
  // GI hook (game_gi.cpp): at the end of the opaque pass the bounce light goes in (after the AO's occlusion).
  if (scene_gi_) GiDrawHook(cl, call, d, colors, depth, record, *vs, ps.get());
  // Cel hook (game_cel.cpp): at the end of the opaque pass the lines and bands go in (after AO and GI).
  if (scene_cel_) CelDrawHook(cl, call, d, colors, depth, record, *vs, ps.get());
  // Photo hook (game_photo.cpp): the depth of field's depth at the end of the opaque pass.
  if (scene_photo_) PhotoDrawHook(cl, call, d, colors, depth, record, *vs, ps.get());
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
    demand_record_ = &record;
    // Task 179: a scene draw is skipped until its pipeline is made; a draw later draws depend on waits.
    const PipelineMissClass miss_class = DrawMissClass(record);
    pipeline = GetPipeline(pd, framebuffer_info, key, DrawDemand(miss_class), miss_class);
    demand_record_ = nullptr;
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
      if (auto* prepared = GetPipeline(pd, info, variant_key, PipelineDemand::kOptional)) {
        pipeline = prepared;
        ps = std::move(variant);
        ++(water ? live_draws_.water : live_draws_.shadows);
      } else {
        ++(water ? live_draws_.water_waiting : live_draws_.shadows_waiting);
      }
    }
  }

  if (stop_at == 2) return;
  resource_prepare_call_ = &call;
  resource_prepare_device_ = &d;
  resource_prepare_vs_ = vs.get();
  resource_prepare_ps_ = ps.get();
  resource_prepare_tried_ = false;
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
  if (call.tags & kDrawTagHud) {
    // Ultrawide mod (task 231): the game's 2D in a centred 16:9 box, full-width draws (fades) left as they are.
    const float a = HudWidthScale();
    if (a > 1.0f && !HudDrawCoversWidth(call, d)) {
      dc.ndcScale[0] /= a;
      dc.ndcOffset[0] /= a;
    }
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
    const bool changed = UpdateShaderConstants(raw, last_raw, last);
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
  // Reuse the current volatile version only inside the same list/frame, just as for the
  // VS/PS and scale constants. Compare every byte, including alpha test and tessellation.
  if (new_list || std::memcmp(&dc, &last_draw_, sizeof(dc)) != 0) {
    cl->writeBuffer(draw_constants_, &dc, sizeof(dc));
    stats_.upload_bytes += sizeof(dc);
    ++stats_.draw_constant_uploads;
    last_draw_ = dc;
  } else {
    ++stats_.draw_constant_identical;
  }
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
                           .setOffset(uint32_t(BufferOffset(host)))
                           .setSize(host->range.valid() ? host->view_bytes : 0);  // task 309: a pooled range
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
  // AO on the first-person hands (game_ao.cpp): their colour drawn again, times the occlusion.
  if (scene_ao_ && !ao_hands_.empty()) AoRedrawHands(cl, call, record, *vs, gs, args, indexed_draw, *size_from);
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
