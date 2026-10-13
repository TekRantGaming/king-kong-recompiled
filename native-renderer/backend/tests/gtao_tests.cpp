// XeGTAO (backend/gtao.*) on a Vulkan device of its own (lavapipe), without the game renderer:
// a depth image ray-cast on the CPU (a box on a floor in the corner of two walls, sky above
// them) through a known camera, the occlusion read back and checked for what must hold: the
// creases are darkened, open floor and the sky are not, half resolution and render scale 2 give
// the same picture, a picture inside a larger target lands where it should, and the apply
// passes multiply the colour by it. Every test checks that the Khronos and NVRHI validation
// layers stayed silent.
//
// NR_TEST_IMAGES=<dir> writes the occlusion images there as PPM files.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend/ao_camera.h"
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

struct Image {
  uint32_t w = 0, h = 0;
  std::vector<float> v;
  float at(int x, int y) const { return v[size_t(std::clamp(y, 0, int(h) - 1)) * w + std::clamp(x, 0, int(w) - 1)]; }
  // The mean over a (2r+1) square: the occlusion is noisy per pixel by design.
  float around(int x, int y, int r = 1) const {
    float s = 0;
    int n = 0;
    for (int j = -r; j <= r; ++j)
      for (int i = -r; i <= r; ++i, ++n) s += at(x + i, y + j);
    return s / float(n);
  }
};

// Reads a 2D R32_FLOAT texture back.
Image ReadR32(nvrhi::IDevice* device, nvrhi::ITexture* texture) {
  Image out;
  const nvrhi::TextureDesc& td = texture->getDesc();
  nvrhi::TextureDesc sd;
  sd.width = td.width;
  sd.height = td.height;
  sd.format = td.format;
  sd.debugName = "Readback R32";
  nvrhi::StagingTextureHandle staging = device->createStagingTexture(sd, nvrhi::CpuAccessMode::Read);
  nvrhi::CommandListHandle cl = device->createCommandList();
  cl->open();
  cl->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
  cl->close();
  device->executeCommandList(cl);
  device->waitForIdle();
  size_t pitch = 0;
  auto* data = static_cast<const uint8_t*>(
      device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
  if (!data) return out;
  out.w = td.width;
  out.h = td.height;
  out.v.resize(size_t(out.w) * out.h);
  for (uint32_t y = 0; y < out.h; ++y) std::memcpy(&out.v[size_t(y) * out.w], data + y * pitch, size_t(out.w) * 4);
  device->unmapStagingTexture(staging);
  return out;
}

void Save(const Image& img, const std::string& name) {
  const char* dir = std::getenv("NR_TEST_IMAGES");
  if (!dir) return;
  Pixels p;
  p.width = img.w;
  p.height = img.h;
  p.rgba.resize(img.v.size() * 4);
  for (size_t i = 0; i < img.v.size(); ++i) p.rgba[i * 4] = p.rgba[i * 4 + 1] = p.rgba[i * 4 + 2] = img.v[i];
  WritePpm(std::string(dir) + "/gtao_" + name + ".ppm", p);
}

// The picture: 192 x 108 guest pixels (16:9).
constexpr uint32_t kGw = 192, kGh = 108;

struct Run {
  Camera cam;
  AoScreen screen;
  uint32_t w = 0, h = 0;
  std::vector<float> z;  // the stored depth (1: sky)
  Image ao;
};

// The occlusion of the scene at a render scale, optionally inside a larger depth texture.
Run Occlusion(float scale, bool half, int strength = 1, uint32_t ox = 0, uint32_t oy = 0, int mode = 1) {
  Run run;
  VulkanTestDevice* gpu = Device();
  run.cam.Setup(float(kGw) / float(kGh));
  run.screen = Screen(float(kGw), float(kGh), scale);
  run.w = uint32_t(std::lround(kGw * scale));
  run.h = uint32_t(std::lround(kGh * scale));
  run.z = RayCast(run.cam, run.screen, run.w, run.h);
  nvrhi::TextureHandle depth = DepthTexture(gpu->device(), run.z, run.w, run.h, run.w + ox + 7, run.h + oy + 5, ox, oy);
  Gtao gtao(gpu->device());
  CHECK(gtao.Initialize());
  float rows[16];
  run.cam.Rows(rows);
  GtaoInput in;
  in.depth = depth;
  in.x = int32_t(ox);
  in.y = int32_t(oy);
  in.width = run.w;
  in.height = run.h;
  CHECK(ExtractAoProjection(rows, in.projection));
  // The viewport of the scene's draws puts the picture at (ox, oy) of the target.
  in.screen = run.screen;
  in.screen.x_offset += float(ox) / scale;
  in.screen.y_offset += float(oy) / scale;
  GtaoSettings s;
  s.mode = mode;
  s.strength = strength;
  s.half = half;
  nvrhi::CommandListHandle cl = gpu->device()->createCommandList();
  cl->open();
  nvrhi::ITexture* out = gtao.Compute(cl, in, s);
  cl->close();
  CHECK(out != nullptr);
  if (!out) return run;
  gpu->Execute(cl);
  gpu->WaitIdle();
  run.ao = ReadR32(gpu->device(), out);
  CHECK_EQ(run.ao.w, run.w);
  CHECK_EQ(run.ao.h, run.h);
  CHECK_EQ(gtao.stats().computed, uint64_t(1));
  return run;
}

// Probe points (world): the floor in the corner by the back wall, the floor beside the box, open
// floor, the middle of the back wall, and the sky above the walls.
const V3 kCrease{2.0f, 0.0f, 3.93f}, kBoxFoot{-0.52f, 0.0f, 2.9f}, kOpenFloor{0.8f, 0.0f, 1.2f},
    kWallMiddle{1.6f, 1.6f, 4.0f};

float Probe(const Run& r, V3 p, int radius = 1) {
  int x, y;
  ToPixel(r.cam, r.screen, p, x, y);
  return r.ao.around(x, y, int(std::lround(radius * r.screen.host_scale_x)));
}

struct ErrorsChecked {
  int before = Device() ? Device()->errors() : 0;
  ~ErrorsChecked() {
    if (Device()) CHECK_EQ(Device()->errors(), before);  // the validation layers stayed silent
  }
};

}  // namespace

TEST(Gtao_CreasesDarkenedOpenSurfacesNot) {
  if (!Device()) {
    Fail(__FILE__, __LINE__, "no Vulkan device (install mesa-vulkan-drivers for lavapipe)");
    return;
  }
  ErrorsChecked errors;
  const Run r = Occlusion(1.0f, false);
  Save(r.ao, "full_scale1");
  const float crease = Probe(r, kCrease), foot = Probe(r, kBoxFoot), open = Probe(r, kOpenFloor),
              wall = Probe(r, kWallMiddle);
  std::printf("  crease %.3f, box foot %.3f, open floor %.3f, wall %.3f\n", crease, foot, open, wall);
  CHECK(crease < 0.75f);
  CHECK(foot < 0.8f);
  CHECK(open > 0.93f);
  CHECK(wall > 0.9f);
  // The sky (above the walls): exactly 1.
  float sky_min = 1.0f;
  int sky = 0;
  for (size_t i = 0; i < r.z.size(); ++i) {
    if (r.z[i] < 1.0f) continue;
    ++sky;
    sky_min = std::min(sky_min, r.ao.v[i]);
  }
  CHECK(sky > 50);
  CHECK_EQ(sky_min, 1.0f);
  // Never negative, never above 1.
  for (float v : r.ao.v) {
    if (v < 0.0f || v > 1.0f) {
      CHECK(v >= 0.0f && v <= 1.0f);
      break;
    }
  }
}

TEST(Gtao_StrengthDarkensMore) {
  if (!Device()) return;
  ErrorsChecked errors;
  const Run one = Occlusion(1.0f, false, 1), three = Occlusion(1.0f, false, 3);
  CHECK(Probe(three, kCrease) < Probe(one, kCrease) - 0.03f);
  CHECK(Probe(three, kOpenFloor) > 0.9f);
}

TEST(Gtao_HalfResolutionMatchesFull) {
  if (!Device()) return;
  ErrorsChecked errors;
  const Run full = Occlusion(1.0f, false), half = Occlusion(1.0f, true);
  Save(half.ao, "half_scale1");
  double sum = 0;
  for (size_t i = 0; i < full.ao.v.size(); ++i) sum += std::fabs(full.ao.v[i] - half.ao.v[i]);
  const double mae = sum / double(full.ao.v.size());
  std::printf("  half against full: mean difference %.4f; crease %.3f / %.3f\n", mae, Probe(half, kCrease),
              Probe(full, kCrease));
  CHECK(mae < 0.04);
  CHECK(Probe(half, kCrease) < 0.8f);
  CHECK(Probe(half, kOpenFloor) > 0.93f);
}

TEST(Gtao_RenderScaleTwoMatchesOneAfterDownsampling) {
  if (!Device()) return;
  ErrorsChecked errors;
  for (bool half : {false, true}) {
    const Run one = Occlusion(1.0f, half), two = Occlusion(2.0f, half);
    Save(two.ao, half ? "half_scale2" : "full_scale2");
    // Scale 2 reduced to the guest's size by an area average, then compared pixel by pixel and at
    // the probes (the same windows on both).
    Run down = one;
    double sum = 0;
    for (uint32_t y = 0; y < one.h; ++y) {
      for (uint32_t x = 0; x < one.w; ++x) {
        const float d = 0.25f * (two.ao.at(2 * x, 2 * y) + two.ao.at(2 * x + 1, 2 * y) + two.ao.at(2 * x, 2 * y + 1) +
                                 two.ao.at(2 * x + 1, 2 * y + 1));
        down.ao.v[size_t(y) * one.w + x] = d;
        sum += std::fabs(d - one.ao.at(int(x), int(y)));
      }
    }
    float worst_probe = 0;
    for (V3 p : {kCrease, kBoxFoot, kOpenFloor, kWallMiddle}) {
      std::printf("    probe %.2f %.2f %.2f: %.3f at 1, %.3f at 2\n", p.x, p.y, p.z, Probe(one, p), Probe(down, p));
      worst_probe = std::max(worst_probe, std::fabs(Probe(down, p) - Probe(one, p)));
    }
    const double mae = sum / double(one.w * one.h);
    std::printf("  %s: scale 2 downsampled against scale 1: mean difference %.4f, worst probe %.3f\n",
                half ? "half" : "full", mae, worst_probe);
    CHECK(mae < 0.02);
    // At half resolution the working image of scale 1 is 96 x 54: the box's thin contact shadow is
    // softer there than at scale 2 (whose working image is the guest's size). Full resolution has
    // the same detail at both scales.
    CHECK(worst_probe < (half ? 0.1f : 0.05f));
  }
}

TEST(Gtao_PictureInsideALargerTarget) {
  if (!Device()) return;
  ErrorsChecked errors;
  // The picture at (24, 10) of a larger depth texture holding something else around it: the result
  // is the picture's alone, the same as without the offset.
  const Run plain = Occlusion(1.0f, false), inside = Occlusion(1.0f, false, 1, 24, 10);
  float worst = 0;
  for (size_t i = 0; i < plain.ao.v.size(); ++i) worst = std::max(worst, std::fabs(plain.ao.v[i] - inside.ao.v[i]));
  CHECK(worst < 1e-5f);
}

TEST(Gtao_ApplyPasses) {
  if (!Device()) return;
  ErrorsChecked errors;
  VulkanTestDevice* gpu = Device();
  nvrhi::IDevice* device = gpu->device();
  Camera cam;
  cam.Setup(float(kGw) / float(kGh));
  const AoScreen screen = Screen(float(kGw), float(kGh), 1.0f);
  const std::vector<float> z = RayCast(cam, screen, kGw, kGh);
  nvrhi::TextureHandle depth = DepthTexture(device, z, kGw, kGh);
  // A colour target a little larger than the picture, grey with alpha 0.5.
  nvrhi::TextureDesc cd;
  cd.width = kGw + 8;
  cd.height = kGh + 4;
  cd.format = nvrhi::Format::RGBA8_UNORM;
  cd.isRenderTarget = true;
  cd.initialState = nvrhi::ResourceStates::RenderTarget;
  cd.keepInitialState = true;
  cd.debugName = "Test colour";
  nvrhi::TextureHandle color = device->createTexture(cd);
  Gtao gtao(device);
  CHECK(gtao.Initialize());
  float rows[16];
  cam.Rows(rows);
  GtaoInput in;
  in.depth = depth;
  in.width = kGw;
  in.height = kGh;
  CHECK(ExtractAoProjection(rows, in.projection));
  in.screen = screen;
  GtaoSettings s;
  nvrhi::CommandListHandle cl = device->createCommandList();
  for (int mode : {1, 2}) {
    cl->open();
    cl->clearTextureFloat(color, nvrhi::AllSubresources, nvrhi::Color(0.8f, 0.6f, 0.4f, 0.5f));
    CHECK(gtao.Compute(cl, in, s) != nullptr);
    CHECK(gtao.ApplyToTarget(cl, color, mode));
    cl->close();
    gpu->Execute(cl);
    gpu->WaitIdle();
    const Image ao = ReadR32(device, gtao.occlusion());
    const Pixels px = gpu->ReadBack(color);
    float worst = 0;
    for (uint32_t y = 0; y < kGh; ++y) {
      for (uint32_t x = 0; x < kGw; ++x) {
        const float a = ao.at(int(x), int(y));
        const float* p = px.at(x, y);
        const float want[3] = {mode == 1 ? 0.8f * a : a, mode == 1 ? 0.6f * a : a, mode == 1 ? 0.4f * a : a};
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(p[c] - want[c]));
        worst = std::max(worst, std::fabs(p[3] - 0.5f) > 1.0f / 255 ? 1.0f : 0.0f);  // alpha untouched
      }
    }
    // Outside the picture: untouched.
    const float* outside = px.at(kGw + 3, kGh + 1);
    CHECK(std::fabs(outside[0] - 0.8f) < 1.5f / 255);
    std::printf("  mode %d: worst difference %.4f\n", mode, worst);
    CHECK(worst <= 1.5f / 255);
  }
  // The copy for the resolve moment, without fog: colour times the occlusion, outside untouched.
  cl->open();
  cl->clearTextureFloat(color, nvrhi::AllSubresources, nvrhi::Color(0.8f, 0.6f, 0.4f, 0.5f));
  CHECK(gtao.Compute(cl, in, s) != nullptr);
  nvrhi::ITexture* copy = gtao.ApplyToCopy(cl, color, depth, AoFog{}, 1);
  CHECK(copy != nullptr);
  cl->close();
  gpu->Execute(cl);
  gpu->WaitIdle();
  if (copy) {
    const Image ao = ReadR32(device, gtao.occlusion());
    const Pixels px = gpu->ReadBack(copy);
    float worst = 0;
    for (uint32_t y = 0; y < kGh; ++y)
      for (uint32_t x = 0; x < kGw; ++x) worst = std::max(worst, std::fabs(px.at(x, y)[0] - 0.8f * ao.at(int(x), int(y))));
    CHECK(worst <= 1.5f / 255);
    CHECK(std::fabs(px.at(kGw + 3, kGh + 1)[0] - 0.8f) < 1.5f / 255);
    CHECK_EQ(gtao.stats().copies, uint64_t(1));
  }
}

TEST(Gtao_DepthTargetsAndTheirCopy) {
  if (!Device()) return;
  ErrorsChecked errors;
  // The game renderer's depth targets are D32S8: read directly and copied (the resolve moment keeps
  // a copy). A plane facing the camera (stored depth 0.995: 20 m away) has no occlusion.
  VulkanTestDevice* gpu = Device();
  nvrhi::IDevice* device = gpu->device();
  nvrhi::TextureDesc dd;
  dd.width = 96;
  dd.height = 64;
  dd.format = nvrhi::Format::D32S8;
  dd.isRenderTarget = true;
  dd.initialState = nvrhi::ResourceStates::DepthWrite;
  dd.keepInitialState = true;
  dd.debugName = "Test D32S8";
  nvrhi::TextureHandle depth = device->createTexture(dd);
  Gtao gtao(device);
  CHECK(gtao.Initialize());
  Camera cam;
  cam.Setup(96.0f / 64.0f);
  float rows[16];
  cam.Rows(rows);
  GtaoInput in;
  in.depth = depth;
  in.width = 96;
  in.height = 64;
  CHECK(ExtractAoProjection(rows, in.projection));
  in.screen = Screen(96, 64, 1.0f);
  nvrhi::CommandListHandle cl = device->createCommandList();
  cl->open();
  cl->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 0.995f, true, 0);
  nvrhi::ITexture* copy = gtao.CopyDepth(cl, depth);
  CHECK(copy != nullptr);
  nvrhi::ITexture* out = gtao.Compute(cl, in, GtaoSettings{});
  CHECK(out != nullptr);
  cl->close();
  gpu->Execute(cl);
  gpu->WaitIdle();
  if (copy) {
    const Image c = ReadR32(device, copy);
    CHECK_EQ(c.w, 96u);
    CHECK(std::fabs(c.at(5, 5) - 0.995f) < 1e-6f && std::fabs(c.at(95, 63) - 0.995f) < 1e-6f);
  }
  if (out) {
    const Image ao = ReadR32(device, out);
    Save(ao, "flat");
    float lowest = 1.0f;
    int lx = 0, ly = 0;
    for (uint32_t i = 0; i < ao.v.size(); ++i) {
      if (ao.v[i] < lowest) {
        lowest = ao.v[i];
        lx = int(i % ao.w);
        ly = int(i / ao.w);
      }
    }
    std::printf("  flat plane: lowest at %d, %d\n", lx, ly);
    // Away from the picture's border (where XeGTAO's samples off the picture repeat the edge):
    // no occlusion to speak of.
    float inner = 1.0f;
    for (uint32_t y = 16; y + 16 < ao.h; ++y)
      for (uint32_t x = 16; x + 16 < ao.w; ++x) inner = std::min(inner, ao.at(int(x), int(y)));
    std::printf("  flat plane: lowest inside %.3f\n", inner);
    CHECK(inner > 0.98f);
    std::printf("  flat plane: lowest %.3f\n", lowest);
    CHECK(lowest > 0.9f);
  }
  CHECK_EQ(gtao.stats().depth_copies, uint64_t(1));
}

TEST(Gtao_TemporalReducesNoiseRejectsDepthAndResets) {
  if (!Device()) return;
  ErrorsChecked errors;
  auto* gpu = Device();
  Camera cam;
  cam.Setup(float(kGw) / kGh);
  GtaoInput in;
  in.width = kGw; in.height = kGh;
  in.screen = Screen(kGw, kGh, 1);
  float rows[16]; cam.Rows(rows);
  CHECK(ExtractAoProjection(rows, in.projection));
  auto z = RayCast(cam, in.screen, kGw, kGh);
  auto depth = DepthTexture(gpu->device(), z, kGw, kGh);
  in.depth = depth;
  in.reprojection_valid = true;
  Gtao temporal(gpu->device()), spatial(gpu->device());
  CHECK(temporal.Initialize() && spatial.Initialize());
  GtaoSettings local, raw;
  local.temporal = true;
  raw.temporal_upscaler = true;  // same rotating noise, without a history here
  auto compute = [&](Gtao& gtao, const GtaoSettings& settings) {
    auto cl = gpu->device()->createCommandList();
    cl->open();
    auto* result = gtao.Compute(cl, in, settings);
    CHECK(result != nullptr);
    cl->close(); gpu->Execute(cl); gpu->WaitIdle();
    return ReadR32(gpu->device(), result);
  };
  auto delta = [&](const Image& a, const Image& b) {
    double sum = 0;
    for (size_t i = 0; i < a.v.size(); ++i) if (z[i] < 1) sum += std::fabs(a.v[i] - b.v[i]);
    return sum / a.v.size();
  };
  double noisy = 0, smooth = 0;
  Image previous_local, previous_raw;
  for (uint64_t f = 1; f <= 24; ++f) {
    in.frame = f;
    Image a = compute(temporal, local), b = compute(spatial, raw);
    if (f > 8) { smooth += delta(a, previous_local); noisy += delta(b, previous_raw); }
    previous_local = a; previous_raw = b;
  }
  std::printf("  rotating spatial MAE %.6f, temporal MAE %.6f\n", noisy / 16, smooth / 16);
  CHECK(smooth < noisy * 0.6);
  CHECK_EQ(temporal.stats().history_frames, uint64_t(23));
  // Skipped frames and a live sample change must start fresh, rather than mix old AO.
  in.frame = 30;
  Image a = compute(temporal, local), b = compute(spatial, raw);
  CHECK(delta(a,b) < 1e-6);
  for (int quality : {1,3,2}) {
    ++in.frame;
    local.sample_quality = raw.sample_quality = quality;
    a = compute(temporal, local); b = compute(spatial, raw);
    CHECK(delta(a,b) < 1e-6);
    for (size_t i = 0; i < a.v.size(); ++i) {
      CHECK(std::isfinite(a.v[i]) && a.v[i] >= 0 && a.v[i] <= 1);
      if (z[i] == 1) CHECK(a.v[i] == 1);
    }
  }
  // A new surface far in front of the previous scene rejects the old depth.
  z.assign(z.size(), 0.5f);
  depth = DepthTexture(gpu->device(), z, kGw, kGh);
  in.depth = depth; ++in.frame;
  a = compute(temporal, local); b = compute(spatial, raw);
  CHECK(delta(a,b) < 1e-6);
  // An off-screen reprojection is also wholly rejected.
  in.reprojection[3] = 4;
  ++in.frame;
  a = compute(temporal, local); b = compute(spatial, raw);
  CHECK(delta(a,b) < 1e-6);
  Save(previous_raw, "temporal_before");
  Save(previous_local, "temporal_after");
}

TEST(Gtao_RefusesWhatItCannotUse) {
  if (!Device()) return;
  ErrorsChecked errors;
  nvrhi::IDevice* device = Device()->device();
  Gtao gtao(device);
  CHECK(gtao.Initialize());
  std::vector<float> z(16 * 16, 0.5f);
  nvrhi::TextureHandle depth = DepthTexture(device, z, 16, 16);
  nvrhi::CommandListHandle cl = device->createCommandList();
  cl->open();
  GtaoInput in;
  CHECK(gtao.Compute(cl, in, GtaoSettings{}) == nullptr);  // no depth
  in.depth = depth;
  in.width = in.height = 16;
  CHECK(gtao.Compute(cl, in, GtaoSettings{}) == nullptr);  // no camera
  Camera cam;
  cam.Setup(1.0f);
  float rows[16];
  cam.Rows(rows);
  CHECK(ExtractAoProjection(rows, in.projection));
  in.width = 17;  // larger than the texture
  CHECK(gtao.Compute(cl, in, GtaoSettings{}) == nullptr);
  CHECK(!gtao.ApplyToTarget(cl, nullptr, 1));
  cl->close();
  Device()->Execute(cl);
  Device()->WaitIdle();
}

NR_TEST_MAIN()
