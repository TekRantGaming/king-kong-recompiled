// The temporal upscalers' GPU-free parts (docs/upscalers.md): the settings, the jitter sequence and its
// clip-space offset, the motion vectors' matrix math, draw matching and camera vote, and where in a frame
// the upscaler runs.
#include <cmath>
#include <cstdio>
#include <random>
#include <set>

#include "backend/tests/check.h"
#include "backend/upscale_config.h"
#include "backend/upscale_frame.h"
#include "backend/upscale_jitter.h"
#include "backend/upscale_motion.h"

using namespace nr;

namespace {

Mat4 Perspective(double fov_y, double aspect, double zn, double zf) {
  // D3D's left-handed perspective, as rows for clip = M * (x, y, z, 1): z in [0, 1].
  const double ys = 1.0 / std::tan(fov_y / 2), xs = ys / aspect;
  Mat4 m{};
  m[0] = xs;
  m[5] = ys;
  m[10] = zf / (zf - zn);
  m[11] = -zn * zf / (zf - zn);
  m[14] = 1.0;
  return m;
}

Mat4 Translation(double x, double y, double z) {
  Mat4 m = Mat4Identity();
  m[3] = x;
  m[7] = y;
  m[11] = z;
  return m;
}

Mat4 RotationY(double a) {
  Mat4 m = Mat4Identity();
  m[0] = std::cos(a);
  m[2] = std::sin(a);
  m[8] = -std::sin(a);
  m[10] = std::cos(a);
  return m;
}

void ToRows(const Mat4& m, float rows[16]) {
  for (int i = 0; i < 16; ++i) rows[i] = float(m[i]);
}

// The guest viewport of a 1280x720 frame (D3D9: y down, half-pixel offset) as MotionScreen sees it.
MotionScreen Screen1280(float host_scale) {
  MotionScreen s;
  s.x_scale = 640;
  s.x_offset = 640;  // the half-pixel offset is in the viewport transform's add; here centres are .5
  s.y_scale = -360;
  s.y_offset = 360;
  s.z_scale = 1;
  s.z_offset = 0;
  s.host_scale_x = s.host_scale_y = host_scale;
  return s;
}

}  // namespace

// ---------------------------------------------------------------- settings

TEST(Config_ParsesKindsQualitiesPresets) {
  UpscalerKind k;
  CHECK(ParseUpscalerKind("off", k) && k == UpscalerKind::kOff);
  CHECK(ParseUpscalerKind("", k) && k == UpscalerKind::kOff);
  CHECK(ParseUpscalerKind("AUTO", k) && k == UpscalerKind::kAuto);
  CHECK(ParseUpscalerKind("fsr3", k) && k == UpscalerKind::kFsr3);
  CHECK(ParseUpscalerKind("fsr4", k) && k == UpscalerKind::kFsr4);
  CHECK(ParseUpscalerKind("dlss", k) && k == UpscalerKind::kDlss);
  CHECK(ParseUpscalerKind("xess", k) && k == UpscalerKind::kXess);
  CHECK(ParseUpscalerKind("bilinear", k) && k == UpscalerKind::kBilinear);
  CHECK(!ParseUpscalerKind("taa", k));
  UpscaleQuality q;
  CHECK(ParseUpscaleQuality("native", q) && q == UpscaleQuality::kNative);
  CHECK(ParseUpscaleQuality("Quality", q) && q == UpscaleQuality::kQuality);
  CHECK(ParseUpscaleQuality("balanced", q) && q == UpscaleQuality::kBalanced);
  CHECK(ParseUpscaleQuality("performance", q) && q == UpscaleQuality::kPerformance);
  CHECK(ParseUpscaleQuality("ultra-performance", q) && q == UpscaleQuality::kUltraPerformance);
  CHECK(ParseUpscaleQuality("ultra_performance", q) && q == UpscaleQuality::kUltraPerformance);
  CHECK(!ParseUpscaleQuality("extreme", q));
  CHECK_NEAR(DefaultUpscaleRatio(UpscaleQuality::kNative), 1.0, 0);
  CHECK_NEAR(DefaultUpscaleRatio(UpscaleQuality::kQuality), 1.5, 1e-6);
  CHECK_NEAR(DefaultUpscaleRatio(UpscaleQuality::kBalanced), 1.7, 1e-6);
  CHECK_NEAR(DefaultUpscaleRatio(UpscaleQuality::kPerformance), 2.0, 0);
  CHECK_NEAR(DefaultUpscaleRatio(UpscaleQuality::kUltraPerformance), 3.0, 0);
  char p = 'x';
  CHECK(ParseDlssPreset("default", p) && p == 0);
  CHECK(ParseDlssPreset("k", p) && p == 'K');
  CHECK(ParseDlssPreset("M", p) && p == 'M');
  CHECK(!ParseDlssPreset("KK", p));
  // Input sizes as the SDKs round them.
  CHECK_EQ(UpscaleInputSize(2560, 1.5f), 1707u);
  CHECK_EQ(UpscaleInputSize(1440, 1.5f), 960u);
  CHECK_EQ(UpscaleInputSize(2560, 1.7f), 1506u);
  CHECK_EQ(UpscaleInputSize(3840, 2.0f), 1920u);
  CHECK_EQ(UpscaleInputSize(2160, 3.0f), 720u);
  CHECK_EQ(UpscaleInputSize(1280, 1.0f), 1280u);
  CHECK_EQ(UpscaleInputSize(1, 3.0f), 1u);
}

// ---------------------------------------------------------------- jitter

TEST(Jitter_HaltonValues) {
  CHECK_NEAR(Halton(1, 2), 0.5, 1e-7);
  CHECK_NEAR(Halton(2, 2), 0.25, 1e-7);
  CHECK_NEAR(Halton(3, 2), 0.75, 1e-7);
  CHECK_NEAR(Halton(4, 2), 0.125, 1e-7);
  CHECK_NEAR(Halton(1, 3), 1.0 / 3, 1e-7);
  CHECK_NEAR(Halton(2, 3), 2.0 / 3, 1e-7);
  CHECK_NEAR(Halton(3, 3), 1.0 / 9, 1e-7);
  CHECK_NEAR(Halton(0, 2), 0.0, 0);
}

TEST(Jitter_PhaseCountsFollowTheSdksGuidance) {
  // 8 * ratio^2 (FSR's ffxFsr3GetJitterPhaseCount, DLSS and XeSS guidance).
  CHECK_EQ(JitterPhaseCount(1.0f, 1.0f), 8u);
  CHECK_EQ(JitterPhaseCount(1.5f, 1.5f), 18u);
  CHECK_EQ(JitterPhaseCount(1.7f, 1.7f), 24u);
  CHECK_EQ(JitterPhaseCount(2.0f, 2.0f), 32u);
  CHECK_EQ(JitterPhaseCount(3.0f, 3.0f), 72u);
  CHECK_EQ(JitterPhaseCount(0.5f, 0.5f), 8u);  // never fewer than 8
  // An input size rounded to whole pixels (2560 / 1707 = 1.49971): still 18.
  CHECK_EQ(JitterPhaseCount(2560.0f / 1707.0f, 1440.0f / 960.0f), 18u);
}

TEST(Jitter_SequenceStaysInThePixelAndCoversIt) {
  for (uint32_t phases : {8u, 18u, 32u, 72u}) {
    std::set<std::pair<int, int>> seen;
    double sx = 0, sy = 0;
    // A grid as fine as the output's pixels inside one input pixel (ratio = sqrt(phases / 8)).
    const int cells = int(std::lround(std::sqrt(phases / 8.0)));
    std::set<std::pair<int, int>> cells_hit;
    for (uint64_t f = 0; f < phases; ++f) {
      const Jitter j = JitterForFrame(f, phases);
      CHECK(j.x >= -0.5f && j.x < 0.5f);
      CHECK(j.y >= -0.5f && j.y < 0.5f);
      sx += j.x;
      sy += j.y;
      seen.insert({int(std::lround(j.x * 1e5)), int(std::lround(j.y * 1e5))});
      cells_hit.insert({std::min(int((j.x + 0.5f) * cells), cells - 1), std::min(int((j.y + 0.5f) * cells), cells - 1)});
    }
    CHECK_EQ(seen.size(), size_t(phases));  // no phase repeats another
    CHECK_NEAR(sx / phases, 0.0, 0.07);      // centred
    CHECK_NEAR(sy / phases, 0.0, 0.07);
    CHECK_EQ(cells_hit.size(), size_t(cells * cells));  // every output pixel of the input pixel is sampled
  }
  // The sequence repeats with its period.
  const Jitter a = JitterForFrame(5, 18), b = JitterForFrame(5 + 18 * 7, 18);
  CHECK_EQ(a.x, b.x);
  CHECK_EQ(a.y, b.y);
}

TEST(Jitter_NdcOffsetMovesThePictureRightAndDown) {
  float ox, oy;
  // +0.5 pixel right and down on a 1000x500 target: 2 * 0.5 / 1000 in x; D3D's NDC y points up.
  JitterNdcOffset({0.5f, 0.5f}, 1000, 500, false, ox, oy);
  CHECK_NEAR(ox, 0.001, 1e-7);
  CHECK_NEAR(oy, -0.002, 1e-7);
  // Vulkan: the translated shaders negate y after the offset.
  JitterNdcOffset({0.5f, 0.5f}, 1000, 500, true, ox, oy);
  CHECK_NEAR(ox, 0.001, 1e-7);
  CHECK_NEAR(oy, 0.002, 1e-7);
  JitterNdcOffset({0.0f, 0.0f}, 1000, 500, true, ox, oy);
  CHECK_EQ(ox, 0.0f);
  CHECK_EQ(oy, 0.0f);
  // What the offset does to a vertex in D3D's convention: host pixel x = (ndc + 1) / 2 * width,
  // pixel y = (1 - ndc) / 2 * height.
  const Jitter j{-0.25f, 0.375f};
  JitterNdcOffset(j, 1707, 960, false, ox, oy);
  const double ndc_x = 0.1, ndc_y = -0.3;
  const double px0 = (ndc_x + 1) / 2 * 1707, py0 = (1 - ndc_y) / 2 * 960;
  const double px1 = (ndc_x + ox + 1) / 2 * 1707, py1 = (1 - (ndc_y + oy)) / 2 * 960;
  CHECK_NEAR(px1 - px0, j.x, 1e-4);
  CHECK_NEAR(py1 - py0, j.y, 1e-4);
}

TEST(Jitter_MipBias) {
  CHECK_NEAR(UpscaleMipBias(1.0f), 0.0, 1e-7);
  CHECK_NEAR(UpscaleMipBias(1.5f), -0.5849625, 1e-6);
  CHECK_NEAR(UpscaleMipBias(2.0f), -1.0, 1e-7);
  CHECK_NEAR(UpscaleMipBias(3.0f), -1.5849625, 1e-6);
}

// ---------------------------------------------------------------- matrices

TEST(Motion_MatrixInverse) {
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> u(-10, 10);
  for (int n = 0; n < 50; ++n) {
    Mat4 m;
    for (double& v : m) v = u(rng);
    Mat4 inv;
    if (!Mat4Invert(m, inv)) continue;  // a singular draw is fine, rare
    const Mat4 p = Mat4Multiply(m, inv);
    for (int i = 0; i < 16; ++i) CHECK_NEAR(p[i], (i % 5 == 0) ? 1.0 : 0.0, 1e-9);
  }
  // A projection with a tiny near plane and a far one 200,000 away (the game's camera): invertible.
  Mat4 inv;
  const Mat4 p = Perspective(1.0, 16.0 / 9, 0.05, 200000);
  CHECK(Mat4Invert(p, inv));
  const Mat4 i = Mat4Multiply(p, inv);
  for (int k = 0; k < 16; ++k) CHECK_NEAR(i[k], (k % 5 == 0) ? 1.0 : 0.0, 1e-9);
  Mat4 zero{};
  CHECK(!Mat4Invert(zero, inv));
  Mat4 singular = Mat4Identity();
  singular[15] = 0;
  CHECK(!Mat4Invert(singular, inv));
}

// ---------------------------------------------------------------- draw matching

TEST(Motion_HistoryMatchesDrawsAcrossFrames) {
  MotionHistory h;
  MotionDrawKey a;
  a.vs_hash = 0x1111;
  a.vertex_buffer = 0x1000;
  a.index_buffer = 0x2000;
  a.count = 300;
  MotionDrawKey b = a;
  b.vertex_buffer = 0x3000;  // another mesh
  auto constants = [](float v) {
    VsConstants c{};
    c.fill(v);
    return c;
  };
  h.BeginFrame(10);
  // Three instances of a (fur shells: the same mesh, a shell number in the constants), one of b.
  for (float shell : {1.0f, 2.0f, 3.0f}) h.Record(a, constants(shell));
  h.Record(b, constants(50.0f));
  CHECK_EQ(h.CountThisFrame(a), 3u);
  h.BeginFrame(11);
  CHECK_EQ(h.previous_draws(), size_t(4));
  // This frame: the same three instances, slightly animated: matched by their order.
  for (uint32_t i = 0; i < 3; ++i) {
    const VsConstants cur = constants(float(i + 1) + 0.4f);
    const uint32_t ord = h.Record(a, cur);
    CHECK_EQ(ord, i);
  }
  for (uint32_t i = 0; i < 3; ++i) {
    const VsConstants* p = h.FindPrevious(a, i, constants(float(i + 1) + 0.4f), h.CountThisFrame(a));
    CHECK(p != nullptr);
    if (p) CHECK_EQ((*p)[0], float(i + 1));
  }
  // A key last frame did not have: nothing (camera motion only).
  MotionDrawKey c = a;
  c.start = 99;
  CHECK(h.FindPrevious(c, 0, constants(1), 1) == nullptr);
  // b alone: its only draw.
  const VsConstants* pb = h.FindPrevious(b, 0, constants(0), 1);
  CHECK(pb && (*pb)[0] == 50.0f);
  // Two of a's three instances this frame (one culled): matched by the closest constants.
  h.BeginFrame(12);
  h.Record(a, constants(3.1f));
  h.Record(a, constants(1.1f));
  {
    MotionHistory& hh = h;
    hh.BeginFrame(13);  // frame 12 becomes the previous one: two draws of a
    for (float v : {1.0f, 2.0f, 3.0f}) hh.Record(a, constants(v));
    // Three this frame against two last frame: closest wins.
    const VsConstants* p0 = hh.FindPrevious(a, 0, constants(1.0f), 3);
    const VsConstants* p2 = hh.FindPrevious(a, 2, constants(3.0f), 3);
    CHECK(p0 && std::fabs((*p0)[0] - 1.1f) < 1e-6f);
    CHECK(p2 && std::fabs((*p2)[0] - 3.1f) < 1e-6f);
  }
  // A gap of a frame: no previous draws at all (their motion would span two frames).
  h.BeginFrame(15);
  CHECK_EQ(h.previous_draws(), size_t(0));
  CHECK(h.FindPrevious(a, 0, constants(1), 1) == nullptr);
}

// ---------------------------------------------------------------- camera motion

TEST(Motion_CameraVoteFindsTheCameraAmongMovingObjects) {
  const Mat4 proj = Perspective(1.1, 16.0 / 9, 0.05, 200000);
  // Last frame's view and this frame's: the camera turned and stepped.
  const Mat4 view_prev = Mat4Multiply(RotationY(0.02), Translation(-3, -1.7, 4));
  const Mat4 view_cur = Mat4Multiply(RotationY(0.035), Translation(-3.2, -1.7, 4.3));
  CameraReprojectionVote vote;
  vote.BeginFrame(1);
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> u(-50, 50);
  // Twelve static objects, five that moved between the frames.
  for (int i = 0; i < 17; ++i) {
    const Mat4 world = Mat4Multiply(Translation(u(rng), u(rng) * 0.1, u(rng) + 80), RotationY(u(rng)));
    const Mat4 world_prev = i < 12 ? world : Mat4Multiply(Translation(0.3 * (i - 11), 0, 0.1), world);
    float cur[16], prev[16];
    ToRows(Mat4Multiply(proj, Mat4Multiply(view_cur, world)), cur);
    ToRows(Mat4Multiply(proj, Mat4Multiply(view_prev, world_prev)), prev);
    vote.Offer(cur, prev);
  }
  Mat4 r;
  uint32_t votes = 0, offers = 0;
  CHECK(vote.Result(r, &votes, &offers));
  CHECK_EQ(offers, 17u);
  CHECK_EQ(votes, 12u);
  // The camera's own reprojection: P * Vprev * Vcur^-1 * P^-1, compared after normalising w.
  Mat4 pinv, vinv;
  CHECK(Mat4Invert(proj, pinv));
  CHECK(Mat4Invert(view_cur, vinv));
  Mat4 want = Mat4Multiply(proj, Mat4Multiply(view_prev, Mat4Multiply(vinv, pinv)));
  for (double& v : want) v /= want[15];
  // Both map points the same way: compare on a few clip points. The matrices come from float constants
  // (as the game's do) of objects up to 130 units from the origin with a 0.05 near plane: the camera
  // pass is good to a few hundredths of a pixel on a 1280-wide picture (NDC 1e-4 = 0.064 pixels).
  double worst = 0;
  for (double z : {0.2, 0.9, 0.999, 0.99999}) {
    for (double x : {-0.8, 0.0, 0.7}) {
      const double p[4] = {x, -x * 0.5, z, 1};
      double a[4], b[4];
      Mat4Transform(r, p, a);
      Mat4Transform(want, p, b);
      worst = std::max({worst, std::fabs(a[0] / a[3] - b[0] / b[3]), std::fabs(a[1] / a[3] - b[1] / b[3])});
    }
  }
  std::printf("  camera vote: worst NDC error %.2e (%.3f pixels at 1280)\n", worst, worst * 640);
  CHECK(worst < 2e-4);
  // A new frame starts empty.
  vote.BeginFrame(2);
  CHECK(!vote.Result(r));
}

TEST(Motion_CameraMotionMatchesTheProjectedPoints) {
  const Mat4 proj = Perspective(1.0, 16.0 / 9, 0.05, 200000);
  const Mat4 view_prev = Mat4Multiply(RotationY(-0.01), Translation(0, -2, 6));
  const Mat4 view_cur = Mat4Multiply(RotationY(0.012), Translation(0.4, -2.1, 5.5));
  const Mat4 vp_prev = Mat4Multiply(proj, view_prev), vp_cur = Mat4Multiply(proj, view_cur);
  Mat4 vp_cur_inv;
  CHECK(Mat4Invert(vp_cur, vp_cur_inv));
  const Mat4 reprojection = Mat4Multiply(vp_prev, vp_cur_inv);
  for (float scale : {1.0f, 1.3333f, 2.0f}) {
    const MotionScreen s = Screen1280(scale);
    static const double kPoints[3][4] = {{1.5, 0.2, 20, 1}, {-4, 3, 60, 1}, {0.1, -1.5, 2.5, 1}};
    for (const Jitter j : {Jitter{0, 0}, Jitter{0.31f, -0.27f}}) {
      for (const auto& pt : kPoints) {
        // Where the point is this frame and last frame, in host pixels (centres at .5), unjittered.
        double c[4], p[4];
        Mat4Transform(vp_cur, pt, c);
        Mat4Transform(vp_prev, pt, p);
        const double cx = (c[0] / c[3] * s.x_scale + s.x_offset) * scale;
        const double cy = (c[1] / c[3] * s.y_scale + s.y_offset) * scale;
        const double px = (p[0] / p[3] * s.x_scale + s.x_offset) * scale;
        const double py = (p[1] / p[3] * s.y_scale + s.y_offset) * scale;
        // The pixel it lands on with the jitter, and the depth stored there.
        const double hx = cx + j.x - 0.5, hy = cy + j.y - 0.5;
        const double depth = c[2] / c[3] * s.z_scale + s.z_offset;
        double m[2];
        CHECK(CameraMotion(s, reprojection, j.x, j.y, hx, hy, depth, m));
        CHECK_NEAR(m[0], px - cx, 2e-3 * scale);
        CHECK_NEAR(m[1], py - cy, 2e-3 * scale);
      }
    }
  }
  // A still camera: no motion anywhere.
  double m[2];
  const MotionScreen s = Screen1280(1.5f);
  CHECK(CameraMotion(s, Mat4Identity(), 0.2f, -0.1f, 300, 200, 0.97, m));
  CHECK_NEAR(m[0], 0, 1e-9);
  CHECK_NEAR(m[1], 0, 1e-9);
}

// ---------------------------------------------------------------- where it runs

namespace {

constexpr uint64_t kScene = 0x1000, kDepth = 0x8000000000001000ull, kShadow = 0x2000, kShadowDepth = 0x8000000000002000ull;
constexpr uint64_t kFogPs = 0x1B65AC7429B67FE9ull;

UpscaleFrameTracker::Draw DepthOnly(uint64_t frame) {
  UpscaleFrameTracker::Draw d;
  d.frame = frame;
  d.state.depth = d.state.depth_test = d.state.depth_write = true;
  d.depth_key = kDepth;
  d.frame_size = true;
  return d;
}
UpscaleFrameTracker::Draw Opaque(uint64_t frame, bool depth_write = false) {
  UpscaleFrameTracker::Draw d;
  d.frame = frame;
  d.state.color = true;
  d.state.depth = d.state.depth_test = true;
  d.state.depth_write = depth_write;
  d.state.ps_hash = 0x5B4F894B06E0F459ull;
  d.color_key = kScene;
  d.depth_key = kDepth;
  d.frame_size = true;
  return d;
}
UpscaleFrameTracker::Draw Fog(uint64_t frame) {
  UpscaleFrameTracker::Draw d = Opaque(frame);
  d.state.depth_test = false;
  d.state.ps_hash = kFogPs;
  d.state.blend_control = 0x07060706;
  return d;
}
UpscaleFrameTracker::Draw Particle(uint64_t frame) {
  UpscaleFrameTracker::Draw d = Opaque(frame);
  d.state.blend_control = 0x07060706;
  d.state.ps_hash = 0xA71CAAE3F888F1DBull;
  return d;
}
UpscaleFrameTracker::Draw Downsample(uint64_t frame) {
  UpscaleFrameTracker::Draw d = Opaque(frame);
  d.state.depth_test = false;
  d.state.ps_hash = 0x2B4F1D2C5EDCC366ull;  // the library's clear draw into a 640x360 corner
  d.covers_target = false;
  return d;
}
UpscaleFrameTracker::Draw PostShader(uint64_t frame) {
  UpscaleFrameTracker::Draw d = Opaque(frame);
  d.state.depth_test = false;
  d.state.ps_hash = 0x199FFBAEA54F3AA1ull;  // an AfterEffects pass
  return d;
}
UpscaleFrameTracker::Draw ShadowMap(uint64_t frame) {
  UpscaleFrameTracker::Draw d = Opaque(frame, true);
  d.color_key = kShadow;
  d.depth_key = kShadowDepth;
  d.frame_size = false;
  return d;
}

}  // namespace

TEST(Point_FirstFrameIs2DThenTheSceneGetsItsPoint) {
  UpscaleRules rules;
  UpscaleFrameTracker t;
  t.set_rules(&rules);
  // Frame 1: nothing before it had a scene: drawn at the output size, no upscaler.
  auto r = t.OnDraw(DepthOnly(1));
  CHECK(t.phase() == UpscaleFrameTracker::Phase::k2D);
  CHECK(!r.jitter);
  t.OnDraw(Opaque(1));
  CHECK(t.has_scene());
  t.OnDraw(Fog(1));        // frame 2 then waits for its fog pass to end the opaque pass
  CHECK(!t.OnPresent(1));  // a 2D frame is never upscaled
  // Frame 2: the scene phase.
  r = t.OnDraw(ShadowMap(2));
  CHECK(t.phase() == UpscaleFrameTracker::Phase::kScene);
  CHECK(!r.jitter);  // not the frame's size
  CHECK(!r.motion);
  r = t.OnDraw(DepthOnly(2));
  CHECK(r.jitter);
  CHECK(r.motion);  // writes the scene's depth
  r = t.OnDraw(Opaque(2));
  CHECK(r.jitter);
  CHECK(!r.motion);  // the lit pass does not write depth
  r = t.OnDraw(Opaque(2, true));
  CHECK(r.motion);
  // The post chain's corner draw before the opaque pass ended: not the point.
  r = t.OnDraw(Downsample(2));
  CHECK(!r.upscale_before);
  r = t.OnDraw(Fog(2));
  CHECK(r.opaque_complete);
  CHECK(!r.upscale_before);
  CHECK(!r.jitter);  // no depth test
  r = t.OnDraw(Particle(2));
  CHECK(r.jitter);
  CHECK(!r.upscale_before);
  r = t.OnDraw(Downsample(2));
  CHECK(r.upscale_before);
  CHECK(r.trigger == UpscaleFrameTracker::Trigger::kPartialTarget);
  CHECK(r.phase == UpscaleFrameTracker::Phase::kPost);
  CHECK(!r.jitter);
  t.MarkUpscaled();
  CHECK(t.phase() == UpscaleFrameTracker::Phase::kPost);
  // After the point: no second one, nothing jittered (the HUD drawn with a depth test).
  r = t.OnDraw(PostShader(2));
  CHECK(!r.upscale_before);
  r = t.OnDraw(Particle(2));
  CHECK(!r.jitter);
  CHECK(!r.motion);
  CHECK(!t.OnPresent(2));  // already upscaled
  CHECK_EQ(t.stats().by_trigger[uint32_t(UpscaleFrameTracker::Trigger::kPartialTarget)], 1ull);
}

TEST(Point_PostShaderResolveClearAndPresent) {
  UpscaleRules rules;
  UpscaleFrameTracker t;
  t.set_rules(&rules);
  t.OnDraw(DepthOnly(1));
  t.OnDraw(Opaque(1));
  t.OnPresent(1);
  // Frame 2: an AfterEffects pass after the fog is the point.
  t.OnDraw(DepthOnly(2));
  t.OnDraw(Opaque(2));
  t.OnDraw(Fog(2));
  auto r = t.OnDraw(PostShader(2));
  CHECK(r.upscale_before);
  CHECK(r.trigger == UpscaleFrameTracker::Trigger::kPostShader);
  t.MarkUpscaled();
  t.OnPresent(2);
  // Frame 3: the scene copy that clears the target is the point.
  t.OnDraw(DepthOnly(3));
  t.OnDraw(Opaque(3));
  t.OnDraw(Fog(3));
  UpscaleFrameTracker::Trigger trigger = UpscaleFrameTracker::Trigger::kNone;
  CHECK(!t.OnResolve({3, kScene, false}, &trigger));  // a plain copy (the water's refraction): not it
  CHECK(!t.OnResolve({3, kShadow, true}, &trigger));  // another target
  CHECK(t.OnResolve({3, kScene, true}, &trigger));
  CHECK(trigger == UpscaleFrameTracker::Trigger::kResolveClear);
  t.MarkUpscaled();
  t.OnPresent(3);
  // Frame 4: a partial clear of the scene target.
  t.OnDraw(DepthOnly(4));
  t.OnDraw(Opaque(4));
  t.OnDraw(Fog(4));
  CHECK(!t.OnClear({4, kScene, 0, true}, &trigger));
  CHECK(t.OnClear({4, kScene, 0, false}, &trigger));
  t.MarkUpscaled();
  t.OnPresent(4);
  // Frame 5: no post chain at all: Present upscales.
  t.OnDraw(DepthOnly(5));
  t.OnDraw(Opaque(5));
  t.OnDraw(Fog(5));
  t.OnDraw(Particle(5));
  CHECK(t.OnPresent(5));
  // Frame 6: a menu frame with no scene (it starts in the scene phase: the last frame had one);
  // nothing to upscale at Present, and frame 7 is 2D.
  UpscaleFrameTracker::Draw menu;
  menu.frame = 6;
  menu.state.color = true;
  menu.state.blend_control = 0x07060706;
  menu.color_key = kScene;
  menu.frame_size = true;
  t.OnDraw(menu);
  CHECK(t.phase() == UpscaleFrameTracker::Phase::kScene);
  CHECK(!t.OnPresent(6));
  menu.frame = 7;
  t.OnDraw(menu);
  CHECK(t.phase() == UpscaleFrameTracker::Phase::k2D);
  CHECK(!t.OnPresent(7));
  CHECK_EQ(t.stats().frames_2d, 2ull);  // frames 1 and 7
}

TEST(Point_AtPresentOnly) {
  UpscaleRules rules;
  UpscaleFrameTracker t(UpscalePoint::kPresent);
  t.set_rules(&rules);
  t.OnDraw(DepthOnly(1));
  t.OnDraw(Opaque(1));
  t.OnPresent(1);
  t.OnDraw(DepthOnly(2));
  t.OnDraw(Opaque(2));
  t.OnDraw(Fog(2));
  auto r = t.OnDraw(PostShader(2));
  CHECK(!r.upscale_before);
  CHECK(r.jitter == false);  // no depth test
  r = t.OnDraw(Particle(2));
  CHECK(r.jitter);  // still the scene phase: the post chain is in the input
  CHECK(t.OnPresent(2));
}

TEST(Point_RuleFile) {
  UpscaleRules rules;
  CHECK(rules.IsPost(0x199FFBAEA54F3AA1ull));
  CHECK(!rules.IsPost(kFogPs));  // the fog belongs to the scene
  std::vector<std::string> errors;
  rules.AddFile("# comment\nscene ps=199FFBAEA54F3AA1\npost ps=0123456789ABCDEF  # a new one\nbogus\npost ps=xyz\n", &errors);
  CHECK(!rules.IsPost(0x199FFBAEA54F3AA1ull));
  CHECK(rules.IsPost(0x0123456789ABCDEFull));
  CHECK_EQ(errors.size(), size_t(2));
}

NR_TEST_MAIN()
