// The game renderer's temporal upscalers (docs/upscalers.md; the state is GameUpscale, game_upscale.h).
//
// With --native_upscaler on, every frame of a scene goes like this:
//  - Before the upscale point (UpscaleFrameTracker, upscale_frame.h) the frame-size targets are the
//    input generation, the upscaler's input size; every depth-tested draw into them is jittered by
//    this frame's Halton offset (through the draw constants' ndcOffset), samples its textures with
//    the mip bias, and a draw that writes the scene's depth is recorded for the motion pass with its
//    vertex constants.
//  - After the opaque pass and its fog, a copy of the scene colour (the reactive mask's reference).
//  - At the upscale point (RunUpscale): the camera's motion of every pixel from the depth, then the
//    recorded draws again with their vertex shaders' motion variants (this frame's and last frame's
//    constants) where their depth is the scene's, the reactive mask, the upscaler (or the bilinear
//    stretch), the result into the output generation's scene target, the depth carried over, and the
//    full-frame copies of the scene made since its last draw done again from the upscaled picture
//    (the post effects read them).
//  - After it the frame-size targets are the output generation: the post effects, the HUD and Present
//    at the output size, nothing jittered. A target first used there with this frame's picture in its
//    input twin gets that picture stretched (MigrateTarget).
// Frames without a scene (menus, movies) are drawn at the output size from their start.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "backend/ao_camera.h"
#include "backend/game_renderer.h"
#include "backend/guest_layout.h"
#include "backend/log.h"
#include "backend/shaders/generated/up_camera_ps_dxil.h"
#include "backend/shaders/generated/up_camera_ps_spirv.h"
#include "backend/shaders/generated/up_depth_ps_dxil.h"
#include "backend/shaders/generated/up_depth_ps_spirv.h"
#include "backend/shaders/generated/up_object_ps_dxil.h"
#include "backend/shaders/generated/up_object_ps_spirv.h"
#include "backend/shaders/generated/up_reactive_ps_dxil.h"
#include "backend/shaders/generated/up_reactive_ps_spirv.h"
#include "backend/shaders/generated/up_show_motion_ps_dxil.h"
#include "backend/shaders/generated/up_show_motion_ps_spirv.h"
#include "backend/shaders/generated/up_stretch_ps_dxil.h"
#include "backend/shaders/generated/up_stretch_ps_spirv.h"
#include "backend/shaders/generated/up_vs_dxil.h"
#include "backend/shaders/generated/up_vs_spirv.h"

namespace nr {

namespace {

// upscale.hlsl's UpscaleParams (push constants).
struct UpscaleParams {
  float rows[4][4] = {};
  float p0[4] = {}, p1[4] = {}, p2[4] = {};
};
static_assert(sizeof(UpscaleParams) == 112, "UpscaleParams must match upscale.hlsl");

inline uint64_t Mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 29);
}

// The parts of a target key (game_resources.cpp TargetKey).
struct KeyParts {
  bool depth;
  uint32_t base, pitch, format;
};
KeyParts DecodeKey(uint64_t key) {
  return {(key >> 63) != 0, uint32_t(key & 0xFFF), uint32_t((key >> 16) & 0x3FFF), uint32_t((key >> 40) & 0xFF)};
}

uint32_t RoundSize(uint32_t guest, float scale) {
  return uint32_t(std::max(1L, std::lround(double(guest) * double(scale))));
}

}  // namespace

// ---------------------------------------------------------------- setup

bool GameRenderer::UpscaleSetup(PreparedUpscaler* prepared) {
  auto u = std::make_unique<GameUpscale>();
  u->config = options_.upscale;
  u->out_x = options_.render_scale_x;
  u->out_y = options_.render_scale_y;
  const UpscalerSize out{RoundSize(options_.guest_frame_width, u->out_x), RoundSize(options_.guest_frame_height, u->out_y)};
  std::string why;
  if (u->config.kind == UpscalerKind::kBilinear) {
    u->kind = UpscalerKind::kBilinear;
    u->name = "bilinear stretch";
  } else {
    UpscalerKind chosen = UpscalerKind::kOff;
    if (prepared) {
      // Made on a worker while the game kept drawing (a live change, game_effects.cpp).
      u->upscaler = std::move(prepared->upscaler);
      chosen = prepared->chosen;
      why = prepared->why;
    } else {
      u->upscaler = CreateUpscaler(device_, u->config, &chosen, &why);
    }
    if (u->upscaler) {
      u->kind = chosen;
      u->name = u->upscaler->Name();
    } else {
      // Nothing the SDKs offer runs here: the same frame plumbing with a plain stretch, so the setting
      // never leaves the player without a picture.
      u->kind = UpscalerKind::kBilinear;
      u->name = "bilinear stretch (no upscaler available)";
      Logf(LogLevel::kWarning, "rexgpu-native: upscaler %s: none available (%s); a bilinear stretch instead",
           UpscalerKindName(u->config.kind), why.c_str());
    }
  }
  UpscalerSize in;
  if (u->upscaler) {
    in = u->upscaler->InputSize(out);
  } else {
    const float ratio = DefaultUpscaleRatio(u->config.quality);
    in = {UpscaleInputSize(out.width, ratio), UpscaleInputSize(out.height, ratio)};
  }
  in.width = std::clamp(in.width, 1u, out.width);
  in.height = std::clamp(in.height, 1u, out.height);
  u->in_x = float(in.width) / float(options_.guest_frame_width);
  u->in_y = float(in.height) / float(options_.guest_frame_height);
  // A scale the targets round back to the same sizes (ScaleCoord rounds guest * scale).
  if (RoundSize(options_.guest_frame_width, u->in_x) != in.width) u->in_x = (float(in.width) + 0.25f) / float(options_.guest_frame_width);
  if (RoundSize(options_.guest_frame_height, u->in_y) != in.height) u->in_y = (float(in.height) + 0.25f) / float(options_.guest_frame_height);
  const float ratio_x = float(out.width) / float(in.width), ratio_y = float(out.height) / float(in.height);
  u->jitter_phases = JitterPhaseCount(ratio_x, ratio_y);
  u->ratio = std::max(ratio_x, ratio_y);
  u->mip_bias = UpscaleMipBiasFor(*u);
  u->tracker = UpscaleFrameTracker(u->config.point);
  if (!u->config.rules_file.empty()) {
    std::ifstream f(u->config.rules_file, std::ios::binary);
    std::stringstream text;
    text << f.rdbuf();
    std::vector<std::string> errors;
    u->rules.AddFile(text.str(), &errors);
    for (const std::string& e : errors) Logf(LogLevel::kWarning, "rexgpu-native: upscaler rules %s: %s", u->config.rules_file.c_str(), e.c_str());
  }
  if (options_.fxaa) {
    Logf(LogLevel::kInfo, "rexgpu-native: upscaler: FXAA is off (the upscaler anti-aliases)");
    options_.fxaa = 0;
  }
  Logf(LogLevel::kInfo,
       "rexgpu-native: upscaler %s (asked: %s): %ux%u to %ux%u (%.3fx), jitter %u phases, mip bias %.3f, point %s%s%s",
       u->name.c_str(), DescribeUpscaleConfig(u->config).c_str(), in.width, in.height, out.width, out.height, ratio_x,
       u->jitter_phases, u->mip_bias, u->config.point == UpscalePoint::kPost ? "before the post effects" : "Present",
       why.empty() ? "" : "; ", why.c_str());
  upscale_ = std::move(u);
  return true;
}

float GameRenderer::UpscaleMipBiasFor(const GameUpscale& u) {
  if (u.config.debug & 64) return 0.0f;
  return u.config.mip_bias_auto ? UpscaleMipBias(u.ratio) : u.config.mip_bias;
}

bool GameRenderer::UpscaleInitialize() {
  GameUpscale& u = *upscale_;
  u.tracker.set_rules(&u.rules);
  u.tracker.set_classifier(&u.classifier);
  if (upscale_passes_.pass_layout) {
    // Made again for a live change: the passes of the first one (their pipelines are keyed by these
    // shaders, so they are made once).
    const UpscalePasses& p = upscale_passes_;
    u.vs_fullscreen = p.vs_fullscreen;
    u.camera_ps = p.camera_ps;
    u.object_ps = p.object_ps;
    u.reactive_ps = p.reactive_ps;
    u.depth_ps = p.depth_ps;
    u.stretch_ps = p.stretch_ps;
    u.show_motion_ps = p.show_motion_ps;
    u.pass_layout = p.pass_layout;
    u.motion_layout = p.motion_layout;
    u.motion_set = p.motion_set;
    u.motion_vs_constants = p.motion_vs_constants;
    u.motion_draw_constants = p.motion_draw_constants;
    u.motion_constants = p.motion_constants;
    u.linear_clamp = p.linear_clamp;
    UpscaleInitializeTimers();
    return true;
  }
  const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  auto shader = [&](const void* dxil, size_t dxil_size, const void* spv, size_t spv_size, nvrhi::ShaderType type,
                    const char* entry, const char* name) {
    nvrhi::ShaderDesc d;
    d.shaderType = type;
    d.entryName = entry;
    d.debugName = name;
    return spirv ? device_->createShader(d, spv, spv_size) : device_->createShader(d, dxil, dxil_size);
  };
#define NR_UP_SHADER(var, blob, type, entry) \
  u.var = shader(k_##blob##_dxil, sizeof(k_##blob##_dxil), k_##blob##_spirv, sizeof(k_##blob##_spirv), type, entry, #blob)
  NR_UP_SHADER(vs_fullscreen, up_vs, nvrhi::ShaderType::Vertex, "vs_main");
  NR_UP_SHADER(camera_ps, up_camera_ps, nvrhi::ShaderType::Pixel, "ps_camera_motion");
  NR_UP_SHADER(object_ps, up_object_ps, nvrhi::ShaderType::Pixel, "ps_object_motion");
  NR_UP_SHADER(reactive_ps, up_reactive_ps, nvrhi::ShaderType::Pixel, "ps_reactive");
  NR_UP_SHADER(depth_ps, up_depth_ps, nvrhi::ShaderType::Pixel, "ps_depth_upsample");
  NR_UP_SHADER(stretch_ps, up_stretch_ps, nvrhi::ShaderType::Pixel, "ps_stretch");
  NR_UP_SHADER(show_motion_ps, up_show_motion_ps, nvrhi::ShaderType::Pixel, "ps_show_motion");
#undef NR_UP_SHADER
  if (!u.vs_fullscreen || !u.camera_ps || !u.object_ps || !u.reactive_ps || !u.depth_ps || !u.stretch_ps ||
      !u.show_motion_ps)
    return false;

  nvrhi::BindingLayoutDesc pl;
  pl.visibility = nvrhi::ShaderType::All;
  pl.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(UpscaleParams)),
                 nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
                 nvrhi::BindingLayoutItem::Sampler(0)};
  u.pass_layout = device_->createBindingLayout(pl);
  // Set 0 of the motion pass: this frame's constants (b0), the draw constants (b2), last frame's (b5).
  nvrhi::BindingLayoutDesc ml;
  ml.visibility = nvrhi::ShaderType::All;
  ml.registerSpace = 0;
  ml.bindingOffsets = nvrhi::VulkanBindingOffsets()
                          .setShaderResourceOffset(0)
                          .setSamplerOffset(0)
                          .setConstantBufferOffset(0)
                          .setUnorderedAccessViewOffset(0);
  ml.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0), nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),
                 nvrhi::BindingLayoutItem::VolatileConstantBuffer(kkshaders::kMotionConstantsRegister)};
  u.motion_layout = device_->createBindingLayout(ml);
  if (!u.pass_layout || !u.motion_layout) return false;
  auto make_cb = [&](uint32_t size, const char* name) {
    nvrhi::BufferDesc d;
    d.byteSize = size;
    d.isConstantBuffer = true;
    d.isVolatile = true;
    d.maxVersions = 8192;
    d.debugName = name;
    return device_->createBuffer(d);
  };
  u.motion_vs_constants = make_cb(4096, "Motion pass constants");
  u.motion_draw_constants = make_cb(sizeof(kkshaders::DrawConstants), "Motion pass draw constants");
  u.motion_constants = make_cb(sizeof(kkshaders::MotionConstants), "Motion pass previous constants");
  if (!u.motion_vs_constants || !u.motion_draw_constants || !u.motion_constants) return false;
  nvrhi::BindingSetDesc ms;
  ms.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, u.motion_vs_constants),
                 nvrhi::BindingSetItem::ConstantBuffer(2, u.motion_draw_constants),
                 nvrhi::BindingSetItem::ConstantBuffer(kkshaders::kMotionConstantsRegister, u.motion_constants)};
  u.motion_set = device_->createBindingSet(ms, u.motion_layout);
  nvrhi::SamplerDesc sd;
  sd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
  u.linear_clamp = device_->createSampler(sd);
  if (!u.motion_set || !u.linear_clamp) return false;
  // These formats are independent of resolution and SDK choice. Compile the same
  // descriptions UpscalePass uses before any frame reaches an upscale point.
  nvrhi::FramebufferInfo info;
  info.colorFormats = {nvrhi::Format::RG16_FLOAT, nvrhi::Format::R32_FLOAT};
  if (!UpscalePipeline(u.camera_ps, info, nvrhi::ColorMask::All)) return false;
  info.colorFormats = {nvrhi::Format::R8_UNORM};
  if (!UpscalePipeline(u.reactive_ps, info, nvrhi::ColorMask::All)) return false;
  info.colorFormats = {};
  info.depthFormat = nvrhi::Format::D32S8;
  if (!UpscalePipeline(u.depth_ps, info, nvrhi::ColorMask::All)) return false;
  info.depthFormat = nvrhi::Format::UNKNOWN;
  for (uint32_t f = 0; f < 16; ++f) {
    const auto format = ColorTargetFormat(f);
    if (format == nvrhi::Format::UNKNOWN) continue;
    info.colorFormats = {format};
    if (!UpscalePipeline(u.stretch_ps, info, nvrhi::ColorMask::All) ||
        !UpscalePipeline(u.stretch_ps, info, nvrhi::ColorMask::Alpha) ||
        !UpscalePipeline(u.show_motion_ps, info, nvrhi::ColorMask::All)) return false;
  }
  UpscalePasses& p = upscale_passes_;
  p.vs_fullscreen = u.vs_fullscreen;
  p.camera_ps = u.camera_ps;
  p.object_ps = u.object_ps;
  p.reactive_ps = u.reactive_ps;
  p.depth_ps = u.depth_ps;
  p.stretch_ps = u.stretch_ps;
  p.show_motion_ps = u.show_motion_ps;
  p.pass_layout = u.pass_layout;
  p.motion_layout = u.motion_layout;
  p.motion_set = u.motion_set;
  p.motion_vs_constants = u.motion_vs_constants;
  p.motion_draw_constants = u.motion_draw_constants;
  p.motion_constants = u.motion_constants;
  p.linear_clamp = u.linear_clamp;
  UpscaleInitializeTimers();
  return true;
}

void GameRenderer::UpscaleInitializeTimers() {
  GameUpscale& u = *upscale_;
  if (u.config.gpu_time) {
    u.timer = std::make_unique<AoGpuTimer>(device_, "everything at the upscale point", "upscaler GPU time");
    u.sdk_timer = std::make_unique<AoGpuTimer>(device_, u.name.c_str(), "upscaler GPU time");
    u.motion_timer = std::make_unique<AoGpuTimer>(device_, "motion replay", "upscaler GPU time");
    u.timer->EnableTrace(".upscale-gpu.csv");
    u.sdk_timer->EnableTrace(".sdk-gpu.csv");
    u.motion_timer->EnableTrace(".motion-gpu.csv");
  } else if (options_.effect_settings) {
    // The graphics panel's readout only (no log lines).
    u.timer = std::make_unique<AoGpuTimer>(device_, "everything at the upscale point", "upscaler GPU time");
    u.timer->set_quiet(true);
  }
}

bool GameRenderer::OutputGeneration() const {
  return !upscale_ || upscale_->upscaled || upscale_->tracker.phase() != UpscaleFrameTracker::Phase::kScene;
}

void GameRenderer::UpscaleNewFrame(uint64_t frame) {
  GameUpscale& u = *upscale_;
  if (frame == u.frame) return;
  // A new frame (its first draw, clear or resolve): its jitter, and nothing of the last one's draws.
  u.frame = frame;
  u.upscaled = false;
  u.snapshot_due = u.snapshot_taken = false;
  u.screen_valid = false;
  u.camera_valid = false;
  u.motion_draws.clear();
  u.scene_copies.clear();
  u.history.BeginFrame(frame);
  u.vote.BeginFrame(frame);
  u.jitter = (u.config.debug & 8) ? Jitter{} : JitterForFrame(frame, u.jitter_phases);
  // The tracker starts its frame here too, so the phase is right before any target is taken.
  u.tracker.OnFrame(frame);
}

// ---------------------------------------------------------------- the frame's calls

GameRenderer::UpscaleDraw GameRenderer::UpscaleBeginDraw(nvrhi::ICommandList* cl, const DrawCall& call,
                                                         const dev::DeviceView& d, uint32_t pitch, uint32_t height,
                                                         uint64_t color_key, uint64_t depth_key, bool writes_color,
                                                         const GameShader& vs, const GameShader* ps) {
  GameUpscale& u = *upscale_;
  UpscaleNewFrame(call.frame);
  UpscaleFrameTracker::Draw td;
  td.frame = call.frame;
  const uint32_t depth_control = d.u32(dev::kRbDepthControl);
  td.state.color = writes_color;
  td.state.depth = depth_key != 0;
  td.state.depth_test = depth_key && (depth_control & 2);
  td.state.depth_write = td.state.depth_test && (depth_control & 4);
  td.state.alpha_test = (d.u32(dev::kRbColorControl) & 8) != 0;
  td.state.blend_control = d.u32(dev::kRbBlendControl0);
  td.state.primitive = uint32_t(call.primitive);
  td.state.vs_hash = vs.ucode_hash;
  td.state.ps_hash = ps ? ps->ucode_hash : 0;
  td.color_key = color_key;
  td.depth_key = depth_key;
  td.frame_size = pitch == options_.guest_frame_width;
  {
    // The window scissor (guest pixels) against the target: a draw into a corner of it.
    const uint32_t tl = d.u32(dev::kPaScWindowScissorTl), br = d.u32(dev::kPaScWindowScissorBr);
    int32_t x0 = int32_t(tl & 0x7FFF), y0 = int32_t((tl >> 16) & 0x7FFF);
    int32_t x1 = int32_t(br & 0x7FFF), y1 = int32_t((br >> 16) & 0x7FFF);
    if (!(tl & 0x80000000u)) {
      const uint32_t window_offset = d.u32(dev::kPaScWindowOffset);
      const int32_t wx = int32_t(window_offset << 17) >> 17, wy = int32_t((window_offset >> 16) << 17) >> 17;
      x0 += wx;
      x1 += wx;
      y0 += wy;
      y1 += wy;
    }
    const int32_t want_h = int32_t(std::min(height, options_.guest_frame_height));
    td.covers_target = x0 <= 0 && y0 <= 0 && x1 >= int32_t(pitch) && y1 >= want_h;
  }
  const UpscaleFrameTracker::DrawResult r = u.tracker.OnDraw(td);
  // The reactive mask's reference: the scene after its opaque pass and the fog. Taken before the draw
  // that ends the opaque pass, or, when that is the fog pass, before the first draw after it that is
  // not the fog.
  bool take = false;
  if (r.opaque_complete) {
    if (r.cls == AoDrawClass::kEnd) u.snapshot_due = true;
    else take = true;
  } else if (u.snapshot_due && r.in_scene && r.cls != AoDrawClass::kEnd) {
    take = true;
  }
  // A post draw needs its reference before RunUpscale; scene draws take it after the
  // AO, GI and volumetric hooks, so opaque lighting is not marked as reactive.
  if (take && r.upscale_before) UpscaleTakeSnapshot(cl);
  if (r.upscale_before) RunUpscale(cl, r.trigger);
  // A draw into the scene: the copies of it made so far are not the final picture.
  if (r.in_scene && r.phase == UpscaleFrameTracker::Phase::kScene) u.scene_copies.clear();
  // How the viewport puts clip space on the scene (the camera motion pass), from its first depth-tested
  // draw.
  if (r.jitter && r.in_scene && !u.screen_valid) {
    const uint32_t vte = d.u32(dev::kPaClVteCntl), mode = d.u32(dev::kPaSuScModeCntl);
    MotionScreen& s = u.screen;
    s.x_scale = (vte & 1) ? d.f32(dev::kPaClVport + 0) : 1.0f;
    s.x_offset = (vte & 2) ? d.f32(dev::kPaClVport + 4) : 0.0f;
    s.y_scale = (vte & 4) ? d.f32(dev::kPaClVport + 8) : 1.0f;
    s.y_offset = (vte & 8) ? d.f32(dev::kPaClVport + 12) : 0.0f;
    s.z_scale = (vte & 16) ? d.f32(dev::kPaClVport + 16) : 1.0f;
    s.z_offset = (vte & 32) ? d.f32(dev::kPaClVport + 20) : 0.0f;
    if (mode & (1u << 16)) {  // vtx_window_offset_enable
      const uint32_t window_offset = d.u32(dev::kPaScWindowOffset);
      s.x_offset += float(int32_t(window_offset << 17) >> 17);
      s.y_offset += float(int32_t((window_offset >> 16) << 17) >> 17);
    }
    if ((d.u32(dev::kPaSuVtxCntl) & 1) == 0) {  // D3D9's half-pixel offset
      s.x_offset += 0.5f;
      s.y_offset += 0.5f;
    }
    s.host_scale_x = u.in_x;
    s.host_scale_y = u.in_y;
    u.screen_valid = s.x_scale != 0.0f && s.y_scale != 0.0f;
  }
  UpscaleDraw out;
  out.snapshot = take && !r.upscale_before;
  out.jitter = r.jitter;
  out.motion = r.motion;  // recorded for the camera's vote even when the object pass is off (debug 4)
  out.scene_phase = r.phase == UpscaleFrameTracker::Phase::kScene;
  return out;
}

void GameRenderer::UpscaleTakeSnapshot(nvrhi::ICommandList* cl) {
  GameUpscale& u = *upscale_;
  if (!u.upscaled && !u.snapshot_taken) {
    u.snapshot_due = false;
    auto it = targets_.find(u.tracker.scene_color());
    if (it != targets_.end() && it->second && u.config.reactive > 0.0f) {
      HostTarget* scene = it->second.get();
      const nvrhi::TextureDesc& sd = scene->texture->getDesc();
      if (!u.snapshot || u.snapshot->getDesc().width != sd.width || u.snapshot->getDesc().height != sd.height ||
          u.snapshot->getDesc().format != sd.format) {
        nvrhi::TextureDesc td2;
        td2.width = sd.width;
        td2.height = sd.height;
        td2.format = sd.format;
        td2.initialState = nvrhi::ResourceStates::ShaderResource;
        td2.keepInitialState = true;
        td2.debugName = "Upscaler opaque picture";
        if (u.snapshot) RetireResource(u.snapshot);
        u.snapshot = device_->createTexture(td2);
        u.pass_sets.clear();
      }
      if (u.snapshot) {
        cl->copyTexture(u.snapshot, nvrhi::TextureSlice(), scene->texture, nvrhi::TextureSlice());
        u.snapshot_taken = true;
      }
    }
  }
}

void GameRenderer::UpscaleRecordMotion(const DrawCall& call, const dev::DeviceView& d,
                                       const std::shared_ptr<const GameShader>& vs, const kkshaders::DrawConstants& dc,
                                       const PipelineRecord& record, const nvrhi::GraphicsState& gs,
                                       const nvrhi::DrawArguments& args, bool indexed) {
  GameUpscale& u = *upscale_;
  if (u.upscaled || u.motion_draws.size() >= 8192) return;
  GameUpscale::MotionDraw m;
  const AnnNormalLayout* ann_normal = nullptr;
  m.previous_tessellation_flags = tessellation_previous_flags_;
  if (record.flags & kRecordTessellation) {
    const auto it = ann_normals_.find(GeometryContentHash(call));
    if (it != ann_normals_.end()) {
      ann_normal = &it->second;
      m.previous_tessellation_flags = it->second.previous_flags;
    }
  }
  m.key.vs_hash = vs->ucode_hash;
  for (uint32_t stream = 0; stream < 2; ++stream) {
    const uint32_t object = d.u32(dev::kStreamBuffer + 8 * stream);
    VertexBufferInfo info;
    if (!object || !DecodeVertexBuffer(memory_.Virtual(object), info)) continue;
    // A vertex buffer the game rewrites (V-Rex's world, the V-Rex dinosaur): it moves in memory, so the
    // draw is matched by its shader and size (below). It is replayed with last frame's constants like any
    // other (the dinosaur's scales smeared under FSR 3.1 and XeSS without it) and votes for the camera
    // (V-Rex's world had no camera motion at all without it).
    if (const kknr::BufferCache::Entry* e = vertex_buffers_.Find(info.physical, info.size_bytes, 0)) {
      if (auto* h = static_cast<const HostBuffer*>(e->host); h && h->dynamic) m.camera_only = true;
    }
    if (stream == 0) {
      m.key.vertex_buffer = info.physical;
      m.key.vertex_offset = d.u32(dev::kStreamOffset);
    } else {
      m.key.vertex_buffer2 = info.physical;
    }
  }
  if (ann_normal)
    m.key.vertex_buffer2 = ann_normal->element.stream ? ann_normal->buffer.physical : 0;
  m.key.index_buffer = call.indexed ? call.index_info.physical : 0;
  m.key.start = call.start;
  m.key.count = call.count;
  m.key.base_vertex = call.base_vertex;
  m.key.primitive = uint32_t(call.primitive);
  if (m.camera_only) {
    // Rewritten buffers move (V-Rex's world is streamed into a ring each frame: no draw kept its
    // address, so none met its last-frame self and the camera had no vote). For the camera's vote
    // such a draw is known by its shader, size and kind alone; draws alike are told apart by order or
    // by the closest constants (MotionHistory::FindPrevious).
    m.key.vertex_buffer = m.key.vertex_offset = m.key.vertex_buffer2 = m.key.index_buffer = 0;
    m.key.start = 0;
    m.key.base_vertex = 0;
  }
  m.vs = vs;
  std::memcpy(m.constants.data(), last_vs_.data(), sizeof(float) * 1024);
  float rows[16];
  m.wvp_register = FindAoPerspectiveRegister(d.at(dev::kVsConstants), rows);
  m.dc = dc;
  m.pipeline = MotionPipelineRecord(record, uint32_t(nvrhi::Format::RG16_FLOAT));
  const uint64_t motion_key = PipelineKey(m.pipeline);
  NoteRecord(m.pipeline, motion_key);
  // Ask before this draw can be matched with a previous frame. Pipelines from a shipped list
  // are queued earlier still, when the game creates their vertex shader containers.
  // Once per description, rather than taking the motion and cache locks for
  // every depth draw in every frame. Deferred jobs retain their existing retry
  // path (RetryDeferred); motion replay still checks readiness without waiting.
  if (u.motion_prewarm_seen.insert(motion_key).second) {
      if (!(m.pipeline.flags & kRecordTessellation)) shaders_->MotionVariant(*vs);
    std::lock_guard lock(cache_mutex_);
    QueueOrDefer(m.pipeline);
  }
  m.indexed = indexed;
  m.index = gs.indexBuffer;
  m.args = args;
  if (!gs.viewport.viewports.empty()) m.viewport = gs.viewport.viewports[0];
  if (!gs.viewport.scissorRects.empty()) m.scissor = gs.viewport.scissorRects[0];
  m.ordinal = u.history.Record(m.key, m.constants);
  // FSR 3.1's heuristics want the camera: from the first draw with a projection.
  if (!u.camera_valid && m.wvp_register >= 0) {
    AoProjection p;
    if (ExtractAoProjection(rows, p) && p.valid()) {
      u.camera_near = p.near_plane();
      u.camera_far = p.far_plane();
      u.camera_fov_y = 2.0f * std::atan(1.0f / p.py);
      u.camera_valid = true;
    }
  }
  ++u.stats.motion_draws;
  if (m.camera_only) ++u.stats.motion_rewritten;
  u.motion_draws.push_back(std::move(m));
}

void GameRenderer::UpscaleBeginClear(nvrhi::ICommandList* cl, const ClearCall& call, uint64_t color_key,
                                     uint64_t depth_key, bool covers_target) {
  GameUpscale& u = *upscale_;
  UpscaleNewFrame(call.frame);
  UpscaleFrameTracker::Trigger trigger = UpscaleFrameTracker::Trigger::kNone;
  if (u.tracker.OnClear({call.frame, color_key, call.depth ? depth_key : 0, covers_target}, &trigger))
    RunUpscale(cl, trigger);
}

void GameRenderer::UpscaleBeginResolve(nvrhi::ICommandList* cl, const ResolveCall& call, uint64_t source_key,
                                       bool clears_color) {
  GameUpscale& u = *upscale_;
  UpscaleNewFrame(call.frame);
  UpscaleFrameTracker::Trigger trigger = UpscaleFrameTracker::Trigger::kNone;
  if (u.tracker.OnResolve({call.frame, source_key, clears_color}, &trigger)) RunUpscale(cl, trigger);
}

void GameRenderer::UpscaleAfterResolve(const ResolveCall& call, const HostTarget* source, bool from_depth,
                                       uint32_t format, int32_t sx, int32_t sy, int32_t w, int32_t h,
                                       const kknr::TextureFetch* fetch, bool copied) {
  GameUpscale& u = *upscale_;
  if (!copied || from_depth || !fetch || u.upscaled || source->output_gen) return;
  if (u.tracker.phase() != UpscaleFrameTracker::Phase::kScene || source->key != u.tracker.scene_color()) return;
  if (sx != 0 || sy != 0 || w < int32_t(options_.guest_frame_width) || h < int32_t(options_.guest_frame_height)) return;
  GameUpscale::SceneCopy c;
  c.fetch = *fetch;
  c.sx = sx;
  c.sy = sy;
  c.w = w;
  c.h = h;
  c.dx = call.has_point ? call.point[0] : 0;
  c.dy = call.has_point ? call.point[1] : 0;
  c.level = call.dest_level;
  c.slice = call.dest_slice;
  c.format = format;
  u.scene_copies.push_back(c);
}

void GameRenderer::UpscaleBeginPresent(nvrhi::ICommandList* cl) {
  GameUpscale& u = *upscale_;
  if (u.tracker.OnPresent(u.frame)) {
    RunUpscale(cl, UpscaleFrameTracker::Trigger::kPresent);
  } else if (!u.upscaled && u.tracker.phase() == UpscaleFrameTracker::Phase::kScene) {
    // A frame drawn in the scene phase that had no scene after all (a menu after gameplay): its
    // targets carry their picture to the output size when Present takes them (MigrateTarget).
    u.upscaled = true;
    u.tracker.MarkUpscaled();
  }
  if (u.timer) u.timer->FrameEnd();
  if (u.sdk_timer) u.sdk_timer->FrameEnd();
  if (u.motion_timer) u.motion_timer->FrameEnd();
}

void GameRenderer::UpscaleEndFrame() {
  GameUpscale& u = *upscale_;
  if (frame_ % 600 == 599) UpscaleLogStats();
  (void)u;
}

void GameRenderer::UpscaleLogStats() {
  GameUpscale& u = *upscale_;
  const GameUpscale::Stats& s = u.stats;
  const GameUpscale::Stats& l = u.logged;
  const ShaderLibrary::MotionStats ms = shaders_->motion_stats();
  const UpscaleFrameTracker::Stats& ts = u.tracker.stats();
  Logf(LogLevel::kInfo,
       "rexgpu-native: upscaler %s, last 600 frames: %llu upscaled (point: post shader %llu, corner %llu, clearing "
       "copy %llu, Present %llu in all), %llu resets (%llu camera cuts); motion draws %llu (%llu on rewritten vertex buffers), matched %llu, drawn %llu, waiting %llu; "
       "camera %llu frames; no depth %llu; scene copies redone %llu; targets carried %llu; failures %llu; motion "
       "variants %llu made, %llu failed, %.0f ms",
       u.name.c_str(), static_cast<unsigned long long>(s.frames - l.frames),
       static_cast<unsigned long long>(ts.by_trigger[1]), static_cast<unsigned long long>(ts.by_trigger[2]),
       static_cast<unsigned long long>(ts.by_trigger[3]), static_cast<unsigned long long>(ts.by_trigger[4]),
       static_cast<unsigned long long>(s.resets - l.resets),
       static_cast<unsigned long long>(s.cuts - l.cuts), static_cast<unsigned long long>(s.motion_draws - l.motion_draws),
       static_cast<unsigned long long>(s.motion_rewritten - l.motion_rewritten),
       static_cast<unsigned long long>(s.motion_matched - l.motion_matched),
       static_cast<unsigned long long>(s.motion_drawn - l.motion_drawn),
       static_cast<unsigned long long>(s.motion_waiting - l.motion_waiting),
       static_cast<unsigned long long>(s.camera_frames - l.camera_frames), static_cast<unsigned long long>(s.no_depth - l.no_depth),
       static_cast<unsigned long long>(s.scene_copies - l.scene_copies), static_cast<unsigned long long>(s.migrations - l.migrations),
       static_cast<unsigned long long>(s.failures - l.failures), static_cast<unsigned long long>(ms.made),
       static_cast<unsigned long long>(ms.failed), ms.compile_ms);
  u.logged = s;
}

// ---------------------------------------------------------------- passes

bool GameRenderer::UpscalePass(nvrhi::ICommandList* cl, nvrhi::IShader* ps, const void* params, uint32_t params_size,
                               nvrhi::ITexture* source0, nvrhi::ITexture* source1, nvrhi::ITexture* color0,
                               nvrhi::ITexture* color1, nvrhi::ITexture* depth, const nvrhi::Rect& rect, uint64_t /*key*/,
                               nvrhi::ColorMask mask) {
  GameUpscale& u = *upscale_;
  if (rect.maxX <= rect.minX || rect.maxY <= rect.minY) return false;
  uint64_t fb_key = Mix64(0xF00B, reinterpret_cast<uintptr_t>(color0));
  fb_key = Mix64(fb_key, reinterpret_cast<uintptr_t>(color1));
  fb_key = Mix64(fb_key, reinterpret_cast<uintptr_t>(depth));
  nvrhi::IFramebuffer* fb = nullptr;
  if (auto it = u.framebuffers.find(fb_key); it != u.framebuffers.end()) {
    fb = it->second;
  } else {
    nvrhi::FramebufferDesc fd;
    if (color0) fd.addColorAttachment(color0);
    if (color1) fd.addColorAttachment(color1);
    if (depth) fd.setDepthAttachment(depth);
    nvrhi::FramebufferHandle h = device_->createFramebuffer(fd);
    if (!h) return false;
    u.framebuffers[fb_key] = h;
    fb = h;
  }
  nvrhi::IGraphicsPipeline* pipeline = UpscalePipeline(ps, fb->getFramebufferInfo(), mask);
  if (!pipeline) return false;
  nvrhi::ITexture* s0 = source0 ? source0 : dummy_2d_.Get();
  nvrhi::ITexture* s1 = source1 ? source1 : dummy_2d_.Get();
  const uint64_t set_key = Mix64(reinterpret_cast<uintptr_t>(s0), reinterpret_cast<uintptr_t>(s1));
  nvrhi::IBindingSet* set = nullptr;
  if (auto it = u.pass_sets.find(set_key); it != u.pass_sets.end()) {
    set = it->second;
  } else {
    nvrhi::BindingSetDesc bs;
    bs.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(UpscaleParams)),
                   nvrhi::BindingSetItem::Texture_SRV(0, s0, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   nvrhi::BindingSetItem::Texture_SRV(1, s1, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   nvrhi::BindingSetItem::Sampler(0, u.linear_clamp)};
    nvrhi::BindingSetHandle h = device_->createBindingSet(bs, u.pass_layout);
    if (!h) return false;
    u.pass_sets[set_key] = h;
    set = h;
  }
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.bindings = {set};
  gs.viewport.addViewport(nvrhi::Viewport(float(rect.minX), float(rect.maxX), float(rect.minY), float(rect.maxY), 0.0f, 1.0f));
  gs.viewport.addScissorRect(rect);
  cl->setGraphicsState(gs);
  UpscaleParams p;
  std::memcpy(&p, params, std::min<size_t>(params_size, sizeof(p)));
  cl->setPushConstants(&p, sizeof(p));
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  cl->draw(args);
  return true;
}

nvrhi::IGraphicsPipeline* GameRenderer::UpscalePipeline(nvrhi::IShader* ps,
                                                       const nvrhi::FramebufferInfo& info,
                                                       nvrhi::ColorMask mask) {
  GameUpscale& u = *upscale_;
  nvrhi::GraphicsPipelineDesc pd;
  pd.primType = nvrhi::PrimitiveType::TriangleList;
  pd.VS = u.vs_fullscreen;
  pd.PS = ps;
  pd.bindingLayouts = {u.pass_layout};
  pd.renderState.depthStencilState.depthTestEnable = info.depthFormat != nvrhi::Format::UNKNOWN;
  pd.renderState.depthStencilState.depthWriteEnable = info.depthFormat != nvrhi::Format::UNKNOWN;
  pd.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Always;
  pd.renderState.depthStencilState.stencilEnable = false;
  pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  pd.renderState.rasterState.scissorEnable = true;
  pd.renderState.rasterState.depthClipEnable = false;
  for (auto& target : pd.renderState.blendState.targets) target.colorWriteMask = mask;
  // The pass's pipeline: its pixel shader and the attachments' formats.
  uint64_t pipe_key = Mix64(Mix64(0x0BCA1E00ull, reinterpret_cast<uintptr_t>(ps)), uint64_t(mask));
  for (nvrhi::Format f : info.colorFormats) pipe_key = Mix64(pipe_key, uint64_t(f) + 1);
  pipe_key = Mix64(pipe_key, uint64_t(info.depthFormat) + 0x100);
  return GetPipeline(pd, info, pipe_key);
}

void GameRenderer::MigrateTarget(nvrhi::ICommandList* cl, HostTarget* target) {
  GameUpscale& u = *upscale_;
  if (!target->output_gen || target->migrated_frame == frame_ || u.running) return;
  target->migrated_frame = frame_;
  auto it = targets_.find(target->key & ~kOutputGeneration);
  if (it == targets_.end() || !it->second || it->second->used_frame != frame_) return;
  HostTarget* in = it->second.get();
  // The input twin's picture of the guest rows both hold.
  const nvrhi::Rect dest(0, int(target->host_width), 0, int(target->host_height));
  UpscaleParams p;
  if (target->depth) {
    p.p0[0] = float(in->host_width) / float(target->host_width);
    p.p0[1] = float(in->host_height) / float(target->host_height) * float(target->height) / float(std::max(in->height, 1u));
    p.p0[2] = float(in->host_width);
    p.p0[3] = float(in->host_height);
    cl->clearDepthStencilTexture(target->texture, nvrhi::AllSubresources, true, 1.0f, true, 0);
    UpscalePass(cl, u.depth_ps, &p, sizeof(p), in->texture, nullptr, nullptr, nullptr, target->texture, dest, 3);
  } else {
    p.p0[0] = 0.0f;
    p.p0[1] = 0.0f;
    p.p0[2] = 1.0f;
    p.p0[3] = std::min(1.0f, float(target->height) / float(std::max(in->height, 1u)));
    UpscalePass(cl, u.stretch_ps, &p, sizeof(p), in->texture, nullptr, target->texture, nullptr, nullptr, dest, 4);
  }
  ++u.stats.migrations;
}

void GameRenderer::UpscaleMotionPass(nvrhi::ICommandList* cl, HostTarget* depth_target) {
  GameUpscale& u = *upscale_;
  if (u.motion_draws.empty()) return;
  // The motion texture over the scene's depth, which the replayed draws test for equality.
  const uint64_t fb_key = Mix64(Mix64(0x40710, reinterpret_cast<uintptr_t>(u.motion.Get())),
                                reinterpret_cast<uintptr_t>(depth_target->texture.Get()));
  nvrhi::IFramebuffer* fb = nullptr;
  if (auto it = u.framebuffers.find(fb_key); it != u.framebuffers.end()) {
    fb = it->second;
  } else {
    nvrhi::FramebufferDesc fd;
    fd.addColorAttachment(u.motion);
    fd.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(depth_target->texture).setReadOnly(true));
    nvrhi::FramebufferHandle h = device_->createFramebuffer(fd);
    if (!h) return;
    u.framebuffers[fb_key] = h;
    fb = h;
  }
  const bool vulkan = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  const nvrhi::TextureDesc& md = u.motion->getDesc();
  for (const GameUpscale::MotionDraw& m : u.motion_draws) {
    if (m.camera_only && (u.config.debug & 1024)) continue;  // debugging: their camera motion only
    const VsConstants* prev = u.history.FindPrevious(m.key, m.ordinal, m.constants, u.history.CountThisFrame(m.key));
    if (!prev) continue;
    nvrhi::IShader* mvs = (m.pipeline.flags&kRecordTessellation) ? m.vs->handle.Get() : shaders_->MotionVariant(*m.vs);
    if (!mvs) {
      ++u.stats.motion_waiting;
      continue;
    }
    PipelineRecord r = m.pipeline;
    r.depth_format = uint32_t(depth_target->format);
    const uint64_t key = PipelineKey(r);
    nvrhi::IGraphicsPipeline* pipeline = nullptr;
    if (auto known = pipelines_.find(key); known != pipelines_.end()) {
      pipeline = known->second;
    } else {
      nvrhi::GraphicsPipelineDesc pd;
      nvrhi::FramebufferInfo fbi;
      BuildPipelineDesc(r, mvs, u.object_ps, pd, fbi);
      if (!pd.bindingLayouts.empty()) pd.bindingLayouts[0] = u.motion_layout;
      pipeline = GetPipeline(pd, fbi, key, true, true);
    }
    if (!pipeline) {
      ++u.stats.motion_waiting;
      continue;
    }
    kkshaders::MotionConstants mc;
    std::memcpy(mc.previous, prev->data(), sizeof(mc.previous));
    mc.params[0] = 0.5f * float(md.width);
    mc.params[1] = 0.5f * float(md.height);
    mc.params[2] = vulkan ? 1.0f : -1.0f;
    mc.params[3] = 0.0f;
    if (m.pipeline.flags&kRecordTessellation) std::memcpy(&mc.params[3],&m.previous_tessellation_flags,sizeof(uint32_t));
    cl->writeBuffer(u.motion_vs_constants, m.constants.data(), sizeof(float) * 1024);
    cl->writeBuffer(u.motion_draw_constants, &m.dc, sizeof(m.dc));
    cl->writeBuffer(u.motion_constants, &mc, sizeof(mc));
    nvrhi::GraphicsState gs;
    gs.pipeline = pipeline;
    gs.framebuffer = fb;
    gs.viewport.addViewport(m.viewport);
    gs.viewport.addScissorRect(m.scissor);
    gs.bindings.push_back(u.motion_set);
    for (DescriptorTable& t : tables_) gs.bindings.push_back(t.table);
    if (m.indexed) gs.indexBuffer = m.index;
    cl->setGraphicsState(gs);
    if (m.indexed) {
      cl->drawIndexed(m.args);
    } else {
      cl->draw(m.args);
    }
    ++u.stats.motion_drawn;
  }
  // The volatile buffers the game's draws bind must be written again in this list before use.
  constants_valid_ = false;
}

// ---------------------------------------------------------------- the upscale point

void GameRenderer::RunUpscale(nvrhi::ICommandList* cl, UpscaleFrameTracker::Trigger trigger) {
  GameUpscale& u = *upscale_;
  if (u.upscaled) return;
  // The ambient occlusion goes in first when its moments missed this scene.
  if (scene_ao_) scene_ao_->OnPresent(cl, AoSettingsFor(u.frame));
  u.upscaled = true;
  u.tracker.MarkUpscaled();
  auto find = [&](uint64_t key) -> HostTarget* {
    auto it = targets_.find(key);
    return it == targets_.end() ? nullptr : it->second.get();
  };
  HostTarget* color = find(u.tracker.scene_color());
  HostTarget* depth = find(u.tracker.scene_depth());
  if (!color) return;  // nothing drawn at the input size: the targets carry over when used
  u.running = true;
  if (u.timer) u.timer->Begin(cl);
  ++u.stats.frames;

  // The inputs, as big as the scene target.
  const nvrhi::TextureDesc& cd = color->texture->getDesc();
  auto ensure = [&](nvrhi::TextureHandle& t, nvrhi::Format format, const char* name, bool rt) {
    if (t && t->getDesc().width == cd.width && t->getDesc().height == cd.height && t->getDesc().format == format) return;
    if (t) RetireResource(t);
    nvrhi::TextureDesc d;
    d.width = cd.width;
    d.height = cd.height;
    d.format = format;
    d.isRenderTarget = rt;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = name;
    t = device_->createTexture(d);
    ++frame_resource_creations_;
    u.framebuffers.clear();
    u.pass_sets.clear();
  };
  ensure(u.motion, nvrhi::Format::RG16_FLOAT, "Upscaler motion vectors", true);
  ensure(u.depth, nvrhi::Format::R32_FLOAT, "Upscaler depth", true);
  ensure(u.reactive, nvrhi::Format::R8_UNORM, "Upscaler reactive mask", true);
  const UpscalerSize render{std::min(RoundSize(options_.guest_frame_width, u.in_x), uint32_t(cd.width)),
                            std::min(RoundSize(options_.guest_frame_height, u.in_y), uint32_t(cd.height))};
  const nvrhi::Rect input_rect(0, int(cd.width), 0, int(cd.height));

  // The output generation's scene target and the output.
  const KeyParts ck = DecodeKey(color->key);
  HostTarget* out_color = GetTarget(false, ck.base, ck.pitch, ck.format, color->height, 1);
  if (!out_color || !u.motion || !u.depth || !u.reactive) {
    u.running = false;
    if (u.timer) u.timer->End(cl);
    return;
  }
  const nvrhi::TextureDesc& od = out_color->texture->getDesc();
  if (!u.output || u.output->getDesc().width != od.width || u.output->getDesc().height != od.height ||
      u.output->getDesc().format != od.format) {
    if (u.output) RetireResource(u.output);
    nvrhi::TextureDesc d;
    d.width = od.width;
    d.height = od.height;
    d.format = od.format;
    d.isUAV = true;
    d.isRenderTarget = true;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = "Upscaler output";
    u.output = device_->createTexture(d);
    ++frame_resource_creations_;
    u.framebuffers.clear();
    u.pass_sets.clear();
  }
  const UpscalerSize out_size{std::min(RoundSize(options_.guest_frame_width, u.out_x), uint32_t(od.width)),
                              std::min(RoundSize(options_.guest_frame_height, u.out_y), uint32_t(od.height))};

  // 1. The camera's motion everywhere, and the depth as R32F.
  const bool have_depth = depth && u.tracker.depth_valid() && depth->texture->getDesc().width == cd.width &&
                          depth->texture->getDesc().height == cd.height;
  if (!have_depth) ++u.stats.no_depth;
  // The camera: the draws that met again vote.
  uint32_t matched = 0, changed = 0, objects = 0, objects_matched = 0;
  for (const GameUpscale::MotionDraw& m : u.motion_draws) {
    if (!m.camera_only) ++objects;
    const VsConstants* prev = u.history.FindPrevious(m.key, m.ordinal, m.constants, u.history.CountThisFrame(m.key));
    if (!prev) continue;
    ++matched;
    if (!m.camera_only) ++objects_matched;
    if (std::memcmp(prev->data(), m.constants.data(), sizeof(float) * 1024) != 0) {
      ++changed;
      if ((u.config.debug & 128) && changed <= 3) {
        // Which registers changed (debugging the camera vote).
        std::string regs;
        for (int r = 0; r < 256 && regs.size() < 200; ++r)
          if (std::memcmp(&(*prev)[size_t(r) * 4], &m.constants[size_t(r) * 4], sizeof(float) * 4) != 0)
            regs += " c" + std::to_string(r);
        const float* c = m.wvp_register >= 0 ? &m.constants[size_t(m.wvp_register) * 4] : nullptr;
        Logf(LogLevel::kInfo,
             "rexgpu-native: upscaler debug: frame %llu draw vs %016llX wvp c%d [%.4f %.4f %.4f %.4f / %.4f %.4f %.4f "
             "%.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f] changed:%s",
             static_cast<unsigned long long>(u.frame), static_cast<unsigned long long>(m.key.vs_hash), m.wvp_register,
             c ? c[0] : 0, c ? c[1] : 0, c ? c[2] : 0, c ? c[3] : 0, c ? c[4] : 0, c ? c[5] : 0, c ? c[6] : 0,
             c ? c[7] : 0, c ? c[8] : 0, c ? c[9] : 0, c ? c[10] : 0, c ? c[11] : 0, c ? c[12] : 0, c ? c[13] : 0,
             c ? c[14] : 0, c ? c[15] : 0, regs.c_str());
      }
    }
    if (m.wvp_register >= 0 && m.wvp_register <= 252)
      u.vote.Offer(&m.constants[size_t(m.wvp_register) * 4], &(*prev)[size_t(m.wvp_register) * 4]);
  }
  u.stats.motion_matched += matched;
  Mat4 reprojection;
  const bool camera = u.screen_valid && u.vote.Result(reprojection);
  if (u.config.debug & 128) {
    uint32_t votes = 0, offers = 0;
    Mat4 r{};
    u.vote.Result(r, &votes, &offers);
    Logf(LogLevel::kInfo,
         "rexgpu-native: upscaler debug: frame %llu vote %u of %u, screen %d (%.1f %.1f %.1f %.1f), R [%.5f %.5f %.5f "
         "%.5f / %.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f / %.5f %.5f %.5f %.5f]",
         static_cast<unsigned long long>(u.frame), votes, offers, int(u.screen_valid), u.screen.x_scale,
         u.screen.x_offset, u.screen.y_scale, u.screen.y_offset, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8],
         r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
  }
  if (camera) ++u.stats.camera_frames;
  // A camera cut: the camera's reprojection moves the middle of the screen by more than an eighth of
  // its width or height in one frame (a turn of 250 degrees a second at 30 FPS), near and far alike.
  // The history is then of another shot: the upscaler starts again.
  bool cut = false;
  if (camera) {
    double shift = INFINITY;
    for (const double z : {0.5, 0.999}) {
      const double cur[4] = {0.0, 0.0, z, 1.0};
      double prev[4];
      Mat4Transform(reprojection, cur, prev);
      if (prev[3] > 1e-6) shift = std::min(shift, std::hypot(prev[0] / prev[3], prev[1] / prev[3]));
    }
    cut = shift > 0.25;
    if (cut) ++u.stats.cuts;
  }
  if (have_depth) {
    UpscaleParams p;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) p.rows[r][c] = camera ? float(reprojection[r * 4 + c]) : (r == c ? 1.0f : 0.0f);
    p.p0[0] = u.screen.x_scale;
    p.p0[1] = u.screen.x_offset;
    p.p0[2] = u.screen.y_scale;
    p.p0[3] = u.screen.y_offset;
    p.p1[0] = u.screen.z_scale;
    p.p1[1] = u.screen.z_offset;
    p.p1[2] = u.in_x;
    p.p1[3] = u.in_y;
    p.p2[0] = u.jitter.x;
    p.p2[1] = u.jitter.y;
    p.p2[2] = camera ? 1.0f : 0.0f;
    UpscalePass(cl, u.camera_ps, &p, sizeof(p), depth->texture, nullptr, u.motion, u.depth, nullptr, input_rect, 1);
    // 2. Each object's own motion where it is what the depth shows.
    if (!(u.config.debug & 4)) {
      const auto start = std::chrono::steady_clock::now();
      if (u.motion_timer) u.motion_timer->Begin(cl);
      UpscaleMotionPass(cl, depth);
      if (u.motion_timer) u.motion_timer->End(cl);
      frame_motion_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
  } else {
    cl->clearTextureFloat(u.motion, nvrhi::AllSubresources, nvrhi::Color(0.0f));
    cl->clearTextureFloat(u.depth, nvrhi::AllSubresources, nvrhi::Color(1.0f));
  }
  // 3. The reactive mask: what changed after the opaque pass.
  if (u.snapshot_taken && u.snapshot && u.config.reactive > 0.0f) {
    UpscaleParams p;
    p.p0[0] = 2.0f * u.config.reactive;
    p.p0[1] = 0.9f;
    UpscalePass(cl, u.reactive_ps, &p, sizeof(p), u.snapshot, color->texture, u.reactive, nullptr, nullptr, input_rect, 2);
  } else {
    cl->clearTextureFloat(u.reactive, nvrhi::AllSubresources, nvrhi::Color(0.0f));
  }

  // 4. The upscaler.
  const bool reset = u.last_upscaled_frame == UINT64_MAX || u.last_upscaled_frame + 1 != u.frame ||
                     u.last_output.width != out_size.width || u.last_output.height != out_size.height ||
                     (objects >= 20 && objects_matched * 4 < objects) || cut;
  if (reset) ++u.stats.resets;
  bool done = false;
  if (u.upscaler) {
    UpscalerInputs in;
    in.color = color->texture;
    in.depth = u.depth;
    in.motion = u.motion;
    // Always the mask (cleared to 0 when its strength is 0): XeSS made with its responsive-mask flag needs one
    // every frame, and the strength can change while the game runs.
    in.reactive = u.reactive.Get();
    in.output = u.output;
    in.render = render;
    in.output_size = out_size;
    in.jitter_x = u.jitter.x;
    in.jitter_y = u.jitter.y;
    in.reset = reset;
    in.sharpness = u.config.sharpness;
    in.camera_near = u.camera_near;
    in.camera_far = u.camera_far;
    in.camera_fov_y = u.camera_fov_y;
    if (u.sdk_timer) u.sdk_timer->Begin(cl);
    const auto sdk_start = std::chrono::steady_clock::now();
    done = u.upscaler->Evaluate(cl, in);
    frame_sdk_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sdk_start).count();
    if (u.sdk_timer) u.sdk_timer->End(cl);
    if (!done && u.stats.failures++ < 10)
      Logf(LogLevel::kWarning, "rexgpu-native: upscaler %s failed this frame: a bilinear stretch instead", u.name.c_str());
  }
  const nvrhi::Rect out_rect(0, int(out_size.width), 0, int(out_size.height));
  UpscaleParams stretch;
  stretch.p0[0] = 0.0f;
  stretch.p0[1] = 0.0f;
  stretch.p0[2] = float(render.width) / float(cd.width);
  stretch.p0[3] = float(render.height) / float(cd.height);
  if (!done) UpscalePass(cl, u.stretch_ps, &stretch, sizeof(stretch), color->texture, nullptr, u.output, nullptr, nullptr, out_rect, 5);
  // The scene's alpha, which the game's post chain reads (V-Rex's mist and glow: FSR 3.1's and XeSS's
  // outputs had none, and the post effects washed the picture out). The SDKs rebuild colour only: the
  // alpha is the input's, stretched.
  if (done) {
    UpscalePass(cl, u.stretch_ps, &stretch, sizeof(stretch), color->texture, nullptr, u.output, nullptr, nullptr, out_rect,
                9, nvrhi::ColorMask::Alpha);
  }
  // Debugging: the motion vectors read back (every 150 frames, mapped 4 frames later).
  if (u.config.debug & 512) {
    if (u.probe && u.frame >= u.probe_frame + 4) {
      size_t pitch = 0;
      const auto* data = static_cast<const uint8_t*>(
          device_->mapStagingTexture(u.probe, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
      if (data) {
        auto half = [](uint16_t h) {
          const uint32_t e = (h >> 10) & 31, m = h & 1023;
          const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.0f : std::ldexp(float(m | 1024), int(e) - 25);
          return (h & 0x8000) ? -v : v;
        };
        uint64_t nonzero = 0, total = 0;
        float maxv = 0.0f;
        double sx = 0, sy = 0;
        for (uint32_t y = 0; y < render.height; ++y) {
          const auto* row = reinterpret_cast<const uint16_t*>(data + y * pitch);
          for (uint32_t x = 0; x < render.width; ++x) {
            const float mx = half(row[x * 2]), my = half(row[x * 2 + 1]);
            ++total;
            if (mx != 0.0f || my != 0.0f) ++nonzero;
            maxv = std::max(maxv, std::max(std::fabs(mx), std::fabs(my)));
            sx += mx;
            sy += my;
          }
        }
        device_->unmapStagingTexture(u.probe);
        Logf(LogLevel::kInfo,
             "rexgpu-native: upscaler debug: motion of frame %llu: %llu of %llu pixels move, largest %.3f px, mean "
             "%.4f %.4f",
             static_cast<unsigned long long>(u.probe_frame), static_cast<unsigned long long>(nonzero),
             static_cast<unsigned long long>(total), maxv, total ? sx / double(total) : 0.0,
             total ? sy / double(total) : 0.0);
      }
      u.probe = nullptr;
    }
    if (!u.probe && u.frame % 150 == 0) {
      nvrhi::TextureDesc sd = u.motion->getDesc();
      sd.isRenderTarget = false;
      sd.initialState = nvrhi::ResourceStates::CopyDest;
      sd.keepInitialState = false;
      sd.debugName = "Upscaler motion probe";
      u.probe = device_->createStagingTexture(sd, nvrhi::CpuAccessMode::Read);
      if (u.probe) {
        cl->copyTexture(u.probe, nvrhi::TextureSlice(), u.motion, nvrhi::TextureSlice());
        u.probe_frame = u.frame;
      }
    }
  }
  // Debugging views over the output.
  if (u.config.debug & 1) {
    UpscalePass(cl, u.show_motion_ps, &stretch, sizeof(stretch), u.motion, nullptr, u.output, nullptr, nullptr, out_rect, 6);
  } else if (u.config.debug & 2) {
    UpscalePass(cl, u.stretch_ps, &stretch, sizeof(stretch), u.reactive, nullptr, u.output, nullptr, nullptr, out_rect, 7);
  } else if (u.config.debug & 32) {
    UpscalePass(cl, u.stretch_ps, &stretch, sizeof(stretch), u.depth, nullptr, u.output, nullptr, nullptr, out_rect, 8);
  }

  // 5. Into the output generation's scene target.
  cl->copyTexture(out_color->texture, nvrhi::TextureSlice(), u.output, nvrhi::TextureSlice());
  out_color->migrated_frame = frame_;
  out_color->used_frame = frame_;
  // 6. The depth, for the depth-tested draws after the point.
  if (depth) {
    const KeyParts dk = DecodeKey(depth->key);
    if (HostTarget* out_depth = GetTarget(true, dk.base, dk.pitch, dk.format, depth->height, 1)) {
      cl->clearDepthStencilTexture(out_depth->texture, nvrhi::AllSubresources, true, 1.0f, true, 0);
      if (have_depth) {
        UpscaleParams p;
        p.p0[0] = float(depth->host_width) / float(out_depth->host_width);
        p.p0[1] = float(depth->host_height) / float(out_depth->host_height);
        p.p0[2] = float(depth->host_width);
        p.p0[3] = float(depth->host_height);
        UpscalePass(cl, u.depth_ps, &p, sizeof(p), depth->texture, nullptr, nullptr, nullptr, out_depth->texture,
                    nvrhi::Rect(0, int(out_depth->host_width), 0, int(out_depth->host_height)), 3);
      }
      out_depth->migrated_frame = frame_;
      out_depth->used_frame = frame_;
    }
  }
  u.running = false;
  // 7. The copies of the finished scene, again from the upscaled picture.
  for (const GameUpscale::SceneCopy& c : u.scene_copies) {
    if (ResolveCopy(cl, out_color, false, c.format, c.sx, c.sy, c.w, c.h, c.fetch, c.dx, c.dy, c.level, c.slice))
      ++u.stats.scene_copies;
  }
  u.scene_copies.clear();
  if (u.timer) u.timer->End(cl);
  u.last_upscaled_frame = u.frame;
  u.last_output = out_size;
  if (u.config.debug & 16) {
    Logf(LogLevel::kInfo,
         "rexgpu-native: upscaled frame %llu at %s: %ux%u to %ux%u, jitter %.3f %.3f, %zu motion draws (%u matched, %u with "
         "other constants than last frame), camera %d (translation %.5f %.5f), depth %d, reactive %d, reset %d, %s",
         static_cast<unsigned long long>(u.frame), UpscaleFrameTracker::TriggerName(trigger), render.width,
         render.height, out_size.width, out_size.height, u.jitter.x, u.jitter.y, u.motion_draws.size(), matched,
         changed, int(camera), camera ? reprojection[3] : 0.0, camera ? reprojection[7] : 0.0, int(have_depth), int(u.snapshot_taken), int(reset), done ? u.name.c_str() : "bilinear");
  }
}

}  // namespace nr
