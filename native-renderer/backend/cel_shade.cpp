// Cel shading's pass: see cel_shade.h.

#include "backend/cel_shade.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "thirdparty/XeGTAO/XeGTAO.h"

#include "backend/log.h"
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
  float levels[4] = {};             // level, top level
};

namespace {

static_assert(sizeof(XeGTAO::GTAOConstants) == 96, "GTAOConstants is 6 float4");
constexpr size_t kGtaoShared = 96 + 16 + 16;

nvrhi::ShaderHandle MakeShader(nvrhi::IDevice* device, bool pixel) {
  const bool spirv = device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  const void* data = pixel ? (spirv ? static_cast<const void*>(k_cel_ps_spirv) : k_cel_ps_dxil)
                           : (spirv ? static_cast<const void*>(k_cel_vs_spirv) : k_cel_vs_dxil);
  const size_t size = pixel ? (spirv ? sizeof(k_cel_ps_spirv) : sizeof(k_cel_ps_dxil))
                            : (spirv ? sizeof(k_cel_vs_spirv) : sizeof(k_cel_vs_dxil));
  nvrhi::ShaderDesc desc;
  desc.shaderType = pixel ? nvrhi::ShaderType::Pixel : nvrhi::ShaderType::Vertex;
  desc.entryName = pixel ? "ps_cel" : "vs_main";
  return device->createShader(desc, data, size);
}

}  // namespace

CelShade::CelShade(nvrhi::IDevice* device) : device_(device) {}
CelShade::~CelShade() = default;

bool CelShade::Initialize() {
  vs_ = MakeShader(device_, false);
  ps_ = MakeShader(device_, true);
  if (!vs_ || !ps_) {
    Logf(LogLevel::kWarning, "rexgpu-native: cel shading: its shaders could not be created, it is unavailable");
    return false;
  }
  using I = nvrhi::BindingLayoutItem;
  nvrhi::BindingLayoutDesc bl;
  bl.visibility = nvrhi::ShaderType::All;
  bl.bindings = {I::VolatileConstantBuffer(0), I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2)};
  layout_ = device_->createBindingLayout(bl);
  nvrhi::BufferDesc bd;
  bd.byteSize = sizeof(Constants);
  bd.isConstantBuffer = true;
  bd.isVolatile = true;
  bd.maxVersions = 64;  // one write a frame
  bd.debugName = "Cel shading constants";
  constants_ = device_->createBuffer(bd);
  ready_ = layout_ && constants_;
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
                     const CelConfig& c) {
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
  k.levels[1] = CelTuning::kTopLevel;
  nvrhi::IFramebuffer* fb = Framebuffer(color);
  nvrhi::IGraphicsPipeline* pipeline = fb ? GraphicsPipeline(fb->getFramebufferInfo()) : nullptr;
  if (!pipeline) return false;
  const nvrhi::TextureSlice slice = nvrhi::TextureSlice().resolve(cd);
  cl->copyTexture(copy_, slice, color, slice);
  cl->writeBuffer(constants_, &k, sizeof(k));
  if (sets_.size() > 16) sets_.clear();
  nvrhi::BindingSetHandle& set = sets_[{depth, gtao.normals()}];
  if (!set) {
    using S = nvrhi::BindingSetItem;
    nvrhi::BindingSetDesc bs;
    bs.bindings = {S::ConstantBuffer(0, constants_), S::Texture_SRV(0, copy_),
                   S::Texture_SRV(1, depth, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   S::Texture_SRV(2, gtao.normals())};
    set = device_->createBindingSet(bs, layout_);
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
  ++stats_.applied;
  return true;
}

}  // namespace nr
