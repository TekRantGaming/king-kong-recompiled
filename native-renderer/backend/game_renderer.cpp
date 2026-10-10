#include "backend/game_renderer.h"

#include <algorithm>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>

#include "backend/guest_layout.h"
#include "backend/log.h"
#include "backend/shaders.h"
#include "kknr/buffers.h"
#include "kknr/nvrhi_format.h"

namespace nr {

namespace {

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
    delete h;
    e.host = nullptr;
  };
  vertex_buffers_.release_host = release_buffer;
  index_buffers_.release_host = release_buffer;
}

GameRenderer::~GameRenderer() {
  if (!pipeline_threads_.empty()) {
    {
      std::lock_guard lock(pipeline_mutex_);
      pipeline_stop_ = true;
    }
    pipeline_cv_.notify_all();
    for (std::thread& t : pipeline_threads_) t.join();
  }
  textures_.Trim(UINT64_MAX / 2, 0);
  vertex_buffers_.Trim(UINT64_MAX / 2, 0);
  index_buffers_.Trim(UINT64_MAX / 2, 0);
}

void GameRenderer::set_watch(std::function<void(uint32_t, uint32_t)> watch) {
  watch_ = std::move(watch);
  textures_.watch_range = watch_;
  vertex_buffers_.watch_range = watch_;
  index_buffers_.watch_range = watch_;
}

bool GameRenderer::Initialize() {
  const nvrhi::GraphicsAPI api = device_->getGraphicsAPI();

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
  return clear_set_ != nullptr;
}

// ---------------------------------------------------------------- tables

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

void GameRenderer::RetireSlot(Table table, uint32_t slot) {
  if (slot) tables_[table].retired.emplace_back(frame_, slot);
}

void GameRenderer::WriteSlot(Table table, const nvrhi::BindingSetItem& item) {
  device_->writeDescriptorTable(tables_[table].table, item);
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
  }
  stats_.invalidations += list.size();
}

void GameRenderer::EndFrame() {
  ++frame_;
  index_ring_used_ = 0;
  for (DescriptorTable& t : tables_) {
    while (!t.retired.empty() && t.retired.front().first + 8 < frame_) {
      t.free.push_back(t.retired.front().second);
      t.retired.pop_front();
    }
  }
  if (frame_ % 300 == 0) {
    textures_.Trim(frame_, 1800);
    vertex_buffers_.Trim(frame_, 1800);
    index_buffers_.Trim(frame_, 1800);
  }
  RetryDeferred();
  std::lock_guard lock(cache_mutex_);
  cache_file_.Flush();
}

void GameRenderer::PipelineWorker() {
  for (;;) {
    PipelineJob job;
    {
      std::unique_lock lock(pipeline_mutex_);
      pipeline_cv_.wait(lock, [&] { return pipeline_stop_ || !pipeline_jobs_.empty(); });
      if (pipeline_stop_) return;
      job = std::move(pipeline_jobs_.front());
      pipeline_jobs_.pop_front();
      ++pipelines_active_;
    }
    const auto start = std::chrono::steady_clock::now();
    nvrhi::GraphicsPipelineHandle p = device_->createGraphicsPipeline(job.desc, job.framebuffer);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    {
      std::lock_guard lock(pipeline_mutex_);
      pipeline_worker_ms_ += ms;
      pipelines_done_.emplace_back(job.key, p);
      --pipelines_active_;
    }
    pipeline_done_cv_.notify_all();
  }
}

bool GameRenderer::IsPending(uint64_t key) {
  std::lock_guard lock(pipeline_mutex_);
  return pipelines_pending_.count(key) != 0;
}

nvrhi::IGraphicsPipeline* GameRenderer::GetPipeline(const nvrhi::GraphicsPipelineDesc& desc,
                                                    const nvrhi::FramebufferInfo& framebuffer, uint64_t key,
                                                    bool async) {
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  const bool for_draws = async;
  async = async && options_.async_pipelines && device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
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
      pipeline_jobs_.push_back({key, desc, framebuffer});
      pipeline_cv_.notify_one();
    }
    if (options_.pipeline_wait) {
      // Wait for the worker that has this pipeline (the draw's own job, or the prewarm's).
      const auto start = std::chrono::steady_clock::now();
      pipeline_done_cv_.wait(lock, [&] {
        drain();
        return pipelines_.count(key) != 0 || pipeline_stop_;
      });
      ++stats_.pipeline_waits;
      stats_.pipeline_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      auto found = pipelines_.find(key);
      return found != pipelines_.end() ? found->second.Get() : nullptr;
    }
    ++stats_.skipped_pending;
    return nullptr;
  }
  const auto start = std::chrono::steady_clock::now();
  nvrhi::GraphicsPipelineHandle p = device_->createGraphicsPipeline(desc, framebuffer);
  stats_.pipeline_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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
  const unsigned n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
  for (unsigned i = 0; i < n; ++i) pipeline_threads_.emplace_back([this] { PipelineWorker(); });
}

void GameRenderer::NoteRecord(const PipelineRecord& record, uint64_t key) {
  if (!cache_recording_.load(std::memory_order_relaxed)) return;
  if (!noted_keys_.insert(key).second) return;  // the render thread's own set: no lock for a known pipeline
  std::lock_guard lock(cache_mutex_);
  if (cache_file_.is_open() && cache_file_.Append(record)) ++stats_.cache_recorded;
}

bool GameRenderer::QueuePrewarm(const PipelineRecord& record) {
  const kkshaders::ShaderKind vk = kkshaders::ShaderKind::Vertex, pk = kkshaders::ShaderKind::Pixel;
  std::shared_ptr<const GameShader> vs = shaders_->GetByHash(vk, record.vs_hash);
  std::shared_ptr<const GameShader> ps = record.ps_hash ? shaders_->GetByHash(pk, record.ps_hash) : nullptr;
  if (!vs || (record.ps_hash && !ps)) return false;
  nvrhi::GraphicsPipelineDesc desc;
  nvrhi::FramebufferInfo framebuffer;
  BuildPipelineDesc(record, vs->handle, ps ? ps->handle.Get() : nullptr, desc, framebuffer);
  const uint64_t key = PipelineKey(record);
  std::lock_guard lock(pipeline_mutex_);
  if (pipelines_pending_.count(key)) return true;
  EnsureWorkers();
  pipelines_pending_[key] = true;
  pipeline_jobs_.push_back({key, std::move(desc), framebuffer});
  pipeline_cv_.notify_one();
  ++stats_.prewarm_queued;
  return true;
}

bool GameRenderer::SetPipelineCache(const std::string& path) {
  std::lock_guard lock(cache_mutex_);
  const bool opened = cache_file_.Open(path);
  cache_recording_.store(opened, std::memory_order_relaxed);
  if (!cache_file_.note().empty()) {
    Logf(LogLevel::kWarning, "rexgpu-native: pipeline cache %s ignored and started again: %s", path.c_str(),
         cache_file_.note().c_str());
  }
  const bool can_prewarm = options_.async_pipelines && device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  if (can_prewarm) {
    for (const PipelineRecord& record : cache_file_.loaded()) {
      if (!QueuePrewarm(record)) {
        deferred_.push_back(record);
        ++stats_.prewarm_deferred;
      }
    }
    deferred_shader_count_ = shaders_->created_count();
  }
  Logf(LogLevel::kInfo, "rexgpu-native: pipeline cache %s: %zu descriptions, %llu queued, %llu waiting for their shaders%s",
       path.c_str(), cache_file_.loaded().size(), static_cast<unsigned long long>(stats_.prewarm_queued),
       static_cast<unsigned long long>(stats_.prewarm_deferred), can_prewarm ? "" : " (no prewarm: needs Vulkan with async pipelines)");
  return opened;
}

void GameRenderer::RetryDeferred() {
  std::lock_guard lock(cache_mutex_);
  if (deferred_.empty()) return;
  const uint64_t created = shaders_->created_count();
  if (created == deferred_shader_count_) return;  // no new shader since the last try
  deferred_shader_count_ = created;
  std::vector<PipelineRecord> still;
  for (const PipelineRecord& record : deferred_) {
    if (!QueuePrewarm(record)) still.push_back(record);
  }
  deferred_ = std::move(still);
}

void GameRenderer::WaitForPrewarm() {
  std::unique_lock lock(pipeline_mutex_);
  pipeline_done_cv_.wait(lock, [&] { return pipeline_stop_ || (pipeline_jobs_.empty() && pipelines_active_ == 0); });
}

// ---------------------------------------------------------------- viewport

GameRenderer::ViewportSetup GameRenderer::ComputeViewport(const dev::DeviceView& d, uint32_t width,
                                                          uint32_t height) const {
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
  v.viewport = nvrhi::Viewport(0.0f, w, 0.0f, h, 0.0f, 1.0f);
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
  v.scissor = nvrhi::Rect(x0, std::max(x0, x1), y0, std::max(y0, y1));
  return v;
}

// ---------------------------------------------------------------- draw

// The pipeline description for a record (pipeline_cache.h): the same in a draw and in the prewarm.
void GameRenderer::BuildPipelineDesc(const PipelineRecord& r, nvrhi::IShader* vs, nvrhi::IShader* ps,
                                     nvrhi::GraphicsPipelineDesc& pd, nvrhi::FramebufferInfo& fb) const {
  pd.primType = nvrhi::PrimitiveType(r.topology);
  pd.VS = vs;
  pd.PS = ps;
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
  if (r.flags & 1) rs.frontCounterClockwise = !rs.frontCounterClockwise;
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
  const bool dump = Dump(call.frame);
  dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
  if (!d.valid()) {
    ++stats_.skipped_device;
    return;
  }
  ProcessInvalidations();

  std::shared_ptr<const GameShader> vs = shaders_->Get(kkshaders::ShaderKind::Vertex, call.vertex_shader);
  std::shared_ptr<const GameShader> ps;
  if (call.pixel_shader) ps = shaders_->Get(kkshaders::ShaderKind::Pixel, call.pixel_shader);
  if (!vs || (call.pixel_shader && !ps)) {
    ++stats_.skipped_shader;
    if (dump) {
      Logf(LogLevel::kInfo, "rexgpu-native: draw %u skipped: shader vs %08X (%s) ps %08X (%s)",
           call.index_in_frame, call.vertex_shader, vs ? "ok" : "missing", call.pixel_shader,
           call.pixel_shader ? (ps ? "ok" : "missing") : "none");
    }
    return;
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
  record.flags = options_.flip_front_face ? 1u : 0u;
  nvrhi::GraphicsPipelineDesc pd;
  nvrhi::FramebufferInfo framebuffer_info;
  BuildPipelineDesc(record, vs->handle, ps ? ps->handle.Get() : nullptr, pd, framebuffer_info);
  const uint64_t key = PipelineKey(record);
  NoteRecord(record, key);
  if (frame_log_ && frame_log_->enabled()) LogDraw(call, d, *vs, ps.get());
  if (dump) DumpVertexShaderHashes(call, d, *vs);
  nvrhi::IGraphicsPipeline* pipeline = GetPipeline(pd, framebuffer_info, key, true);
  if (!pipeline) {
    const bool pending = IsPending(key);
    if (!pending) ++stats_.skipped_pipeline;
    if (frame_log_ && frame_log_->active()) frame_log_->DrawNotDrawn(pending);
    return;
  }

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
  auto bind_textures = [&](const GameShader& shader) {
    for (const kkshaders::TextureUse& t : shader.bindings.textures) {
      uint32_t words[6];
      d.fetch_constant(t.slot & 31, words);
      dc.textureIndex[t.slot & 31] =
          BindTexture(cl, words, t.dimension, (base_map_slots >> (t.slot & 31)) & 1);
      if (dump) {
        Logf(LogLevel::kInfo, "rexgpu-native:   %s texture slot %u dim %u fetch %08X %08X %08X %08X %08X %08X -> %08X",
             &shader == vs.get() ? "vs" : "ps", t.slot, uint32_t(t.dimension), words[0], words[1], words[2],
             words[3], words[4], words[5], dc.textureIndex[t.slot & 31]);
      }
    }
  };
  bind_textures(*vs);
  if (ps) bind_textures(*ps);
  for (size_t i = 0; i < vs->bindings.samplers.size() && i < 32; ++i) {
    uint32_t words[6];
    d.fetch_constant(vs->bindings.samplers[i].slot & 31, words);
    dc.vertexSamplers[i] = BindSampler(words, vs->bindings.samplers[i]);
  }
  if (ps) {
    for (size_t i = 0; i < ps->bindings.samplers.size() && i < 32; ++i) {
      uint32_t words[6];
      d.fetch_constant(ps->bindings.samplers[i].slot & 31, words);
      dc.pixelSamplers[i] = BindSampler(words, ps->bindings.samplers[i]);
    }
  }

  // Vertex fetch: each binding's declaration element, from its stream.
  std::vector<kknr::VertexElement> elements;
  if (const uint8_t* decl = call.vertex_declaration ? memory_.Virtual(call.vertex_declaration) : nullptr)
    elements = kknr::DecodeVertexDeclaration(decl);
  for (size_t b = 0; b < vs->bindings.vertexBindings.size() && b < 16; ++b) {
    const kkshaders::VertexBinding& vb = vs->bindings.vertexBindings[b];
    uint32_t stream = 0;
    const kknr::VertexElement* element = nullptr;
    if (vb.raw) {
      if (vb.fetchConstant < 80) continue;
      stream = 95 - vb.fetchConstant;
    } else {
      for (const kknr::VertexElement& e : elements) {
        if (e.usage == uint8_t(vb.usage) && e.usage_index == vb.usageIndex) {
          element = &e;
          break;
        }
      }
      if (!element) continue;  // format 0: reads (0, 0, 0, 1)
      stream = element->stream;
    }
    if (stream >= 16) continue;
    const uint32_t vb_object = d.u32(dev::kStreamBuffer + 8 * stream);
    const uint32_t offset = d.u32(dev::kStreamOffset + 8 * stream);
    const uint32_t stride = uint32_t(*d.at(12688 + stream)) * 4;
    VertexBufferInfo info;
    if (!vb_object || !DecodeVertexBuffer(memory_.Virtual(vb_object), info)) continue;
    HostBuffer* host = BindVertexBuffer(cl, info.physical, info.size_bytes);
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

  // Clip planes (PA_CL_CLIP_CNTL ucp_ena, unless clipping is off).
  if (!(clip_cntl & (1u << 16))) dc.clipPlaneMask = clip_cntl & 0x3F;
  for (uint32_t p = 0; p < 6; ++p)
    for (uint32_t c = 0; c < 4; ++c) dc.clipPlanes[p][c] = d.f32(dev::kClipPlanes + p * 16 + c * 4);
  const HostTarget* size_from = colors[0] ? colors[0] : (depth ? depth : nullptr);
  for (HostTarget* c : colors)
    if (!size_from && c) size_from = c;
  const ViewportSetup vp = ComputeViewport(d, size_from->width, size_from->height);
  for (uint32_t c = 0; c < 3; ++c) {
    dc.ndcScale[c] = vp.ndc_scale[c];
    dc.ndcOffset[c] = vp.ndc_offset[c];
  }
  const uint32_t color_control = d.u32(dev::kRbColorControl);
  if (color_control & 8) {
    dc.alphaFunc = (color_control & 7) + 1;
    dc.alphaRef = d.f32(dev::kRbAlphaRef);
  }

  // Constants: the device's shadows, byte-swapped.
  std::array<float, 1024> vsc, psc;
  for (uint32_t i = 0; i < 1024; ++i) {
    vsc[i] = LoadBEFloat(d.at(dev::kVsConstants + i * 4));
    psc[i] = LoadBEFloat(d.at(dev::kPsConstants + i * 4));
  }
  // Volatile buffers must be written in every command list that binds them
  // (one per frame; the same object is reopened, hence the frame number).
  const bool new_list = !constants_valid_ || constants_list_ != cl || constants_frame_ != frame_;
  if (new_list || std::memcmp(vsc.data(), last_vs_.data(), sizeof(vsc)) != 0) {
    cl->writeBuffer(vs_constants_, vsc.data(), sizeof(vsc));
    last_vs_ = vsc;
  }
  if (new_list || std::memcmp(psc.data(), last_ps_.data(), sizeof(psc)) != 0) {
    cl->writeBuffer(ps_constants_, psc.data(), sizeof(psc));
    last_ps_ = psc;
  }
  constants_valid_ = true;
  constants_list_ = cl;
  constants_frame_ = frame_;
  cl->writeBuffer(draw_constants_, &dc, sizeof(dc));

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

  cl->setGraphicsState(gs);
  if (indexed_draw) {
    cl->drawIndexed(args);
  } else {
    cl->draw(args);
  }
  ++stats_.draws;
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
