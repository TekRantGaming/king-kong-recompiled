// Cel shading's pass: see cel_shade.h.

#include "backend/cel_shade.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "thirdparty/XeGTAO/XeGTAO.h"

#include "backend/log.h"
#include "backend/shaders/generated/cel_exposure_cs_dxil.h"
#include "backend/shaders/generated/cel_exposure_cs_spirv.h"
#include "backend/shaders/generated/cel_ps_dxil.h"
#include "backend/shaders/generated/cel_ps_spirv.h"
#include "backend/shaders/generated/cel_vs_dxil.h"
#include "backend/shaders/generated/cel_vs_spirv.h"

namespace nr {

// shaders/cel.hlsl's CelConstants.
struct CelShade::Constants {
  uint8_t gtao[96 + 16 + 16] = {};  // XeGTAO's constants, the picture, the working size (gtao.hlsl)
  float params[4] = {};             // step, the sky's stored depth
  float outline[4] = {};            // radius, opacity, darkness, crease radius
  float lines[4] = {};              // depth start / full, cos crease start / full
  float fade[4] = {};               // fade start / end, crease weight, debug view
  float bands[4] = {};              // bands, strength, softness, stops
  float levels[4] = {};             // level, the new average's share, 1 to start again
  float anchor[4] = {};             // the bands' centre's limits, its value before a measurement
};

namespace {

static_assert(sizeof(XeGTAO::GTAOConstants) == 96, "GTAOConstants is 6 float4");
constexpr size_t kGtaoShared = 96 + 16 + 16;

nvrhi::ShaderHandle MakeShader(nvrhi::IDevice* device, nvrhi::ShaderType type) {
  const bool spirv = device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  nvrhi::ShaderDesc desc;
  desc.shaderType = type;
  if (type == nvrhi::ShaderType::Vertex) {
    desc.entryName = "vs_main";
    return spirv ? device->createShader(desc, k_cel_vs_spirv, sizeof(k_cel_vs_spirv))
                 : device->createShader(desc, k_cel_vs_dxil, sizeof(k_cel_vs_dxil));
  }
  if (type == nvrhi::ShaderType::Pixel) {
    desc.entryName = "ps_cel";
    return spirv ? device->createShader(desc, k_cel_ps_spirv, sizeof(k_cel_ps_spirv))
                 : device->createShader(desc, k_cel_ps_dxil, sizeof(k_cel_ps_dxil));
  }
  desc.entryName = "cs_exposure";
  return spirv ? device->createShader(desc, k_cel_exposure_cs_spirv, sizeof(k_cel_exposure_cs_spirv))
               : device->createShader(desc, k_cel_exposure_cs_dxil, sizeof(k_cel_exposure_cs_dxil));
}

}  // namespace

CelShade::CelShade(nvrhi::IDevice* device) : device_(device) {}
CelShade::~CelShade() = default;

bool CelShade::Initialize() {
  vs_ = MakeShader(device_, nvrhi::ShaderType::Vertex);
  ps_ = MakeShader(device_, nvrhi::ShaderType::Pixel);
  cs_ = MakeShader(device_, nvrhi::ShaderType::Compute);
  if (!vs_ || !ps_ || !cs_) {
    Logf(LogLevel::kWarning, "rexgpu-native: cel shading: its shaders could not be created, it is unavailable");
    return false;
  }
  using I = nvrhi::BindingLayoutItem;
  nvrhi::BindingLayoutDesc bl;
  bl.visibility = nvrhi::ShaderType::All;
  bl.bindings = {I::VolatileConstantBuffer(0), I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2), I::Texture_SRV(3)};
  layout_ = device_->createBindingLayout(bl);
  bl.bindings = {I::VolatileConstantBuffer(0), I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_UAV(0)};
  exposure_layout_ = device_->createBindingLayout(bl);
  nvrhi::ComputePipelineDesc cd;
  cd.CS = cs_;
  cd.bindingLayouts = {exposure_layout_};
  if (exposure_layout_) exposure_pipeline_ = device_->createComputePipeline(cd);
  // The followed average: R32F (every Vulkan device stores to it), the log2 mean then a "measured" flag.
  nvrhi::TextureDesc ed;
  ed.width = 2;
  ed.height = 1;
  ed.format = nvrhi::Format::R32_FLOAT;
  ed.isUAV = true;
  ed.initialState = nvrhi::ResourceStates::ShaderResource;
  ed.keepInitialState = true;
  ed.debugName = "Cel shading average";
  exposure_ = device_->createTexture(ed);
  nvrhi::BufferDesc bd;
  bd.byteSize = sizeof(Constants);
  bd.isConstantBuffer = true;
  bd.isVolatile = true;
  bd.maxVersions = 64;  // one write a frame
  bd.debugName = "Cel shading constants";
  constants_ = device_->createBuffer(bd);
  ready_ = layout_ && exposure_pipeline_ && exposure_ && constants_;
  if (ready_) {
    // The scene colour formats the game's targets use (made now, not at the first frame with it).
    for (const auto format : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::R10G10B10A2_UNORM,
                              nvrhi::Format::RGBA16_FLOAT}) {
      nvrhi::FramebufferInfo info;
      info.colorFormats = {format};
      ready_ = GraphicsPipeline(info) && ready_;
    }
  }
  return ready_;
}

nvrhi::IFramebuffer* CelShade::Framebuffer(nvrhi::ITexture* color) {
  auto it = framebuffers_.find(color);
  if (it != framebuffers_.end()) return it->second;
  if (framebuffers_.size() > 16) framebuffers_.clear();
  nvrhi::FramebufferDesc fd;
  fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(color));
  nvrhi::FramebufferHandle fb = device_->createFramebuffer(fd);
  framebuffers_[color] = fb;
  return fb;
}

nvrhi::IGraphicsPipeline* CelShade::GraphicsPipeline(const nvrhi::FramebufferInfo& info) {
  const uint32_t format = info.colorFormats.empty() ? 0 : uint32_t(info.colorFormats[0]);
  nvrhi::GraphicsPipelineHandle& p = pipelines_[format];
  if (p) return p;
  nvrhi::GraphicsPipelineDesc pd;
  pd.primType = nvrhi::PrimitiveType::TriangleList;
  pd.VS = vs_;
  pd.PS = ps_;
  pd.bindingLayouts = {layout_};
  pd.renderState.depthStencilState.depthTestEnable = false;
  pd.renderState.depthStencilState.depthWriteEnable = false;
  pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  // The alpha channel is the game's (later passes may read it).
  pd.renderState.blendState.targets[0].colorWriteMask =
      nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue;
  p = device_->createGraphicsPipeline(pd, info);
  return p;
}

bool CelShade::Apply(nvrhi::ICommandList* cl, const Gtao& gtao, nvrhi::ITexture* color, nvrhi::ITexture* depth,
                     const CelConfig& c, uint64_t frame) {
  if (!ready_ || !color || !depth || !gtao.normals() || gtao.constants_bytes().size() < kGtaoShared + 16) return false;
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
    td.debugName = "Cel shading scene colour";
    copy_ = device_->createTexture(td);
    if (!copy_) return false;
  }
  Constants k;
  const std::vector<uint8_t>& g = gtao.constants_bytes();
  std::memcpy(k.gtao, g.data(), kGtaoShared);
  float gtao_params[4];
  std::memcpy(gtao_params, g.data() + kGtaoShared, sizeof(gtao_params));
  k.params[0] = gtao_params[0];  // step
  k.params[1] = gtao_params[1];  // the sky's stored depth
  int32_t source[4];
  std::memcpy(source, k.gtao + 96, sizeof(source));
  const int32_t x0 = std::max(source[0], 0), y0 = std::max(source[1], 0);
  const int32_t x1 = std::min(source[0] + source[2], int32_t(cd.width));
  const int32_t y1 = std::min(source[1] + source[3], int32_t(cd.height));
  if (x1 <= x0 || y1 <= y0) return false;
  float opacity = 1.0f;
  const float radius = CelRadius(c.outline, uint32_t(source[3]), &opacity);
  const float rad = 3.14159265f / 180.0f;
  k.outline[0] = radius;
  k.outline[1] = c.outline > 0.0f ? opacity : 0.0f;
  k.outline[2] = std::clamp(c.darkness, 0.0f, 1.0f);
  k.outline[3] = std::max(1.0f, std::floor(radius * 0.5f + 0.5f));  // a crease marks both sides: half the radius
  k.lines[0] = CelTuning::kDepthStart;
  k.lines[1] = CelTuning::kDepthFull;
  k.lines[2] = std::cos(CelTuning::kCreaseStartDegrees * rad);
  k.lines[3] = std::cos(CelTuning::kCreaseFullDegrees * rad);
  k.fade[0] = CelTuning::kFadeStart;
  k.fade[1] = CelTuning::kFadeEnd;
  k.fade[2] = CelTuning::kCreaseWeight;
  k.fade[3] = float(std::clamp(c.debug, 0, 2));  // 1 the lines alone, 2 depth lines cyan and creases magenta
  k.bands[0] = c.banding() ? float(std::clamp(c.bands, 2u, 4u)) : 0.0f;
  k.bands[1] = std::clamp(c.strength, 0.0f, 1.0f);
  k.bands[2] = CelTuning::kSoftness;
  k.bands[3] = CelTuning::kStops;
  k.levels[0] = CelTuning::kLevel;
  k.levels[1] = CelTuning::kAnchorFollow;
  // A gap (the effect was off, a new run): the average starts again from this frame's.
  k.levels[2] = frame == UINT64_MAX || last_frame_ == UINT64_MAX || frame != last_frame_ + 1 ? 1.0f : 0.0f;
  last_frame_ = frame;
  k.anchor[0] = CelTuning::kAnchorMin;
  k.anchor[1] = CelTuning::kAnchorMax;
  k.anchor[2] = CelTuning::kAnchorDefault;
  k.anchor[3] = CelBandRadius(uint32_t(source[3]));
  nvrhi::IFramebuffer* fb = Framebuffer(color);
  nvrhi::IGraphicsPipeline* pipeline = fb ? GraphicsPipeline(fb->getFramebufferInfo()) : nullptr;
  if (!pipeline) return false;
  const nvrhi::TextureSlice slice = nvrhi::TextureSlice().resolve(cd);
  cl->copyTexture(copy_, slice, color, slice);
  cl->writeBuffer(constants_, &k, sizeof(k));
  if (sets_.size() > 16) sets_.clear();
  using S = nvrhi::BindingSetItem;
  const auto depth_srv = S::Texture_SRV(1, depth, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1));
  nvrhi::BindingSetHandle& set = sets_[{depth, gtao.normals()}];
  if (!set) {
    nvrhi::BindingSetDesc bs;
    bs.bindings = {S::ConstantBuffer(0, constants_), S::Texture_SRV(0, copy_), depth_srv, S::Texture_SRV(2, gtao.normals()),
                   S::Texture_SRV(3, exposure_)};
    set = device_->createBindingSet(bs, layout_);
  }
  nvrhi::BindingSetHandle& exposure_set = sets_[{depth, nullptr}];
  if (!exposure_set) {
    nvrhi::BindingSetDesc bs;
    bs.bindings = {S::ConstantBuffer(0, constants_), S::Texture_SRV(0, copy_), depth_srv, S::Texture_UAV(0, exposure_)};
    exposure_set = device_->createBindingSet(bs, exposure_layout_);
  }
  if (!set || !exposure_set) return false;
  // The frame's average first (one group), when there are bands.
  if (k.bands[0] > 0.0f) {
    nvrhi::ComputeState cs;
    cs.pipeline = exposure_pipeline_;
    cs.bindings = {exposure_set};
    cl->setComputeState(cs);
    cl->dispatch(1, 1, 1);
  } else {
    last_frame_ = UINT64_MAX;  // not followed this frame
  }
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
  ++stats_.applied;
  return true;
}

}  // namespace nr
