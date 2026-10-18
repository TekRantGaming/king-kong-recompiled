// Photo mode's grade pass: see photo_grade.h.

#include "backend/photo_grade.h"

#include <algorithm>
#include <cmath>

#include "backend/log.h"
#include "backend/photo_config.h"
#include "backend/shaders/generated/photo_ps_dxil.h"
#include "backend/shaders/generated/photo_ps_spirv.h"
#include "backend/shaders/generated/photo_vs_dxil.h"
#include "backend/shaders/generated/photo_vs_spirv.h"

namespace nr {

// shaders/photo.hlsl's PhotoConstants.
struct PhotoGrade::Constants {
  float size[4] = {};
  float grade[4] = {};
  float look[4] = {};
  float effects[4] = {};
  float dof[4] = {};
  float depth[4] = {};
  uint32_t misc[4] = {};
};

PhotoGrade::PhotoGrade(nvrhi::IDevice* device) : device_(device) {}
PhotoGrade::~PhotoGrade() = default;

bool PhotoGrade::Initialize() {
  const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  nvrhi::ShaderDesc vd;
  vd.shaderType = nvrhi::ShaderType::Vertex;
  vd.entryName = "vs_main";
  vs_ = spirv ? device_->createShader(vd, k_photo_vs_spirv, sizeof(k_photo_vs_spirv))
              : device_->createShader(vd, k_photo_vs_dxil, sizeof(k_photo_vs_dxil));
  nvrhi::ShaderDesc pd;
  pd.shaderType = nvrhi::ShaderType::Pixel;
  pd.entryName = "ps_photo";
  ps_ = spirv ? device_->createShader(pd, k_photo_ps_spirv, sizeof(k_photo_ps_spirv))
              : device_->createShader(pd, k_photo_ps_dxil, sizeof(k_photo_ps_dxil));
  if (!vs_ || !ps_) {
    Logf(LogLevel::kWarning, "rexgpu-native: photo mode: its shaders could not be created, no photo grade");
    return false;
  }
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
  bd.debugName = "Photo grade constants";
  constants_ = device_->createBuffer(bd);
  nvrhi::TextureDesc nd;
  nd.width = nd.height = 1;
  nd.format = nvrhi::Format::R32_FLOAT;
  nd.initialState = nvrhi::ResourceStates::ShaderResource;
  nd.keepInitialState = true;
  nd.debugName = "Photo grade: no depth";
  no_depth_ = device_->createTexture(nd);
  ready_ = layout_ && constants_ && no_depth_;
  return ready_;
}


nvrhi::IGraphicsPipeline* PhotoGrade::Pipeline(const nvrhi::FramebufferInfo& info) {
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
  p = device_->createGraphicsPipeline(pd, info);
  return p;
}

bool PhotoGrade::Apply(nvrhi::ICommandList* cl, nvrhi::ITexture* target, uint32_t width, uint32_t height,
                       const NrPhotoSettings& s, const Depth& depth) {
  if (!ready_ || !target || !width || !height) return false;
  const nvrhi::TextureDesc& td = target->getDesc();
  width = std::min(width, td.width);
  height = std::min(height, td.height);
  if (!copy_ || copy_->getDesc().width != td.width || copy_->getDesc().height != td.height ||
      copy_->getDesc().format != td.format) {
    nvrhi::TextureDesc cd;
    cd.width = td.width;
    cd.height = td.height;
    cd.format = td.format;
    cd.initialState = nvrhi::ResourceStates::ShaderResource;
    cd.keepInitialState = true;
    cd.debugName = "Photo grade picture copy";
    copy_ = device_->createTexture(cd);
    if (!copy_) return false;
  }
  // Made each frame (photo mode only): the frame images belong to the renderer and may be made again; the
  // command list keeps what it used until the GPU is done with it.
  nvrhi::FramebufferDesc fd;
  fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(target));
  nvrhi::FramebufferHandle fb = device_->createFramebuffer(fd);
  nvrhi::IGraphicsPipeline* pipeline = fb ? Pipeline(fb->getFramebufferInfo()) : nullptr;
  if (!pipeline) return false;

  Constants k;
  k.size[0] = float(width);
  k.size[1] = float(height);
  k.grade[0] = std::exp2(s.exposure);
  k.grade[1] = s.contrast;
  k.grade[2] = s.saturation;
  k.grade[3] = s.temperature;
  if (s.look && s.look_strength > 0.0f) {
    const auto t = PhotoLookColour(s.look);
    const float tl = t[0] * 0.2126f + t[1] * 0.7152f + t[2] * 0.0722f;
    for (int i = 0; i < 3; ++i) k.look[i] = t[i] / tl;
    k.look[3] = s.look_strength;
    k.misc[0] = s.look <= 2 ? 1u : 2u;
  }
  k.effects[0] = s.vignette;
  k.effects[1] = s.grain;
  k.effects[2] = s.letterbox;
  k.effects[3] = s.flip ? 1.0f : 0.0f;
  nvrhi::ITexture* depth_texture = no_depth_;
  if (s.dof > 0.0f && depth.texture && depth.width && depth.height) {
    depth_texture = depth.texture;
    k.dof[0] = s.dof * 0.016f * float(height);  // the largest radius: 17 px at 1080 lines
    k.dof[1] = s.focus;
    k.dof[2] = s.focus_range;
    k.dof[3] = std::max(1.0f, s.focus * 0.6f);  // the blur grows over this far past the sharp range
    k.depth[0] = depth.texels_per_pixel_x;
    k.depth[1] = depth.texels_per_pixel_y;
    k.depth[2] = float(depth.width - 1);
    k.depth[3] = float(depth.height - 1);
  }
  k.misc[1] = s.seed;

  const nvrhi::TextureSlice slice = nvrhi::TextureSlice().resolve(td);
  cl->copyTexture(copy_, slice, target, slice);
  cl->writeBuffer(constants_, &k, sizeof(k));
  using S = nvrhi::BindingSetItem;
  nvrhi::BindingSetDesc bs;
  bs.bindings = {S::ConstantBuffer(0, constants_), S::Texture_SRV(0, copy_),
                 S::Texture_SRV(1, depth_texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1))};
  nvrhi::BindingSetHandle set = device_->createBindingSet(bs, layout_);
  if (!set) return false;
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {set};
  gs.viewport.addViewport(nvrhi::Viewport(0.0f, float(width), 0.0f, float(height), 0.0f, 1.0f));
  gs.viewport.addScissorRect(nvrhi::Rect(0, int(width), 0, int(height)));
  cl->setGraphicsState(gs);
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
  ++applied_;
  return true;
}

}  // namespace nr
