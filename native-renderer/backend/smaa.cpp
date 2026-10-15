#include "backend/smaa.h"
#include "backend/shaders.h"
#include "thirdparty/smaa/AreaTex.h"
#include "thirdparty/smaa/SearchTex.h"

namespace nr {
namespace {
nvrhi::TextureHandle Image(nvrhi::IDevice* device, uint32_t w, uint32_t h, nvrhi::Format format,
                           const char* name, bool render_target) {
  nvrhi::TextureDesc d;
  d.width = w; d.height = h; d.format = format; d.debugName = name;
  d.isRenderTarget = render_target;
  d.initialState = nvrhi::ResourceStates::ShaderResource; d.keepInitialState = true;
  return device->createTexture(d);
}
}

nvrhi::GraphicsPipelineHandle Smaa::Pipeline(unsigned index, nvrhi::IFramebuffer* framebuffer) {
  auto& p = passes_[index];
  const auto format = framebuffer->getFramebufferInfo().colorFormats[0];
  if (auto it = p.pipelines.find(format); it != p.pipelines.end()) return it->second;
  nvrhi::GraphicsPipelineDesc d;
  d.primType = nvrhi::PrimitiveType::TriangleList;
  d.VS = p.vs; d.PS = p.ps; d.bindingLayouts = {p.layout};
  d.renderState.depthStencilState.depthTestEnable = false;
  d.renderState.depthStencilState.depthWriteEnable = false;
  d.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  auto pipeline = device_->createGraphicsPipeline(d, framebuffer->getFramebufferInfo());
  if (pipeline) p.pipelines[format] = pipeline;
  return pipeline;
}

bool Smaa::Initialize() {
  if (std::getenv("NR_SMAA_GPU_TIME"))
    timer_ = std::make_unique<AoGpuTimer>(device_, "SMAA 1x, three passes and clears", "SMAA GPU time");
  static_assert(sizeof(areaTexBytes) == AREATEX_SIZE && sizeof(searchTexBytes) == SEARCHTEX_SIZE);
  const ShaderId vs[] = {ShaderId::kSmaaEdgeVs, ShaderId::kSmaaWeightVs, ShaderId::kSmaaBlendVs};
  const ShaderId ps[] = {ShaderId::kSmaaEdgePs, ShaderId::kSmaaWeightPs, ShaderId::kSmaaBlendPs};
  const char* vs_entry[] = {"vs_edge", "vs_weight", "vs_blend"};
  const char* ps_entry[] = {"ps_edge", "ps_weight", "ps_blend"};
  for (unsigned i = 0; i < 3; ++i) {
    auto make = [&](ShaderId id, nvrhi::ShaderType type, const char* entry) {
      auto b = GetShaderBytecode(device_->getGraphicsAPI(), id);
      nvrhi::ShaderDesc d; d.shaderType = type; d.entryName = entry;
      return device_->createShader(d, b.data, b.size);
    };
    auto& p = passes_[i];
    p.vs = make(vs[i], nvrhi::ShaderType::Vertex, vs_entry[i]);
    p.ps = make(ps[i], nvrhi::ShaderType::Pixel, ps_entry[i]);
    nvrhi::BindingLayoutDesc l; l.visibility = nvrhi::ShaderType::All;
    using I = nvrhi::BindingLayoutItem;
    l.bindings = {I::PushConstants(0, 16), I::Texture_SRV(0), I::Sampler(0), I::Sampler(1)};
    if (i) l.bindings.push_back(I::Texture_SRV(1));
    if (i == 1) l.bindings.push_back(I::Texture_SRV(2));
    p.layout = device_->createBindingLayout(l);
    if (!p.vs || !p.ps || !p.layout) return false;
  }
  nvrhi::SamplerDesc s;
  s.addressU = s.addressV = s.addressW = nvrhi::SamplerAddressMode::Clamp;
  s.minFilter = s.magFilter = true; s.mipFilter = false;
  linear_ = device_->createSampler(s);
  s.minFilter = s.magFilter = false;
  point_ = device_->createSampler(s);
  area_ = Image(device_, AREATEX_WIDTH, AREATEX_HEIGHT, nvrhi::Format::RG8_UNORM, "SMAA area v2.8", false);
  search_ = Image(device_, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, nvrhi::Format::R8_UNORM, "SMAA search v2.8", false);
  if (!area_ || !search_ || !linear_ || !point_) return false;
  // Fixed shaders are embedded, independent of game shader packs. PipelinePrewarm calls this too.
  for (auto f : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::R10G10B10A2_UNORM}) {
    auto t = Image(device_, 1, 1, f, "SMAA prewarm", true);
    if (!t) return false;
    auto fb = device_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(t));
    if (!fb || !Pipeline(2, fb)) return false;
    if (f == nvrhi::Format::RGBA8_UNORM && (!Pipeline(0, fb) || !Pipeline(1, fb))) return false;
  }
  return true;
}

bool Smaa::Bind(nvrhi::ITexture* source, nvrhi::ITexture* target) {
  const auto& d = target->getDesc();
  if (!edges_ || edges_->getDesc().width != d.width || edges_->getDesc().height != d.height) {
    if (timer_) Logf(LogLevel::kInfo, "rexgpu-native: SMAA 1x at %ux%u", d.width, d.height);
    edges_ = Image(device_, d.width, d.height, nvrhi::Format::RGBA8_UNORM, "SMAA edges", true);
    weights_ = Image(device_, d.width, d.height, nvrhi::Format::RGBA8_UNORM, "SMAA weights", true);
    source_ = nullptr; target_ = nullptr;
  }
  if (!edges_ || !weights_) return false;
  if (source_ == source && target_ == target) return true;
  using I = nvrhi::BindingSetItem;
  for (unsigned i = 0; i < 3; ++i) {
    auto& p = passes_[i];
    nvrhi::BindingSetDesc b;
    b.bindings = {I::PushConstants(0, 16), I::Texture_SRV(0, i == 1 ? edges_.Get() : source),
                  I::Sampler(0, linear_), I::Sampler(1, point_)};
    if (i) b.bindings.push_back(I::Texture_SRV(1, i == 1 ? area_.Get() : weights_.Get()));
    if (i == 1) b.bindings.push_back(I::Texture_SRV(2, search_));
    p.set = device_->createBindingSet(b, p.layout);
    p.framebuffer = device_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(
        i == 0 ? edges_.Get() : i == 1 ? weights_.Get() : target));
    if (!p.set || !p.framebuffer) return false;
  }
  source_ = source; target_ = target;
  return true;
}

bool Smaa::Render(nvrhi::ICommandList* cl, nvrhi::ITexture* source, nvrhi::ITexture* target) {
  if (!cl || !source || !target || source == target) return false;
  const auto& d = target->getDesc();
  if (source->getDesc().width != d.width || source->getDesc().height != d.height ||
      source->getDesc().sampleCount != 1 || d.sampleCount != 1 || !Bind(source, target)) return false;
  if (!uploaded_) {
    cl->writeTexture(area_, 0, 0, areaTexBytes, AREATEX_PITCH);
    cl->writeTexture(search_, 0, 0, searchTexBytes, SEARCHTEX_PITCH);
    uploaded_ = true;
  }
  if (timer_) { timer_->FrameEnd(); timer_->Begin(cl); }
  // Edge detection discards flat pixels. Clear both scratch images, including alpha, every frame.
  cl->clearTextureFloat(edges_, nvrhi::AllSubresources, nvrhi::Color(0.f));
  cl->clearTextureFloat(weights_, nvrhi::AllSubresources, nvrhi::Color(0.f));
  const float constants[4] = {1.f / float(d.width), 1.f / float(d.height), 0, 0};
  for (unsigned i = 0; i < 3; ++i) {
    auto& p = passes_[i];
    auto pipeline = Pipeline(i, p.framebuffer);
    if (!pipeline) { if (timer_) timer_->End(cl); return false; }
    nvrhi::GraphicsState s; s.pipeline = pipeline; s.framebuffer = p.framebuffer; s.bindings = {p.set};
    s.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(d.width), float(d.height)));
    cl->setGraphicsState(s); cl->setPushConstants(constants, sizeof(constants));
    nvrhi::DrawArguments draw; draw.vertexCount = 3; cl->draw(draw);
  }
  if (timer_) timer_->End(cl);
  return true;
}
}  // namespace nr
