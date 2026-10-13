// XeGTAO for the native renderer: see gtao.h.

#include "backend/gtao.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

// The constant buffer's layout comes from XeGTAO's own header (its C++ half).
#include "thirdparty/XeGTAO/XeGTAO.h"

#include "backend/log.h"
#include "backend/shaders/generated/gtao_apply_ps_dxil.h"
#include "backend/shaders/generated/gtao_apply_ps_spirv.h"
#include "backend/shaders/generated/gtao_apply_resolve_ps_dxil.h"
#include "backend/shaders/generated/gtao_apply_resolve_ps_spirv.h"
#include "backend/shaders/generated/gtao_denoise_cs_dxil.h"
#include "backend/shaders/generated/gtao_denoise_cs_spirv.h"
#include "backend/shaders/generated/gtao_denoise_last_cs_dxil.h"
#include "backend/shaders/generated/gtao_denoise_last_cs_spirv.h"
#include "backend/shaders/generated/gtao_main_cs_dxil.h"
#include "backend/shaders/generated/gtao_main_cs_spirv.h"
#include "backend/shaders/generated/gtao_main_low_cs_dxil.h"
#include "backend/shaders/generated/gtao_main_low_cs_spirv.h"
#include "backend/shaders/generated/gtao_main_high_cs_dxil.h"
#include "backend/shaders/generated/gtao_main_high_cs_spirv.h"
#include "backend/shaders/generated/gtao_normals_cs_dxil.h"
#include "backend/shaders/generated/gtao_normals_cs_spirv.h"
#include "backend/shaders/generated/gtao_output_cs_dxil.h"
#include "backend/shaders/generated/gtao_output_cs_spirv.h"
#include "backend/shaders/generated/gtao_temporal_cs_dxil.h"
#include "backend/shaders/generated/gtao_temporal_cs_spirv.h"
#include "backend/shaders/generated/gtao_prefilter_cs_dxil.h"
#include "backend/shaders/generated/gtao_prefilter_cs_spirv.h"
#include "backend/shaders/generated/gtao_prepare_cs_dxil.h"
#include "backend/shaders/generated/gtao_prepare_cs_spirv.h"
#include "backend/shaders/generated/gtao_vs_dxil.h"
#include "backend/shaders/generated/gtao_vs_spirv.h"

namespace nr {

// shaders/gtao.hlsl's GTAOConstantBuffer.
struct Gtao::Constants {
  XeGTAO::GTAOConstants g = {};
  int32_t source[4] = {};      // picture origin xy, size zw (host pixels)
  uint32_t working[4] = {};    // working size xy, padded size zw
  float params[4] = {};        // step, sky depth, mode, unused
  float fog_norm[4] = {};
  float fog_par[4] = {};
  float fog_col[4] = {};
  float fade[4] = {};          // x: view depth (world units) where the occlusion has faded out, 0: no fade
  float reprojection[16] = {};
  float viewport[4] = {};  // x scale/offset, y scale/offset (guest pixels)
  float depth_scale[4] = {};  // depth scale/offset, host scale xy
  float temporal[4] = {};  // history weight, previous depth unpack mul/add, sample quality
};

namespace {

static_assert(sizeof(XeGTAO::GTAOConstants) == 96, "GTAOConstants is 6 float4");

enum BlobId {
  kPrepare,
  kPrefilter,
  kNormals,
  kMain,
  kMainLow,
  kMainHigh,
  kDenoise,
  kDenoiseLast,
  kOutput,
  kTemporal,
  kVs,
  kApply,
  kApplyResolve,
};

struct Blob {
  const void* data = nullptr;
  size_t size = 0;
};

Blob GetBlob(nvrhi::GraphicsAPI api, int id) {
  const bool spirv = api == nvrhi::GraphicsAPI::VULKAN;
#define NR_GTAO_BLOB(name) \
  (spirv ? Blob{k_##name##_spirv, sizeof(k_##name##_spirv)} : Blob{k_##name##_dxil, sizeof(k_##name##_dxil)})
  switch (id) {
    case kPrepare: return NR_GTAO_BLOB(gtao_prepare_cs);
    case kPrefilter: return NR_GTAO_BLOB(gtao_prefilter_cs);
    case kNormals: return NR_GTAO_BLOB(gtao_normals_cs);
    case kMain: return NR_GTAO_BLOB(gtao_main_cs);
    case kMainLow: return NR_GTAO_BLOB(gtao_main_low_cs);
    case kMainHigh: return NR_GTAO_BLOB(gtao_main_high_cs);
    case kDenoise: return NR_GTAO_BLOB(gtao_denoise_cs);
    case kDenoiseLast: return NR_GTAO_BLOB(gtao_denoise_last_cs);
    case kOutput: return NR_GTAO_BLOB(gtao_output_cs);
    case kTemporal: return NR_GTAO_BLOB(gtao_temporal_cs);
    case kVs: return NR_GTAO_BLOB(gtao_vs);
    case kApply: return NR_GTAO_BLOB(gtao_apply_ps);
    case kApplyResolve: return NR_GTAO_BLOB(gtao_apply_resolve_ps);
    default: return {};
  }
#undef NR_GTAO_BLOB
}

nvrhi::ShaderHandle MakeShader(nvrhi::IDevice* device, int id, nvrhi::ShaderType type, const char* entry) {
  const Blob blob = GetBlob(device->getGraphicsAPI(), id);
  if (!blob.data || !blob.size) return nullptr;
  nvrhi::ShaderDesc desc;
  desc.shaderType = type;
  desc.entryName = entry;
  return device->createShader(desc, blob.data, blob.size);
}

uint32_t RoundUp16(uint32_t v) { return (std::max(v, 1u) + 15) / 16 * 16; }

// Sets keyed in sets_ by the pass and the textures that vary.
enum SetKind { kSetPrepare, kSetOutput, kSetApply, kSetApplyResolve, kSetCopyDepth, kSetTemporal };

}  // namespace

Gtao::Gtao(nvrhi::IDevice* device) : device_(device) {}
Gtao::~Gtao() = default;

float Gtao::RadiusInNearPlanes(int strength) {
  // The legacy method's radius for the strength (AmbientOcclusion::TuningFor), in near-plane units.
  return AmbientOcclusion::TuningFor(strength)[0];
}

float Gtao::FinalPower(int strength) {
  // XeGTAO's own tuning is 2.2; strength 1 (the launcher's recommended one) is a little lighter.
  switch (std::clamp(strength, 1, 3)) {
    case 1: return 1.6f;
    case 2: return 2.2f;
    default: return 3.0f;
  }
}

bool Gtao::MakePass(Pass& pass, int id, const char* entry, const std::vector<nvrhi::BindingLayoutItem>& items) {
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

bool Gtao::Initialize() {
  using I = nvrhi::BindingLayoutItem;
  const auto cb = I::VolatileConstantBuffer(0);
  bool ok = MakePass(prepare_, kPrepare, "cs_prepare", {cb, I::Texture_SRV(0), I::Texture_UAV(0)}) &&
            MakePass(prefilter_, kPrefilter, "cs_prefilter",
                     {cb, I::Texture_SRV(0), I::Texture_UAV(0), I::Texture_UAV(1), I::Texture_UAV(2), I::Texture_UAV(3),
                      I::Texture_UAV(4), I::Sampler(0)}) &&
            MakePass(normals_, kNormals, "cs_normals", {cb, I::Texture_SRV(0), I::Texture_UAV(0), I::Sampler(0)}) &&
            MakePass(main_, kMain, "cs_main",
                     {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_UAV(0), I::Texture_UAV(1), I::Sampler(0)}) &&
            MakePass(denoise_, kDenoise, "cs_denoise",
                     {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_UAV(0), I::Sampler(0)}) &&
            MakePass(denoise_last_, kDenoiseLast, "cs_denoise_last",
                     {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_UAV(0), I::Sampler(0)}) &&
            MakePass(output_pass_, kOutput, "cs_output",
                     {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2), I::Texture_UAV(0)}) &&
            MakePass(temporal_, kTemporal, "cs_temporal",
                     {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2), I::Texture_SRV(3),
                      I::Texture_UAV(0), I::Texture_UAV(1)});
  vs_ = MakeShader(device_, kVs, nvrhi::ShaderType::Vertex, "vs_main");
  apply_ps_ = MakeShader(device_, kApply, nvrhi::ShaderType::Pixel, "ps_apply");
  apply_resolve_ps_ = MakeShader(device_, kApplyResolve, nvrhi::ShaderType::Pixel, "ps_apply_resolve");
  if (!ok || !vs_ || !apply_ps_ || !apply_resolve_ps_) {
    Logf(LogLevel::kWarning, "rexgpu-native: GTAO: its shaders could not be created, the GTAO method is unavailable");
    return false;
  }
  auto variant = [&](Pass& pass, int id, const char* entry) {
    pass.shader = MakeShader(device_, id, nvrhi::ShaderType::Compute, entry);
    pass.layout = main_.layout;
    nvrhi::ComputePipelineDesc pd;
    pd.CS = pass.shader;
    pd.bindingLayouts = {pass.layout};
    if (pass.shader) pass.pipeline = device_->createComputePipeline(pd);
    return pass.pipeline != nullptr;
  };
  if (!variant(main_low_, kMainLow, "cs_main_low") || !variant(main_high_, kMainHigh, "cs_main_high")) return false;
  nvrhi::BindingLayoutDesc al;
  al.visibility = nvrhi::ShaderType::All;
  al.bindings = {cb, I::Texture_SRV(1)};
  apply_layout_ = device_->createBindingLayout(al);
  al.bindings = {cb, I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2)};
  apply_resolve_layout_ = device_->createBindingLayout(al);
  nvrhi::SamplerDesc sd;
  sd.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
  point_clamp_ = device_->createSampler(sd);
  nvrhi::BufferDesc bd;
  bd.byteSize = sizeof(Constants);
  bd.isConstantBuffer = true;
  bd.isVolatile = true;
  bd.maxVersions = 256;
  bd.debugName = "GTAO constants";
  constants_ = device_->createBuffer(bd);
  ready_ = apply_layout_ && apply_resolve_layout_ && point_clamp_ && constants_;
  return ready_;
}

nvrhi::TextureHandle Gtao::MakeImage(uint32_t width, uint32_t height, nvrhi::Format format, uint32_t mips, bool uav,
                                     bool render_target, const char* name) {
  nvrhi::TextureDesc td;
  td.width = std::max(width, 1u);
  td.height = std::max(height, 1u);
  td.mipLevels = mips;
  td.format = format;
  td.isUAV = uav;
  td.isRenderTarget = render_target;
  td.initialState = nvrhi::ResourceStates::ShaderResource;
  td.keepInitialState = true;
  td.debugName = name;
  return device_->createTexture(td);
}

bool Gtao::EnsureImages(uint32_t pw, uint32_t ph, uint32_t ow, uint32_t oh, bool temporal) {
  if (pw == padded_w_ && ph == padded_h_ && ow == out_w_ && oh == out_h_ && output_ &&
      (!temporal || (spatial_ && temporal_depth_ && history_ao_ && history_depth_))) {
    if (!temporal && spatial_) {
      // A live switch to an SDK delegates history to it. Drop both the local images
      // and the binding sets retaining them, while keeping the spatial working images.
      sets_.clear();
      spatial_ = temporal_depth_ = history_ao_ = history_depth_ = nullptr;
      history_frame_ = UINT64_MAX;
    }
    return true;
  }
  padded_w_ = padded_h_ = out_w_ = out_h_ = 0;
  fixed_sets_.clear();
  sets_.clear();
  raw_ = MakeImage(pw, ph, nvrhi::Format::R32_FLOAT, 1, true, false, "GTAO stored depth");
  depth_mips_ = MakeImage(pw, ph, nvrhi::Format::R32_FLOAT, XE_GTAO_DEPTH_MIP_LEVELS, true, false, "GTAO view depth");
  normal_map_ = MakeImage(pw, ph, nvrhi::Format::R32_UINT, 1, true, false, "GTAO normals");
  term_a_ = MakeImage(pw, ph, nvrhi::Format::R32_UINT, 1, true, false, "GTAO term a");
  term_b_ = MakeImage(pw, ph, nvrhi::Format::R32_UINT, 1, true, false, "GTAO term b");
  edges_ = MakeImage(pw, ph, nvrhi::Format::R32_FLOAT, 1, true, false, "GTAO edges");
  output_ = MakeImage(ow, oh, nvrhi::Format::R32_FLOAT, 1, true, false, "GTAO occlusion");
  spatial_ = temporal ? MakeImage(ow, oh, nvrhi::Format::R32_FLOAT, 1, true, false, "GTAO spatial result") : nullptr;
  temporal_depth_ = temporal ? MakeImage(ow, oh, nvrhi::Format::R32_FLOAT, 1, true, false, "GTAO temporal depth") : nullptr;
  history_ao_ = temporal ? MakeImage(ow, oh, nvrhi::Format::R32_FLOAT, 1, true, false, "GTAO history") : nullptr;
  history_depth_ = temporal ? MakeImage(ow, oh, nvrhi::Format::R32_FLOAT, 1, true, false, "GTAO history depth") : nullptr;
  history_frame_ = UINT64_MAX;
  if (!raw_ || !depth_mips_ || !normal_map_ || !term_a_ || !term_b_ || !edges_ || !output_ ||
      (temporal && (!spatial_ || !temporal_depth_ || !history_ao_ || !history_depth_))) {
    output_ = nullptr;
    return false;
  }
  using S = nvrhi::BindingSetItem;
  const nvrhi::TextureSubresourceSet mip0(0, 1, 0, 1), all_mips(0, XE_GTAO_DEPTH_MIP_LEVELS, 0, 1);
  const auto cb = S::ConstantBuffer(0, constants_);
  auto uav_mip = [&](uint32_t slot, uint32_t mip) {
    return S::Texture_UAV(slot, depth_mips_, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(mip, 1, 0, 1));
  };
  fixed_sets_ = {
      Set(prefilter_, {cb, S::Texture_SRV(0, raw_), uav_mip(0, 0), uav_mip(1, 1), uav_mip(2, 2), uav_mip(3, 3),
                       uav_mip(4, 4), S::Sampler(0, point_clamp_)}),
      Set(normals_, {cb, S::Texture_SRV(0, raw_), S::Texture_UAV(0, normal_map_), S::Sampler(0, point_clamp_)}),
      Set(main_, {cb, S::Texture_SRV(0, depth_mips_, nvrhi::Format::UNKNOWN, all_mips), S::Texture_SRV(1, normal_map_),
                  S::Texture_UAV(0, term_a_), S::Texture_UAV(1, edges_), S::Sampler(0, point_clamp_)}),
      Set(denoise_, {cb, S::Texture_SRV(0, term_a_), S::Texture_SRV(1, edges_), S::Texture_UAV(0, term_b_),
                     S::Sampler(0, point_clamp_)}),
      Set(denoise_last_, {cb, S::Texture_SRV(0, term_b_), S::Texture_SRV(1, edges_), S::Texture_UAV(0, term_a_),
                          S::Sampler(0, point_clamp_)}),
  };
  for (const nvrhi::BindingSetHandle& s : fixed_sets_) {
    if (!s) {
      output_ = nullptr;
      return false;
    }
  }
  (void)mip0;
  padded_w_ = pw;
  padded_h_ = ph;
  out_w_ = ow;
  out_h_ = oh;
  return true;
}

nvrhi::BindingSetHandle Gtao::Set(Pass& pass, const std::vector<nvrhi::BindingSetItem>& items) {
  nvrhi::BindingSetDesc bs;
  bs.bindings = items;
  return device_->createBindingSet(bs, pass.layout);
}

void Gtao::Dispatch(nvrhi::ICommandList* cl, Pass& pass, nvrhi::IBindingSet* set, uint32_t x, uint32_t y) {
  nvrhi::ComputeState cs;
  cs.pipeline = pass.pipeline;
  cs.bindings = {set};
  cl->setComputeState(cs);
  cl->dispatch(std::max(x, 1u), std::max(y, 1u), 1);
}

void Gtao::FillConstants(Constants& k, const GtaoInput& in, const GtaoSettings& s, uint32_t pw, uint32_t ph,
                         uint32_t step) const {
  const AoViewMapping m = ComputeAoViewMapping(in.projection, in.screen, in.x, in.y, pw, ph, step);
  XeGTAO::GTAOConstants& g = k.g;
  g.ViewportSize = {int(pw), int(ph)};
  g.ViewportPixelSize = {1.0f / float(pw), 1.0f / float(ph)};
  g.DepthUnpackConsts = {m.depth_mul, m.depth_add};
  g.CameraTanHalfFOV = {1.0f / in.projection.px, 1.0f / in.projection.py};
  g.NDCToViewMul = {m.mul[0], m.mul[1]};
  g.NDCToViewAdd = {m.add[0], m.add[1]};
  g.NDCToViewMul_x_PixelSize = {m.mul[0] / float(pw), m.mul[1] / float(ph)};
  const float radius = s.radius > 0.0f
                           ? s.radius
                           : RadiusInNearPlanes(s.strength) * in.projection.near_plane() * std::max(s.radius_scale, 0.01f);
  g.EffectRadius = radius;
  g.EffectFalloffRange = s.falloff > 0.0f ? std::min(s.falloff, 1.0f) : XE_GTAO_DEFAULT_FALLOFF_RANGE;
  g.RadiusMultiplier = XE_GTAO_DEFAULT_RADIUS_MULTIPLIER;
  g.Padding0 = 0;
  g.FinalValuePower = s.power > 0.0f ? s.power : FinalPower(s.strength);
  g.DenoiseBlurBeta = 1.2f;  // XeGTAO's value with denoising on
  g.SampleDistributionPower = XE_GTAO_DEFAULT_SAMPLE_DISTRIBUTION_POWER;
  g.ThinOccluderCompensation = XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION;
  g.DepthMIPSamplingOffset = XE_GTAO_DEFAULT_DEPTH_MIP_SAMPLING_OFFSET;
  g.NoiseIndex = (s.temporal_upscaler || (s.temporal && in.reprojection_valid)) && in.frame != UINT64_MAX
      ? uint32_t(in.frame % 64) : 0;
  k.source[0] = in.x;
  k.source[1] = in.y;
  k.source[2] = int32_t(in.width);
  k.source[3] = int32_t(in.height);
  k.working[0] = (in.width + step - 1) / step;
  k.working[1] = (in.height + step - 1) / step;
  k.working[2] = pw;
  k.working[3] = ph;
  k.params[0] = float(step);
  // Cleared depth (the far plane's stored value or beyond) is sky: no occlusion there.
  const float zs = in.screen.z_scale, zo = in.screen.z_offset;
  k.params[1] = zs > 0.0f ? zo + zs * (1.0f - 1e-6f) : 2.0f;
  k.params[2] = float(s.mode);
  k.fade[0] = s.fade > 0.0f ? s.fade * in.projection.near_plane() : 0.0f;
  for (size_t i = 0; i < 16; ++i) k.reprojection[i] = float(in.reprojection[i]);
  k.viewport[0] = in.screen.x_scale;
  k.viewport[1] = in.screen.x_offset + in.screen.offset_x();
  k.viewport[2] = in.screen.y_scale;
  k.viewport[3] = in.screen.y_offset + in.screen.offset_y();
  k.depth_scale[0] = in.screen.z_scale;
  k.depth_scale[1] = in.screen.z_offset;
  k.depth_scale[2] = in.screen.host_scale_x;
  k.depth_scale[3] = in.screen.host_scale_y;
  k.temporal[3] = float(std::clamp(s.sample_quality, 1, 3));
}

nvrhi::ITexture* Gtao::Compute(nvrhi::ICommandList* cl, const GtaoInput& in, const GtaoSettings& s) {
  if (!ready_ || !in.depth || in.width < 2 || in.height < 2 || !in.projection.valid()) return nullptr;
  const nvrhi::TextureDesc& dd = in.depth->getDesc();
  if (in.x < 0 || in.y < 0 || uint32_t(in.x) + in.width > dd.width || uint32_t(in.y) + in.height > dd.height)
    return nullptr;
  const uint32_t step = s.half ? 2 : 1;
  const uint32_t pw = RoundUp16((in.width + step - 1) / step), ph = RoundUp16((in.height + step - 1) / step);
  if (!EnsureImages(pw, ph, in.width, in.height, s.temporal)) return nullptr;
  origin_x_ = in.x;
  origin_y_ = in.y;
  Constants k;
  FillConstants(k, in, s, pw, ph, step);
  // A contiguous frame with the same AO and viewport can reuse history. Cuts and unmatched
  // cameras fall back to this frame's spatial result, without animating unsupported noise.
  const bool same = history_settings_.mode == s.mode && history_settings_.strength == s.strength &&
      history_settings_.half == s.half && history_settings_.sample_quality == s.sample_quality &&
      history_settings_.radius == s.radius && history_settings_.radius_scale == s.radius_scale &&
      history_settings_.falloff == s.falloff && history_settings_.power == s.power && history_settings_.fade == s.fade &&
      history_x_ == in.x && history_y_ == in.y &&
      history_screen_.x_scale == in.screen.x_scale && history_screen_.y_scale == in.screen.y_scale &&
      history_screen_.x_offset == in.screen.x_offset && history_screen_.y_offset == in.screen.y_offset &&
      history_screen_.host_scale_x == in.screen.host_scale_x && history_screen_.host_scale_y == in.screen.host_scale_y &&
      history_screen_.z_scale == in.screen.z_scale && history_screen_.z_offset == in.screen.z_offset &&
      history_screen_.window_x == in.screen.window_x && history_screen_.window_y == in.screen.window_y &&
      history_screen_.half_pixel == in.screen.half_pixel;
  if (s.temporal && in.reprojection_valid && history_frame_ != UINT64_MAX &&
      in.frame != UINT64_MAX && in.frame == history_frame_ + 1 && same) {
    k.temporal[0] = 0.8f;
    ++stats_.history_frames;
  }
  if (history_frame_ != UINT64_MAX) {
    const AoViewMapping prev = ComputeAoViewMapping(history_projection_, history_screen_, in.x, in.y, pw, ph, step);
    k.temporal[1] = prev.depth_mul;
    k.temporal[2] = prev.depth_add;
  }
  constants_bytes_.assign(reinterpret_cast<const uint8_t*>(&k), reinterpret_cast<const uint8_t*>(&k) + sizeof(k));
  cl->writeBuffer(constants_, &k, sizeof(k));

  if (sets_.size() > 32) sets_.clear();
  using S = nvrhi::BindingSetItem;
  const auto cb = S::ConstantBuffer(0, constants_);
  nvrhi::BindingSetHandle& prepare = sets_[{kSetPrepare, in.depth, nullptr}];
  if (!prepare) prepare = Set(prepare_, {cb, S::Texture_SRV(0, in.depth), S::Texture_UAV(0, raw_)});
  nvrhi::BindingSetHandle& output = sets_[{kSetOutput, in.depth, s.temporal ? spatial_.Get() : output_.Get()}];
  if (!output) {
    output = Set(output_pass_, {cb, S::Texture_SRV(0, term_a_), S::Texture_SRV(1, in.depth),
                                S::Texture_SRV(2, depth_mips_, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                                S::Texture_UAV(0, s.temporal ? spatial_ : output_)});
  }
  if (!prepare || !output) return nullptr;

  Dispatch(cl, prepare_, prepare, pw / 8, ph / 8);
  Dispatch(cl, prefilter_, fixed_sets_[0], pw / 16, ph / 16);
  Dispatch(cl, normals_, fixed_sets_[1], pw / XE_GTAO_NUMTHREADS_X, ph / XE_GTAO_NUMTHREADS_Y);
  Pass& main = s.sample_quality == 1 ? main_low_ : s.sample_quality == 3 ? main_high_ : main_;
  Dispatch(cl, main, fixed_sets_[2], pw / XE_GTAO_NUMTHREADS_X, ph / XE_GTAO_NUMTHREADS_Y);
  Dispatch(cl, denoise_, fixed_sets_[3], pw / (XE_GTAO_NUMTHREADS_X * 2), ph / XE_GTAO_NUMTHREADS_Y);
  Dispatch(cl, denoise_last_, fixed_sets_[4], pw / (XE_GTAO_NUMTHREADS_X * 2), ph / XE_GTAO_NUMTHREADS_Y);
  Dispatch(cl, output_pass_, output, (in.width + 7) / 8, (in.height + 7) / 8);
  if (s.temporal) {
    // Define the initial history even on APIs that do not zero newly allocated textures.
    if (history_frame_ == UINT64_MAX) {
      cl->clearTextureFloat(history_ao_, nvrhi::AllSubresources, nvrhi::Color(1.0f));
      cl->clearTextureFloat(history_depth_, nvrhi::AllSubresources, nvrhi::Color(1.0f));
    }
    nvrhi::BindingSetHandle& temporal = sets_[{kSetTemporal, in.depth, nullptr}];
    if (!temporal) temporal = Set(temporal_, {cb, S::Texture_SRV(0, spatial_), S::Texture_SRV(1, in.depth),
        S::Texture_SRV(2, history_ao_), S::Texture_SRV(3, history_depth_), S::Texture_UAV(0, output_),
        S::Texture_UAV(1, temporal_depth_)});
    if (!temporal) return nullptr;
    Dispatch(cl, temporal_, temporal, (in.width + 7) / 8, (in.height + 7) / 8);
    cl->copyTexture(history_ao_, nvrhi::TextureSlice(), output_, nvrhi::TextureSlice());
    cl->copyTexture(history_depth_, nvrhi::TextureSlice(), temporal_depth_, nvrhi::TextureSlice());
    history_frame_ = in.frame;
    history_settings_ = s;
    history_screen_ = in.screen;
    history_projection_ = in.projection;
    history_x_ = in.x;
    history_y_ = in.y;
  } else {
    history_frame_ = UINT64_MAX;
  }
  // Read next through binding sets (the apply passes) or the bindless tables (ambient-only), which
  // NVRHI does not track: leave it a shader resource.
  cl->setTextureState(output_, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  cl->commitBarriers();
  ++stats_.computed;
  return output_;
}

nvrhi::IFramebuffer* Gtao::Framebuffer(nvrhi::ITexture* color) {
  auto it = framebuffers_.find(color);
  if (it != framebuffers_.end()) return it->second;
  if (framebuffers_.size() > 16) framebuffers_.clear();
  nvrhi::FramebufferDesc fd;
  fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(color));
  nvrhi::FramebufferHandle fb = device_->createFramebuffer(fd);
  framebuffers_[color] = fb;
  return fb;
}

nvrhi::IGraphicsPipeline* Gtao::GraphicsPipeline(bool resolve, int mode, nvrhi::IFramebuffer* fb) {
  const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
  const uint32_t format = info.colorFormats.empty() ? 0 : uint32_t(info.colorFormats[0]);
  nvrhi::GraphicsPipelineHandle& p = pipelines_[{resolve, resolve ? 0 : (mode >= 2 ? 2 : 1), format}];
  if (p) return p;
  nvrhi::GraphicsPipelineDesc pd;
  pd.primType = nvrhi::PrimitiveType::TriangleList;
  pd.VS = vs_;
  pd.PS = resolve ? apply_resolve_ps_ : apply_ps_;
  pd.bindingLayouts = {resolve ? apply_resolve_layout_ : apply_layout_};
  pd.renderState.depthStencilState.depthTestEnable = false;
  pd.renderState.depthStencilState.depthWriteEnable = false;
  pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  nvrhi::BlendState::RenderTarget& t = pd.renderState.blendState.targets[0];
  if (!resolve) {
    // The alpha channel is the game's (it may hold something the later passes read).
    t.colorWriteMask = nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue;
    if (mode < 2) {
      // destination * source: the scene times the occlusion, without reading the target.
      t.blendEnable = true;
      t.srcBlend = nvrhi::BlendFactor::Zero;
      t.destBlend = nvrhi::BlendFactor::SrcColor;
      t.blendOp = nvrhi::BlendOp::Add;
      t.srcBlendAlpha = nvrhi::BlendFactor::Zero;
      t.destBlendAlpha = nvrhi::BlendFactor::One;
      t.blendOpAlpha = nvrhi::BlendOp::Add;
    }
  }
  p = device_->createGraphicsPipeline(pd, info);
  return p;
}

bool Gtao::ApplyToTarget(nvrhi::ICommandList* cl, nvrhi::ITexture* color, int mode) {
  if (!ready_ || !output_ || !color || constants_bytes_.size() != sizeof(Constants)) return false;
  const nvrhi::TextureDesc& cd = color->getDesc();
  const int32_t x0 = std::max(origin_x_, 0), y0 = std::max(origin_y_, 0);
  const int32_t x1 = std::min(origin_x_ + int32_t(out_w_), int32_t(cd.width));
  const int32_t y1 = std::min(origin_y_ + int32_t(out_h_), int32_t(cd.height));
  if (x1 <= x0 || y1 <= y0) return false;
  nvrhi::IFramebuffer* fb = Framebuffer(color);
  nvrhi::IGraphicsPipeline* pipeline = fb ? GraphicsPipeline(false, mode, fb) : nullptr;
  if (!pipeline) return false;
  Constants k;
  std::memcpy(&k, constants_bytes_.data(), sizeof(k));
  k.params[2] = float(mode);
  cl->writeBuffer(constants_, &k, sizeof(k));
  nvrhi::BindingSetHandle& set = sets_[{kSetApply, output_.Get(), nullptr}];
  if (!set) {
    nvrhi::BindingSetDesc bs;
    bs.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, constants_), nvrhi::BindingSetItem::Texture_SRV(1, output_)};
    set = device_->createBindingSet(bs, apply_layout_);
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

nvrhi::ITexture* Gtao::ApplyToCopy(nvrhi::ICommandList* cl, nvrhi::ITexture* color, nvrhi::ITexture* depth,
                                   const AoFog& fog, int mode) {
  if (!ready_ || !output_ || !color || !depth || constants_bytes_.size() != sizeof(Constants)) return nullptr;
  const nvrhi::TextureDesc& cd = color->getDesc();
  if (!scratch_ || scratch_->getDesc().width != cd.width || scratch_->getDesc().height != cd.height ||
      scratch_->getDesc().format != cd.format) {
    framebuffers_.clear();
    sets_.clear();
    scratch_ = MakeImage(cd.width, cd.height, cd.format, 1, false, true, "GTAO scene colour");
    if (!scratch_) return nullptr;
  }
  nvrhi::IFramebuffer* fb = Framebuffer(scratch_);
  nvrhi::IGraphicsPipeline* pipeline = fb ? GraphicsPipeline(true, mode, fb) : nullptr;
  if (!pipeline) return nullptr;
  Constants k;
  std::memcpy(&k, constants_bytes_.data(), sizeof(k));
  k.params[2] = float(mode);
  if (fog.valid) {
    k.fog_norm[0] = fog.normalizing[0];
    k.fog_norm[1] = fog.normalizing[1];
    k.fog_norm[2] = fog.normalizing[2];
    k.fog_norm[3] = 1.0f;
    k.fog_par[0] = fog.near_over_distance;
    k.fog_par[1] = fog.density;
    k.fog_par[2] = fog.inverse_distance;
    k.fog_col[0] = fog.color[0];
    k.fog_col[1] = fog.color[1];
    k.fog_col[2] = fog.color[2];
  }
  cl->writeBuffer(constants_, &k, sizeof(k));
  if (sets_.size() > 32) sets_.clear();
  nvrhi::BindingSetHandle& set = sets_[{kSetApplyResolve, color, depth}];
  if (!set) {
    using S = nvrhi::BindingSetItem;
    nvrhi::BindingSetDesc bs;
    bs.bindings = {S::ConstantBuffer(0, constants_), S::Texture_SRV(0, color, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   S::Texture_SRV(1, output_), S::Texture_SRV(2, depth, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1))};
    set = device_->createBindingSet(bs, apply_resolve_layout_);
  }
  if (!set) return nullptr;
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {set};
  gs.viewport.addViewport(nvrhi::Viewport(float(cd.width), float(cd.height)));
  gs.viewport.addScissorRect(nvrhi::Rect(0, int(cd.width), 0, int(cd.height)));
  cl->setGraphicsState(gs);
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
  ++stats_.copies;
  return scratch_;
}

nvrhi::ITexture* Gtao::CopyDepth(nvrhi::ICommandList* cl, nvrhi::ITexture* depth) {
  if (!ready_ || !depth) return nullptr;
  const nvrhi::TextureDesc& dd = depth->getDesc();
  if (!depth_copy_ || depth_copy_->getDesc().width != dd.width || depth_copy_->getDesc().height != dd.height) {
    sets_.clear();
    depth_copy_ = MakeImage(dd.width, dd.height, nvrhi::Format::R32_FLOAT, 1, true, false, "GTAO depth copy");
    if (!depth_copy_) return nullptr;
  }
  Constants k;
  k.source[2] = int32_t(dd.width);
  k.source[3] = int32_t(dd.height);
  k.working[0] = k.working[2] = dd.width;
  k.working[1] = k.working[3] = dd.height;
  k.params[0] = 1.0f;
  cl->writeBuffer(constants_, &k, sizeof(k));
  if (sets_.size() > 32) sets_.clear();
  nvrhi::BindingSetHandle& set = sets_[{kSetCopyDepth, depth, depth_copy_.Get()}];
  if (!set) {
    using S = nvrhi::BindingSetItem;
    set = Set(prepare_, {S::ConstantBuffer(0, constants_), S::Texture_SRV(0, depth), S::Texture_UAV(0, depth_copy_)});
  }
  if (!set) return nullptr;
  Dispatch(cl, prepare_, set, (dd.width + 7) / 8, (dd.height + 7) / 8);
  ++stats_.depth_copies;
  return depth_copy_;
}

}  // namespace nr
