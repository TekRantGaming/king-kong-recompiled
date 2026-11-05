// Muzzle flashes that cast light: see muzzle_light.h.

#include "backend/muzzle_light.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>


#include "backend/log.h"
#include "backend/shaders/generated/muzzle_ps_dxil.h"
#include "backend/shaders/generated/muzzle_ps_spirv.h"
#include "backend/shaders/generated/muzzle_vs_dxil.h"
#include "backend/shaders/generated/muzzle_vs_spirv.h"

namespace nr {

namespace {

using Clock = std::chrono::steady_clock;

struct Flash {
  float x = 0, y = 0, z = 0;
  Clock::time_point at{};
};
constexpr size_t kFlashes = 16;
std::mutex g_mutex;
std::array<Flash, kFlashes> g_flashes;
size_t g_next = 0;

}  // namespace

void NoteMuzzleFlash(float x, float y, float z) {
  std::lock_guard lock(g_mutex);
  g_flashes[g_next] = {x, y, z, Clock::now()};
  g_next = (g_next + 1) % kFlashes;
}

float MuzzleFade(float age) {
  if (age < 0.0f) age = 0.0f;
  if (age >= MuzzleTuning::kSeconds) return 0.0f;
  const float left = 1.0f - age / MuzzleTuning::kSeconds;
  return left * left;
}

uint32_t LiveMuzzleLights(MuzzleLightNow* out, Clock::time_point now) {
  std::array<MuzzleLightNow, kFlashes> live;
  uint32_t n = 0;
  // KK_DEV_MUZZLE_HOLD=<seconds>: a flash stays at full strength this long (pictures of what it lights).
  static const float hold = [] {
    const char* v = std::getenv("KK_DEV_MUZZLE_HOLD");
    return v ? float(std::atof(v)) : 0.0f;
  }();
  // KK_DEV_MUZZLE_TEST="x,y,z": a light stays at this view position, for pictures with no gun to hand.
  static const std::array<float, 3>* test = []() -> const std::array<float, 3>* {
    const char* v = std::getenv("KK_DEV_MUZZLE_TEST");
    static std::array<float, 3> at;
    return v && std::sscanf(v, "%f,%f,%f", &at[0], &at[1], &at[2]) == 3 ? &at : nullptr;
  }();
  if (test) {
    if (out) *out = {(*test)[0], (*test)[1], (*test)[2], MuzzleTuning::kStrength};
    return 1;
  }
  {
    std::lock_guard lock(g_mutex);
    for (const Flash& f : g_flashes) {
      if (f.at == Clock::time_point{}) continue;
      const float age = std::chrono::duration<float>(now - f.at).count();
      const float fade = age < hold ? 1.0f : MuzzleFade(age - hold);
      if (fade > 0.0f) live[n++] = {f.x, f.y, f.z, fade * MuzzleTuning::kStrength};
    }
  }
  std::sort(live.begin(), live.begin() + n, [](const auto& a, const auto& b) { return a.strength > b.strength; });
  n = std::min(n, MuzzleTuning::kMaxLights);
  if (out) std::copy_n(live.begin(), n, out);
  return n;
}

// shaders/muzzle.hlsl's MuzzleConstants.
struct SceneMuzzle::Constants {
  float unpack[4] = {};      // view z = x / (y - stored depth), the sky's stored depth, lights
  float ndc[4] = {};         // view xy = (xy * uv + zw) * view z
  int32_t source[4] = {};    // the picture's origin and size in the target (host pixels)
  float params[4] = {};      // radius, fall-off, wrap, gain
  float colour[4] = {};      // the light's colour
  float light[4][4] = {};    // view position, strength (muzzle.hlsl has room for 4)
  float pass[4] = {};        // 1 the added draw, 0 the multiplied one; what a dark surface shows
};

SceneMuzzle::SceneMuzzle(nvrhi::IDevice* device, std::vector<AoRule> rules) : device_(device), rules_(std::move(rules)) {}
SceneMuzzle::~SceneMuzzle() = default;

bool SceneMuzzle::Initialize() {
  const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  nvrhi::ShaderDesc vd;
  vd.shaderType = nvrhi::ShaderType::Vertex;
  vd.entryName = "vs_main";
  vs_ = spirv ? device_->createShader(vd, k_muzzle_vs_spirv, sizeof(k_muzzle_vs_spirv))
              : device_->createShader(vd, k_muzzle_vs_dxil, sizeof(k_muzzle_vs_dxil));
  nvrhi::ShaderDesc pd;
  pd.shaderType = nvrhi::ShaderType::Pixel;
  pd.entryName = "ps_muzzle";
  ps_ = spirv ? device_->createShader(pd, k_muzzle_ps_spirv, sizeof(k_muzzle_ps_spirv))
              : device_->createShader(pd, k_muzzle_ps_dxil, sizeof(k_muzzle_ps_dxil));
  if (!vs_ || !ps_) return false;
  using I = nvrhi::BindingLayoutItem;
  nvrhi::BindingLayoutDesc bl;
  bl.visibility = nvrhi::ShaderType::All;
  bl.bindings = {I::VolatileConstantBuffer(0), I::Texture_SRV(0)};
  layout_ = device_->createBindingLayout(bl);
  nvrhi::BufferDesc bd;
  bd.byteSize = sizeof(Constants);
  bd.isConstantBuffer = true;
  bd.isVolatile = true;
  bd.maxVersions = 64;  // one write a frame
  bd.debugName = "Muzzle light constants";
  constants_ = device_->createBuffer(bd);
  bool ready = layout_ && constants_;
  if (ready) {
    // The scene colour formats the game's targets use (made now, not in the middle of a fire fight).
    for (const auto format : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::R10G10B10A2_UNORM, nvrhi::Format::RGBA16_FLOAT}) {
      nvrhi::FramebufferInfo info;
      info.colorFormats = {format};
      ready = GraphicsPipeline(info, false) && GraphicsPipeline(info, true) && ready;
    }
  }
  if (ready) Logf(LogLevel::kInfo, "rexgpu-native: muzzle light: ready");
  return ready;
}

void SceneMuzzle::ForgetTargets() {
  sets_.clear();
  framebuffers_.clear();
  scene_color_ = {};
  scene_depth_ = {};
  tracker_ = {};
  camera_ = {};
  frame_ = UINT64_MAX;
  wanted_ = applied_ = false;
}

bool SceneMuzzle::Wanted(uint64_t frame) {
  if (frame != frame_) {
    frame_ = frame;
    applied_ = false;
    wanted_ = LiveMuzzleLights(nullptr, Clock::now()) > 0;
    if (wanted_) ++stats_.frames;
  }
  return wanted_;
}

nvrhi::IFramebuffer* SceneMuzzle::Framebuffer(nvrhi::ITexture* color) {
  auto it = framebuffers_.find(color);
  if (it != framebuffers_.end()) return it->second;
  if (framebuffers_.size() > 16) framebuffers_.clear();
  nvrhi::FramebufferDesc fd;
  fd.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(color));
  nvrhi::FramebufferHandle fb = device_->createFramebuffer(fd);
  framebuffers_[color] = fb;
  return fb;
}

nvrhi::IGraphicsPipeline* SceneMuzzle::GraphicsPipeline(const nvrhi::FramebufferInfo& info, bool add) {
  const uint32_t format = info.colorFormats.empty() ? 0 : uint32_t(info.colorFormats[0]);
  nvrhi::GraphicsPipelineHandle& p = pipelines_[format * 2 + (add ? 1 : 0)];
  if (p) return p;
  nvrhi::GraphicsPipelineDesc pd;
  pd.primType = nvrhi::PrimitiveType::TriangleList;
  pd.VS = vs_;
  pd.PS = ps_;
  pd.bindingLayouts = {layout_};
  pd.renderState.depthStencilState.depthTestEnable = false;
  pd.renderState.depthStencilState.depthWriteEnable = false;
  pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
  // The picture times one plus the light, or the light added (muzzle.hlsl), so nothing is copied or read back. The
  // alpha channel is the game's (later passes may read it).
  nvrhi::BlendState::RenderTarget& t = pd.renderState.blendState.targets[0];
  t.colorWriteMask = nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue;
  t.blendEnable = true;
  t.srcBlend = add ? nvrhi::BlendFactor::One : nvrhi::BlendFactor::DstColor;
  t.destBlend = nvrhi::BlendFactor::One;
  t.blendOp = nvrhi::BlendOp::Add;
  t.srcBlendAlpha = nvrhi::BlendFactor::Zero;
  t.destBlendAlpha = nvrhi::BlendFactor::One;
  t.blendOpAlpha = nvrhi::BlendOp::Add;
  p = device_->createGraphicsPipeline(pd, info);
  return p;
}

bool SceneMuzzle::Shade(nvrhi::ICommandList* cl, const AoProjection& projection, const AoScreen& screen,
                        nvrhi::ITexture* color, nvrhi::ITexture* depth, const MuzzleLightNow* lights, uint32_t count) {
  const nvrhi::TextureDesc& cd = color->getDesc();
  const nvrhi::TextureDesc& dd = depth->getDesc();
  // The picture in host pixels: the viewport of the scene's draws.
  const int32_t x0 = std::max(0, int32_t(std::lround(screen.left() * screen.host_scale_x)));
  const int32_t y0 = std::max(0, int32_t(std::lround(screen.top() * screen.host_scale_y)));
  const int32_t x1 = std::min({int32_t(dd.width), int32_t(cd.width),
                               int32_t(std::lround((screen.left() + screen.width()) * screen.host_scale_x))});
  const int32_t y1 = std::min({int32_t(dd.height), int32_t(cd.height),
                               int32_t(std::lround((screen.top() + screen.height()) * screen.host_scale_y))});
  if (x1 - x0 < 16 || y1 - y0 < 16) return false;
  const uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);
  const AoViewMapping m = ComputeAoViewMapping(projection, screen, x0, y0, w, h, 1);
  Constants k;
  k.unpack[0] = m.depth_mul;
  k.unpack[1] = m.depth_add;
  // Cleared depth (the far plane's stored value or beyond) is sky (gtao.cpp's FillConstants).
  k.unpack[2] = screen.z_scale > 0.0f ? screen.z_offset + screen.z_scale * (1.0f - 1e-6f) : 2.0f;
  k.unpack[3] = float(count);
  k.ndc[0] = m.mul[0];
  k.ndc[1] = m.mul[1];
  k.ndc[2] = m.add[0];
  k.ndc[3] = m.add[1];
  k.source[0] = x0;
  k.source[1] = y0;
  k.source[2] = int32_t(w);
  k.source[3] = int32_t(h);
  k.params[0] = MuzzleTuning::kRadius;
  k.params[1] = MuzzleTuning::kFalloff;
  k.params[2] = MuzzleTuning::kWrap;
  k.params[3] = MuzzleTuning::kGain;
  std::memcpy(k.colour, MuzzleTuning::kColour, sizeof(MuzzleTuning::kColour));
  for (uint32_t i = 0; i < count; ++i) {
    k.light[i][0] = lights[i].x;
    k.light[i][1] = lights[i].y;
    k.light[i][2] = lights[i].z;
    k.light[i][3] = lights[i].strength;
  }
  // Only the part of the picture the lights reach (their spheres' bounds; the whole picture when one holds the eye).
  float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    const float r = MuzzleTuning::kRadius, z = lights[i].z - r;
    if (z <= 0.05f || m.mul[0] == 0.0f || m.mul[1] == 0.0f) {
      u0 = v0 = 0.0f;
      u1 = v1 = 1.0f;
      break;
    }
    for (const float sx : {-r, r})
      for (const float sy : {-r, r}) {
        const float u = ((lights[i].x + sx) / z - m.add[0]) / m.mul[0];
        const float v = ((lights[i].y + sy) / z - m.add[1]) / m.mul[1];
        u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
      }
  }
  const int32_t sx0 = x0 + int32_t(std::floor(std::clamp(u0, 0.0f, 1.0f) * float(w)));
  const int32_t sx1 = x0 + int32_t(std::ceil(std::clamp(u1, 0.0f, 1.0f) * float(w)));
  const int32_t sy0 = y0 + int32_t(std::floor(std::clamp(v0, 0.0f, 1.0f) * float(h)));
  const int32_t sy1 = y0 + int32_t(std::ceil(std::clamp(v1, 0.0f, 1.0f) * float(h)));
  if (sx1 <= sx0 || sy1 <= sy0) return false;
  nvrhi::IFramebuffer* fb = Framebuffer(color);
  nvrhi::IGraphicsPipeline* multiply = fb ? GraphicsPipeline(fb->getFramebufferInfo(), false) : nullptr;
  nvrhi::IGraphicsPipeline* add = fb ? GraphicsPipeline(fb->getFramebufferInfo(), true) : nullptr;
  if (!multiply || !add) return false;
  if (sets_.size() > 16) sets_.clear();
  using S = nvrhi::BindingSetItem;
  nvrhi::BindingSetHandle& set = sets_[depth];
  if (!set) {
    nvrhi::BindingSetDesc bs;
    bs.bindings = {S::ConstantBuffer(0, constants_),
                   S::Texture_SRV(0, depth, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1))};
    set = device_->createBindingSet(bs, layout_);
  }
  if (!set) return false;
  nvrhi::GraphicsState gs;
  gs.framebuffer = fb;
  gs.bindings = {set};
  gs.viewport.addViewport(nvrhi::Viewport(float(x0), float(x1), float(y0), float(y1), 0.0f, 1.0f));
  gs.viewport.addScissorRect(nvrhi::Rect(sx0, sx1, sy0, sy1));
  nvrhi::DrawArguments args;
  args.vertexCount = 3;
  // First what a dark surface shows (added), then the light on the surface's own colour (times the picture).
  for (const bool added : {true, false}) {
    k.pass[0] = added ? 1.0f : 0.0f;
    k.pass[1] = MuzzleTuning::kDarkShows;
    cl->writeBuffer(constants_, &k, sizeof(k));
    gs.pipeline = added ? add : multiply;
    cl->setGraphicsState(gs);
    cl->draw(args);
  }
  hands_ = {};
  hands_.depth_unpack[0] = m.depth_mul;
  hands_.depth_unpack[1] = m.depth_add;
  hands_.ndc_mul[0] = m.mul[0];
  hands_.ndc_mul[1] = m.mul[1];
  hands_.ndc_add[0] = m.add[0];
  hands_.ndc_add[1] = m.add[1];
  hands_.origin[0] = float(x0);
  hands_.origin[1] = float(y0);
  hands_.size[0] = float(w);
  hands_.size[1] = float(h);
  hands_.host_scale[0] = host_scale_[0];
  hands_.host_scale[1] = host_scale_[1];
  hands_.count = count;
  std::memcpy(hands_.light, k.light, sizeof(hands_.light));
  return true;
}

bool SceneMuzzle::Apply(nvrhi::ICommandList* cl, bool depth_before_draw) {
  if (!depth_before_draw || !tracker_.depth_valid() || !scene_depth_.texture || !scene_color_.texture) return false;
  MuzzleLightNow lights[MuzzleTuning::kMaxLights];
  const uint32_t count = LiveMuzzleLights(lights, Clock::now());
  if (!count) return false;
  AoProjection p;
  AoScreen sc;
  if (!camera_.Get(frame_, p, sc)) {
    if (!warned_camera_) Logf(LogLevel::kWarning, "rexgpu-native: muzzle light: no camera among the scene's draws yet");
    warned_camera_ = true;
    return false;
  }
  if (timer_) timer_->Begin(cl);
  const bool ok = Shade(cl, p, sc, scene_color_.texture, scene_depth_.texture, lights, count);
  if (timer_) timer_->End(cl);
  return ok;
}

void SceneMuzzle::OnDraw(nvrhi::ICommandList* cl, const SceneAo::Draw& d, const Gtao* ao) {
  (void)ao;  // 5 Nov: the light reads the depth alone (muzzle.hlsl), not the AO's normals
  const AoRule* matched = nullptr;
  const AoDrawClass cls = rules_.Classify(d.state, &matched);
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
  td.terminal_transparent = matched && matched->ps_hash && cls == AoDrawClass::kTransparent;
  // OnDraw records a future write; this hook shades before that draw executes.
  const bool depth_before_draw = tracker_.frame() == d.frame && tracker_.depth_valid();
  const AoFrameTracker::Events e = tracker_.OnDraw(td);
  if (e.in_scene) {
    host_scale_[0] = d.screen.host_scale_x;
    host_scale_[1] = d.screen.host_scale_y;
    if (d.color.key && d.color.key == tracker_.scene_color()) scene_color_ = d.color;
    if (d.depth.key && d.depth.key == tracker_.scene_depth()) scene_depth_ = d.depth;
  }
  if (e.opaque_complete && !applied_ && Apply(cl, depth_before_draw)) {
    applied_ = true;
    if (stats_.applied++ == 0)
      Logf(LogLevel::kInfo, "rexgpu-native: muzzle light: first flash lit (frame %llu, normals from the depth)",
           static_cast<unsigned long long>(frame_));
  }
}

void SceneMuzzle::OnDepthClear(uint64_t frame, uint64_t depth_key) { tracker_.OnDepthCleared(frame, depth_key); }

}  // namespace nr
