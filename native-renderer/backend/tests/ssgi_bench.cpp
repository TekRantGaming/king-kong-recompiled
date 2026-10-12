// A timing bench of the screen-space GI's passes (backend/ssgi.*, docs/gi.md, "Cost") on the real GPU,
// without the game: gtao_tests.cpp's ray-cast scene (a box on a floor in the corner of two walls) at a
// chosen size, a colour image with a colour per surface, then Gtao::Prepare, Ssgi::Compute and
// Ssgi::Composite timed with GPU timer queries over many frames.
//
//   nr_ssgi_bench [width height]     default 2560 1440; prints the mean GPU time of each level

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend/ao_camera.h"
#include "backend/gi_config.h"
#include "backend/gtao.h"
#include "backend/ssgi.h"
#include "backend/tests/vk_test_device.h"

using namespace nr;
using namespace nr::test;

namespace {

VulkanTestDevice* Device() {
  static std::unique_ptr<VulkanTestDevice> device = [] {
    auto d = VulkanTestDevice::Create(true);
    if (d) {
      std::printf("  device: %s, Khronos validation %s\n", d->device_name().c_str(),
                  d->khronos_validation() ? "on" : "not installed");
    }
    return d;
  }();
  return device.get();
}

// ------------------------------------------------------------- the scene ---

struct V3 {
  float x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }

// The camera: D3D's left-handed look-at and perspective (view z forward, depth 0..1).
struct Camera {
  V3 eye{1.5f, 1.6f, -1.5f}, target{-1.0f, 0.3f, 2.6f};
  float fov_y = 1.0472f, near_plane = 0.1f, far_plane = 100.0f;
  V3 r, u, f;
  float px = 0, py = 0, a = 0, b = 0;
  void Setup(float aspect) {
    f = Normalize(target - eye);
    r = Normalize(Cross({0, 1, 0}, f));
    u = Cross(f, r);
    py = 1.0f / std::tan(fov_y / 2);
    px = py / aspect;
    a = far_plane / (far_plane - near_plane);
    b = -near_plane * far_plane / (far_plane - near_plane);
  }
  // The world-view-projection rows the game's vertex shaders would get (clip = rows . position).
  void Rows(float m[16]) const {
    const V3 axes[3] = {r, u, f};
    float view[12];
    for (int i = 0; i < 3; ++i) {
      view[i * 4 + 0] = axes[i].x;
      view[i * 4 + 1] = axes[i].y;
      view[i * 4 + 2] = axes[i].z;
      view[i * 4 + 3] = -Dot(axes[i], eye);
    }
    for (int c = 0; c < 4; ++c) {
      m[0 * 4 + c] = px * view[0 * 4 + c];
      m[1 * 4 + c] = py * view[1 * 4 + c];
      m[2 * 4 + c] = a * view[2 * 4 + c] + (c == 3 ? b : 0.0f);
      m[3 * 4 + c] = view[2 * 4 + c];
    }
  }
};

// Axis-aligned boxes; the floor and walls are thin ones.
struct Box {
  V3 lo, hi;
};
const Box kScene[] = {
    {{-2.0f, -0.1f, -6.0f}, {6.0f, 0.0f, 4.0f}},  // floor
    {{-2.0f, 0.0f, 4.0f}, {6.0f, 3.0f, 4.1f}},    // back wall
    {{-2.1f, 0.0f, -6.0f}, {-2.0f, 3.0f, 4.0f}},  // left wall
    {{-1.6f, 0.0f, 2.4f}, {-0.6f, 0.9f, 3.4f}},   // the box
};

// Distance along the ray to the nearest box (or -1).
float Hit(V3 o, V3 d) {
  float best = -1.0f;
  for (const Box& box : kScene) {
    float t0 = 0.0f, t1 = 1e30f;
    const float os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z};
    const float lo[3] = {box.lo.x, box.lo.y, box.lo.z}, hi[3] = {box.hi.x, box.hi.y, box.hi.z};
    bool miss = false;
    for (int i = 0; i < 3 && !miss; ++i) {
      if (std::fabs(ds[i]) < 1e-9f) {
        miss = os[i] < lo[i] || os[i] > hi[i];
        continue;
      }
      float a = (lo[i] - os[i]) / ds[i], b = (hi[i] - os[i]) / ds[i];
      if (a > b) std::swap(a, b);
      t0 = std::max(t0, a);
      t1 = std::min(t1, b);
      miss = t0 > t1;
    }
    if (!miss && t0 > 0.0f && (best < 0.0f || t0 < best)) best = t0;
  }
  return best;
}

// The screen of a picture of gw x gh guest pixels at `scale`, D3D9's half-pixel offset on.
AoScreen Screen(float gw, float gh, float scale) {
  AoScreen s;
  s.x_scale = gw / 2, s.x_offset = gw / 2, s.y_scale = -gh / 2, s.y_offset = gh / 2;
  s.half_pixel = 0.5f;
  s.host_scale_x = s.host_scale_y = scale;
  return s;
}

// The stored depth of every host pixel of the picture (1 = sky), ray-cast through each pixel's
// centre as the rasteriser sees it.
std::vector<float> RayCast(const Camera& cam, const AoScreen& s, uint32_t w, uint32_t h) {
  std::vector<float> z(size_t(w) * h, 1.0f);
  for (uint32_t j = 0; j < h; ++j) {
    for (uint32_t i = 0; i < w; ++i) {
      const float gx = (float(i) + 0.5f) / s.host_scale_x, gy = (float(j) + 0.5f) / s.host_scale_y;
      const float nx = (gx - s.x_offset - s.offset_x()) / s.x_scale, ny = (gy - s.y_offset - s.offset_y()) / s.y_scale;
      const V3 dir = cam.r * (nx / cam.px) + cam.u * (ny / cam.py) + cam.f;  // view z = 1 along it
      const float t = Hit(cam.eye, dir);
      if (t > 0.0f) z[size_t(j) * w + i] = cam.a + cam.b / t;  // t is the view z: dir has view z 1
    }
  }
  return z;
}

// The host pixel a world point lands on.
void ToPixel(const Camera& cam, const AoScreen& s, V3 p, int& x, int& y) {
  const V3 v = p - cam.eye;
  const float vz = Dot(v, cam.f);
  const float nx = cam.px * Dot(v, cam.r) / vz, ny = cam.py * Dot(v, cam.u) / vz;
  x = int(std::floor((nx * s.x_scale + s.x_offset + s.offset_x()) * s.host_scale_x));
  y = int(std::floor((ny * s.y_scale + s.y_offset + s.offset_y()) * s.host_scale_y));
}

// ------------------------------------------------------------- textures ---

nvrhi::TextureHandle DepthTexture(nvrhi::IDevice* device, const std::vector<float>& z, uint32_t w, uint32_t h,
                                  uint32_t tw = 0, uint32_t th = 0, uint32_t ox = 0, uint32_t oy = 0) {
  tw = tw ? tw : w;
  th = th ? th : h;
  std::vector<float> texels(size_t(tw) * th, 0.25f);  // outside the picture: something else
  for (uint32_t j = 0; j < h; ++j)
    for (uint32_t i = 0; i < w; ++i) texels[size_t(j + oy) * tw + i + ox] = z[size_t(j) * w + i];
  nvrhi::TextureDesc td;
  td.width = tw;
  td.height = th;
  td.format = nvrhi::Format::R32_FLOAT;
  td.initialState = nvrhi::ResourceStates::ShaderResource;
  td.keepInitialState = true;
  td.debugName = "Test depth";
  nvrhi::TextureHandle t = device->createTexture(td);
  nvrhi::CommandListHandle cl = device->createCommandList();
  cl->open();
  cl->writeTexture(t, 0, 0, texels.data(), size_t(tw) * 4);
  cl->close();
  device->executeCommandList(cl);
  device->waitForIdle();
  return t;
}


}  // namespace

int main(int argc, char** argv) {
  const uint32_t w = argc > 2 ? uint32_t(std::atoi(argv[1])) : 2560, h = argc > 2 ? uint32_t(std::atoi(argv[2])) : 1440;
  VulkanTestDevice* gpu = Device();
  if (!gpu) return 1;
  nvrhi::IDevice* dev = gpu->device();
  const float scale = float(w) / 192.0f;
  Camera cam;
  cam.Setup(16.0f / 9.0f);
  const AoScreen screen = Screen(192.0f, 108.0f, scale);
  const std::vector<float> z = RayCast(cam, screen, w, h);
  nvrhi::TextureHandle depth = DepthTexture(dev, z, w, h);
  // The colour: a fixed pattern, darker where it is sky (the passes do not care what it holds).
  std::vector<uint32_t> rgba(size_t(w) * h);
  for (uint32_t j = 0; j < h; ++j)
    for (uint32_t i = 0; i < w; ++i)
      rgba[size_t(j) * w + i] = 0xFF000000u | ((i * 255 / w) << 16) | ((j * 255 / h) << 8) | (z[size_t(j) * w + i] < 1 ? 160 : 40);
  nvrhi::TextureDesc cd;
  cd.width = w;
  cd.height = h;
  cd.format = nvrhi::Format::RGBA8_UNORM;
  cd.isRenderTarget = true;
  cd.initialState = nvrhi::ResourceStates::RenderTarget;
  cd.keepInitialState = true;
  cd.debugName = "Bench colour";
  nvrhi::TextureHandle color = dev->createTexture(cd);
  {
    nvrhi::CommandListHandle cl = dev->createCommandList();
    cl->open();
    cl->writeTexture(color, 0, 0, rgba.data(), size_t(w) * 4);
    cl->close();
    dev->executeCommandList(cl);
    dev->waitForIdle();
  }
  float rows[16];
  cam.Rows(rows);
  GtaoInput in;
  in.depth = depth;
  in.width = w;
  in.height = h;
  ExtractAoProjection(rows, in.projection);
  in.screen = screen;
  std::printf("  %ux%u\n", w, h);
  for (GiLevel level : {GiLevel::kLow, GiLevel::kMedium, GiLevel::kHigh}) {
    for (int quality = 0; quality < 2; ++quality) {  // 0: the level's resolution, 1: the other one
      const GiLevelParams p = GiParamsFor(level);
      Gtao gtao(dev);
      Ssgi ssgi(dev);
      if (!gtao.Initialize() || !ssgi.Initialize()) return 1;
      GtaoSettings gs;
      gs.half = quality == 0 ? p.half : !p.half;
      SsgiSettings ss;
      ss.level = p;
      ss.radius = p.radius;
      ss.thickness = 0.25f * p.radius;
      nvrhi::TimerQueryHandle tq[3] = {dev->createTimerQuery(), dev->createTimerQuery(), dev->createTimerQuery()};
      double sum[3] = {};
      const int frames = 60;
      for (int f = 0; f < frames + 5; ++f) {
        nvrhi::CommandListHandle cl = dev->createCommandList();
        cl->open();
        cl->beginTimerQuery(tq[0]);
        gtao.Prepare(cl, in, gs);
        cl->endTimerQuery(tq[0]);
        cl->beginTimerQuery(tq[1]);
        ssgi.Compute(cl, gtao, color, ss);
        cl->endTimerQuery(tq[1]);
        cl->beginTimerQuery(tq[2]);
        ssgi.Composite(cl, gtao, color, depth, ss);
        cl->endTimerQuery(tq[2]);
        cl->close();
        dev->executeCommandList(cl);
        dev->waitForIdle();
        dev->runGarbageCollection();
        for (int i = 0; i < 3; ++i) {
          const double t = double(dev->getTimerQueryTime(tq[i])) * 1000.0;
          if (f >= 5) sum[i] += t;
          dev->resetTimerQuery(tq[i]);
        }
      }
      std::printf("  %-6s %s: prepare %.3f ms, gather and blur %.3f ms, composite %.3f ms, total %.3f ms\n", GiLevelName(level),
                  gs.half ? "half" : "full", sum[0] / frames, sum[1] / frames, sum[2] / frames,
                  (sum[0] + sum[1] + sum[2]) / frames);
    }
  }
  std::printf("  validation errors: %d\n", gpu->errors());
  return 0;
}
