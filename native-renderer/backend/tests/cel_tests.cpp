// Cel shading (backend/cel_shade.*, docs/cel.md) on a Vulkan device of its own, without the game
// renderer: the scene of gtao_tests (a box on a floor in the corner of two walls, sky above them)
// ray-cast on the CPU through a known camera, the normals prepared by Gtao as the game renderer does,
// the pass run over a colour target and read back. Checks: lines along the box's silhouette and the
// wall-floor crease, none on open floor or wall (a flat surface at a grazing angle included), the sky
// and the alpha untouched, the GPU's bands equal to the CPU's (cel_config.cpp), the band function's
// shape, lines twice as wide at twice the resolution, and lines fading out with distance. The Khronos
// and NVRHI validation layers must stay silent.
//
// NR_TEST_IMAGES=<dir> writes the line views as PPM files. NR_BENCH=1 times the pass at 1080p and 4K.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend/ao_camera.h"
#include "backend/cel_config.h"
#include "backend/cel_shade.h"
#include "backend/gtao.h"
#include "backend/tests/check.h"
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

struct ErrorsChecked {
  int before = Device() ? Device()->errors() : 0;
  ~ErrorsChecked() {
    if (Device()) CHECK_EQ(Device()->errors(), before);
  }
};

// ------------------------------------------------------------- the scene (gtao_tests) ---

struct V3 {
  float x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }

// D3D's left-handed look-at and perspective. `world` scales the whole scene (and the eye) about the
// origin: the same picture, every distance times it.
struct Camera {
  V3 eye{1.5f, 1.6f, -1.5f}, target{-1.0f, 0.3f, 2.6f};
  float fov_y = 1.0472f, near_plane = 0.1f, far_plane = 10000.0f, world = 1.0f;
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
  void Rows(float m[16]) const {
    const V3 axes[3] = {r, u, f};
    const V3 e = eye * world;
    float view[12];
    for (int i = 0; i < 3; ++i) {
      view[i * 4 + 0] = axes[i].x;
      view[i * 4 + 1] = axes[i].y;
      view[i * 4 + 2] = axes[i].z;
      view[i * 4 + 3] = -Dot(axes[i], e);
    }
    for (int c = 0; c < 4; ++c) {
      m[0 * 4 + c] = px * view[0 * 4 + c];
      m[1 * 4 + c] = py * view[1 * 4 + c];
      m[2 * 4 + c] = a * view[2 * 4 + c] + (c == 3 ? b : 0.0f);
      m[3 * 4 + c] = view[2 * 4 + c];
    }
  }
};

struct Box {
  V3 lo, hi;
};
const Box kScene[] = {
    {{-2.0f, -0.1f, -6.0f}, {6.0f, 0.0f, 4.0f}},  // floor
    {{-2.0f, 0.0f, 4.0f}, {6.0f, 3.0f, 4.1f}},    // back wall
    {{-2.1f, 0.0f, -6.0f}, {-2.0f, 3.0f, 4.0f}},  // left wall
    {{-1.6f, 0.0f, 2.4f}, {-0.6f, 0.9f, 3.4f}},   // the box
};

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

AoScreen Screen(float gw, float gh, float scale) {
  AoScreen s;
  s.x_scale = gw / 2, s.x_offset = gw / 2, s.y_scale = -gh / 2, s.y_offset = gh / 2;
  s.half_pixel = 0.5f;
  s.host_scale_x = s.host_scale_y = scale;
  return s;
}

std::vector<float> RayCast(const Camera& cam, const AoScreen& s, uint32_t w, uint32_t h) {
  std::vector<float> z(size_t(w) * h, 1.0f);
  for (uint32_t j = 0; j < h; ++j) {
    for (uint32_t i = 0; i < w; ++i) {
      const float gx = (float(i) + 0.5f) / s.host_scale_x, gy = (float(j) + 0.5f) / s.host_scale_y;
      const float nx = (gx - s.x_offset - s.offset_x()) / s.x_scale, ny = (gy - s.y_offset - s.offset_y()) / s.y_scale;
      const V3 dir = cam.r * (nx / cam.px) + cam.u * (ny / cam.py) + cam.f;
      const float t = Hit(cam.eye, dir);
      if (t > 0.0f) z[size_t(j) * w + i] = cam.a + cam.b / (t * cam.world);
    }
  }
  return z;
}

void ToPixel(const Camera& cam, const AoScreen& s, V3 p, int& x, int& y) {
  const V3 v = p - cam.eye;
  const float vz = Dot(v, cam.f);
  const float nx = cam.px * Dot(v, cam.r) / vz, ny = cam.py * Dot(v, cam.u) / vz;
  x = int(std::floor((nx * s.x_scale + s.x_offset + s.offset_x()) * s.host_scale_x));
  y = int(std::floor((ny * s.y_scale + s.y_offset + s.offset_y()) * s.host_scale_y));
}

nvrhi::TextureHandle DepthTexture(nvrhi::IDevice* device, const std::vector<float>& z, uint32_t w, uint32_t h) {
  nvrhi::TextureDesc td;
  td.width = w;
  td.height = h;
  td.format = nvrhi::Format::R32_FLOAT;
  td.initialState = nvrhi::ResourceStates::ShaderResource;
  td.keepInitialState = true;
  td.debugName = "Test depth";
  nvrhi::TextureHandle t = device->createTexture(td);
  nvrhi::CommandListHandle cl = device->createCommandList();
  cl->open();
  cl->writeTexture(t, 0, 0, z.data(), size_t(w) * 4);
  cl->close();
  device->executeCommandList(cl);
  device->waitForIdle();
  return t;
}

// A colour per pixel (RGBA8): a smooth ramp of hues and brightness, alpha 0.5.
std::vector<uint8_t> Ramp(uint32_t w, uint32_t h) {
  std::vector<uint8_t> px(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const float t = float(x) / float(w), v = float(y) / float(h);
      uint8_t* p = &px[(size_t(y) * w + x) * 4];
      p[0] = uint8_t(std::lround(255.0f * std::clamp(0.05f + 0.9f * t * (1.0f - 0.5f * v), 0.0f, 1.0f)));
      p[1] = uint8_t(std::lround(255.0f * std::clamp(0.02f + 0.8f * v * v, 0.0f, 1.0f)));
      p[2] = uint8_t(std::lround(255.0f * std::clamp(0.6f - 0.5f * t + 0.2f * v, 0.0f, 1.0f)));
      p[3] = 128;
    }
  }
  return px;
}

struct Run {
  Camera cam;
  AoScreen screen;
  uint32_t w = 0, h = 0;
  std::vector<float> z;
  std::vector<uint8_t> in;  // the colour before
  Pixels out;               // and after
  bool ok = false;
};

constexpr uint32_t kGw = 640, kGh = 360;

Run Shade(float scale, const CelConfig& c, float world = 1.0f, double* gpu_ms = nullptr, bool own_normals = true) {
  Run run;
  VulkanTestDevice* gpu = Device();
  nvrhi::IDevice* device = gpu->device();
  run.cam.world = world;
  run.cam.Setup(float(kGw) / float(kGh));
  run.screen = Screen(float(kGw), float(kGh), scale);
  run.w = uint32_t(std::lround(kGw * scale));
  run.h = uint32_t(std::lround(kGh * scale));
  run.z = RayCast(run.cam, run.screen, run.w, run.h);
  nvrhi::TextureHandle depth = DepthTexture(device, run.z, run.w, run.h);
  nvrhi::TextureDesc cd;
  cd.width = run.w;
  cd.height = run.h;
  cd.format = nvrhi::Format::RGBA8_UNORM;
  cd.isRenderTarget = true;
  cd.initialState = nvrhi::ResourceStates::RenderTarget;
  cd.keepInitialState = true;
  cd.debugName = "Test colour";
  nvrhi::TextureHandle color = device->createTexture(cd);
  run.in = Ramp(run.w, run.h);
  Gtao gtao(device);
  CelShade cel(device);
  CHECK(gtao.Initialize());
  CHECK(cel.Initialize());
  float rows[16];
  run.cam.Rows(rows);
  GtaoInput in;
  in.depth = depth;
  in.width = run.w;
  in.height = run.h;
  in.screen = run.screen;
  CHECK(ExtractAoProjection(rows, in.projection));
  GtaoSettings gs;
  gs.strength = 2;
  gs.normal_smoothing = 2;
  gs.half = run.h > 1500;  // the game renderer's rule (render scale above 1.5 of 1280x720: 4K)
  const int runs = gpu_ms ? 70 : 1;
  std::vector<double> ms;
  for (int i = 0; i < runs; ++i) {
    nvrhi::CommandListHandle cl = device->createCommandList();
    cl->open();
    cl->writeTexture(color, 0, 0, run.in.data(), size_t(run.w) * 4);
    if (!own_normals) {
      // Benchmark: the AO's normals reused (prepared outside the timed part).
      CHECK(gtao.Prepare(cl, in, gs));
      CHECK(gtao.SmoothNormals(cl, gs));
    }
    nvrhi::TimerQueryHandle q = gpu_ms ? device->createTimerQuery() : nullptr;
    if (q) cl->beginTimerQuery(q);
    bool ok = true;
    if (own_normals) {
      ok = gtao.Prepare(cl, in, gs);
      CHECK(ok);
      CHECK(gtao.SmoothNormals(cl, gs));
      CHECK(gtao.normals_smoothed());
    }
    ok = ok && cel.Apply(cl, gtao, color, depth, c);
    if (q) cl->endTimerQuery(q);
    cl->close();
    CHECK(ok);
    gpu->Execute(cl);
    gpu->WaitIdle();
    run.ok = ok;
    if (q && i >= 10) ms.push_back(1000.0 * device->getTimerQueryTime(q));
  }
  if (gpu_ms && !ms.empty()) {
    std::sort(ms.begin(), ms.end());
    *gpu_ms = ms[ms.size() / 2];
  }
  run.out = gpu->ReadBack(color);
  return run;
}

// The line view (native_cel_debug 1): 1 white, 0 a full line.
CelConfig LinesView(float outline = 3.0f) {
  CelConfig c;
  c.enabled = true;
  c.outline = outline;
  c.darkness = 1.0f;
  c.strength = 0.0f;
  c.debug = 1;
  return c;
}

float Ink(const Run& r, int x, int y) {
  x = std::clamp(x, 0, int(r.w) - 1);
  y = std::clamp(y, 0, int(r.h) - 1);
  return 1.0f - r.out.at(uint32_t(x), uint32_t(y))[0];
}

// The strongest line within a square of radius `rad` around a world point.
float InkNear(const Run& r, V3 p, int rad) {
  int x, y;
  ToPixel(r.cam, r.screen, p, x, y);
  float best = 0;
  for (int j = -rad; j <= rad; ++j)
    for (int i = -rad; i <= rad; ++i) best = std::max(best, Ink(r, x + i, y + j));
  return best;
}

uint32_t LinePixels(const Run& r, float above = 0.5f) {
  uint32_t n = 0;
  for (uint32_t y = 0; y < r.h; ++y)
    for (uint32_t x = 0; x < r.w; ++x) n += Ink(r, int(x), int(y)) > above ? 1 : 0;
  return n;
}

void Save(const Run& r, const std::string& name) {
  const char* dir = std::getenv("NR_TEST_IMAGES");
  if (dir) WritePpm(std::string(dir) + "/cel_" + name + ".ppm", r.out);
}

// World probes: open floor (near and at a grazing angle far away), the back wall, the floor-wall
// crease, and the box's top face just in front of its back edge (the wall far behind it).
const V3 kOpenFloor{0.8f, 0.0f, 1.2f}, kFarFloor{3.5f, 0.0f, 3.0f}, kWall{1.6f, 1.6f, 4.0f}, kCrease{2.5f, 0.0f, 4.0f},
    kBoxBackEdge{-1.1f, 0.9f, 3.38f}, kBoxCorner{-0.6f, 0.45f, 2.4f};

}  // namespace

TEST(Cel_BandFunction) {
  for (uint32_t bands : {2u, 3u, 4u}) {
    float last = 0;
    std::vector<float> levels;
    for (int i = 1; i <= 4000; ++i) {
      const float y = float(i) / 4000.0f;
      const float v = CelBandLuma(y, bands);
      CHECK(v >= last - 1e-6f);  // never darker for a brighter input
      CHECK(v <= 1.0f);
      last = v;
      // Flat inside a band: the middle of each band in log2 steps gives the band's level.
      const float s = std::log2(y) / CelTuning::kStops + 1.0f;
      const float f = s * float(bands) - std::floor(s * float(bands));
      if (s > 0.02f && f > 0.2f && f < 0.8f) {
        bool seen = false;
        for (float l : levels) seen = seen || std::fabs(l - v) < 1e-5f;
        if (!seen) levels.push_back(v);
      }
    }
    CHECK_EQ(levels.size(), size_t(bands));
    // The top band stays bright, the darkest pixels keep their value.
    CHECK(CelBandLuma(0.95f, bands) > 0.7f);
    CHECK_NEAR(CelBandLuma(0.005f, bands), 0.005f, 1e-6);
    CHECK_NEAR(CelBandLuma(1.5f, bands), 1.5f, 1e-6);
  }
  // The colour: the hue kept, strength 0 the original, white not passed.
  const float orange[3] = {0.8f, 0.4f, 0.1f};
  float out[3];
  CelBandColour(orange, 3, 1.0f, out);
  CHECK_NEAR(out[1] / out[0], 0.5, 1e-4);
  CHECK_NEAR(out[2] / out[0], 0.125, 1e-4);
  CelBandColour(orange, 3, 0.0f, out);
  CHECK_NEAR(out[0], 0.8, 1e-6);
  const float red[3] = {0.95f, 0.05f, 0.05f};  // dark luminance, bright channel: scaled up past white
  CelBandColour(red, 2, 1.0f, out);
  CHECK(out[0] <= 1.0f + 1e-6f);
  CHECK_NEAR(out[1] / out[0], 0.05 / 0.95, 1e-4);
  float opacity = 0;
  CHECK_NEAR(CelRadius(1.5f, 1080, &opacity), 1.5, 1e-6);
  CHECK_NEAR(opacity, 1.0, 1e-6);
  CHECK_NEAR(CelRadius(1.5f, 2160, nullptr), 3.0, 1e-6);
  CHECK_NEAR(CelRadius(0.5f, 1080, &opacity), 1.0, 1e-6);  // thin lines: radius 1, fainter
  CHECK_NEAR(opacity, 0.5, 1e-6);
}

TEST(Cel_LinesOnSilhouettesAndCreasesNotOnFlatSurfaces) {
  if (!Device()) {
    Fail(__FILE__, __LINE__, "no Vulkan device (install mesa-vulkan-drivers for lavapipe)");
    return;
  }
  ErrorsChecked errors;
  const Run r = Shade(1.0f, LinesView());
  CHECK(r.ok);
  Save(r, "lines");
  if (std::getenv("NR_TEST_IMAGES")) {
    CelConfig kinds = LinesView();
    kinds.debug = 2;  // depth lines cyan, creases magenta
    Save(Shade(1.0f, kinds), "line_kinds");
  }
  const float box = InkNear(r, kBoxBackEdge, 1), crease = InkNear(r, kCrease, 2), open = InkNear(r, kOpenFloor, 2),
              far_floor = InkNear(r, kFarFloor, 2), wall = InkNear(r, kWall, 2);
  std::printf("  ink: box edge %.2f, crease %.2f, open floor %.2f, far floor %.2f, wall %.2f, %u line pixels\n", box,
              crease, open, far_floor, wall, LinePixels(r));
  CHECK(box > 0.9f);
  CHECK(crease > 0.5f);
  // The box's vertical edge between its two visible faces (a convex right angle) and the whole floor-wall
  // crease (a concave one), not dashed.
  const float corner = InkNear(r, kBoxCorner, 1);
  float crease_min = 1.0f;
  int crease_points = 0;
  for (float x = -0.4f; x <= 6.0f; x += 0.2f) {
    int px, py;
    ToPixel(r.cam, r.screen, {x, 0.0f, 4.0f}, px, py);
    if (px < 3 || py < 3 || px > int(r.w) - 4 || py > int(r.h) - 4) continue;  // in the picture
    crease_min = std::min(crease_min, InkNear(r, {x, 0.0f, 4.0f}, 1));
    ++crease_points;
  }
  CHECK(crease_points > 10);
  std::printf("  ink: box corner %.2f, weakest point of the floor-wall crease %.2f\n", corner, crease_min);
  CHECK(corner > 0.7f);
  CHECK(crease_min > 0.5f);
  CHECK(open < 0.02f);
  CHECK(far_floor < 0.02f);
  CHECK(wall < 0.02f);
  // Every silhouette pixel of the box against what lies behind it (a neighbour over 20% farther) is inked.
  uint32_t edges = 0, inked = 0;
  for (uint32_t y = 1; y + 1 < r.h; ++y) {
    for (uint32_t x = 1; x + 1 < r.w; ++x) {
      const float d = r.z[size_t(y) * r.w + x];
      if (d >= 1.0f) continue;
      const float z = r.cam.b / (d - r.cam.a);
      bool edge = false;
      for (int o = 0; o < 4; ++o) {
        const int dx = o == 0 ? 1 : o == 1 ? -1 : 0, dy = o == 2 ? 1 : o == 3 ? -1 : 0;
        const float n = r.z[size_t(int(y) + dy) * r.w + uint32_t(int(x) + dx)];
        edge = edge || n >= 1.0f || r.cam.b / (n - r.cam.a) > z * 1.2f;
      }
      if (!edge) continue;
      ++edges;
      inked += Ink(r, int(x), int(y)) > 0.9f ? 1 : 0;
    }
  }
  std::printf("  silhouette pixels %u, inked %u\n", edges, inked);
  CHECK(edges > 100);
  CHECK(inked * 100 >= edges * 97);
}

TEST(Cel_SkyAlphaAndGpuBandsMatchTheCpu) {
  if (!Device()) return;
  ErrorsChecked errors;
  CelConfig c;
  c.enabled = true;
  c.outline = 0.0f;  // bands only
  c.bands = 3;
  c.strength = 0.8f;
  const Run r = Shade(1.0f, c);
  CHECK(r.ok);
  Save(r, "bands");
  float worst = 0, sky_worst = 0, alpha_worst = 0;
  uint32_t sky = 0, changed = 0;
  for (uint32_t y = 0; y < r.h; ++y) {
    for (uint32_t x = 0; x < r.w; ++x) {
      const uint8_t* p = &r.in[(size_t(y) * r.w + x) * 4];
      const float in[3] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
      const float* o = r.out.at(x, y);
      alpha_worst = std::max(alpha_worst, std::fabs(o[3] - p[3] / 255.0f));
      if (r.z[size_t(y) * r.w + x] >= 1.0f) {
        ++sky;
        for (int i = 0; i < 3; ++i) sky_worst = std::max(sky_worst, std::fabs(o[i] - in[i]));
        continue;
      }
      float want[3];
      CelBandColour(in, c.bands, c.strength, want);
      for (int i = 0; i < 3; ++i) {
        worst = std::max(worst, std::fabs(o[i] - want[i]));
        changed += std::fabs(o[i] - in[i]) > 2.0f / 255 ? 1 : 0;
      }
    }
  }
  std::printf("  bands: worst GPU-CPU difference %.4f, %u channels changed; sky %u pixels, worst %.4f; alpha worst %.4f\n",
              worst, changed, sky, sky_worst, alpha_worst);
  CHECK(worst <= 1.5f / 255);
  CHECK(changed > 1000);
  CHECK(sky > 1000);
  CHECK_EQ(sky_worst, 0.0f);
  CHECK_EQ(alpha_worst, 0.0f);
  // Off in every way: the picture comes back as it was.
  CelConfig none = c;
  none.strength = 0.0f;
  const Run same = Shade(1.0f, none);
  float off_worst = 0;
  for (uint32_t i = 0; i < same.w * same.h * 4; ++i)
    off_worst = std::max(off_worst, std::fabs(same.out.rgba[i] - same.in[i] / 255.0f));
  CHECK_EQ(off_worst, 0.0f);
}

TEST(Cel_LinesScaleWithResolution) {
  if (!Device()) return;
  ErrorsChecked errors;
  const Run one = Shade(1.0f, LinesView(6.0f)), two = Shade(2.0f, LinesView(6.0f));  // radius 2 and 4
  const uint32_t a = LinePixels(one), b = LinePixels(two);
  // Twice the resolution: lines twice as wide and twice as long, about four times the pixels.
  std::printf("  line pixels at 640x360 %u, at 1280x720 %u (x%.2f)\n", a, b, double(b) / std::max(a, 1u));
  CHECK(b > a * 3);
  CHECK(b < a * 5);
}

TEST(Cel_LinesFadeWithDistance) {
  if (!Device()) return;
  ErrorsChecked errors;
  const Run near_run = Shade(1.0f, LinesView(), 1.0f), mid = Shade(1.0f, LinesView(), 8.0f),
            far_run = Shade(1.0f, LinesView(), 40.0f);
  const uint32_t a = LinePixels(near_run, 0.05f), b = LinePixels(mid, 0.05f), c = LinePixels(far_run, 0.05f);
  const float ink_mid = InkNear(mid, kBoxBackEdge, 1);
  std::printf("  line pixels: scene x1 %u, x8 (box at about 44 m) %u (box edge %.2f), x40 %u\n", a, b, ink_mid, c);
  CHECK(b < a);
  CHECK(ink_mid < 0.9f && ink_mid > 0.05f);  // the box at about 44 m: a faint line
  CHECK_EQ(c, 0u);
}

// GPU time (NR_BENCH=1 only; not a check): 1920x1080 and 3840x2160, lines and three bands, the
// median of 60 runs: with its own normals (Prepare + smoothing + the pass) and with the AO's (the pass).
TEST(Bench_CelShading) {
  if (!std::getenv("NR_BENCH") || !Device()) return;
  CelConfig c;
  c.enabled = true;
  for (float scale : {3.0f, 6.0f}) {
    for (bool own : {true, false}) {
      double ms = 0;
      Shade(scale, c, 1.0f, &ms, own);
      std::printf("  BENCH cel shading %ux%u, %s: median %.3f ms GPU\n", uint32_t(kGw * scale), uint32_t(kGh * scale),
                  own ? "own normals (prepare + smoothing + pass)" : "the AO's normals (pass only)", ms);
    }
  }
}

NR_TEST_MAIN()
