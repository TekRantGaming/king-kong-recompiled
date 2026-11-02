// The red band on human skin: see skin_band.h.

#include "backend/skin_band.h"

#include <algorithm>
#include <cstring>

#include "backend/log.h"
#include "backend/shaders/generated/skin_band_ps_dxil.h"
#include "backend/shaders/generated/skin_band_ps_spirv.h"
#include "backend/shaders/generated/skin_band_vs_dxil.h"
#include "backend/shaders/generated/skin_band_vs_spirv.h"

namespace nr {

namespace {
struct Constants {
  float size[4] = {};   // the picture's width and height, how far to look (pixels), unused
  float tune[4] = {};   // the band's strength, the contrast for the full band, unused, unused
};
}  // namespace

bool SkinTexture(uint64_t base_hash) {
  // From documentary/sss (REPORT-089): Ann; Jack's first-person arms and body; three men.
  static constexpr uint64_t kKeys[] = {
      UINT64_C(0x4B3484FFDEF93A7E), UINT64_C(0x58100ABDE36DDA40), UINT64_C(0x2E68B518ADE08109),
      UINT64_C(0x9D51BC05102C9EA7), UINT64_C(0xBDD56166B78A8A59),
  };
  return std::find(std::begin(kKeys), std::end(kKeys), base_hash) != std::end(kKeys);
}

struct SkinBand::Target {
  nvrhi::TextureHandle skin, copy;
  nvrhi::BindingSetHandle set;
  std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> framebuffers;  // by depth
};

SkinBand::SkinBand(nvrhi::IDevice* device) : device_(device) {}
SkinBand::~SkinBand() = default;

bool SkinBand::Initialize() {
  const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  nvrhi::ShaderDesc vd;
  vd.shaderType = nvrhi::ShaderType::Vertex;
  vd.entryName = "vs_main";
  vs_ = spirv ? device_->createShader(vd, k_skin_band_vs_spirv, sizeof(k_skin_band_vs_spirv))
              : device_->createShader(vd, k_skin_band_vs_dxil, sizeof(k_skin_band_vs_dxil));
  nvrhi::ShaderDesc pd;
  pd.shaderType = nvrhi::ShaderType::Pixel;
  pd.entryName = "ps_main";
  ps_ = spirv ? device_->createShader(pd, k_skin_band_ps_spirv, sizeof(k_skin_band_ps_spirv))
              : device_->createShader(pd, k_skin_band_ps_dxil, sizeof(k_skin_band_ps_dxil));
  if (!vs_ || !ps_) return false;
  using I = nvrhi::BindingLayoutItem;
  nvrhi::BindingLayoutDesc bl;
  bl.visibility = nvrhi::ShaderType::All;
  bl.bindings = {I::VolatileConstantBuffer(0), I::Texture_SRV(0), I::Texture_SRV(1)};
  layout_ = device_->createBindingLayout(bl);
  nvrhi::BufferDesc bd;
  bd.byteSize = sizeof(Constants);
  bd.isConstantBuffer = true;
  bd.isVolatile = true;
  bd.maxVersions = 64;  // two writes a frame
  bd.debugName = "Skin band constants";
  constants_ = device_->createBuffer(bd);
  bool ready = layout_ && constants_;
  if (ready) {
    // The scene colour formats the game's targets use (made now, not in the middle of play).
    for (const auto format : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::R10G10B10A2_UNORM, nvrhi::Format::RGBA16_FLOAT}) {
      nvrhi::FramebufferInfo info;
      info.colorFormats = {format};
      ready = Pipeline(info) && ready;
    }
  }
  if (ready) Logf(LogLevel::kInfo, "rexgpu-native: skin band: ready");
  return ready;
}

void SkinBand::ForgetTargets() {
  targets_.clear();
  framebuffers_.clear();
}

SkinBand::Target* SkinBand::TargetFor(nvrhi::ITexture* color) {
  const nvrhi::TextureDesc& cd = color->getDesc();
  const uint64_t key = uint64_t(cd.width) << 32 | uint64_t(cd.height) << 8 | uint64_t(cd.format);
  auto& t = targets_[key];
  if (t) return t.get();
  if (targets_.size() > 4) {
    targets_.clear();
    return TargetFor(color);
  }
  auto made = std::make_unique<Target>();
  nvrhi::TextureDesc td;
  td.width = cd.width;
  td.height = cd.height;
  td.format = nvrhi::Format::RGBA8_UNORM;
  td.isRenderTarget = true;
  td.useClearValue = true;
  td.clearValue = nvrhi::Color(0.0f);
  td.initialState = nvrhi::ResourceStates::ShaderResource;
  td.keepInitialState = true;
  td.debugName = "Skin band skin colour";
  made->skin = device_->createTexture(td);
  nvrhi::TextureDesc cdesc;
  cdesc.width = cd.width;
  cdesc.height = cd.height;
  cdesc.format = cd.format;
  cdesc.initialState = nvrhi::ResourceStates::ShaderResource;
  cdesc.keepInitialState = true;
  cdesc.debugName = "Skin band scene colour";
  made->copy = device_->createTexture(cdesc);
  if (!made->skin || !made->copy) {
    targets_.erase(key);
    return nullptr;
  }
  using S = nvrhi::BindingSetItem;
  nvrhi::BindingSetDesc bs;
  bs.bindings = {S::ConstantBuffer(0, constants_), S::Texture_SRV(0, made->copy), S::Texture_SRV(1, made->skin)};
  made->set = device_->createBindingSet(bs, layout_);
  if (!made->set) {
    targets_.erase(key);
    return nullptr;
  }
  t = std::move(made);
  return t.get();
}

nvrhi::IFramebuffer* SkinBand::MaskFramebuffer(nvrhi::ICommandList* cl, const nvrhi::FramebufferDesc& game, bool first) {
  nvrhi::ITexture* color = game.colorAttachments.empty() ? nullptr : game.colorAttachments[0].texture;
  nvrhi::ITexture* depth = game.depthAttachment.texture;
  if (!color || !depth) return nullptr;
  Target* t = TargetFor(color);
  if (!t) return nullptr;
  if (first) cl->clearTextureFloat(t->skin, nvrhi::AllSubresources, nvrhi::Color(0.0f));
  if (t->framebuffers.size() > 8) t->framebuffers.clear();
  nvrhi::FramebufferHandle& fb = t->framebuffers[depth];
  if (!fb) {
    nvrhi::FramebufferDesc fd;
    fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(t->skin));
    nvrhi::FramebufferAttachment d = game.depthAttachment;
    d.isReadOnly = true;
    fd.setDepthAttachment(d);
    fb = device_->createFramebuffer(fd);
  }
  return fb;
}

nvrhi::IFramebuffer* SkinBand::Framebuffer(nvrhi::ITexture* color) {
  auto it = framebuffers_.find(color);
  if (it != framebuffers_.end()) return it->second;
  if (framebuffers_.size() > 16) framebuffers_.clear();
  nvrhi::FramebufferDesc fd;
  fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(color));
  nvrhi::FramebufferHandle fb = device_->createFramebuffer(fd);
  framebuffers_[color] = fb;
  return fb;
}

nvrhi::IGraphicsPipeline* SkinBand::Pipeline(const nvrhi::FramebufferInfo& info) {
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

bool SkinBand::Apply(nvrhi::ICommandList* cl, nvrhi::ITexture* color) {
  if (!color) return false;
  Target* t = TargetFor(color);
  nvrhi::IFramebuffer* fb = t ? Framebuffer(color) : nullptr;
  nvrhi::IGraphicsPipeline* pipeline = fb ? Pipeline(fb->getFramebufferInfo()) : nullptr;
  if (!pipeline) return false;
  const nvrhi::TextureDesc& cd = color->getDesc();
  if (timer_) timer_->Begin(cl);
  const nvrhi::TextureSlice slice = nvrhi::TextureSlice().resolve(cd);
  cl->copyTexture(t->copy, slice, color, slice);
  Constants k;
  k.size[0] = float(cd.width);
  k.size[1] = float(cd.height);
  k.size[2] = float(cd.height) * SkinBandTuning::kReach;
  k.tune[0] = SkinBandTuning::kStrength;
  k.tune[1] = SkinBandTuning::kContrast;
  cl->writeBuffer(constants_, &k, sizeof(k));
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {t->set};
  gs.viewport.addViewport(nvrhi::Viewport(float(cd.width), float(cd.height)));
  gs.viewport.addScissorRect(nvrhi::Rect(int(cd.width), int(cd.height)));
  cl->setGraphicsState(gs);
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
  if (timer_) timer_->End(cl);
  return true;
}

}  // namespace nr
