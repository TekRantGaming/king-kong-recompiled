// Ambient occlusion (backend/ambient_occlusion.*) on a Vulkan device of its own
// (lavapipe or SwiftShader): synthetic depth and colour in, the scratch colour
// out, read back and compared with a CPU reference of the same technique.
//
// The "depth" the AO reads is any texture whose .x is the depth the game stored
// (0..1, linear distance in near-plane units = 1 / (1 - z)): the tests use an
// R32_FLOAT texture they write exactly, and one D32S8 render target (the format
// of the game renderer's depth targets) cleared to a constant.
//
// The reference (RefAo) follows ambient_occlusion.hlsl line by line, with the
// working images rounded to the formats they are stored in (R16F, RG16F). What
// it cannot match exactly is the GPU's sin / cos / exp / rsqrt and the rounding
// of the 10 sample positions, so the comparison has a stated tolerance
// (kMaxLevels / kMeanLevels below) and the tests also check facts that hold
// without it (flat surfaces are not darkened, the crease beside a box is,
// the sky and the far distance are not).
//
// NR_TEST_IMAGES=<dir> writes the read-back images as PPM files.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "backend/ambient_occlusion.h"
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

void MaybeSave(const Pixels& p, const std::string& name) {
  if (const char* dir = std::getenv("NR_TEST_IMAGES")) WritePpm(std::string(dir) + "/ao_" + name + ".ppm", p);
}

// ------------------------------------------------------------------ reference

float Half(float v) {  // round to binary16 (nearest even) and back
  uint32_t b;
  std::memcpy(&b, &v, 4);
  const uint32_t sign = b & 0x80000000u;
  const int32_t exp = int32_t((b >> 23) & 0xFF) - 127;
  float out;
  if (std::isnan(v) || std::isinf(v)) return v;
  if (exp > 15) {
    out = std::numeric_limits<float>::infinity();
  } else if (exp < -24) {
    out = 0.0f;
  } else {
    // Round the 24-bit significand to the half's precision at this exponent.
    const int bits = exp >= -14 ? 10 : 10 + (exp + 14);  // fraction bits kept (subnormals fewer)
    const double scale = std::ldexp(1.0, bits - exp);
    out = float(std::nearbyint(std::fabs(double(v)) * scale) / scale);
  }
  uint32_t o;
  std::memcpy(&o, &out, 4);
  o |= sign;
  std::memcpy(&out, &o, 4);
  return out;
}

struct V3 {
  float x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Normalize(V3 a) {
  const float l = std::sqrt(Dot(a, a));
  return {a.x / l, a.y / l, a.z / l};
}
float Saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
float Frac(float v) { return v - std::floor(v); }

constexpr float kSky = 60000.0f;
float LinearDepth(float z) {
  return z >= 0.99999f ? kSky : std::min(1.0f / std::max(1.0f - z, 1.0f / 65536.0f), 50000.0f);
}

struct Scene {
  uint32_t w = 0, h = 0;
  std::vector<float> z;         // raw depth 0..1
  std::vector<uint8_t> rgba;    // colour
};

struct RefIn {
  Scene scene;
  AoSettings settings;
  AoFog fog;
  float fog_color_rgb[3] = {};
};

struct RefOut {
  std::vector<float> ao;      // full size: x multiplier
  std::vector<float> fog;     // full size: y fog colour to add back
  std::vector<uint8_t> color; // after the apply pass (mode 1), RGBA8
  std::vector<uint8_t> grey;  // mode 2 (AO only), RGBA8
};

RefOut RefAo(const RefIn& in) {
  const uint32_t w = in.scene.w, h = in.scene.h;
  const uint32_t hw = std::max(w / 2, 1u), hh = std::max(h / 2, 1u);
  const std::array<float, 3> tune = AmbientOcclusion::TuningFor(in.settings.strength);
  const float tan_x = in.settings.tan_half_fov_x, tan_y = tan_x * float(h) / float(w);
  const float focal = float(hh) * 0.5f / tan_y;
  const float fade = in.settings.fade_distance;

  std::vector<float> lin(size_t(hw) * hh);
  for (uint32_t y = 0; y < hh; ++y)
    for (uint32_t x = 0; x < hw; ++x) {
      const uint32_t px = std::min(x * 2, w - 1), py = std::min(y * 2, h - 1);
      lin[size_t(y) * hw + x] = LinearDepth(in.scene.z[size_t(py) * w + px]);
    }
  auto view_pos = [&](int qx, int qy) {
    qx = std::clamp(qx, 0, int(hw) - 1);
    qy = std::clamp(qy, 0, int(hh) - 1);
    const float zl = lin[size_t(qy) * hw + qx];
    const float nx = (float(qx) + 0.5f) / float(hw) * 2.0f - 1.0f;
    const float ny = (float(qy) + 0.5f) / float(hh) * 2.0f - 1.0f;
    return V3{nx * tan_x * zl, -ny * tan_y * zl, zl};
  };

  std::vector<float> ao_a(size_t(hw) * hh, 1.0f), ao_b(size_t(hw) * hh, 1.0f);
  for (int y = 0; y < int(hh); ++y)
    for (int x = 0; x < int(hw); ++x) {
      float ao = 1.0f;
      const V3 P = view_pos(x, y);
      if (P.z < kSky) {
        const V3 pr = view_pos(x + 1, y), pl = view_pos(x - 1, y), pd = view_pos(x, y + 1), pu = view_pos(x, y - 1);
        const V3 dx = std::fabs(pr.z - P.z) < std::fabs(P.z - pl.z) ? pr - P : P - pl;
        const V3 dy = std::fabs(pd.z - P.z) < std::fabs(P.z - pu.z) ? pd - P : P - pu;
        V3 N = Normalize(Cross(dx, dy));
        if (Dot(N, P) > 0.0f) N = {-N.x, -N.y, -N.z};
        const float R = tune[0];
        const float radius_px = std::clamp(R * focal / P.z, 1.0f, float(hh) * 0.05f);
        const float rotation = Frac(52.9829189f * Frac(0.06711056f * float(x) + 0.00583715f * float(y))) * 6.2831853f;
        float sum = 0.0f;
        for (int i = 0; i < 10; ++i) {
          const float a = rotation + float(i) * 2.3999632f;
          const float r = std::sqrt((float(i) + 0.5f) / 10.0f) * radius_px;
          const int qx = x + int(std::nearbyint(std::cos(a) * r)), qy = y + int(std::nearbyint(std::sin(a) * r));
          if (qx < 0 || qy < 0 || qx >= int(hw) || qy >= int(hh)) continue;
          const V3 v = view_pos(qx, qy) - P;
          const float vv = Dot(v, v);
          const float falloff = Saturate(1.0f - vv / (R * R));
          sum += std::max(0.0f, Dot(v, N) / std::sqrt(vv + 1e-6f) - tune[1]) * falloff;
        }
        ao = Saturate(1.0f - tune[2] * sum / 10.0f);
      }
      ao_a[size_t(y) * hw + x] = Half(ao);
    }
  auto blur = [&](const std::vector<float>& src, std::vector<float>& dst, int dx, int dy) {
    for (int y = 0; y < int(hh); ++y)
      for (int x = 0; x < int(hw); ++x) {
        const float zc = lin[size_t(y) * hw + x];
        float total = 0.0f, weight = 0.0f;
        for (int i = -4; i <= 4; ++i) {
          const int qx = std::clamp(x + dx * i, 0, int(hw) - 1), qy = std::clamp(y + dy * i, 0, int(hh) - 1);
          const float wgt = std::exp(-std::fabs(lin[size_t(qy) * hw + qx] - zc) / (zc * 0.02f + 0.0001f)) *
                            (5.0f - std::fabs(float(i)));
          total += src[size_t(qy) * hw + qx] * wgt;
          weight += wgt;
        }
        dst[size_t(y) * hw + x] = Half(weight > 0.0f ? total / weight : 1.0f);
      }
  };
  blur(ao_a, ao_b, 1, 0);
  blur(ao_b, ao_a, 0, 1);

  auto fog_amount = [&](float z) {
    if (!in.fog.valid) return 0.0f;
    const float inverse_depth = in.fog.normalizing[0] * std::min(z, in.fog.normalizing[2]) - in.fog.normalizing[1];
    if (inverse_depth <= 0.0f) return 1.0f;
    const float fog = Saturate((1.0f / inverse_depth) * in.fog.inverse_distance - in.fog.near_over_distance);
    return Saturate(fog * in.fog.density);
  };

  RefOut out;
  out.ao.resize(size_t(w) * h);
  out.fog.resize(size_t(w) * h);
  out.color.resize(size_t(w) * h * 4);
  out.grey.resize(size_t(w) * h * 4);
  for (int y = 0; y < int(h); ++y)
    for (int x = 0; x < int(w); ++x) {
      const float z = in.scene.z[size_t(y) * w + x];
      const float zc = LinearDepth(z);
      float ao = 1.0f, fog = 0.0f;
      if (zc < kSky) {
        const float hpx = (float(x) + 0.5f) * (float(hw) / float(w)) - 0.5f;
        const float hpy = (float(y) + 0.5f) * (float(hh) / float(h)) - 0.5f;
        const int bx = int(std::floor(hpx)), by = int(std::floor(hpy));
        const float fx = hpx - float(bx), fy = hpy - float(by);
        float total = 0.0f, weight = 0.0f;
        for (int j = 0; j < 4; ++j) {
          const int ox = j & 1, oy = j >> 1;
          const int qx = std::clamp(bx + ox, 0, int(hw) - 1), qy = std::clamp(by + oy, 0, int(hh) - 1);
          const float bilinear = (ox ? fx : 1.0f - fx) * (oy ? fy : 1.0f - fy);
          const float wgt = (bilinear + 0.001f) *
                            std::exp(-std::fabs(lin[size_t(qy) * hw + qx] - zc) / (zc * 0.02f + 0.0001f));
          total += ao_a[size_t(qy) * hw + qx] * wgt;
          weight += wgt;
        }
        ao = weight > 0.0f ? total / weight : 1.0f;
        ao = 1.0f + (ao - 1.0f) * Saturate(1.0f - zc / fade);
        fog = fog_amount(z);
      }
      ao *= ao;
      const float ax = Half(ao), ay = Half(fog * (1.0f - ao));
      out.ao[size_t(y) * w + x] = ax;
      out.fog[size_t(y) * w + x] = ay;
      const uint8_t* c = &in.scene.rgba[(size_t(y) * w + x) * 4];
      uint8_t* o = &out.color[(size_t(y) * w + x) * 4];
      uint8_t* g = &out.grey[(size_t(y) * w + x) * 4];
      for (int k = 0; k < 3; ++k) {
        const float v = Saturate(float(c[k]) / 255.0f * ax + Saturate(in.fog.color[k]) * ay);
        o[k] = uint8_t(std::lround(v * 255.0f));
        g[k] = uint8_t(std::lround(Saturate(ax + ay) * 255.0f));
      }
      o[3] = g[3] = c[3];
    }
  return out;
}

// ------------------------------------------------------------------ scenes

float InvToZ(float inv_linear) { return 1.0f - inv_linear; }  // z = 1 - 1 / L

// A wall at L = 50 with a box 6 near-plane units in front of it in the middle,
// a floor (a plane: 1 / L linear down the screen) in the bottom quarter, sky on
// the right, a strip far past the distance fade on the left. Defined on the
// unit square, so the same scene exists at any size.
Scene MakeScene(uint32_t w, uint32_t h) {
  Scene s;
  s.w = w;
  s.h = h;
  s.z.resize(size_t(w) * h);
  s.rgba.resize(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const float u = (float(x) + 0.5f) / float(w), v = (float(y) + 0.5f) / float(h);
      float z;
      if (u > 0.88f) {
        z = 1.0f;  // sky
      } else if (u < 0.09f) {
        z = InvToZ(1.0f / 3000.0f);  // beyond the fade
      } else if (v > 0.72f) {
        const float t = (v - 0.72f) / 0.28f;  // the floor
        z = InvToZ(1.0f / 60.0f + t * (1.0f / 18.0f - 1.0f / 60.0f));
      } else if (u > 0.375f && u < 0.625f && v > 0.33f && v < 0.66f) {
        z = InvToZ(1.0f / 44.0f);  // the box, 6 units in front of the wall
      } else {
        z = InvToZ(1.0f / 50.0f);  // the wall
      }
      s.z[size_t(y) * w + x] = z;
      uint8_t* c = &s.rgba[(size_t(y) * w + x) * 4];
      c[0] = uint8_t(60 + 190 * u);
      c[1] = uint8_t(80 + 150 * v);
      c[2] = uint8_t(200 - 100 * u);
      c[3] = 200;
    }
  return s;
}

Scene MakeFlat(uint32_t w, uint32_t h, float linear) {
  Scene s = MakeScene(w, h);
  std::fill(s.z.begin(), s.z.end(), InvToZ(1.0f / linear));
  return s;
}

// ------------------------------------------------------------------ rig

struct Rig {
  VulkanTestDevice* gpu = Device();
  std::unique_ptr<AmbientOcclusion> ao;
  nvrhi::TextureHandle depth, color;
  uint32_t color_h = 0;
  uint64_t frame = 100;
  int errors_before = 0;

  bool Init() {
    if (!gpu) {
      Fail(__FILE__, __LINE__, "no Vulkan device (install mesa-vulkan-drivers for lavapipe)");
      return false;
    }
    errors_before = gpu->errors();
    ao = std::make_unique<AmbientOcclusion>(gpu->device());
    if (!ao->Initialize()) {
      Fail(__FILE__, __LINE__, "AmbientOcclusion::Initialize failed");
      return false;
    }
    return true;
  }
  ~Rig() {
    if (gpu) {
      gpu->WaitIdle();
      CHECK_EQ(gpu->errors(), errors_before);  // the validation layers stayed silent
    }
  }

  // The game's colour target is RGBA8 and a render target; the depth stands in as R32F.
  void Upload(const Scene& s, uint32_t colour_height = 0) {
    color_h = std::max(colour_height, s.h);
    nvrhi::TextureDesc dd;
    dd.width = s.w;
    dd.height = s.h;
    dd.format = nvrhi::Format::R32_FLOAT;
    dd.initialState = nvrhi::ResourceStates::ShaderResource;
    dd.keepInitialState = true;
    dd.debugName = "test depth";
    depth = gpu->device()->createTexture(dd);
    nvrhi::TextureDesc cd;
    cd.width = s.w;
    cd.height = color_h;
    cd.format = nvrhi::Format::RGBA8_UNORM;
    cd.isRenderTarget = true;
    cd.initialState = nvrhi::ResourceStates::RenderTarget;
    cd.keepInitialState = true;
    cd.debugName = "test colour";
    color = gpu->device()->createTexture(cd);
    std::vector<uint8_t> pixels(size_t(s.w) * color_h * 4, 77);  // rows below the picture: 77
    std::memcpy(pixels.data(), s.rgba.data(), s.rgba.size());
    nvrhi::CommandListHandle cl = gpu->device()->createCommandList();
    cl->open();
    cl->writeTexture(depth, 0, 0, s.z.data(), size_t(s.w) * 4);
    cl->writeTexture(color, 0, 0, pixels.data(), size_t(s.w) * 4);
    cl->close();
    gpu->Execute(cl);
  }

  AoResolve Resolve(bool from_depth, uint32_t w, uint32_t h, uint32_t dest = 0x1000) {
    AoResolve r;
    r.frame = frame;
    r.from_depth = from_depth;
    r.full_screen = true;
    r.format_ok = true;
    r.dest_key = dest;
    r.source = from_depth ? depth.Get() : color.Get();
    r.width = w;
    r.height = h;
    return r;
  }

  // One game frame as the AO sees it: the fog draw (optional), the depth
  // resolve, then the scene colour copy. Returns the scratch colour read back
  // (empty when the AO substituted nothing).
  Pixels Frame(const Scene& s, const AoSettings& settings, const AoFog* fog = nullptr, bool* substituted = nullptr) {
    ++frame;
    nvrhi::CommandListHandle cl = gpu->device()->createCommandList();
    cl->open();
    if (fog) {
      float c[16] = {};
      std::memcpy(c, fog->color, sizeof(fog->color));
      c[4] = fog->near_over_distance;
      c[6] = fog->density;
      c[7] = fog->inverse_distance;
      c[8] = fog->normalizing[0];
      c[9] = fog->normalizing[1];
      c[10] = fog->normalizing[2];
      ao->NoteFog(c, frame);
    }
    CHECK(ao->OnResolve(cl, Resolve(true, s.w, s.h), settings) == nullptr);
    nvrhi::ITexture* scene = ao->OnResolve(cl, Resolve(false, s.w, s.h), settings);
    cl->close();
    gpu->Execute(cl);
    if (substituted) *substituted = scene != nullptr;
    return scene ? gpu->ReadBack(scene) : Pixels{};
  }
};

AoSettings Settings(int mode, int strength = 1) {
  AoSettings s;
  s.mode = mode;
  s.strength = strength;
  return s;
}

// Differences against the reference in levels (0..255) over the picture.
struct Diff {
  double mean = 0, max = 0;
  uint32_t over = 0;  // pixels off by more than kPixelLevels
};
constexpr double kPixelLevels = 8.0;
Diff Compare(const Pixels& got, const std::vector<uint8_t>& want, uint32_t w, uint32_t h) {
  Diff d;
  double total = 0;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x)
      for (int k = 0; k < 4; ++k) {
        const double g = std::lround(got.at(x, y)[k] * 255.0f), e = want[(size_t(y) * w + x) * 4 + k];
        const double diff = std::fabs(g - e);
        total += diff;
        d.max = std::max(d.max, diff);
        if (diff > kPixelLevels) ++d.over;
      }
  d.mean = total / (double(w) * h * 4);
  return d;
}
// The GPU's transcendental functions and the sample positions' rounding differ from the CPU's a
// little: stated tolerances for the comparison with the reference (levels of 255).
constexpr double kMeanLevels = 0.8;
constexpr double kMaxLevels = 40.0;
constexpr double kOverShare = 0.01;  // at most 1% of the values off by more than kPixelLevels

void CheckAgainstReference(const Pixels& got, const std::vector<uint8_t>& want, uint32_t w, uint32_t h,
                           const char* what) {
  CHECK_EQ(got.width, w);
  if (got.width != w) return;
  const Diff d = Compare(got, want, w, h);
  std::printf("  %s: mean %.3f max %.1f over %u of %u\n", what, d.mean, d.max, d.over, w * h * 4);
  if (d.mean > kMeanLevels || d.max > kMaxLevels || d.over > uint32_t(kOverShare * w * h * 4)) {
    std::ostringstream s;
    s << what << ": mean " << d.mean << " (limit " << kMeanLevels << "), max " << d.max << " (limit " << kMaxLevels
      << "), values off by more than " << kPixelLevels << ": " << d.over << " (limit " << kOverShare * w * h * 4 << ")";
    Fail(__FILE__, __LINE__, s.str());
  }
}

constexpr uint32_t kW = 256, kH = 144;

float GreyAt(const Pixels& p, uint32_t x, uint32_t y) { return p.at(x, y)[0]; }

}  // namespace

// ------------------------------------------------------------------ tests

TEST(Ao_ShadersAreHere) {
  Rig rig;
  if (!rig.Init()) return;
  CHECK(rig.ao->ready());
}

TEST(Ao_FlatWallIsNotDarkened) {
  Rig rig;
  if (!rig.Init()) return;
  const Scene s = MakeFlat(kW, kH, 50.0f);
  rig.Upload(s);
  bool substituted = false;
  // AO only: every pixel is 1.
  Pixels grey = rig.Frame(s, Settings(2), nullptr, &substituted);
  CHECK(substituted);
  MaybeSave(grey, "flat_only");
  float lowest = 1.0f;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) lowest = std::min(lowest, GreyAt(grey, x, y));
  CHECK(lowest >= 1.0f - 1.5f / 255.0f);
  // On: the colour comes back unchanged (and its alpha).
  Pixels on = rig.Frame(s, Settings(1));
  for (uint32_t y = 0; y < kH; y += 7)
    for (uint32_t x = 0; x < kW; x += 5)
      for (int k = 0; k < 4; ++k)
        CHECK_NEAR(on.at(x, y)[k] * 255.0, double(s.rgba[(size_t(y) * kW + x) * 4 + k]), 1.01);
}

TEST(Ao_BoxOnWallAgainstReference) {
  Rig rig;
  if (!rig.Init()) return;
  const Scene s = MakeScene(kW, kH);
  rig.Upload(s);
  RefIn in;
  in.scene = s;
  in.settings = Settings(1);
  const RefOut ref = RefAo(in);

  Pixels grey = rig.Frame(s, Settings(2));
  MaybeSave(grey, "box_only");
  CheckAgainstReference(grey, ref.grey, kW, kH, "AO only, strength 1");

  Pixels applied = rig.Frame(s, Settings(1));
  MaybeSave(applied, "box_applied");
  CheckAgainstReference(applied, ref.color, kW, kH, "applied, strength 1");

  // Facts that hold without the reference.
  auto at = [&](double u, double v) { return GreyAt(grey, uint32_t(u * kW), uint32_t(v * kH)); };
  CHECK(at(0.30, 0.50) < 0.97);   // the wall beside the box, in its crease
  CHECK(at(0.20, 0.10) > 0.995);  // the wall far from the box
  CHECK(at(0.50, 0.50) > 0.95);   // the top of the box
  CHECK(at(0.95, 0.50) > 0.995);  // the sky
  CHECK(at(0.04, 0.50) > 0.995);  // past the distance fade
  CHECK(at(0.60, 0.88) > 0.97);   // the floor, a plane
}

TEST(Ao_StrengthSetsDarkenInOrder) {
  Rig rig;
  if (!rig.Init()) return;
  const Scene s = MakeScene(kW, kH);
  rig.Upload(s);
  double mean[3];
  for (int strength = 1; strength <= 3; ++strength) {
    Pixels grey = rig.Frame(s, Settings(2, strength));
    double total = 0;
    for (uint32_t y = 0; y < kH; ++y)
      for (uint32_t x = 0; x < kW; ++x) total += GreyAt(grey, x, y);
    mean[strength - 1] = total / (double(kW) * kH);
    RefIn in;
    in.scene = s;
    in.settings = Settings(1, strength);
    CheckAgainstReference(grey, RefAo(in).grey, kW, kH, strength == 1 ? "strength 1" : strength == 2 ? "strength 2" : "strength 3");
  }
  CHECK(mean[0] > mean[1]);
  CHECK(mean[1] > mean[2]);
}

TEST(Ao_FogKeepsTheFogColour) {
  Rig rig;
  if (!rig.Init()) return;
  const Scene s = MakeScene(kW, kH);
  rig.Upload(s);
  // A fog pass that covers everything beyond L = 40 fully (inverse depth <= 0 at
  // the box and farther is 1, so the formula is the second branch for nearer pixels).
  AoFog fog;
  fog.valid = true;
  fog.color[0] = 0.9f;
  fog.color[1] = 0.8f;
  fog.color[2] = 0.7f;
  fog.normalizing[0] = 1.0f;  // inverse depth = 1 * min(z, 0.99) - 0
  fog.normalizing[1] = 0.0f;
  fog.normalizing[2] = 0.99f;
  fog.near_over_distance = 0.0f;
  fog.density = 0.6f;
  fog.inverse_distance = 1.0f;
  RefIn in;
  in.scene = s;
  in.settings = Settings(1);
  in.fog = fog;
  const RefOut ref = RefAo(in);
  Pixels applied = rig.Frame(s, Settings(1), &fog);
  MaybeSave(applied, "fog_applied");
  CheckAgainstReference(applied, ref.color, kW, kH, "applied under fog");
  // Fogged pixels are lighter than without fog where the AO darkens.
  RefIn plain = in;
  plain.fog = AoFog{};
  const RefOut no_fog = RefAo(plain);
  const uint32_t x = uint32_t(0.30 * kW), y = uint32_t(0.50 * kH);
  CHECK(applied.at(x, y)[0] >= float(no_fog.color[(size_t(y) * kW + x) * 4]) / 255.0f);
  // A fog from an earlier frame does not apply.
  Pixels stale = rig.Frame(s, Settings(1));
  RefIn none = in;
  none.fog = AoFog{};
  CheckAgainstReference(stale, RefAo(none).color, kW, kH, "no fog this frame");
}

TEST(Ao_ColourTargetTallerThanThePicture) {
  Rig rig;
  if (!rig.Init()) return;
  const Scene s = MakeScene(kW, kH);
  rig.Upload(s, kH + 16);  // the game's targets are as tall as the tallest surface seen
  bool substituted = false;
  Pixels out = rig.Frame(s, Settings(1), nullptr, &substituted);
  CHECK(substituted);
  CHECK_EQ(out.height, kH + 16);
  if (out.height != kH + 16) return;
  RefIn in;
  in.scene = s;
  in.settings = Settings(1);
  const RefOut ref = RefAo(in);
  Pixels picture;
  picture.width = kW;
  picture.height = kH;
  picture.rgba.assign(out.rgba.begin(), out.rgba.begin() + size_t(kW) * kH * 4);
  CheckAgainstReference(picture, ref.color, kW, kH, "picture part");
  // Below the picture the colour passes through.
  for (uint32_t yy = kH; yy < kH + 16; yy += 3)
    for (uint32_t xx = 0; xx < kW; xx += 11) CHECK_NEAR(out.at(xx, yy)[0] * 255.0, 77.0, 1.01);
}

TEST(Ao_TheGamesTargetIsNotTouched) {
  Rig rig;
  if (!rig.Init()) return;
  const Scene s = MakeScene(kW, kH);
  rig.Upload(s);
  Pixels first = rig.Frame(s, Settings(1));
  Pixels source = rig.gpu->ReadBack(rig.color);
  for (uint32_t y = 0; y < kH; y += 9)
    for (uint32_t x = 0; x < kW; x += 7)
      for (int k = 0; k < 4; ++k)
        CHECK_NEAR(source.at(x, y)[k] * 255.0, double(s.rgba[(size_t(y) * kW + x) * 4 + k]), 0.51);
  // A second scene copy in the frame starts from the same scene: the same result, not AO twice.
  rig.frame += 1;
  nvrhi::CommandListHandle cl = rig.gpu->device()->createCommandList();
  cl->open();
  CHECK(rig.ao->OnResolve(cl, rig.Resolve(true, kW, kH), Settings(1)) == nullptr);
  nvrhi::ITexture* one = rig.ao->OnResolve(cl, rig.Resolve(false, kW, kH), Settings(1));
  CHECK(one != nullptr);
  nvrhi::ITexture* two = rig.ao->OnResolve(cl, rig.Resolve(false, kW, kH), Settings(1));
  CHECK(two != nullptr);
  cl->close();
  rig.gpu->Execute(cl);
  if (!two) return;
  Pixels again = rig.gpu->ReadBack(two);
  CheckAgainstReference(again, [&] {
    RefIn in;
    in.scene = s;
    in.settings = Settings(1);
    return RefAo(in).color;
  }(), kW, kH, "second copy of the frame");
  CHECK_EQ(rig.ao->stats().computed_frames, uint64_t(2));  // worked out once per frame
  CHECK_EQ(rig.ao->stats().applied, uint64_t(3));
}

TEST(Ao_DetectionFollowsTheXenosPluginsRules) {
  Rig rig;
  if (!rig.Init()) return;
  const Scene s = MakeScene(kW, kH);
  rig.Upload(s);
  nvrhi::CommandListHandle cl = rig.gpu->device()->createCommandList();
  cl->open();
  AmbientOcclusion& ao = *rig.ao;
  const AoSettings on = Settings(1);
  ++rig.frame;
  // A colour copy before any depth resolve: nothing.
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW, kH), on) == nullptr);
  // A depth resolve that is not full screen, or not D24S8: not noticed.
  AoResolve partial = rig.Resolve(true, kW, kH);
  partial.full_screen = false;
  CHECK(ao.OnResolve(cl, partial, on) == nullptr);
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW, kH), on) == nullptr);
  AoResolve wrong_format = rig.Resolve(true, kW, kH);
  wrong_format.format_ok = false;
  CHECK(ao.OnResolve(cl, wrong_format, on) == nullptr);
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW, kH), on) == nullptr);
  CHECK_EQ(ao.stats().depth_passes, uint64_t(0));
  // The depth resolve.
  CHECK(ao.OnResolve(cl, rig.Resolve(true, kW, kH), on) == nullptr);
  CHECK_EQ(ao.stats().depth_passes, uint64_t(1));
  // A colour copy that is not full screen or not 8:8:8:8: no.
  AoResolve small = rig.Resolve(false, kW, kH);
  small.full_screen = false;
  CHECK(ao.OnResolve(cl, small, on) == nullptr);
  AoResolve other_format = rig.Resolve(false, kW, kH);
  other_format.format_ok = false;
  CHECK(ao.OnResolve(cl, other_format, on) == nullptr);
  // The first scene copy: yes, and its destination becomes the frame's scene destination.
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW, kH, 0x1000), on) != nullptr);
  // Another destination (the final image's, post-processed): no. The same one again: yes.
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW, kH, 0x2000), on) == nullptr);
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW, kH, 0x1000), on) != nullptr);
  // The next frame without a depth resolve: no.
  ++rig.frame;
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW, kH, 0x1000), on) == nullptr);
  // A new depth resolve starts over: the destination may differ.
  CHECK(ao.OnResolve(cl, rig.Resolve(true, kW, kH), on) == nullptr);
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW, kH, 0x3000), on) != nullptr);
  // Off: nothing, and a copy after switching on mid-frame has no depth.
  ++rig.frame;
  CHECK(ao.OnResolve(cl, rig.Resolve(true, kW, kH), Settings(0)) == nullptr);
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW, kH), on) == nullptr);
  // A different picture size than the depth's: no.
  ++rig.frame;
  CHECK(ao.OnResolve(cl, rig.Resolve(true, kW, kH), on) == nullptr);
  CHECK(ao.OnResolve(cl, rig.Resolve(false, kW - 2, kH), on) == nullptr);
  cl->close();
  rig.gpu->Execute(cl);
  CHECK_EQ(ao.stats().applied, uint64_t(3));
}

TEST(Ao_OffDoesNothingAtAll) {
  Rig rig;
  if (!rig.Init()) return;
  const Scene s = MakeScene(kW, kH);
  rig.Upload(s);
  bool substituted = true;
  Pixels none = rig.Frame(s, Settings(0), nullptr, &substituted);
  CHECK(!substituted);
  CHECK_EQ(rig.ao->stats().depth_passes, uint64_t(0));
  CHECK_EQ(rig.ao->stats().computed_frames, uint64_t(0));
}

TEST(Ao_RealDepthTargetFormat) {
  Rig rig;
  if (!rig.Init()) return;
  // The game renderer's depth targets are D32S8 render targets: a flat one, cleared.
  const Scene s = MakeFlat(kW, kH, 30.0f);
  rig.Upload(s);
  nvrhi::TextureDesc dd;
  dd.width = kW;
  dd.height = kH;
  dd.format = nvrhi::Format::D32S8;
  dd.isRenderTarget = true;
  dd.initialState = nvrhi::ResourceStates::DepthWrite;
  dd.keepInitialState = true;
  dd.useClearValue = true;
  dd.clearValue = nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f);
  dd.debugName = "test D32S8";
  rig.depth = rig.gpu->device()->createTexture(dd);
  nvrhi::CommandListHandle cl = rig.gpu->device()->createCommandList();
  cl->open();
  cl->clearDepthStencilTexture(rig.depth, nvrhi::AllSubresources, true, 1.0f - 1.0f / 30.0f, true, 0);
  cl->close();
  rig.gpu->Execute(cl);
  bool substituted = false;
  Pixels grey = rig.Frame(s, Settings(2), nullptr, &substituted);
  CHECK(substituted);
  if (!substituted) return;
  float lowest = 1.0f;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) lowest = std::min(lowest, GreyAt(grey, x, y));
  CHECK(lowest >= 1.0f - 1.5f / 255.0f);
  // Sky: depth 1 is no surface at all.
  cl = rig.gpu->device()->createCommandList();
  cl->open();
  cl->clearDepthStencilTexture(rig.depth, nvrhi::AllSubresources, true, 1.0f, true, 0);
  cl->close();
  rig.gpu->Execute(cl);
  Pixels sky = rig.Frame(s, Settings(2));
  for (uint32_t y = 0; y < kH; y += 11)
    for (uint32_t x = 0; x < kW; x += 13) CHECK(GreyAt(sky, x, y) >= 1.0f - 1.5f / 255.0f);
}

TEST(Ao_DepthIsCopiedWhenResolved) {
  // The resolve may clear the depth right after: the copy made at the depth resolve is what counts.
  Rig rig;
  if (!rig.Init()) return;
  const Scene s = MakeScene(kW, kH);
  rig.Upload(s);
  ++rig.frame;
  nvrhi::CommandListHandle cl = rig.gpu->device()->createCommandList();
  cl->open();
  CHECK(rig.ao->OnResolve(cl, rig.Resolve(true, kW, kH), Settings(2)) == nullptr);
  const Scene flat = MakeFlat(kW, kH, 90.0f);  // the "cleared" depth
  cl->writeTexture(rig.depth, 0, 0, flat.z.data(), size_t(kW) * 4);
  nvrhi::ITexture* scene = rig.ao->OnResolve(cl, rig.Resolve(false, kW, kH), Settings(2));
  cl->close();
  rig.gpu->Execute(cl);
  CHECK(scene != nullptr);
  if (!scene) return;
  RefIn in;
  in.scene = s;
  in.settings = Settings(1);
  CheckAgainstReference(rig.gpu->ReadBack(scene), RefAo(in).grey, kW, kH, "from the copy, not the cleared depth");
}

TEST(Ao_RenderScale) {
  // The same scene at twice the size: the occlusion, scaled back down, matches the 1x one. The
  // working images follow the size (half of the scaled picture), the blur keeps its footprint
  // in the picture, the sample radius is in view space.
  Rig rig;
  if (!rig.Init()) return;
  const Scene s1 = MakeScene(kW, kH), s2 = MakeScene(kW * 2, kH * 2);
  rig.Upload(s1);
  Pixels a = rig.Frame(s1, Settings(2));
  rig.Upload(s2);
  Pixels b = rig.Frame(s2, Settings(2));
  CHECK_EQ(b.width, kW * 2);
  if (b.width != kW * 2) return;
  MaybeSave(b, "scale2_only");
  double total = 0, worst = 0;
  uint32_t over = 0;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) {
      const double down = (b.at(2 * x, 2 * y)[0] + b.at(2 * x + 1, 2 * y)[0] + b.at(2 * x, 2 * y + 1)[0] +
                           b.at(2 * x + 1, 2 * y + 1)[0]) / 4.0;
      const double d = std::fabs(down - a.at(x, y)[0]);
      total += d;
      worst = std::max(worst, d);
      if (d > 0.1) ++over;
    }
  const double mean = total / (double(kW) * kH);
  std::printf("  scale 2 vs 1 (downsampled): mean %.4f worst %.3f, %u pixels off by more than 0.1\n", mean, worst, over);
  CHECK(mean < 0.02);
  CHECK(over < kW * kH / 50);
  // Odd sizes work (half sizes round down).
  const Scene odd = MakeScene(255, 143);
  rig.Upload(odd);
  Pixels c = rig.Frame(odd, Settings(2));
  CHECK_EQ(c.width, 255u);
  CHECK(c.width == 255 && GreyAt(c, 20, 20) > 0.99f);
}

NR_TEST_MAIN()
