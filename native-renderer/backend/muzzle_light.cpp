// Muzzle flashes that cast light: see muzzle_light.h.

#include "backend/muzzle_light.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "thirdparty/XeGTAO/XeGTAO.h"

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

static_assert(sizeof(XeGTAO::GTAOConstants) == 96, "GTAOConstants is 6 float4");
constexpr size_t kGtaoShared = 96 + 16 + 16;

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
#if defined(KK_DEV_TOOLS)
  // KK_DEV_MUZZLE_HOLD=<seconds>: a flash stays at full strength this long (pictures of what it lights).
  static const float hold = [] {
    const char* v = std::getenv("KK_DEV_MUZZLE_HOLD");
    return v ? float(std::atof(v)) : 0.0f;
  }();
#else
  constexpr float hold = 0.0f;
#endif
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
  uint8_t gtao[kGtaoShared] = {};  // XeGTAO's constants, the picture, the working size (gtao.hlsl)
  float params[4] = {};            // step, the sky's stored depth, radius, fall-off
  float colour[4] = {};            // the light's colour, lights
  float surface[4] = {};           // reflectance in the dark, in bright light, wrap, hue floor
  float light[MuzzleTuning::kMaxLights][4] = {};
};

SceneMuzzle::SceneMuzzle(nvrhi::IDevice* device, std::vector<AoRule> rules) : device_(device), rules_(std::move(rules)) {}
SceneMuzzle::~SceneMuzzle() = default;

bool SceneMuzzle::Initialize() {
  gtao_ = std::make_unique<Gtao>(device_);
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
  if (!gtao_->Initialize() || !vs_ || !ps_) {
    gtao_.reset();
    return false;
  }
  using I = nvrhi::BindingLayoutItem;
  nvrhi::BindingLayoutDesc bl;
  bl.visibility = nvrhi::ShaderType::All;
  bl.bindings = {I::VolatileConstantBuffer(0), I::Texture_SRV(0), I::Texture_SRV(1), I::Texture_SRV(2)};
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
      ready = GraphicsPipeline(info) && ready;
    }
  }
  if (ready) Logf(LogLevel::kInfo, "rexgpu-native: muzzle light: ready");
  return ready;
}

void SceneMuzzle::ForgetTargets() {
  if (gtao_) gtao_->ForgetTargetViews();
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

nvrhi::IGraphicsPipeline* SceneMuzzle::GraphicsPipeline(const nvrhi::FramebufferInfo& info) {
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

bool SceneMuzzle::Shade(nvrhi::ICommandList* cl, const Gtao& gtao, nvrhi::ITexture* color, nvrhi::ITexture* depth,
                        const MuzzleLightNow* lights, uint32_t count) {
  if (!gtao.normals() || gtao.constants_bytes().size() < kGtaoShared + 16) return false;
  const nvrhi::TextureDesc& cd = color->getDesc();
  if (!copy_ || copy_->getDesc().width != cd.width || copy_->getDesc().height != cd.height ||
      copy_->getDesc().format != cd.format) {
    sets_.clear();
    nvrhi::TextureDesc td;
    td.width = cd.width;
    td.height = cd.height;
    td.format = cd.format;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "Muzzle light scene colour";
    copy_ = device_->createTexture(td);
    if (!copy_) return false;
  }
  Constants k;
  const std::vector<uint8_t>& g = gtao.constants_bytes();
  std::memcpy(k.gtao, g.data(), kGtaoShared);
  float gtao_params[4];
  std::memcpy(gtao_params, g.data() + kGtaoShared, sizeof(gtao_params));
  k.params[0] = gtao_params[0];  // step
  k.params[1] = gtao_params[1];  // the sky's stored depth
  k.params[2] = MuzzleTuning::kRadius;
  k.params[3] = MuzzleTuning::kFalloff;
  k.colour[0] = MuzzleTuning::kColour[0];
  k.colour[1] = MuzzleTuning::kColour[1];
  k.colour[2] = MuzzleTuning::kColour[2];
  k.colour[3] = float(count);
  k.surface[0] = MuzzleTuning::kDarkSurface;
  k.surface[1] = MuzzleTuning::kBrightSurface;
  k.surface[2] = MuzzleTuning::kWrap;
  k.surface[3] = MuzzleTuning::kHueFloor;
  for (uint32_t i = 0; i < count; ++i) {
    k.light[i][0] = lights[i].x;
    k.light[i][1] = lights[i].y;
    k.light[i][2] = lights[i].z;
    k.light[i][3] = lights[i].strength;
  }
  int32_t source[4];
  std::memcpy(source, k.gtao + 96, sizeof(source));
  const int32_t x0 = std::max(source[0], 0), y0 = std::max(source[1], 0);
  const int32_t x1 = std::min(source[0] + source[2], int32_t(cd.width));
  const int32_t y1 = std::min(source[1] + source[3], int32_t(cd.height));
  if (x1 <= x0 || y1 <= y0) return false;
  nvrhi::IFramebuffer* fb = Framebuffer(color);
  nvrhi::IGraphicsPipeline* pipeline = fb ? GraphicsPipeline(fb->getFramebufferInfo()) : nullptr;
  if (!pipeline) return false;
  const nvrhi::TextureSlice slice = nvrhi::TextureSlice().resolve(cd);
  cl->copyTexture(copy_, slice, color, slice);
  cl->writeBuffer(constants_, &k, sizeof(k));
  if (sets_.size() > 16) sets_.clear();
  using S = nvrhi::BindingSetItem;
  nvrhi::BindingSetHandle& set = sets_[{depth, gtao.normals()}];
  if (!set) {
    nvrhi::BindingSetDesc bs;
    bs.bindings = {S::ConstantBuffer(0, constants_), S::Texture_SRV(0, copy_),
                   S::Texture_SRV(1, depth, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
                   S::Texture_SRV(2, gtao.normals())};
    set = device_->createBindingSet(bs, layout_);
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
  return true;
}

bool SceneMuzzle::Apply(nvrhi::ICommandList* cl, const Gtao* ao, bool depth_before_draw) {
  if (!depth_before_draw || !tracker_.depth_valid() || !scene_depth_.texture || !scene_color_.texture) return false;
  MuzzleLightNow lights[MuzzleTuning::kMaxLights];
  const uint32_t count = LiveMuzzleLights(lights, Clock::now());
  if (!count) return false;
  nvrhi::ITexture* depth = scene_depth_.texture;
  const Gtao* source = ao && ao->normals() ? ao : gtao_.get();
  if (timer_) timer_->Begin(cl);
  bool ok = true;
  if (source == gtao_.get()) {
    AoProjection p;
    AoScreen sc;
    if (!camera_.Get(frame_, p, sc)) {
      if (timer_) timer_->End(cl);
      if (!warned_camera_) Logf(LogLevel::kWarning, "rexgpu-native: muzzle light: no camera among the scene's draws yet");
      warned_camera_ = true;
      return false;
    }
    const nvrhi::TextureDesc& dd = depth->getDesc();
    const int32_t x0 = std::max(0, int32_t(std::lround(sc.left() * sc.host_scale_x)));
    const int32_t y0 = std::max(0, int32_t(std::lround(sc.top() * sc.host_scale_y)));
    const int32_t x1 = std::min(int32_t(dd.width), int32_t(std::lround((sc.left() + sc.width()) * sc.host_scale_x)));
    const int32_t y1 = std::min(int32_t(dd.height), int32_t(std::lround((sc.top() + sc.height()) * sc.host_scale_y)));
    if (x1 - x0 < 16 || y1 - y0 < 16) {
      if (timer_) timer_->End(cl);
      return false;
    }
    GtaoInput in;
    in.depth = depth;
    in.x = x0;
    in.y = y0;
    in.width = uint32_t(x1 - x0);
    in.height = uint32_t(y1 - y0);
    in.projection = p;
    in.screen = sc;
    in.frame = frame_;
    GtaoSettings gs;
    gs.half = sc.host_scale_x > 1.5f || sc.host_scale_y > 1.5f;  // as the AO's "auto" (4K: half)
    gs.strength = 2;
    gs.normal_smoothing = 0;  // a soft light over a few frames: the plain normals do
    ok = gtao_->Prepare(cl, in, gs);
  }
  ok = ok && Shade(cl, *source, scene_color_.texture, depth, lights, count);
  if (timer_) timer_->End(cl);
  return ok;
}

void SceneMuzzle::OnDraw(nvrhi::ICommandList* cl, const SceneAo::Draw& d, const Gtao* ao) {
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
    if (d.color.key && d.color.key == tracker_.scene_color()) scene_color_ = d.color;
    if (d.depth.key && d.depth.key == tracker_.scene_depth()) scene_depth_ = d.depth;
  }
  if (e.opaque_complete && !applied_ && Apply(cl, ao, depth_before_draw)) {
    applied_ = true;
    if (stats_.applied++ == 0)
      Logf(LogLevel::kInfo, "rexgpu-native: muzzle light: first flash lit (frame %llu, %s normals)",
           static_cast<unsigned long long>(frame_), ao && ao->normals() ? "the AO's" : "its own");
  }
}

void SceneMuzzle::OnDepthClear(uint64_t frame, uint64_t depth_key) { tracker_.OnDepthCleared(frame, depth_key); }

}  // namespace nr
