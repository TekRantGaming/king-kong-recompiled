// A timing bench of the screen-space GI's passes (backend/ssgi.*, docs/gi.md, "Cost") on the real GPU,
// without the game: gtao_tests.cpp's ray-cast scene (a box on a floor in the corner of two walls) at a
// chosen size, a colour image with a colour per surface, then Gtao::Prepare, Ssgi::Compute and
// Ssgi::Composite timed with GPU timer queries over many frames.
//
//   nr_ssgi_bench [width height]     default 2560 1440; prints the mean GPU time of each level
//
// Environment (task 085):
//   NR_SSGI_LEVELS=high          only these levels (comma separated: low, medium, high)
//   NR_SSGI_DETAIL=1             also time the gather alone, the temporal pass and the blur (by
//                                running Compute without the later passes)
//   NR_SSGI_BUMPY=1              a bumpy scene (the depth and normals vary per pixel)
//   NR_SSGI_DUMP=<file>          write the level's result (the light, then the composited colour, floats)
//   NR_SSGI_REF=<file>           compare the result with such a dump: values that differ, largest difference

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
#include "backend/tests/readback.h"
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
      static const bool bumpy = std::getenv("NR_SSGI_BUMPY") != nullptr;
      const float tb = bumpy ? t * (1.0f + 0.004f * std::sin(float(i) * 0.37f) * std::sin(float(j) * 0.23f) +
                                    0.002f * std::sin(float(i * 7 + j * 3) * 0.11f))
                             : t;
      if (t > 0.0f) z[size_t(j) * w + i] = cam.a + cam.b / tb;  // t is the view z: dir has view z 1
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
  // The composite writes into the colour: each configuration starts from the same picture.
  auto reset_color = [&] {
    nvrhi::CommandListHandle cl = dev->createCommandList();
    cl->open();
    cl->writeTexture(color, 0, 0, rgba.data(), size_t(w) * 4);
    cl->close();
    dev->executeCommandList(cl);
    dev->waitForIdle();
  };
  float rows[16];
  cam.Rows(rows);
  GtaoInput in;
  in.depth = depth;
  in.width = w;
  in.height = h;
  ExtractAoProjection(rows, in.projection);
  in.screen = screen;
  std::printf("  %ux%u%s\n", w, h, std::getenv("NR_SSGI_BUMPY") ? " (bumpy)" : "");
  const char* levels_env = std::getenv("NR_SSGI_LEVELS");
  const bool detail = std::getenv("NR_SSGI_DETAIL") != nullptr;
  const char* dump = std::getenv("NR_SSGI_DUMP");
  const char* ref = std::getenv("NR_SSGI_REF");
  int failures = 0;
  // One configuration from a fresh Gtao, Ssgi and colour: `frames` timed frames after 5 warm-up ones.
  // times: the mean prepare, compute and composite GPU times; out: the light and the colour at the end.
  auto run = [&](GiLevel level, bool half, bool temporal, uint32_t denoise, int frames, double times[3],
                 std::vector<float>* out) -> bool {
    reset_color();
    const GiLevelParams p = GiParamsFor(level);
    Gtao gtao(dev);
    Ssgi ssgi(dev);
    if (!gtao.Initialize() || !ssgi.Initialize()) return false;
    GtaoSettings gs;
    gs.half = half;
    SsgiSettings ss;
    ss.level = p;
    ss.level.denoise = denoise;
    ss.radius = p.radius;
    ss.thickness = 0.25f * p.radius;
    // Temporal accumulation as in the game (a still camera: the identity reprojection).
    ss.temporal = temporal;
    ss.history = temporal;
    for (int i = 0; i < 4; ++i) ss.reproject[i * 5] = 1.0f;
    ss.screen_xy[0] = screen.x_scale;
    ss.screen_xy[1] = screen.x_offset + screen.offset_x();
    ss.screen_xy[2] = screen.y_scale;
    ss.screen_xy[3] = screen.y_offset + screen.offset_y();
    ss.screen_z[0] = 1.0f;
    ss.screen_z[2] = screen.host_scale_x;
    ss.screen_z[3] = screen.host_scale_y;
    nvrhi::TimerQueryHandle tq[3] = {dev->createTimerQuery(), dev->createTimerQuery(), dev->createTimerQuery()};
    double sum[3] = {};
    for (int f = 0; f < frames + 5; ++f) {
      nvrhi::CommandListHandle cl = dev->createCommandList();
      cl->open();
      cl->beginTimerQuery(tq[0]);
      gtao.Prepare(cl, in, gs);
      cl->endTimerQuery(tq[0]);
      cl->beginTimerQuery(tq[1]);
      ss.frame = uint32_t(f);
      ssgi.Compute(cl, gtao, color, depth, ss);
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
    for (int i = 0; i < 3; ++i) times[i] = sum[i] / frames;
    if (out) {
      const Pixels light = ReadBackTexture(dev, ssgi.result());
      const Pixels pic = ReadBackTexture(dev, color);
      out->assign(light.rgba.begin(), light.rgba.end());
      out->insert(out->end(), pic.rgba.begin(), pic.rgba.end());
    }
    return true;
  };
  for (GiLevel level : {GiLevel::kLow, GiLevel::kMedium, GiLevel::kHigh}) {
    if (levels_env && !std::strstr(levels_env, GiLevelName(level))) continue;
    for (int quality = 0; quality < 2; ++quality) {  // 0: the level's resolution, 1: the other one
      const GiLevelParams p = GiParamsFor(level);
      const bool half = quality == 0 ? p.half : !p.half;
      double t[3];
      std::vector<float> result;
      if (!run(level, half, true, p.denoise, 60, t, quality == 0 && (dump || ref) ? &result : nullptr)) return 1;
      std::printf("  %-6s %s: prepare %.3f ms, gather and blur %.3f ms, composite %.3f ms, total %.3f ms\n",
                  GiLevelName(level), half ? "half" : "full", t[0], t[1], t[2], t[0] + t[1] + t[2]);
      if (detail && quality == 0) {
        double g[3], gt[3];
        if (!run(level, half, false, 0, 60, g, nullptr) || !run(level, half, true, 0, 60, gt, nullptr)) return 1;
        std::printf("         radiance and gather %.3f ms, temporal %.3f ms, blur (%u passes) %.3f ms\n", g[1],
                    gt[1] - g[1], p.denoise, t[1] - gt[1]);
      }
      if (quality == 0 && dump) {
        std::string path = std::string(dump) + "." + GiLevelName(level);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
          std::fwrite(result.data(), sizeof(float), result.size(), f);
          std::fclose(f);
        }
      }
      if (quality == 0 && ref) {
        std::string path = std::string(ref) + "." + GiLevelName(level);
        std::vector<float> want(result.size());
        FILE* f = std::fopen(path.c_str(), "rb");
        const size_t n = f ? std::fread(want.data(), sizeof(float), want.size(), f) : 0;
        if (f) std::fclose(f);
        if (n != result.size()) {
          std::printf("  reference %s: missing or of another size\n", path.c_str());
          ++failures;
        } else {
          double largest = 0;
          size_t differ = 0;
          for (size_t i = 0; i < n; ++i) {
            const double d = std::fabs(double(result[i]) - double(want[i]));
            if (d > 0) ++differ;
            largest = std::max(largest, d);
          }
          std::printf("  %-6s against the reference: %zu of %zu values differ, largest difference %.3g\n",
                      GiLevelName(level), differ, n, largest);
        }
      }
    }
  }
  std::printf("  validation errors: %d\n", gpu->errors());
  return failures;
}
