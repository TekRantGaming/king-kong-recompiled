// Depth of field on the first-person hands: see hands_dof.h.

#include "backend/hands_dof.h"

#include "backend/log.h"
#include "backend/shaders/generated/hands_dof_ps_dxil.h"
#include "backend/shaders/generated/hands_dof_ps_spirv.h"
#include "backend/shaders/generated/hands_dof_vs_dxil.h"
#include "backend/shaders/generated/hands_dof_vs_spirv.h"

namespace nr {

namespace {
struct Constants {
  float size[4] = {};  // the picture's width and height, the largest radius (pixels), unused
};
}  // namespace

bool HandsDof::Initialize() {
  const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  nvrhi::ShaderDesc vd;
  vd.shaderType = nvrhi::ShaderType::Vertex;
  vd.entryName = "vs_main";
  vs_ = spirv ? device_->createShader(vd, k_hands_dof_vs_spirv, sizeof(k_hands_dof_vs_spirv))
              : device_->createShader(vd, k_hands_dof_vs_dxil, sizeof(k_hands_dof_vs_dxil));
  nvrhi::ShaderDesc pd;
  pd.shaderType = nvrhi::ShaderType::Pixel;
  pd.entryName = "ps_main";
  ps_ = spirv ? device_->createShader(pd, k_hands_dof_ps_spirv, sizeof(k_hands_dof_ps_spirv))
              : device_->createShader(pd, k_hands_dof_ps_dxil, sizeof(k_hands_dof_ps_dxil));
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
  bd.maxVersions = 64;  // one write a frame
  bd.debugName = "Hands depth of field constants";
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
  if (ready) Logf(LogLevel::kInfo, "rexgpu-native: hands depth of field: ready");
  return ready;
}

void HandsDof::ForgetTargets() {
  set_ = nullptr;
  mask_framebuffers_.clear();
  framebuffers_.clear();
}

nvrhi::IFramebuffer* HandsDof::MaskFramebuffer(nvrhi::ICommandList* cl, const nvrhi::FramebufferDesc& game, bool first_this_frame) {
  nvrhi::ITexture* color = game.colorAttachments.empty() ? nullptr : game.colorAttachments[0].texture;
  nvrhi::ITexture* depth = game.depthAttachment.texture;
  if (!color || !depth) return nullptr;
  const nvrhi::TextureDesc& cd = color->getDesc();
  if (!mask_ || mask_->getDesc().width != cd.width || mask_->getDesc().height != cd.height) {
    ForgetTargets();
    nvrhi::TextureDesc td;
    td.width = cd.width;
    td.height = cd.height;
    td.format = nvrhi::Format::R8_UNORM;
    td.isRenderTarget = true;
    td.useClearValue = true;
    td.clearValue = nvrhi::Color(0.0f);
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "Hands depth of field mask";
    mask_ = device_->createTexture(td);
    if (!mask_) return nullptr;
  }
  if (first_this_frame) cl->clearTextureFloat(mask_, nvrhi::AllSubresources, nvrhi::Color(0.0f));
  nvrhi::FramebufferHandle& fb = mask_framebuffers_[depth];
  if (!fb) {
    if (mask_framebuffers_.size() > 8) {
      mask_framebuffers_.clear();
      return MaskFramebuffer(cl, game, false);
    }
    nvrhi::FramebufferDesc fd;
    fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(mask_));
    nvrhi::FramebufferAttachment d = game.depthAttachment;
    d.isReadOnly = true;
    fd.setDepthAttachment(d);
    fb = device_->createFramebuffer(fd);
  }
  return fb;
}

nvrhi::IFramebuffer* HandsDof::Framebuffer(nvrhi::ITexture* color) {
  auto it = framebuffers_.find(color);
  if (it != framebuffers_.end()) return it->second;
  if (framebuffers_.size() > 16) framebuffers_.clear();
  nvrhi::FramebufferDesc fd;
  fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(color));
  nvrhi::FramebufferHandle fb = device_->createFramebuffer(fd);
  framebuffers_[color] = fb;
  return fb;
}

nvrhi::IGraphicsPipeline* HandsDof::Pipeline(const nvrhi::FramebufferInfo& info) {
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

bool HandsDof::Apply(nvrhi::ICommandList* cl, nvrhi::ITexture* color) {
  if (!color || !mask_) return false;
  const nvrhi::TextureDesc& cd = color->getDesc();
  if (cd.width != mask_->getDesc().width || cd.height != mask_->getDesc().height) return false;
  if (!copy_ || copy_->getDesc().width != cd.width || copy_->getDesc().height != cd.height ||
      copy_->getDesc().format != cd.format) {
    set_ = nullptr;
    nvrhi::TextureDesc td;
    td.width = cd.width;
    td.height = cd.height;
    td.format = cd.format;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "Hands depth of field colour";
    copy_ = device_->createTexture(td);
    if (!copy_) return false;
  }
  nvrhi::IFramebuffer* fb = Framebuffer(color);
  nvrhi::IGraphicsPipeline* pipeline = fb ? Pipeline(fb->getFramebufferInfo()) : nullptr;
  if (!pipeline) return false;
  if (!set_) {
    using S = nvrhi::BindingSetItem;
    nvrhi::BindingSetDesc bs;
    bs.bindings = {S::ConstantBuffer(0, constants_), S::Texture_SRV(0, copy_), S::Texture_SRV(1, mask_)};
    set_ = device_->createBindingSet(bs, layout_);
    if (!set_) return false;
  }
  if (timer_) timer_->Begin(cl);
  const nvrhi::TextureSlice slice = nvrhi::TextureSlice().resolve(cd);
  cl->copyTexture(copy_, slice, color, slice);
  Constants k;
  k.size[0] = float(cd.width);
  k.size[1] = float(cd.height);
  k.size[2] = float(cd.height) * kRadius;
  cl->writeBuffer(constants_, &k, sizeof(k));
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {set_};
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
