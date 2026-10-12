// Screen-space ambient occlusion: see ambient_occlusion.h.

#include "backend/ambient_occlusion.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "backend/log.h"
#include "backend/renderer.h"
#include "backend/shaders.h"

namespace nr {

// The push constants of every pass (shaders/ambient_occlusion.hlsl, AoConstants).
struct AmbientOcclusion::Constants {
  uint32_t size[4] = {};       // full width, height; half width, height
  float view[4] = {};          // tan x, tan y, focal length (half-size pixels), fade distance
  float tuning[4] = {};        // radius, bias, intensity, render scale
  float fog_norm[4] = {};      // g_vFogNormalizing xyz, 1 with a fog pass
  float fog_par[4] = {};       // near over distance, density, inverse distance, mode
  float fog_col[4] = {};       // fog colour
  float blur[4] = {};          // tap direction
};

namespace {

// The guest's picture width: the render scale of a picture is its width over this.
constexpr float kGuestWidth = 1280.0f;

nvrhi::ShaderHandle MakeShader(nvrhi::IDevice* device, ShaderId id, nvrhi::ShaderType type, const char* entry) {
  const ShaderBytecode code = GetShaderBytecode(device->getGraphicsAPI(), id);
  if (!code.data || !code.size) return nullptr;
  nvrhi::ShaderDesc desc;
  desc.shaderType = type;
  desc.entryName = entry;
  return device->createShader(desc, code.data, code.size);
}

}  // namespace

AmbientOcclusion::AmbientOcclusion(nvrhi::IDevice* device) : device_(device) {}
AmbientOcclusion::~AmbientOcclusion() = default;

std::array<float, 3> AmbientOcclusion::TuningFor(int strength) {
  // radius (near-plane units), bias, intensity: the Xenos plugin's three sets.
  switch (std::clamp(strength, 1, 3)) {
    case 1: return {6.0f, 0.25f, 2.5f};
    case 2: return {8.0f, 0.3f, 3.5f};
    default: return {12.0f, 0.3f, 3.0f};
  }
}

bool AmbientOcclusion::Initialize() {
  static_assert(sizeof(Constants) == 112, "AoConstants is 7 float4");
  vs_ = MakeShader(device_, ShaderId::kAoVs, nvrhi::ShaderType::Vertex, "vs_main");
  copy_pass_.ps = MakeShader(device_, ShaderId::kAoCopyPs, nvrhi::ShaderType::Pixel, "ps_copy");
  depth_pass_.ps = MakeShader(device_, ShaderId::kAoDepthPs, nvrhi::ShaderType::Pixel, "ps_depth");
  ao_pass_.ps = MakeShader(device_, ShaderId::kAoPs, nvrhi::ShaderType::Pixel, "ps_ao");
  blur_pass_.ps = MakeShader(device_, ShaderId::kAoBlurPs, nvrhi::ShaderType::Pixel, "ps_blur");
  upsample_pass_.ps = MakeShader(device_, ShaderId::kAoUpsamplePs, nvrhi::ShaderType::Pixel, "ps_upsample");
  apply_pass_.ps = MakeShader(device_, ShaderId::kAoApplyPs, nvrhi::ShaderType::Pixel, "ps_apply");
  if (!vs_ || !copy_pass_.ps || !depth_pass_.ps || !ao_pass_.ps || !blur_pass_.ps || !upsample_pass_.ps ||
      !apply_pass_.ps) {
    Logf(LogLevel::kWarning, "rexgpu-native: ambient occlusion: its shaders could not be created, AO is unavailable");
    return false;
  }
  nvrhi::BindingLayoutDesc bl;
  bl.visibility = nvrhi::ShaderType::All;
  bl.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)),
                 nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
                 nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Sampler(0)};
  layout_ = device_->createBindingLayout(bl);
  nvrhi::SamplerDesc sd;
  sd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
  sampler_ = device_->createSampler(sd);
  nvrhi::TextureDesc td;
  td.width = td.height = 1;
  td.format = nvrhi::Format::R32_FLOAT;
  td.isRenderTarget = true;
  td.initialState = nvrhi::ResourceStates::ShaderResource;
  td.keepInitialState = true;
  td.debugName = "AO dummy";
  dummy_ = device_->createTexture(td);
  ready_ = layout_ && sampler_ && dummy_;
  return ready_;
}

nvrhi::GraphicsPipelineHandle AmbientOcclusion::MakePipeline(const nvrhi::ShaderHandle& ps, nvrhi::IFramebuffer* fb) {
  nvrhi::GraphicsPipelineDesc pd;
  pd.primType = nvrhi::PrimitiveType::TriangleList;
  pd.VS = vs_;
  pd.PS = ps;
  pd.bindingLayouts = {layout_};
  pd.renderState.depthStencilState.depthTestEnable = false;
  pd.renderState.depthStencilState.depthWriteEnable = false;
  pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  return CreateGraphicsPipeline(device_, pd, fb->getFramebufferInfo());
}

AmbientOcclusion::Image AmbientOcclusion::MakeImage(uint32_t width, uint32_t height, nvrhi::Format format,
                                                    const char* name) {
  Image image;
  nvrhi::TextureDesc td;
  td.width = std::max(width, 1u);
  td.height = std::max(height, 1u);
  td.format = format;
  td.isRenderTarget = true;
  td.initialState = nvrhi::ResourceStates::RenderTarget;
  td.keepInitialState = true;
  td.debugName = name;
  image.texture = device_->createTexture(td);
  if (!image.texture) return image;
  nvrhi::FramebufferDesc fd;
  fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(image.texture));
  image.framebuffer = device_->createFramebuffer(fd);
  return image;
}

bool AmbientOcclusion::EnsureImages(uint32_t width, uint32_t height) {
  if (width == picture_w_ && height == picture_h_ && depth_full_.texture) return true;
  sets_.clear();
  depth_full_ = MakeImage(width, height, nvrhi::Format::R32_FLOAT, "AO depth copy");
  depth_half_ = MakeImage(width / 2, height / 2, nvrhi::Format::R32_FLOAT, "AO linear depth");
  ao_a_ = MakeImage(width / 2, height / 2, nvrhi::Format::R16_FLOAT, "AO a");
  ao_b_ = MakeImage(width / 2, height / 2, nvrhi::Format::R16_FLOAT, "AO b");
  ao_full_ = MakeImage(width, height, nvrhi::Format::RG16_FLOAT, "AO full size");
  picture_w_ = picture_h_ = 0;
  depth_ready_ = false;  // the copy was lost with the old image
  if (!depth_full_.framebuffer || !depth_half_.framebuffer || !ao_a_.framebuffer || !ao_b_.framebuffer ||
      !ao_full_.framebuffer)
    return false;
  copy_pass_.pipeline = MakePipeline(copy_pass_.ps, depth_full_.framebuffer);
  depth_pass_.pipeline = MakePipeline(depth_pass_.ps, depth_half_.framebuffer);
  ao_pass_.pipeline = MakePipeline(ao_pass_.ps, ao_a_.framebuffer);
  blur_pass_.pipeline = MakePipeline(blur_pass_.ps, ao_a_.framebuffer);
  upsample_pass_.pipeline = MakePipeline(upsample_pass_.ps, ao_full_.framebuffer);
  if (!copy_pass_.pipeline || !depth_pass_.pipeline || !ao_pass_.pipeline || !blur_pass_.pipeline ||
      !upsample_pass_.pipeline)
    return false;
  picture_w_ = width;
  picture_h_ = height;
  return true;
}

bool AmbientOcclusion::EnsureScratch(nvrhi::ITexture* color) {
  const nvrhi::TextureDesc& cd = color->getDesc();
  if (scratch_.texture && scratch_w_ == cd.width && scratch_h_ == cd.height && scratch_format_ == cd.format)
    return true;
  sets_.clear();
  scratch_ = MakeImage(cd.width, cd.height, cd.format, "AO scene colour");
  scratch_w_ = cd.width;
  scratch_h_ = cd.height;
  scratch_format_ = cd.format;
  if (!scratch_.framebuffer) return false;
  apply_pass_.pipeline = MakePipeline(apply_pass_.ps, scratch_.framebuffer);
  return apply_pass_.pipeline != nullptr;
}

void AmbientOcclusion::Run(nvrhi::ICommandList* cl, nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* fb,
                           nvrhi::ITexture* a, nvrhi::ITexture* b, nvrhi::ITexture* c, const Constants& constants) {
  nvrhi::ITexture* slots[3] = {a ? a : dummy_.Get(), b ? b : dummy_.Get(), c ? c : dummy_.Get()};
  const auto key = std::make_tuple(slots[0], slots[1], slots[2]);
  // Keyed by raw texture pointers: drop stale sets so freed textures are not kept alive.
  if (sets_.size() > 64 && sets_.find(key) == sets_.end()) {
    sets_.clear();
  }
  nvrhi::BindingSetHandle& set = sets_[key];
  if (!set) {
    nvrhi::BindingSetDesc bs;
    bs.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants)),
                   nvrhi::BindingSetItem::Texture_SRV(0, slots[0], nvrhi::Format::UNKNOWN,
                                                      nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   nvrhi::BindingSetItem::Texture_SRV(1, slots[1], nvrhi::Format::UNKNOWN,
                                                      nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   nvrhi::BindingSetItem::Texture_SRV(2, slots[2], nvrhi::Format::UNKNOWN,
                                                      nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   nvrhi::BindingSetItem::Sampler(0, sampler_)};
    set = device_->createBindingSet(bs, layout_);
  }
  if (!set) return;
  const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {set};
  gs.viewport.addViewport(nvrhi::Viewport(float(info.width), float(info.height)));
  gs.viewport.addScissorRect(nvrhi::Rect(0, int(info.width), 0, int(info.height)));
  cl->setGraphicsState(gs);
  cl->setPushConstants(&constants, sizeof(constants));
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
}

void AmbientOcclusion::NoteFog(const float c[16], uint64_t frame) {
  fog_.valid = true;
  fog_.normalizing[0] = c[2 * 4 + 0];
  fog_.normalizing[1] = c[2 * 4 + 1];
  fog_.normalizing[2] = c[2 * 4 + 2];
  fog_.near_over_distance = c[1 * 4 + 0];
  fog_.density = c[1 * 4 + 2];
  fog_.inverse_distance = c[1 * 4 + 3];
  std::memcpy(fog_.color, c, sizeof(fog_.color));
  fog_frame_ = frame;
}

void AmbientOcclusion::Compute(nvrhi::ICommandList* cl, const AoSettings& settings) {
  const uint32_t w = picture_w_, h = picture_h_;
  const uint32_t hw = std::max(w / 2, 1u), hh = std::max(h / 2, 1u);
  const std::array<float, 3> tuning = TuningFor(settings.strength);
  Constants k;
  k.size[0] = w;
  k.size[1] = h;
  k.size[2] = hw;
  k.size[3] = hh;
  const float tan_x = settings.tan_half_fov_x > 0.0f ? settings.tan_half_fov_x : 0.684f;
  const float tan_y = tan_x * float(h) / float(w);
  k.view[0] = tan_x;
  k.view[1] = tan_y;
  k.view[2] = float(hh) * 0.5f / tan_y;
  k.view[3] = settings.fade_distance > 0.0f ? settings.fade_distance : 2000.0f;
  k.tuning[0] = tuning[0];
  k.tuning[1] = tuning[1];
  k.tuning[2] = tuning[2];
  k.tuning[3] = std::max(float(w) / kGuestWidth, 1.0f);
  k.fog_par[3] = float(settings.mode);

  Run(cl, depth_pass_.pipeline, depth_half_.framebuffer, depth_full_.texture, nullptr, nullptr, k);
  Run(cl, ao_pass_.pipeline, ao_a_.framebuffer, depth_half_.texture, nullptr, nullptr, k);
  Constants horizontal = k, vertical = k;
  horizontal.blur[0] = 1.0f;
  vertical.blur[1] = 1.0f;
  Run(cl, blur_pass_.pipeline, ao_b_.framebuffer, depth_half_.texture, ao_a_.texture, nullptr, horizontal);
  Run(cl, blur_pass_.pipeline, ao_a_.framebuffer, depth_half_.texture, ao_b_.texture, nullptr, vertical);
  // blur_pass_'s pipeline was made for ao_a_'s format; ao_b_ has the same one.
  if (fog_.valid && fog_frame_ == computed_frame_) {
    k.fog_norm[0] = fog_.normalizing[0];
    k.fog_norm[1] = fog_.normalizing[1];
    k.fog_norm[2] = fog_.normalizing[2];
    k.fog_norm[3] = 1.0f;
    k.fog_par[0] = fog_.near_over_distance;
    k.fog_par[1] = fog_.density;
    k.fog_par[2] = fog_.inverse_distance;
  }
  Run(cl, upsample_pass_.pipeline, ao_full_.framebuffer, depth_full_.texture, depth_half_.texture, ao_a_.texture, k);
}

nvrhi::ITexture* AmbientOcclusion::OnResolve(nvrhi::ICommandList* cl, const AoResolve& r,
                                             const AoSettings& settings) {
  if (!ready_ || settings.mode <= 0 || !r.source || !r.full_screen || !r.format_ok || r.width < 2 || r.height < 2) {
    if (settings.mode <= 0) depth_ready_ = false;
    return nullptr;
  }
  if (r.from_depth) {
    // The scene depth is complete: keep a copy (the resolve may clear it).
    if (!EnsureImages(r.width, r.height)) {
      depth_ready_ = false;
      return nullptr;
    }
    Constants k;
    k.size[0] = r.width;
    k.size[1] = r.height;
    Run(cl, copy_pass_.pipeline, depth_full_.framebuffer, r.source, nullptr, nullptr, k);
    ++stats_.depth_passes;
    depth_ready_ = true;
    depth_frame_ = r.frame;
    scene_dest_set_ = false;
    return nullptr;
  }
  // A full-screen colour copy in the frame whose depth was copied: the scene
  // going to an effect or to post-processing. The final image goes elsewhere.
  if (!depth_ready_ || depth_frame_ != r.frame || r.width != picture_w_ || r.height != picture_h_) return nullptr;
  if (scene_dest_set_ && r.dest_key != scene_dest_) return nullptr;
  if (!EnsureScratch(r.source)) return nullptr;
  if (computed_frame_ != r.frame) {
    computed_frame_ = r.frame;
    Compute(cl, settings);
    ++stats_.computed_frames;
  }
  scene_dest_ = r.dest_key;
  scene_dest_set_ = true;

  Constants k;
  k.size[0] = r.width;
  k.size[1] = r.height;
  k.fog_par[3] = float(settings.mode);
  if (fog_.valid && fog_frame_ == r.frame) std::memcpy(k.fog_col, fog_.color, sizeof(fog_.color));
  Run(cl, apply_pass_.pipeline, scratch_.framebuffer, r.source, ao_full_.texture, nullptr, k);
  ++stats_.applied;
  return scratch_.texture;
}

}  // namespace nr
