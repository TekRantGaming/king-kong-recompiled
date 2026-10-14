// The lighting options' work on the game's frame: see scene_lighting.h.

#include "backend/scene_lighting.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "backend/log.h"
#include "kkshaders/lighting.h"
#include "backend/shaders/generated/lighting_ssr_color_cs_dxil.h"
#include "backend/shaders/generated/lighting_ssr_color_cs_spirv.h"
#include "backend/shaders/generated/lighting_ssr_color_down_cs_dxil.h"
#include "backend/shaders/generated/lighting_ssr_color_down_cs_spirv.h"
#include "backend/shaders/generated/lighting_ssr_depth_cs_dxil.h"
#include "backend/shaders/generated/lighting_ssr_depth_cs_spirv.h"
#include "backend/shaders/generated/lighting_ssr_depth_down_cs_dxil.h"
#include "backend/shaders/generated/lighting_ssr_depth_down_cs_spirv.h"
#include "backend/shaders/generated/lighting_volumetric_centroid_cs_dxil.h"
#include "backend/shaders/generated/lighting_volumetric_centroid_cs_spirv.h"
#include "backend/shaders/generated/lighting_volumetric_mask_cs_dxil.h"
#include "backend/shaders/generated/lighting_volumetric_mask_cs_spirv.h"
#include "backend/shaders/generated/lighting_volumetric_ps_dxil.h"
#include "backend/shaders/generated/lighting_volumetric_ps_spirv.h"
#include "backend/shaders/generated/lighting_volumetric_radial_cs_dxil.h"
#include "backend/shaders/generated/lighting_volumetric_radial_cs_spirv.h"
#include "backend/shaders/generated/lighting_volumetric_vs_dxil.h"
#include "backend/shaders/generated/lighting_volumetric_vs_spirv.h"

namespace nr {

namespace {

// shaders/lighting_ssr.hlsl's SsrPrepare.
struct SsrPrepareConstants {
  int32_t source[4];      // picture origin xy, size zw
  float depth[4];         // view z = x / (y - stored); z: sky threshold; w: far plane
  int32_t mip[4];         // x: mip written, yz: its size
  int32_t mip_source[4];  // xy: the source mip's size
};

// shaders/lighting_volumetric.hlsl's VolumetricConstants.
struct VolumetricConstants {
  float light[16];
  float shadow[4];      // bias, steps, share of the fog a shadowed ray loses, strength
  float fog_color[4];
  float fog_params[4];  // near over distance, density, inverse distance, depth clamp
  float fog_norm[4];    // 1 / view z = x * d - y; z: 1 with a shadow map
  float screen[4];      // 1 / size, size of the colour target
  float sun[4];         // view-space direction to the sun (0: not known), the phase function's g
  float camera[4];      // projection x and y scales, god rays' strength, their samples
  float rays[4];        // quarter-resolution size, the sky's brightness threshold, decay per sample
};

// The god rays work at a quarter of the colour target's resolution.
constexpr uint32_t kRaysDivisor = 4;

// The colour copy's mips: enough for rough water (the 16th of the picture is blurry enough).
constexpr uint32_t kColorMips = 5;

uint32_t MipCount(uint32_t w, uint32_t h) {
  uint32_t n = 1;
  while ((w > 1 || h > 1) && n < 12) {
    w = std::max(w / 2, 1u);
    h = std::max(h / 2, 1u);
    ++n;
  }
  return n;
}

}  // namespace

SceneLighting::SceneLighting(nvrhi::IDevice* device, const LightingConfig& config) : device_(device), config_(config) {}

SceneLighting::~SceneLighting() = default;

bool SceneLighting::MakePass(Pass& pass, const void* data, size_t size, const char* entry,
                             const std::vector<nvrhi::BindingLayoutItem>& items) {
  nvrhi::ShaderDesc sd;
  sd.shaderType = nvrhi::ShaderType::Compute;
  sd.entryName = entry;
  pass.shader = device_->createShader(sd, data, size);
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

bool SceneLighting::Initialize() {
  if (config_.gpu_time) {
    timers_[0] = std::make_unique<AoGpuTimer>(device_, "the screen-space shadow", "Lighting GPU time");
    timers_[1] = std::make_unique<AoGpuTimer>(device_, "the water and its reflections", "Lighting GPU time");
    timers_[2] = std::make_unique<AoGpuTimer>(device_, "the volumetric light", "Lighting GPU time");
  }
  if (config_.water_on()) {
    const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
#define NR_SSR_BLOB(name) \
  (spirv ? (const void*)k_##name##_spirv : (const void*)k_##name##_dxil), (spirv ? sizeof(k_##name##_spirv) : sizeof(k_##name##_dxil))
    using I = nvrhi::BindingLayoutItem;
    const auto cb = I::VolatileConstantBuffer(0);
    const bool ok =
        MakePass(depth_pass_, NR_SSR_BLOB(lighting_ssr_depth_cs), "cs_depth", {cb, I::Texture_SRV(0), I::Texture_UAV(0)}) &&
        MakePass(depth_down_pass_, NR_SSR_BLOB(lighting_ssr_depth_down_cs), "cs_depth_down",
                 {cb, I::Texture_SRV(1), I::Texture_UAV(0)}) &&
        MakePass(color_pass_, NR_SSR_BLOB(lighting_ssr_color_cs), "cs_color", {cb, I::Texture_SRV(0), I::Texture_UAV(0)}) &&
        MakePass(color_down_pass_, NR_SSR_BLOB(lighting_ssr_color_down_cs), "cs_color_down",
                 {cb, I::Texture_SRV(1), I::Texture_UAV(0)});
#undef NR_SSR_BLOB
    nvrhi::BufferDesc bd;
    bd.byteSize = sizeof(SsrPrepareConstants);
    bd.isConstantBuffer = true;
    bd.isVolatile = true;
    bd.maxVersions = 256;
    bd.debugName = "Lighting SSR constants";
    constants_ = device_->createBuffer(bd);
    if (!ok || !constants_) {
      Logf(LogLevel::kWarning, "rexgpu-native: lighting: the water reflections' shaders could not be created; the game's own stay");
      config_.water = WaterReflections::kOriginal;
    }
  }
  if (config_.volumetrics_on()) {
    const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
    auto shader = [&](nvrhi::ShaderType type, const char* entry, const void* data, size_t size) {
      nvrhi::ShaderDesc sd;
      sd.shaderType = type;
      sd.entryName = entry;
      return device_->createShader(sd, data, size);
    };
    vol_vs_ = spirv ? shader(nvrhi::ShaderType::Vertex, "vs_main", k_lighting_volumetric_vs_spirv, sizeof(k_lighting_volumetric_vs_spirv))
                    : shader(nvrhi::ShaderType::Vertex, "vs_main", k_lighting_volumetric_vs_dxil, sizeof(k_lighting_volumetric_vs_dxil));
    vol_ps_ = spirv ? shader(nvrhi::ShaderType::Pixel, "ps_volumetric", k_lighting_volumetric_ps_spirv, sizeof(k_lighting_volumetric_ps_spirv))
                    : shader(nvrhi::ShaderType::Pixel, "ps_volumetric", k_lighting_volumetric_ps_dxil, sizeof(k_lighting_volumetric_ps_dxil));
    using I = nvrhi::BindingLayoutItem;
    nvrhi::BindingLayoutDesc bl;
    bl.visibility = nvrhi::ShaderType::All;
    bl.bindings = {I::VolatileConstantBuffer(0), I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2),
                   I::Texture_SRV(3), I::Sampler(0), I::Sampler(1)};
    vol_layout_ = device_->createBindingLayout(bl);
#define NR_VOL_BLOB(name) \
  (spirv ? (const void*)k_##name##_spirv : (const void*)k_##name##_dxil), (spirv ? sizeof(k_##name##_spirv) : sizeof(k_##name##_dxil))
    const auto vcb = I::VolatileConstantBuffer(0);
    const bool passes =
        MakePass(mask_pass_, NR_VOL_BLOB(lighting_volumetric_mask_cs), "cs_mask",
                 {vcb, I::Texture_SRV(0), I::Texture_SRV(2), I::Sampler(0), I::Sampler(1), I::Texture_UAV(0)}) &&
        MakePass(centroid_pass_, NR_VOL_BLOB(lighting_volumetric_centroid_cs), "cs_centroid",
                 {vcb, I::Texture_SRV(3), I::Texture_UAV(0)}) &&
        MakePass(radial_pass_, NR_VOL_BLOB(lighting_volumetric_radial_cs), "cs_radial",
                 {vcb, I::Texture_SRV(3), I::Texture_SRV(4), I::Sampler(1), I::Texture_UAV(0)});
#undef NR_VOL_BLOB
    {
      nvrhi::TextureDesc td;
      td.width = td.height = 1;
      td.format = nvrhi::Format::R32_FLOAT;
      td.initialState = nvrhi::ResourceStates::ShaderResource;
      td.keepInitialState = true;
      td.debugName = "Lighting no shadow map";
      vol_no_shadow_ = device_->createTexture(td);
    }
    nvrhi::BufferDesc bd;
    bd.byteSize = sizeof(VolumetricConstants);
    bd.isConstantBuffer = true;
    bd.isVolatile = true;
    bd.maxVersions = 256;
    bd.debugName = "Lighting volumetric constants";
    vol_constants_ = device_->createBuffer(bd);
    nvrhi::SamplerDesc sd;
    sd.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    point_clamp_ = device_->createSampler(sd);
    sd.setAllFilters(true);
    linear_clamp_ = device_->createSampler(sd);
    if (!vol_vs_ || !vol_ps_ || !vol_layout_ || !vol_constants_ || !point_clamp_ || !linear_clamp_ || !passes ||
        !vol_no_shadow_) {
      Logf(LogLevel::kWarning, "rexgpu-native: lighting: the volumetric light's shaders could not be created; it stays off");
      config_.volumetrics = Volumetrics::kOff;
    }
  }
  if (config_.volumetrics_on()) {
    for (const auto format : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::R10G10B10A2_UNORM,
                              nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RG16_FLOAT,
                              nvrhi::Format::R32_FLOAT, nvrhi::Format::RG32_FLOAT}) {
      nvrhi::FramebufferInfo info;
      info.colorFormats = {format};
      if (!VolumetricPipeline(info)) return false;
    }
  }
  return true;
}

void SceneLighting::NoteShadowQuad(uint64_t frame, const ShadowQuad& q) {
  if (shadow_quad_frame_ == frame) return;  // the frame's first (the static shadows)
  shadow_quad_ = q;
  shadow_quad_frame_ = frame;
}

void SceneLighting::NoteFog(uint64_t frame, const Fog& f) {
  fog_ = f;
  fog_frame_ = frame;
}

void SceneLighting::NoteShafts(uint64_t frame) { shafts_frame_ = frame; }

bool SunDirectionFromShadow(const float light[16], float norm_x, float norm_y, float px, float py, float out[3]) {
  if (norm_x == 0.0f || px == 0.0f || py == 0.0f) return false;
  // The light's NDC of a view-space point, as lighting_volumetric.hlsl (and the game) computes it.
  auto light_ndc = [&](const double p[3], double l[3]) {
    const double z = p[2];
    const double x = p[0] * px / z, y = p[1] * py / z;
    const double ds = (1.0 / z + norm_y) / norm_x;
    const double v[4] = {x * ds, y * ds, ds, 1.0};
    double c[4];
    for (int r = 0; r < 4; ++r)
      c[r] = light[r * 4 + 0] * v[0] + light[r * 4 + 1] * v[1] + light[r * 4 + 2] * v[2] + light[r * 4 + 3] * v[3];
    if (std::fabs(c[3]) < 1e-12) return false;
    for (int r = 0; r < 3; ++r) l[r] = c[r] / c[3];
    return true;
  };
  // The Jacobian at a point 20 units ahead: how the light's x, y and depth change along view x, y, z.
  double j[3][3];
  const double h = 0.25;
  for (int a = 0; a < 3; ++a) {
    double p0[3] = {0.0, 0.0, 20.0}, p1[3] = {0.0, 0.0, 20.0}, l0[3], l1[3];
    p0[a] -= h;
    p1[a] += h;
    if (!light_ndc(p0, l0) || !light_ndc(p1, l1)) return false;
    for (int r = 0; r < 3; ++r) j[r][a] = (l1[r] - l0[r]) / (2.0 * h);
  }
  // Along the light's direction the shadow-map position stays: across the gradients of x and y.
  const double d[3] = {j[0][1] * j[1][2] - j[0][2] * j[1][1], j[0][2] * j[1][0] - j[0][0] * j[1][2],
                       j[0][0] * j[1][1] - j[0][1] * j[1][0]};
  const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (!(len > 1e-12) || !std::isfinite(len)) return false;
  // Towards the light: where the light-space depth gets smaller.
  const double depth_change = j[2][0] * d[0] + j[2][1] * d[1] + j[2][2] * d[2];
  const double sign = depth_change > 0.0 ? -1.0 : 1.0;
  for (int i = 0; i < 3; ++i) out[i] = float(sign * d[i] / len);
  return true;
}

bool SceneLighting::RunVolumetric(nvrhi::ICommandList* cl) {
  if (fog_frame_ != frame_ || !scene_color_.texture || !scene_depth_.texture) return false;
  VolumetricInput in;
  in.color = scene_color_.texture;
  in.depth = scene_depth_.texture;
  in.fog = fog_;
  if (shadow_quad_frame_ == frame_ && shadow_quad_.shadow_map) {
    in.shadow = &shadow_quad_;
  } else {
    ++stats_.volumetric_no_shadow;
  }
  AoProjection p;
  AoScreen sc;
  if (camera_.Get(frame_, p, sc)) {
    in.px = p.px;
    in.py = p.py;
  }
  if (!Volumetric(cl, in)) return false;
  ++stats_.volumetric;
  if (shafts_frame_ == frame_) ++stats_.volumetric_shafts;
  if (stats_.volumetric == 1)
    Logf(LogLevel::kInfo, "rexgpu-native: lighting: first volumetric light (frame %llu): fog %.3g %.3g %.3g, %s",
         static_cast<unsigned long long>(frame_), fog_.params[1], fog_.params[2], fog_.params[3],
         in.shadow ? "with the shadow map" : "god rays only (no screen-space shadow this frame)");
  return true;
}

bool SceneLighting::EnsureVolumetricImages(const nvrhi::TextureDesc& cd) {
  const uint32_t qw = std::max(cd.width / kRaysDivisor, 1u), qh = std::max(cd.height / kRaysDivisor, 1u);
  if (vol_copy_ && vol_copy_->getDesc().width == cd.width && vol_copy_->getDesc().height == cd.height &&
      vol_copy_->getDesc().format == cd.format && vol_mask_)
    return true;
  nvrhi::TextureDesc td;
  td.width = cd.width;
  td.height = cd.height;
  td.format = cd.format;
  td.initialState = nvrhi::ResourceStates::ShaderResource;
  td.keepInitialState = true;
  td.debugName = "Lighting volumetric scene colour";
  vol_copy_ = device_->createTexture(td);
  auto make = [&](uint32_t w, uint32_t h, nvrhi::Format format, const char* name) {
    nvrhi::TextureDesc q;
    q.width = w;
    q.height = h;
    q.format = format;
    q.isUAV = true;
    q.initialState = nvrhi::ResourceStates::ShaderResource;
    q.keepInitialState = true;
    q.debugName = name;
    return device_->createTexture(q);
  };
  vol_mask_ = make(qw, qh, nvrhi::Format::RGBA16_FLOAT, "Lighting god rays sky");
  vol_rays_ = make(qw, qh, nvrhi::Format::RGBA16_FLOAT, "Lighting god rays");
  vol_centroid_ = make(1, 1, nvrhi::Format::RGBA32_FLOAT, "Lighting god rays centre");
  if (!vol_copy_ || !vol_mask_ || !vol_rays_ || !vol_centroid_) {
    vol_copy_ = vol_mask_ = vol_rays_ = vol_centroid_ = nullptr;
    return false;
  }
  return true;
}

bool SceneLighting::Volumetric(nvrhi::ICommandList* cl, const VolumetricInput& in) {
  if (!in.color || !in.depth || !vol_ps_ || !mask_pass_.pipeline) return false;
  const Fog& fog = in.fog;
  // No fog, nothing to light (a clear scene stays clear).
  if (!(fog.params[2] > 0.0f) || !(fog.params[3] > 0.0f) || fog.normalizing[0] == 0.0f) return false;
  nvrhi::ITexture* color = in.color;
  nvrhi::FramebufferHandle& fb = vol_framebuffers_[color];
  if (!fb) {
    if (vol_framebuffers_.size() > 16) {
      vol_framebuffers_.clear();
      return false;
    }
    nvrhi::FramebufferDesc fd;
    fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(color));
    fb = device_->createFramebuffer(fd);
    if (!fb) return false;
  }
  nvrhi::IGraphicsPipeline* pipeline = VolumetricPipeline(fb->getFramebufferInfo());
  if (!pipeline) return false;
  const nvrhi::TextureDesc& cd = color->getDesc();
  const nvrhi::TextureDesc& dd = in.depth->getDesc();
  const uint32_t w = std::min(dd.width, cd.width), h = std::min(dd.height, cd.height);
  if (w < 8 || h < 8 || !EnsureVolumetricImages(cd)) return false;
  const uint32_t qw = vol_mask_->getDesc().width, qh = vol_mask_->getDesc().height;

  VolumetricConstants k = {};
  const bool high = config_.volumetrics == Volumetrics::kHigh;
  const float strength = std::max(config_.volumetric_strength, 0.0f);
  if (in.shadow) std::memcpy(k.light, in.shadow->light, sizeof(k.light));
  k.shadow[0] = in.shadow ? in.shadow->bias : 0.0f;
  k.shadow[1] = high ? 32.0f : 16.0f;
  k.shadow[2] = high ? 0.4f : 0.3f;  // the share of the fog's light a shadowed ray loses
  k.shadow[3] = strength;
  std::memcpy(k.fog_color, fog.color, sizeof(k.fog_color));
  k.fog_params[0] = fog.params[0];
  k.fog_params[1] = fog.params[2];
  k.fog_params[2] = fog.params[3];
  k.fog_params[3] = fog.normalizing[2] > 0.0f ? fog.normalizing[2] : 1.0f;
  k.fog_norm[0] = fog.normalizing[0];
  k.fog_norm[1] = fog.normalizing[1];
  k.fog_norm[2] = in.shadow ? 1.0f : 0.0f;
  k.screen[0] = 1.0f / float(w);
  k.screen[1] = 1.0f / float(h);
  k.screen[2] = float(w);
  k.screen[3] = float(h);
  float sun[3] = {0.0f, 0.0f, 0.0f};
  if (in.shadow && SunDirectionFromShadow(in.shadow->light, fog.normalizing[0], fog.normalizing[1], in.px, in.py, sun))
    std::memcpy(k.sun, sun, sizeof(sun));
  k.sun[3] = 0.6f;
  k.camera[0] = in.px > 0.0f ? in.px : 1.0f;
  k.camera[1] = in.py > 0.0f ? in.py : 1.0f;
  // The god rays scale with the fog: the game's fog amount 30 units away (denser fog, more light).
  const float fog30 = std::clamp(std::clamp(30.0f * fog.params[3] - fog.params[0], 0.0f, 1.0f) * fog.params[2], 0.0f, 1.0f);
  k.camera[2] = strength * kGodRayExposure * std::min(1.0f, 0.25f + 1.25f * fog30);
  k.camera[3] = high ? 64.0f : 32.0f;
  k.rays[0] = float(qw);
  k.rays[1] = float(qh);
  k.rays[2] = 0.45f;  // the sky brighter than this (a picture value) sends rays
  k.rays[3] = high ? 0.985f : 0.97f;
  cl->writeBuffer(vol_constants_, &k, sizeof(k));

  using S = nvrhi::BindingSetItem;
  const auto cb = S::ConstantBuffer(0, vol_constants_);
  const nvrhi::TextureSubresourceSet first(0, 1, 0, 1);
  auto make_set = [&](nvrhi::IBindingLayout* layout, const std::vector<S>& items) {
    nvrhi::BindingSetDesc bs;
    bs.bindings = items;
    return device_->createBindingSet(bs, layout);
  };
  nvrhi::ITexture* shadow_map = in.shadow && in.shadow->shadow_map ? in.shadow->shadow_map : vol_no_shadow_.Get();
  nvrhi::BindingSetHandle mask =
      make_set(mask_pass_.layout, {cb, S::Texture_SRV(0, in.depth, nvrhi::Format::UNKNOWN, first), S::Texture_SRV(2, vol_copy_),
                                   S::Sampler(0, point_clamp_), S::Sampler(1, linear_clamp_), S::Texture_UAV(0, vol_mask_)});
  nvrhi::BindingSetHandle centroid =
      make_set(centroid_pass_.layout, {cb, S::Texture_SRV(3, vol_mask_), S::Texture_UAV(0, vol_centroid_)});
  nvrhi::BindingSetHandle radial =
      make_set(radial_pass_.layout, {cb, S::Texture_SRV(3, vol_mask_), S::Texture_SRV(4, vol_centroid_),
                                     S::Sampler(1, linear_clamp_), S::Texture_UAV(0, vol_rays_)});
  nvrhi::BindingSetHandle composite =
      make_set(vol_layout_, {cb, S::Texture_SRV(0, in.depth, nvrhi::Format::UNKNOWN, first),
                             S::Texture_SRV(1, shadow_map, nvrhi::Format::UNKNOWN, first), S::Texture_SRV(2, vol_copy_),
                             S::Texture_SRV(3, vol_rays_), S::Sampler(0, point_clamp_), S::Sampler(1, linear_clamp_)});
  if (!mask || !centroid || !radial || !composite) return false;
  if (timers_[2]) timers_[2]->Begin(cl);
  // The scene as it is, read by every pass; the composite writes the target whole.
  const nvrhi::TextureSlice slice = nvrhi::TextureSlice().resolve(cd);
  cl->copyTexture(vol_copy_, slice, color, slice);
  Dispatch(cl, mask_pass_, mask, qw, qh);
  {
    nvrhi::ComputeState cs;
    cs.pipeline = centroid_pass_.pipeline;
    cs.bindings = {centroid};
    cl->setComputeState(cs);
    cl->dispatch(1, 1, 1);
  }
  Dispatch(cl, radial_pass_, radial, qw, qh);
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {composite};
  gs.viewport.addViewport(nvrhi::Viewport(float(w), float(h)));
  gs.viewport.addScissorRect(nvrhi::Rect(0, int(w), 0, int(h)));
  cl->setGraphicsState(gs);
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
  if (timers_[2]) timers_[2]->End(cl);
  return true;
}

AoGpuTimer* SceneLighting::timer(int which) const {
  return which >= 0 && which < 3 ? timers_[which].get() : nullptr;
}

void SceneLighting::NewFrame(uint64_t frame) {
  frame_ = frame;
  ssr_.valid = false;
  ssr_tried_ = false;
  vol_pending_ = false;
  ++stats_.frames;
  if (stats_.frames % 600 == 0 && config_.water_on()) {
    Logf(LogLevel::kInfo,
         "rexgpu-native: lighting last 600 frames: water reflections prepared in %llu, %llu without a camera, %llu with "
         "water outside the scene",
         static_cast<unsigned long long>(stats_.ssr_prepared - logged_.ssr_prepared),
         static_cast<unsigned long long>(stats_.ssr_no_camera - logged_.ssr_no_camera),
         static_cast<unsigned long long>(stats_.ssr_no_scene - logged_.ssr_no_scene));
  }
  if (stats_.frames % 600 == 0 && config_.volumetrics_on()) {
    Logf(LogLevel::kInfo,
         "rexgpu-native: lighting last 600 frames: volumetric light in %llu (%llu with the game's shafts), %llu fog passes "
         "without a screen-space shadow",
         static_cast<unsigned long long>(stats_.volumetric - logged_.volumetric),
         static_cast<unsigned long long>(stats_.volumetric_shafts - logged_.volumetric_shafts),
         static_cast<unsigned long long>(stats_.volumetric_no_shadow - logged_.volumetric_no_shadow));
  }
  if (stats_.frames % 600 == 0) {
    logged_ = stats_;
  }
}

void SceneLighting::OnDraw(nvrhi::ICommandList* cl, const Draw& d) {
  if (d.frame != frame_) NewFrame(d.frame);
  const AoDrawClass cls = rules_.Classify(d.state);
  if ((cls == AoDrawClass::kOpaque || cls == AoDrawClass::kDepthOnly) && d.frame_size && d.depth.key && d.vs_constants &&
      camera_.offers(frame_) < 16) {
    float rows[16];
    AoProjection p;
    if (FindAoPerspectiveRegister(d.vs_constants, rows) >= 0 && ExtractAoProjection(rows, p)) camera_.Offer(frame_, p, d.screen);
  }
  AoFrameTracker::Draw td;
  td.frame = d.frame;
  td.cls = cls;
  td.color_key = d.color.key;
  td.depth_key = d.depth.key;
  td.frame_size = d.frame_size;
  td.writes_depth = d.state.depth_write && d.depth.key;
  // Reflections are prepared before the upcoming draw writes its depth.
  const bool depth_before_draw = tracker_.frame() == d.frame && tracker_.depth_valid();
  const AoFrameTracker::Events e = tracker_.OnDraw(td);
  if (e.in_scene) {
    if (d.color.key && d.color.key == tracker_.scene_color()) scene_color_ = d.color;
    if (d.depth.key && d.depth.key == tracker_.scene_depth()) scene_depth_ = d.depth;
  }

  // The volumetric light: right after the game's fog pass, before whatever comes next.
  if (vol_pending_ && d.family != LightFamily::kFog) {
    vol_pending_ = false;
    if (timing_ >= 0) {  // not inside a timed run of the game's draws
      timers_[timing_]->End(cl);
      timing_ = -1;
    }
    RunVolumetric(cl);
  }
  if (d.family == LightFamily::kFog && config_.volumetrics_on() && e.in_scene) vol_pending_ = true;

  // GPU time of the game's own runs of draws the options change (--native_lighting_gpu_time).
  const bool water = d.family == LightFamily::kWater || d.family == LightFamily::kOcean;
  const int run = d.family == LightFamily::kShadowQuad ? 0 : (water ? 1 : -1);
  if (timing_ >= 0 && timing_ != run) {
    timers_[timing_]->End(cl);
    timing_ = -1;
  }
  if (run >= 0 && timing_ < 0 && timers_[run]) {
    timers_[run]->Begin(cl);
    timing_ = run;
  }

  // The water's reflections: the scene, once, before its first water draw.
  // Only when the frame before drew a water shader with the reflection hook: the game's water masks
  // (pswater's mask shader, psocean's depth-only pass) come first and write the water's depth, so the
  // scene is taken before them, and frames with masks only (Ann, The Cave) do not pay for it.
  const bool hooked = kkshaders::waterReflectionHookFor(d.state.ps_hash).fetchMask != 0;
  if (hooked) hooked_water_frame_ = frame_;
  if (water && config_.water_on() && !ssr_tried_ && hooked_water_frame_ + 1 >= frame_) {
    ssr_tried_ = true;
    if (!e.in_scene || !scene_color_.texture || !scene_depth_.texture || !depth_before_draw || !tracker_.depth_valid()) {
      ++stats_.ssr_no_scene;
    } else if (PrepareSsr(cl)) {
      ++stats_.ssr_prepared;
    }
  }
}

void SceneLighting::BeforeDraw(nvrhi::ICommandList* cl, uint64_t frame, LightFamily family) {
  const bool water = family == LightFamily::kWater || family == LightFamily::kOcean;
  const int run = family == LightFamily::kShadowQuad ? 0 : (water ? 1 : -1);
  if (frame != frame_ || run != timing_) EndTimedDraws(cl);
}

void SceneLighting::OnDepthClear(uint64_t frame, uint64_t depth_key) {
  if (frame != frame_) NewFrame(frame);
  tracker_.OnDepthCleared(frame, depth_key);
}

void SceneLighting::EndTimedDraws(nvrhi::ICommandList* cl) {
  if (timing_ >= 0) {
    timers_[timing_]->End(cl);
    timing_ = -1;
  }
}

void SceneLighting::OnPresent(nvrhi::ICommandList* cl) {
  EndTimedDraws(cl);
  // Fog can be the last eligible draw. Finish its deferred work before presentation.
  if (vol_pending_) {
    vol_pending_ = false;
    RunVolumetric(cl);
  }
  for (auto& t : timers_)
    if (t) t->FrameEnd();
}

bool SceneLighting::EnsureSsrImages(uint32_t w, uint32_t h) {
  if (ssr_color_ && ssr_w_ == w && ssr_h_ == h) return true;
  auto make = [&](nvrhi::Format format, uint32_t mips, const char* name) {
    nvrhi::TextureDesc td;
    td.width = w;
    td.height = h;
    td.mipLevels = mips;
    td.format = format;
    td.isUAV = true;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = name;
    return device_->createTexture(td);
  };
  ssr_color_ = make(nvrhi::Format::RGBA16_FLOAT, std::min(kColorMips, MipCount(w, h)), "Lighting SSR scene colour");
  ssr_depth_ = make(nvrhi::Format::R32_FLOAT, MipCount(w, h), "Lighting SSR view depth");
  if (!ssr_color_ || !ssr_depth_) {
    ssr_color_ = ssr_depth_ = nullptr;
    return false;
  }
  ssr_w_ = w;
  ssr_h_ = h;
  return true;
}

bool SceneLighting::Dispatch(nvrhi::ICommandList* cl, Pass& pass, nvrhi::IBindingSet* set, uint32_t w, uint32_t h) {
  if (!set) return false;
  nvrhi::ComputeState cs;
  cs.pipeline = pass.pipeline;
  cs.bindings = {set};
  cl->setComputeState(cs);
  cl->dispatch(std::max((w + 7) / 8, 1u), std::max((h + 7) / 8, 1u), 1);
  return true;
}

bool SceneLighting::PrepareSsr(nvrhi::ICommandList* cl) {
  ssr_.valid = false;
  AoProjection p;
  AoScreen sc;
  if (!camera_.Get(frame_, p, sc)) {
    ++stats_.ssr_no_camera;
    if (!warned_camera_) {
      Logf(LogLevel::kWarning, "rexgpu-native: lighting: no camera found among the scene's draws yet; no water reflections");
      warned_camera_ = true;
    }
    return false;
  }
  // The picture: the camera's viewport in host pixels of the scene's targets, as the AO takes it.
  const nvrhi::TextureDesc& dd = scene_depth_.texture->getDesc();
  const nvrhi::TextureDesc& cd = scene_color_.texture->getDesc();
  const int32_t x0 = std::max(0, int32_t(std::lround(sc.left() * sc.host_scale_x)));
  const int32_t y0 = std::max(0, int32_t(std::lround(sc.top() * sc.host_scale_y)));
  const int32_t x1 = std::min({int32_t(dd.width), int32_t(cd.width), int32_t(std::lround((sc.left() + sc.width()) * sc.host_scale_x))});
  const int32_t y1 = std::min({int32_t(dd.height), int32_t(cd.height), int32_t(std::lround((sc.top() + sc.height()) * sc.host_scale_y))});
  if (x1 - x0 < 8 || y1 - y0 < 8) return false;
  const uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);
  if (!EnsureSsrImages(w, h)) return false;
  const AoViewMapping m = ComputeAoViewMapping(p, sc, x0, y0, w, h, 1);

  using S = nvrhi::BindingSetItem;
  auto write = [&](const SsrPrepareConstants& k) { cl->writeBuffer(constants_, &k, sizeof(k)); };
  auto make_set = [&](Pass& pass, const std::vector<S>& items) {
    nvrhi::BindingSetDesc bs;
    bs.bindings = items;
    return device_->createBindingSet(bs, pass.layout);
  };
  SsrPrepareConstants k = {};
  k.source[0] = x0;
  k.source[1] = y0;
  k.source[2] = int32_t(w);
  k.source[3] = int32_t(h);
  k.depth[0] = m.depth_mul;
  k.depth[1] = m.depth_add;
  // Cleared depth (the far plane's stored value or beyond) is sky.
  k.depth[2] = sc.z_scale > 0.0f ? sc.z_offset + sc.z_scale * (1.0f - 1e-6f) : 2.0f;
  k.depth[3] = p.far_plane() > 0.0f ? p.far_plane() : 1e9f;
  const auto cb = S::ConstantBuffer(0, constants_);
  // Binding sets are made per frame: the scene's targets can change (a new size, a new chapter), and
  // NVRHI keeps them alive while the GPU uses them.
  write(k);
  if (!Dispatch(cl, depth_pass_,
           make_set(depth_pass_, {cb, S::Texture_SRV(0, scene_depth_.texture),
                                  S::Texture_UAV(0, ssr_depth_, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1))}),
           w, h)) return false;
  if (!Dispatch(cl, color_pass_,
           make_set(color_pass_, {cb, S::Texture_SRV(0, scene_color_.texture),
                                  S::Texture_UAV(0, ssr_color_, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1))}),
           w, h)) return false;
  auto down = [&](Pass& pass, nvrhi::ITexture* image, uint32_t mips) {
    uint32_t sw = w, sh = h;
    for (uint32_t mip = 1; mip < mips; ++mip) {
      const uint32_t mw = std::max(sw / 2, 1u), mh = std::max(sh / 2, 1u);
      SsrPrepareConstants km = k;
      km.mip[0] = int32_t(mip);
      km.mip[1] = int32_t(mw);
      km.mip[2] = int32_t(mh);
      km.mip_source[0] = int32_t(sw);
      km.mip_source[1] = int32_t(sh);
      write(km);
      if (!Dispatch(cl, pass,
               make_set(pass, {cb, S::Texture_SRV(1, image, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(mip - 1, 1, 0, 1)),
                               S::Texture_UAV(0, image, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(mip, 1, 0, 1))}),
               mw, mh)) return false;
      sw = mw;
      sh = mh;
    }
    return true;
  };
  const uint32_t depth_mips = ssr_depth_->getDesc().mipLevels, color_mips = ssr_color_->getDesc().mipLevels;
  if (!down(depth_down_pass_, ssr_depth_, depth_mips) ||
      !down(color_down_pass_, ssr_color_, color_mips)) return false;
  // The water's draws sample them through the bindless table.
  cl->setTextureState(ssr_depth_, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  cl->setTextureState(ssr_color_, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  cl->commitBarriers();
  ssr_.valid = true;
  ssr_.color = ssr_color_;
  ssr_.depth = ssr_depth_;
  ssr_.color_mips = color_mips;
  ssr_.depth_mips = depth_mips;
  ssr_.x = x0;
  ssr_.y = y0;
  ssr_.width = w;
  ssr_.height = h;
  ssr_.mapping = m;
  ssr_.near_plane = p.near_plane();
  ssr_.far_plane = p.far_plane();
  if (stats_.ssr_prepared == 0)
    Logf(LogLevel::kInfo, "rexgpu-native: lighting: first water reflections (frame %llu): picture %ux%u at %d,%d, near %.4f far %.1f",
         static_cast<unsigned long long>(frame_), w, h, x0, y0, p.near_plane(), p.far_plane());
  return true;
}

nvrhi::IGraphicsPipeline* SceneLighting::VolumetricPipeline(const nvrhi::FramebufferInfo& info) {
  const uint32_t format = info.colorFormats.empty() ? 0 : uint32_t(info.colorFormats[0]);
  nvrhi::GraphicsPipelineHandle& pipeline = vol_pipelines_[format];
  if (!pipeline) {
    nvrhi::GraphicsPipelineDesc pd;
    pd.primType = nvrhi::PrimitiveType::TriangleList;
    pd.VS = vol_vs_;
    pd.PS = vol_ps_;
    pd.bindingLayouts = {vol_layout_};
    pd.renderState.depthStencilState.depthTestEnable = false;
    pd.renderState.depthStencilState.depthWriteEnable = false;
    pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    // The composite writes the colour whole from its copy (light added, a shadowed ray's share taken
    // away); the alpha is the game's.
    pd.renderState.blendState.targets[0].colorWriteMask =
        nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue;
    pipeline = device_->createGraphicsPipeline(pd, info);
  }
  return pipeline;
}

}  // namespace nr
