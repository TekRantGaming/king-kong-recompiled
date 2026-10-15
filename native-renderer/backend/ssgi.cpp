// Screen-space global illumination: see ssgi.h.

#include "backend/ssgi.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "thirdparty/XeGTAO/XeGTAO.h"

#include "backend/log.h"
#include "backend/shaders/generated/ssgi_composite_ps_dxil.h"
#include "backend/shaders/generated/ssgi_composite_ps_spirv.h"
#include "backend/shaders/generated/ssgi_denoise_cs_dxil.h"
#include "backend/shaders/generated/ssgi_denoise_cs_spirv.h"
#include "backend/shaders/generated/ssgi_gather_cs_dxil.h"
#include "backend/shaders/generated/ssgi_gather_cs_spirv.h"
#include "backend/shaders/generated/ssgi_radiance_cs_dxil.h"
#include "backend/shaders/generated/ssgi_radiance_cs_spirv.h"
#include "backend/shaders/generated/ssgi_radiance_down_cs_dxil.h"
#include "backend/shaders/generated/ssgi_radiance_down_cs_spirv.h"
#include "backend/shaders/generated/ssgi_temporal_cs_dxil.h"
#include "backend/shaders/generated/ssgi_temporal_cs_spirv.h"
#include "backend/shaders/generated/ssgi_vs_dxil.h"
#include "backend/shaders/generated/ssgi_vs_spirv.h"

namespace nr {

// shaders/ssgi.hlsl's SsgiConstants.
struct Ssgi::Constants {
  XeGTAO::GTAOConstants g = {};
  int32_t source[4] = {};
  uint32_t working[4] = {};
  float params[4] = {};   // step, sky depth, radius, thickness
  float gather[4] = {};   // slices, steps, largest radius on the screen (working pixels), intensity
  float ambient[4] = {};  // rgb ambient, w the albedo limit
  float mode[4] = {};     // occlusion, debug, radiance mips, denoise step
  float extra[4] = {};    // x: the AO's occlusion bound, y: noise index, z: history, w: the new light's weight
  float reproject[16] = {};
  float screen_xy[4] = {};
  float screen_z[4] = {};
  float shape[4] = {};    // multi-bounce strength, largest darkening share, gamma
};

namespace {

static_assert(sizeof(XeGTAO::GTAOConstants) == 96, "GTAOConstants is 6 float4");

enum BlobId { kRadiance, kRadianceDown, kGather, kDenoise, kTemporal, kVs, kComposite };

struct Blob {
  const void* data = nullptr;
  size_t size = 0;
};

Blob GetBlob(nvrhi::GraphicsAPI api, int id) {
  const bool spirv = api == nvrhi::GraphicsAPI::VULKAN;
#define NR_SSGI_BLOB(name) \
  (spirv ? Blob{k_##name##_spirv, sizeof(k_##name##_spirv)} : Blob{k_##name##_dxil, sizeof(k_##name##_dxil)})
  switch (id) {
    case kRadiance: return NR_SSGI_BLOB(ssgi_radiance_cs);
    case kRadianceDown: return NR_SSGI_BLOB(ssgi_radiance_down_cs);
    case kGather: return NR_SSGI_BLOB(ssgi_gather_cs);
    case kDenoise: return NR_SSGI_BLOB(ssgi_denoise_cs);
    case kTemporal: return NR_SSGI_BLOB(ssgi_temporal_cs);
    case kVs: return NR_SSGI_BLOB(ssgi_vs);
    case kComposite: return NR_SSGI_BLOB(ssgi_composite_ps);
    default: return {};
  }
#undef NR_SSGI_BLOB
}

nvrhi::ShaderHandle MakeShader(nvrhi::IDevice* device, int id, nvrhi::ShaderType type, const char* entry) {
  const Blob blob = GetBlob(device->getGraphicsAPI(), id);
  if (!blob.data || !blob.size) return nullptr;
  nvrhi::ShaderDesc desc;
  desc.shaderType = type;
  desc.entryName = entry;
  return device->createShader(desc, blob.data, blob.size);
}

enum SetKind { kSetRadiance, kSetComposite };
// fixed_sets_ (EnsureImages).
enum FixedSet { kSetGather, kSetDenoiseAB, kSetDenoiseBA, kSetTemporal, kSetDown };

// The part of shaders/gtao.hlsl's constants the GI reuses: XeGTAO's, the picture, the working size,
// then the step and the sky's stored depth.
constexpr size_t kGtaoShared = 96 + 16 + 16;

}  // namespace

Ssgi::Ssgi(nvrhi::IDevice* device) : device_(device) {}
Ssgi::~Ssgi() = default;

bool Ssgi::MakePass(Pass& pass, int id, const char* entry, const std::vector<nvrhi::BindingLayoutItem>& items) {
  pass.shader = MakeShader(device_, id, nvrhi::ShaderType::Compute, entry);
  if (!pass.shader) return false;
  nvrhi::BindingLayoutDesc bl;
  bl.visibility = nvrhi::ShaderType::All;
  bl.bindings = items;
  pass.layout = device_->createBindingLayout(bl);
  if (!pass.layout) return false;
  nvrhi::ComputePipelineDesc pd;
  pd.CS = pass.shader;
  pd.bindingLayouts = {pass.layout};
  pass.pipeline = device_->createComputePipeline(pd);
  return pass.pipeline != nullptr;
}

bool Ssgi::Initialize() {
  using I = nvrhi::BindingLayoutItem;
  const auto cb = I::VolatileConstantBuffer(0);
  const bool ok =
      MakePass(radiance_pass_, kRadiance, "cs_radiance", {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_UAV(0)}) &&
      MakePass(radiance_down_, kRadianceDown, "cs_radiance_down", {cb, I::Texture_SRV(0), I::Texture_UAV(0)}) &&
      MakePass(gather_, kGather, "cs_gather",
               {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2), I::Texture_SRV(3), I::Sampler(0), I::Sampler(1),
                I::Texture_UAV(0)}) &&
      MakePass(denoise_, kDenoise, "cs_denoise",
               {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2), I::Texture_SRV(3), I::Texture_UAV(0)}) &&
      MakePass(temporal_, kTemporal, "cs_temporal",
               {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2), I::Texture_SRV(3), I::Texture_UAV(0)});
  vs_ = MakeShader(device_, kVs, nvrhi::ShaderType::Vertex, "vs_main");
  composite_ps_ = MakeShader(device_, kComposite, nvrhi::ShaderType::Pixel, "ps_composite");
  if (!ok || !vs_ || !composite_ps_) {
    Logf(LogLevel::kWarning, "rexgpu-native: GI: its shaders could not be created, the GI is unavailable");
    return false;
  }
  nvrhi::BindingLayoutDesc cl;
  cl.visibility = nvrhi::ShaderType::All;
  cl.bindings = {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2), I::Texture_SRV(3), I::Texture_SRV(4)};
  composite_layout_ = device_->createBindingLayout(cl);
  nvrhi::SamplerDesc sd;
  sd.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
  point_clamp_ = device_->createSampler(sd);
  sd.setAllFilters(true);
  linear_clamp_ = device_->createSampler(sd);
  nvrhi::BufferDesc bd;
  bd.byteSize = sizeof(Constants);
  bd.isConstantBuffer = true;
  bd.isVolatile = true;
  bd.maxVersions = 1024;  // up to six writes a frame (the blur passes write their step)
  bd.debugName = "GI constants";
  constants_ = device_->createBuffer(bd);
  ready_ = composite_layout_ && point_clamp_ && linear_clamp_ && constants_;
  if (ready_) {
    for (const auto format : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::R10G10B10A2_UNORM,
                              nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RG16_FLOAT,
                              nvrhi::Format::R32_FLOAT, nvrhi::Format::RG32_FLOAT}) {
      nvrhi::FramebufferInfo info;
      info.colorFormats = {format};
      ready_ = GraphicsPipeline(info) && ready_;
    }
  }
  return ready_;
}

nvrhi::BindingSetHandle Ssgi::Set(Pass& pass, const std::vector<nvrhi::BindingSetItem>& items) {
  nvrhi::BindingSetDesc bs;
  bs.bindings = items;
  return device_->createBindingSet(bs, pass.layout);
}

void Ssgi::Dispatch(nvrhi::ICommandList* cl, Pass& pass, nvrhi::IBindingSet* set, uint32_t x, uint32_t y) {
  nvrhi::ComputeState cs;
  cs.pipeline = pass.pipeline;
  cs.bindings = {set};
  cl->setComputeState(cs);
  cl->dispatch(std::max(x, 1u), std::max(y, 1u), 1);
}

bool Ssgi::EnsureImages(const Gtao& gtao) {
  const uint32_t pw = gtao.padded_width(), ph = gtao.padded_height();
  if (pw == padded_w_ && ph == padded_h_ && images_of_ == &gtao && gtao_depth_ == gtao.view_depth() && gi_a_) return true;
  padded_w_ = padded_h_ = 0;
  fixed_sets_.clear();
  sets_.clear();
  auto make = [&](uint32_t mips, const char* name) {
    nvrhi::TextureDesc td;
    td.width = std::max(pw, 1u);
    td.height = std::max(ph, 1u);
    td.mipLevels = mips;
    td.format = nvrhi::Format::RGBA16_FLOAT;
    td.isUAV = true;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = name;
    return device_->createTexture(td);
  };
  history_valid_ = false;
  radiance_ = make(kRadianceMips, "GI radiance");
  gi_a_ = make(1, "GI term a");
  gi_b_ = make(1, "GI term b");
  history_ = make(1, "GI history");
  {
    nvrhi::TextureDesc td;
    td.width = std::max(pw, 1u);
    td.height = std::max(ph, 1u);
    td.format = nvrhi::Format::R32_FLOAT;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "GI history depth";
    history_depth_ = device_->createTexture(td);
  }
  if (!radiance_ || !gi_a_ || !gi_b_ || !history_ || !history_depth_ || !gtao.view_depth() || !gtao.normals() ||
      !gtao.raw_depth()) {
    gi_a_ = nullptr;
    return false;
  }
  using S = nvrhi::BindingSetItem;
  const auto cb = S::ConstantBuffer(0, constants_);
  const nvrhi::TextureSubresourceSet all_depth(0, XE_GTAO_DEPTH_MIP_LEVELS, 0, 1);
  const nvrhi::TextureSubresourceSet all_radiance(0, kRadianceMips, 0, 1);
  auto mip = [](uint32_t m) { return nvrhi::TextureSubresourceSet(m, 1, 0, 1); };
  nvrhi::ITexture* depth = gtao.view_depth();
  nvrhi::ITexture* normals = gtao.normals();
  nvrhi::ITexture* raw = gtao.raw_depth();
  // fixed_sets_: the gather, the two blur directions, the temporal pass, then one per radiance mip
  // below the first (kSetDown + mip - 1).
  fixed_sets_ = {
      Set(gather_, {cb, S::Texture_SRV(0, depth, nvrhi::Format::UNKNOWN, all_depth), S::Texture_SRV(1, normals),
                    S::Texture_SRV(2, radiance_, nvrhi::Format::UNKNOWN, all_radiance), S::Texture_SRV(3, raw),
                    S::Sampler(0, point_clamp_), S::Sampler(1, linear_clamp_), S::Texture_UAV(0, gi_a_)}),
      Set(denoise_, {cb, S::Texture_SRV(0, gi_a_), S::Texture_SRV(1, depth, nvrhi::Format::UNKNOWN, mip(0)),
                     S::Texture_SRV(2, normals), S::Texture_SRV(3, raw), S::Texture_UAV(0, gi_b_)}),
      Set(denoise_, {cb, S::Texture_SRV(0, gi_b_), S::Texture_SRV(1, depth, nvrhi::Format::UNKNOWN, mip(0)),
                     S::Texture_SRV(2, normals), S::Texture_SRV(3, raw), S::Texture_UAV(0, gi_a_)}),
      Set(temporal_, {cb, S::Texture_SRV(0, gi_a_), S::Texture_SRV(1, history_), S::Texture_SRV(2, history_depth_),
                      S::Texture_SRV(3, raw), S::Texture_UAV(0, gi_b_)}),
  };
  for (uint32_t m = 1; m < kRadianceMips; ++m)
    fixed_sets_.push_back(Set(radiance_down_, {cb, S::Texture_SRV(0, radiance_, nvrhi::Format::UNKNOWN, mip(m - 1)),
                                               S::Texture_UAV(0, radiance_, nvrhi::Format::UNKNOWN, mip(m))}));
  for (const nvrhi::BindingSetHandle& s : fixed_sets_) {
    if (!s) {
      gi_a_ = nullptr;
      return false;
    }
  }
  padded_w_ = pw;
  padded_h_ = ph;
  images_of_ = &gtao;
  gtao_depth_ = depth;
  return true;
}

void Ssgi::FillConstants(Constants& k, const Gtao& gtao, const SsgiSettings& s, float denoise_step) const {
  const std::vector<uint8_t>& g = gtao.constants_bytes();
  std::memcpy(&k, g.data(), kGtaoShared);
  float gtao_params[4];
  std::memcpy(gtao_params, g.data() + kGtaoShared, sizeof(gtao_params));
  k.params[0] = gtao_params[0];  // step
  k.params[1] = gtao_params[1];  // the sky's stored depth
  k.params[2] = std::max(s.radius, 1e-3f);
  k.params[3] = std::max(s.thickness, 1e-4f);
  k.gather[0] = float(std::max(s.level.slices, 1u));
  k.gather[1] = float(std::max(s.level.steps, 1u));
  // At most this share of the picture's height (working pixels): the radiance's mips and the
  // temporal accumulation keep a scene-scale gather smooth.
  k.gather[2] = std::max(8.0f, kMaxScreenRadius * float(k.working[1]));
  // Calibrate the screen-space bounce against a bright, already baked-lit receiver, rather
  // than the old nearly black yard. The visible screen is only part of the surroundings;
  // intensity 1 is the normal presentation strength, not a claim of complete physical GI.
  // Keep this in the composite: gathering/history and quality costs are unchanged.
  k.gather[3] = 3.0f * s.intensity;
  k.ambient[0] = s.ambient[0];
  k.ambient[1] = s.ambient[1];
  k.ambient[2] = s.ambient[2];
  // The albedo limit is given as a picture value (gamma); the composite works in linear light.
  k.ambient[3] = std::pow(std::clamp(s.albedo, 0.05f, 1.0f), kGamma);
  k.mode[0] = std::clamp(s.occlusion, 0.0f, 1.0f);
  k.mode[1] = float(s.debug);
  k.mode[2] = float(kRadianceMips);
  k.mode[3] = denoise_step;
  k.extra[0] = 0.0f;
  k.extra[1] = s.temporal ? float(s.frame % 64u) : 0.0f;
  k.extra[2] = s.temporal && s.history && history_valid_ ? 1.0f : 0.0f;
  k.extra[3] = std::clamp(s.new_weight, 0.02f, 1.0f);
  std::memcpy(k.reproject, s.reproject, sizeof(k.reproject));
  std::memcpy(k.screen_xy, s.screen_xy, sizeof(k.screen_xy));
  std::memcpy(k.screen_z, s.screen_z, sizeof(k.screen_z));
  k.shape[0] = std::clamp(s.bounces, 0.0f, 1.0f);
  k.shape[1] = std::clamp(s.darkest, 0.0f, 1.0f);
  k.shape[2] = kGamma;
}

bool Ssgi::Compute(nvrhi::ICommandList* cl, const Gtao& gtao, nvrhi::ITexture* color, nvrhi::ITexture* depth, const SsgiSettings& s) {
  result_ = nullptr;
  if (!ready_ || !color || !depth || gtao.constants_bytes().size() < kGtaoShared + 16 || !gtao.padded_width()) return false;
  if (!EnsureImages(gtao)) return false;
  const uint32_t pw = padded_w_, ph = padded_h_;
  Constants k;
  FillConstants(k, gtao, s, 1.0f);
  cl->writeBuffer(constants_, &k, sizeof(k));
  if (sets_.size() > 16) sets_.clear();
  using S = nvrhi::BindingSetItem;
  nvrhi::BindingSetHandle& radiance = sets_[{kSetRadiance, color, depth}];
  if (!radiance) {
    radiance = Set(radiance_pass_, {S::ConstantBuffer(0, constants_),
                               S::Texture_SRV(0, color, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                               S::Texture_SRV(1, depth, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                               S::Texture_UAV(0, radiance_, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1))});
  }
  if (!radiance) return false;
  Dispatch(cl, radiance_pass_, radiance, (pw + 7) / 8, (ph + 7) / 8);
  for (uint32_t m = 1; m < kRadianceMips; ++m) {
    const uint32_t mw = std::max(pw >> m, 1u), mh = std::max(ph >> m, 1u);
    Dispatch(cl, radiance_down_, fixed_sets_[kSetDown + m - 1], (mw + 7) / 8, (mh + 7) / 8);
  }
  Dispatch(cl, gather_, fixed_sets_[kSetGather], (pw + 7) / 8, (ph + 7) / 8);
  // Temporal accumulation: a into b with the history, b kept as the next frame's history and this
  // frame's view depth with it; the blur then starts from b.
  bool from_b = false;
  if (s.temporal) {
    Dispatch(cl, temporal_, fixed_sets_[kSetTemporal], (pw + 7) / 8, (ph + 7) / 8);
    const nvrhi::TextureSlice all;
    cl->copyTexture(history_, all, gi_b_, all);
    cl->copyTexture(history_depth_, all, gtao.view_depth(), nvrhi::TextureSlice().setMipLevel(0));
    history_valid_ = true;
    from_b = true;
  } else {
    history_valid_ = false;
  }
  // The blur: a to b, b to a, ... with steps 1, 2, 4 (the result ends where the last pass wrote).
  nvrhi::ITexture* last = from_b ? gi_b_.Get() : gi_a_.Get();
  for (uint32_t i = 0; i < s.level.denoise; ++i) {
    FillConstants(k, gtao, s, float(1u << i));
    cl->writeBuffer(constants_, &k, sizeof(k));
    const bool a_to_b = ((i & 1) == 0) != from_b;
    Dispatch(cl, denoise_, fixed_sets_[a_to_b ? kSetDenoiseAB : kSetDenoiseBA], (pw + 7) / 8, (ph + 7) / 8);
    last = a_to_b ? gi_b_.Get() : gi_a_.Get();
  }
  result_ = last;
  ++stats_.computed;
  return true;
}

nvrhi::IFramebuffer* Ssgi::Framebuffer(nvrhi::ITexture* color) {
  auto it = framebuffers_.find(color);
  if (it != framebuffers_.end()) return it->second;
  if (framebuffers_.size() > 16) framebuffers_.clear();
  nvrhi::FramebufferDesc fd;
  fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(color));
  nvrhi::FramebufferHandle fb = device_->createFramebuffer(fd);
  framebuffers_[color] = fb;
  return fb;
}

nvrhi::IGraphicsPipeline* Ssgi::GraphicsPipeline(nvrhi::IFramebuffer* fb) {
  return GraphicsPipeline(fb->getFramebufferInfo());
}
nvrhi::IGraphicsPipeline* Ssgi::GraphicsPipeline(const nvrhi::FramebufferInfo& info) {
  const uint32_t format = info.colorFormats.empty() ? 0 : uint32_t(info.colorFormats[0]);
  nvrhi::GraphicsPipelineHandle& p = pipelines_[format];
  if (p) return p;
  nvrhi::GraphicsPipelineDesc pd;
  pd.primType = nvrhi::PrimitiveType::TriangleList;
  pd.VS = vs_;
  pd.PS = composite_ps_;
  pd.bindingLayouts = {composite_layout_};
  pd.renderState.depthStencilState.depthTestEnable = false;
  pd.renderState.depthStencilState.depthWriteEnable = false;
  pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  // The alpha channel is the game's (later passes may read it).
  pd.renderState.blendState.targets[0].colorWriteMask =
      nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue;
  p = device_->createGraphicsPipeline(pd, info);
  return p;
}

bool Ssgi::Composite(nvrhi::ICommandList* cl, const Gtao& gtao, nvrhi::ITexture* color, nvrhi::ITexture* depth,
                     const SsgiSettings& s) {
  if (!ready_ || !result_ || !color || !depth || gtao.constants_bytes().size() < kGtaoShared + 16) return false;
  const nvrhi::TextureDesc& cd = color->getDesc();
  if (!copy_ || copy_->getDesc().width != cd.width || copy_->getDesc().height != cd.height ||
      copy_->getDesc().format != cd.format) {
    sets_.clear();
    nvrhi::TextureDesc td;
    td.width = cd.width;
    td.height = cd.height;
    td.format = cd.format;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "GI scene colour";
    copy_ = device_->createTexture(td);
    if (!copy_) return false;
  }
  // The picture in the target (host pixels).
  Constants k;
  FillConstants(k, gtao, s, 1.0f);
  const int32_t x0 = std::max(k.source[0], 0), y0 = std::max(k.source[1], 0);
  const int32_t x1 = std::min(k.source[0] + k.source[2], int32_t(cd.width));
  const int32_t y1 = std::min(k.source[1] + k.source[3], int32_t(cd.height));
  if (x1 <= x0 || y1 <= y0) return false;
  nvrhi::IFramebuffer* fb = Framebuffer(color);
  nvrhi::IGraphicsPipeline* pipeline = fb ? GraphicsPipeline(fb) : nullptr;
  if (!pipeline) return false;
  const nvrhi::TextureSlice slice = nvrhi::TextureSlice().resolve(cd);
  cl->copyTexture(copy_, slice, color, slice);
  cl->writeBuffer(constants_, &k, sizeof(k));
  if (sets_.size() > 16) sets_.clear();
  nvrhi::BindingSetHandle& set = sets_[{kSetComposite, result_, depth}];
  if (!set) {
    using S = nvrhi::BindingSetItem;
    nvrhi::BindingSetDesc bs;
    bs.bindings = {S::ConstantBuffer(0, constants_),
                   S::Texture_SRV(0, copy_),
                   S::Texture_SRV(1, result_),
                   S::Texture_SRV(2, gtao.raw_depth()),  // the AO's occlusion is not multiplied here (kk_Extra.x 0)
                   S::Texture_SRV(3, gtao.view_depth(), nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   S::Texture_SRV(4, depth, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1))};
    set = device_->createBindingSet(bs, composite_layout_);
  }
  if (!set) return false;
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {set};
  gs.viewport.addViewport(nvrhi::Viewport(float(x0), float(x1), float(y0), float(y1), 0.0f, 1.0f));
  gs.viewport.addScissorRect(nvrhi::Rect(x0, x1, y0, y1));
  cl->setGraphicsState(gs);
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
  ++stats_.composited;
  return true;
}

}  // namespace nr
