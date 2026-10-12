#include "backend/output_blit.h"

#include <cstdint>

#include "backend/shaders.h"

namespace nr {

namespace {

// shaders/blit.hlsl's constants: the source rectangle, the channel mapping, flags (0: a plain copy).
struct Constants {
  int32_t src_rect[4];
  uint32_t channels[4];
  uint32_t flags[4];
};
static_assert(sizeof(Constants) == 48);

}  // namespace

bool OutputBlit::Initialize() {
  if (tried_) return layout_ != nullptr;
  tried_ = true;
  const nvrhi::GraphicsAPI api = device_->getGraphicsAPI();
  auto make_shader = [&](ShaderId id, nvrhi::ShaderType type) -> nvrhi::ShaderHandle {
    ShaderBytecode code = GetShaderBytecode(api, id);
    if (!code.data) return nullptr;
    nvrhi::ShaderDesc desc;
    desc.shaderType = type;
    desc.entryName = type == nvrhi::ShaderType::Vertex ? "vs_main" : "ps_main";
    return device_->createShader(desc, code.data, code.size);
  };
  vs_ = make_shader(ShaderId::kBlitVs, nvrhi::ShaderType::Vertex);
  ps_ = make_shader(ShaderId::kBlitPs, nvrhi::ShaderType::Pixel);
  if (!vs_ || !ps_) return false;
  nvrhi::BindingLayoutDesc bl;
  bl.visibility = nvrhi::ShaderType::All;
  bl.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)),
                 nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1)};
  layout_ = device_->createBindingLayout(bl);
  return layout_ != nullptr;
}

void OutputBlit::Forget(nvrhi::ITexture* target) { framebuffers_.erase(target); }

bool OutputBlit::Record(nvrhi::ICommandList* cl, nvrhi::ITexture* source, nvrhi::ITexture* target, uint32_t width,
                        uint32_t height) {
  if (!source || !target || !width || !height || !Initialize()) return false;
  nvrhi::IFramebuffer* fb = nullptr;
  if (auto it = framebuffers_.find(target); it != framebuffers_.end()) {
    fb = it->second;
  } else {
    nvrhi::FramebufferDesc fd;
    fd.addColorAttachment(target);
    nvrhi::FramebufferHandle handle = device_->createFramebuffer(fd);
    if (!handle) return false;
    framebuffers_[target] = handle;
    fb = handle;
  }
  const nvrhi::Format format = target->getDesc().format;
  nvrhi::IGraphicsPipeline* pipeline = nullptr;
  if (auto it = pipelines_.find(format); it != pipelines_.end()) {
    pipeline = it->second;
  } else {
    nvrhi::GraphicsPipelineDesc pd;
    pd.primType = nvrhi::PrimitiveType::TriangleList;
    pd.VS = vs_;
    pd.PS = ps_;
    pd.bindingLayouts = {layout_};
    pd.renderState.depthStencilState.depthTestEnable = false;
    pd.renderState.depthStencilState.depthWriteEnable = false;
    pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    nvrhi::GraphicsPipelineHandle handle = device_->createGraphicsPipeline(pd, fb);
    if (!handle) return false;
    pipelines_[format] = handle;
    pipeline = handle;
  }
  nvrhi::IBindingSet* set = nullptr;
  if (auto it = sets_.find(source); it != sets_.end()) {
    set = it->second;
  } else {
    // The ramp slot is not read without the gamma flag; the source fills it.
    nvrhi::BindingSetDesc bs;
    bs.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants)),
                   nvrhi::BindingSetItem::Texture_SRV(0, source, nvrhi::Format::UNKNOWN,
                                                      nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   nvrhi::BindingSetItem::Texture_SRV(1, source, nvrhi::Format::UNKNOWN,
                                                      nvrhi::TextureSubresourceSet(0, 1, 0, 1))};
    nvrhi::BindingSetHandle handle = device_->createBindingSet(bs, layout_);
    if (!handle) return false;
    // The renderer's frame images live as long as the renderer: a handful of sources at most.
    if (sets_.size() > 8) sets_.clear();
    sets_[source] = handle;
    set = handle;
  }
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {set};
  gs.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(width), float(height)));
  cl->setGraphicsState(gs);
  const Constants c = {{0, 0, int32_t(width), int32_t(height)}, {0, 1, 2, 3}, {0, 0, 0, 0}};
  cl->setPushConstants(&c, sizeof(c));
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
  // Close the render pass: the caller's own barriers follow.
  cl->clearState();
  return true;
}

}  // namespace nr
