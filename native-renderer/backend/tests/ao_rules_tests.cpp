// The ambient occlusion's draw classifier, frame tracker and camera finder, without a GPU
// (docs/ao.md): the built-in rules on the draw states a frame of the game has, rule files,
// the frame's phases, the projection read back from a world-view-projection matrix and the
// mapping from host pixels and stored depth back to view space.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "backend/ao_camera.h"
#include "backend/ao_classifier.h"
#include "backend/ao_config.h"
#include "backend/guest_memory.h"
#include "backend/lighting_config.h"
#include "backend/lighting_shaders.h"
#include "backend/tests/check.h"

using namespace nr;

namespace {

constexpr uint32_t kOpaqueBlend = 0x00010001;    // ONE, ZERO, ADD
constexpr uint32_t kAlphaBlend = 0x00010706;     // SRCALPHA, INVSRCALPHA (alpha ONE ZERO)
constexpr uint32_t kAdditiveBlend = 0x00010101;  // ONE, ONE
constexpr uint32_t kMultiplyBlend = 0x00010400;  // ZERO, SRCCOLOR

AoDrawState State(bool color, bool test, bool write, uint32_t blend, uint64_t ps = 0x1111) {
  AoDrawState s;
  s.color = color;
  s.depth = true;
  s.depth_test = test;
  s.depth_write = test && write;
  s.blend_control = blend;
  s.vs_hash = 0x2222;
  s.ps_hash = ps;
  return s;
}

}  // namespace

TEST(AoRules_BlendKinds) {
  CHECK(AoBlendKind(kOpaqueBlend) == AoBlend::kOff);
  CHECK(AoBlendKind(kAlphaBlend) == AoBlend::kAlpha);
  CHECK(AoBlendKind(0x00010701) == AoBlend::kAlpha);  // premultiplied: ONE, INVSRCALPHA
  CHECK(AoBlendKind(kAdditiveBlend) == AoBlend::kAdditive);
  CHECK(AoBlendKind(0x00010106) == AoBlend::kAdditive);  // SRCALPHA, ONE
  CHECK(AoBlendKind(kMultiplyBlend) == AoBlend::kMultiply);
  CHECK(AoBlendKind(0x00010008) == AoBlend::kMultiply);  // DESTCOLOR, ZERO
  CHECK(AoBlendKind(0x00010504) == AoBlend::kOther);
}

TEST(AoRules_BuiltInTableOnAFrame) {
  const AoRuleTable table;
  // The depth pre-pass, an occlusion survey, the sky without a depth test, the opaque scene with
  // depth writes off (after the pre-pass) and on, alpha-tested foliage, water, a particle, the fog.
  CHECK(table.Classify(State(false, true, true, kOpaqueBlend, 0)) == AoDrawClass::kDepthOnly);
  CHECK(table.Classify(State(false, true, false, kOpaqueBlend)) == AoDrawClass::kIgnore);
  CHECK(table.Classify(State(true, false, false, kOpaqueBlend)) == AoDrawClass::kEffect);
  CHECK(table.Classify(State(true, true, false, kOpaqueBlend)) == AoDrawClass::kOpaque);
  CHECK(table.Classify(State(true, true, true, kOpaqueBlend)) == AoDrawClass::kOpaque);
  AoDrawState foliage = State(true, true, true, kOpaqueBlend);
  foliage.alpha_test = true;
  CHECK(table.Classify(foliage) == AoDrawClass::kOpaque);
  CHECK(table.Classify(State(true, true, false, kAlphaBlend)) == AoDrawClass::kTransparent);
  CHECK(table.Classify(State(true, true, false, kAdditiveBlend)) == AoDrawClass::kTransparent);
  CHECK(table.Classify(State(true, false, false, kAlphaBlend)) == AoDrawClass::kEffect);
  // Every state gets a class from the built-in rules.
  const AoRule* matched = nullptr;
  table.Classify(State(true, true, false, kMultiplyBlend), &matched);
  CHECK(matched != nullptr);
  CHECK(matched && matched->line < 0);  // a built-in rule
}

TEST(AoRules_FileRulesComeFirst) {
  std::vector<std::string> errors;
  const std::vector<AoRule> mine = ParseAoRules(
      "# Kong's fur shells are blended but belong to the opaque scene\n"
      "opaque ps=0x00000000000F0F0F blend=alpha\n"
      "ignore prim=point,quad depth_test=yes blend=additive   # sparks\n"
      "transparent vs=1234ABCD\n",
      &errors);
  CHECK_EQ(errors.size(), size_t(0));
  CHECK_EQ(mine.size(), size_t(3));
  const AoRuleTable table(mine);
  CHECK(table.Classify(State(true, true, false, kAlphaBlend, 0xF0F0F)) == AoDrawClass::kOpaque);
  CHECK(table.Classify(State(true, true, false, kAlphaBlend, 0xF0F0E)) == AoDrawClass::kTransparent);
  AoDrawState spark = State(true, true, false, kAdditiveBlend);
  spark.primitive = 13;
  CHECK(table.Classify(spark) == AoDrawClass::kIgnore);
  spark.primitive = 4;
  CHECK(table.Classify(spark) == AoDrawClass::kTransparent);
  AoDrawState marked = State(true, true, true, kOpaqueBlend);
  marked.vs_hash = 0x1234ABCD;
  CHECK(table.Classify(marked) == AoDrawClass::kTransparent);
  CHECK_EQ(table.rules().size(), mine.size() + ParseAoRules(kDefaultAoRules).size());
  // Printed back and read again: the same rules.
  for (const AoRule& r : mine) {
    const std::vector<AoRule> again = ParseAoRules(FormatAoRule(r));
    CHECK_EQ(again.size(), size_t(1));
    if (again.size() == 1) CHECK_EQ(FormatAoRule(again[0]), FormatAoRule(r));
  }
}

TEST(AoRules_BadLinesAreSkippedAndReported) {
  std::vector<std::string> errors;
  const std::vector<AoRule> rules = ParseAoRules(
      "opaque blend=sideways\n"
      "shiny color=yes\n"
      "opaque ps=xyz\n"
      "depth color\n"
      "opaque prim=list,hexagon\n"
      "effect depth_test=no\n",
      &errors);
  CHECK_EQ(rules.size(), size_t(1));
  CHECK_EQ(errors.size(), size_t(5));
  if (!errors.empty()) CHECK(errors[0].rfind("line 1:", 0) == 0);
}

TEST(AoSettings_Names) {
  AoMethod m;
  AoApply a;
  AoQuality q;
  CHECK(ParseAoMethod("gtao", m) && m == AoMethod::kGtao);
  CHECK(ParseAoMethod("legacy", m) && m == AoMethod::kLegacy);
  CHECK(!ParseAoMethod("ssao", m));
  CHECK(ParseAoApply("opaque", a) && a == AoApply::kOpaque);
  CHECK(ParseAoApply("ambient", a) && a == AoApply::kAmbient);
  CHECK(ParseAoApply("resolve", a) && a == AoApply::kResolve);
  CHECK(!ParseAoApply("late", a));
  CHECK(ParseAoQuality("half", q) && q == AoQuality::kHalf);
  CHECK(ParseAoQuality("full", q) && q == AoQuality::kFull);
  CHECK(ParseAoQuality("auto", q) && q == AoQuality::kAuto);
  CHECK(!ParseAoQuality("ultra", q));
  // The defaults are today's AO.
  const AoConfig defaults;
  CHECK(defaults.method == AoMethod::kLegacy);
  CHECK(defaults.apply == AoApply::kResolve);
  CHECK(!defaults.gtao());
  CHECK(!defaults.ambient());
}

namespace {
constexpr uint64_t kSceneColor = 0x100, kSceneDepth = 0x200, kShadowColor = 0x300, kShadowDepth = 0x400;
AoFrameTracker::Draw Draw(uint64_t frame, AoDrawClass cls, uint64_t color, uint64_t depth, bool writes = false,
                          bool frame_size = true) {
  AoFrameTracker::Draw d;
  d.frame = frame;
  d.cls = cls;
  d.color_key = color;
  d.depth_key = depth;
  d.writes_depth = writes;
  d.frame_size = frame_size;
  return d;
}
}  // namespace

TEST(AoTracker_PhasesOfAFrame) {
  AoFrameTracker t;
  using C = AoDrawClass;
  // A shadow map first (another size): nothing.
  auto e = t.OnDraw(Draw(5, C::kOpaque, kShadowColor, kShadowDepth, true, false));
  CHECK(!e.in_scene && !e.depth_complete && !e.opaque_complete);
  // The sky before the scene: an effect, ignored.
  e = t.OnDraw(Draw(5, C::kEffect, kSceneColor, kSceneDepth));
  CHECK(!e.opaque_complete);
  // The depth pre-pass.
  e = t.OnDraw(Draw(5, C::kDepthOnly, kSceneColor, kSceneDepth, true));
  CHECK(e.in_scene && !e.depth_complete);
  t.OnDraw(Draw(5, C::kDepthOnly, kSceneColor, kSceneDepth, true));
  CHECK(t.phase() == AoFrameTracker::Phase::kDepthPass);
  CHECK(t.depth_valid());
  // The opaque colour pass (depth writes off): the pre-pass ended before its first draw.
  e = t.OnDraw(Draw(5, C::kOpaque, kSceneColor, kSceneDepth));
  CHECK(e.in_scene && e.depth_complete && !e.opaque_complete);
  e = t.OnDraw(Draw(5, C::kOpaque, kSceneColor, kSceneDepth));
  CHECK(!e.depth_complete && !e.opaque_complete);
  CHECK_EQ(t.scene_color(), kSceneColor);
  CHECK_EQ(t.scene_depth(), kSceneDepth);
  // A reflection into another target in between does not end the pass.
  e = t.OnDraw(Draw(5, C::kTransparent, kShadowColor, kShadowDepth, false, false));
  CHECK(!e.in_scene && !e.opaque_complete);
  // The water: the opaque pass ended before it, once.
  e = t.OnDraw(Draw(5, C::kTransparent, kSceneColor, kSceneDepth));
  CHECK(e.in_scene && e.opaque_complete);
  e = t.OnDraw(Draw(5, C::kEffect, kSceneColor, 0));
  CHECK(e.in_scene && !e.opaque_complete);
  e = t.OnDraw(Draw(5, C::kOpaque, kSceneColor, kSceneDepth));
  CHECK(!e.opaque_complete);
  CHECK(t.phase() == AoFrameTracker::Phase::kAfterOpaque);
  CHECK_EQ(t.depth_only_draws(), 2u);
  CHECK_EQ(t.opaque_draws(), 2u);
  // The next frame starts over.
  e = t.OnDraw(Draw(6, C::kOpaque, kSceneColor, kSceneDepth, true));
  CHECK(!e.depth_complete);  // no pre-pass this frame
  CHECK(t.phase() == AoFrameTracker::Phase::kOpaquePass);
  e = t.OnDraw(Draw(6, C::kEffect, kSceneColor, kSceneDepth));
  CHECK(e.opaque_complete);
}

TEST(AoRules_TheGamesRules) {
  // The built-in rules for King Kong's frames (docs/ao.md, "The classifier in the game").
  const AoRuleTable table;
  CHECK(table.Classify(State(true, false, false, kAlphaBlend, 0x1B65AC7429B67FE9)) == AoDrawClass::kEnd);  // fog
  CHECK(table.Classify(State(true, false, false, kAlphaBlend, 0x9F699E763A41FE07)) == AoDrawClass::kEnd);
  CHECK(table.Classify(State(true, true, true, kOpaqueBlend, 0x115AA5F3FE8AB69A)) == AoDrawClass::kIgnore);  // reflection
  CHECK(table.Classify(State(true, false, false, kOpaqueBlend, 0x16C954AF5D9DEBD0)) == AoDrawClass::kIgnore);  // shaft mask
  CHECK(table.Classify(State(true, true, true, kOpaqueBlend, 0xAC2239A7CA9EDF1C)) == AoDrawClass::kTransparent);  // water
  CHECK(table.Classify(State(true, true, true, kAlphaBlend, 0x2556ECFEAC993F0B)) == AoDrawClass::kTransparent);  // fur
  std::vector<std::string> errors;
  ParseAoRules(kDefaultAoRules, &errors);
  CHECK_EQ(errors.size(), size_t(0));
  CHECK(ParseAoRules("end ps=1234\n").size() == 1);
}

TEST(AoTracker_TheFogPassEndsTheOpaquePass) {
  // The game interleaves decals with its opaque draws and draws its fog pass at the end of them.
  AoFrameTracker t;
  using C = AoDrawClass;
  // The first frame knows of no end draw: the first blended draw ends the opaque pass.
  t.OnDraw(Draw(1, C::kDepthOnly, kSceneColor, kSceneDepth, true));
  t.OnDraw(Draw(1, C::kOpaque, kSceneColor, kSceneDepth));
  CHECK(!t.expects_end());
  auto e = t.OnDraw(Draw(1, C::kTransparent, kSceneColor, kSceneDepth));
  CHECK(e.opaque_complete);
  e = t.OnDraw(Draw(1, C::kEnd, kSceneColor, 0));
  CHECK(e.in_scene && !e.opaque_complete);
  // The next frame waits for it: the decal does not end the pass, the fog does.
  t.OnDraw(Draw(2, C::kDepthOnly, kSceneColor, kSceneDepth, true));
  t.OnDraw(Draw(2, C::kOpaque, kSceneColor, kSceneDepth));
  CHECK(t.expects_end());
  e = t.OnDraw(Draw(2, C::kTransparent, kSceneColor, kSceneDepth));
  CHECK(e.in_scene && !e.opaque_complete);
  e = t.OnDraw(Draw(2, C::kEffect, kSceneColor, kSceneDepth));
  CHECK(!e.opaque_complete);
  e = t.OnDraw(Draw(2, C::kOpaque, kSceneColor, kSceneDepth, true));
  CHECK(!e.opaque_complete);
  CHECK_EQ(t.opaque_draws(), 2u);
  e = t.OnDraw(Draw(2, C::kEnd, kSceneColor, 0));
  CHECK(e.opaque_complete);
  CHECK(t.phase() == AoFrameTracker::Phase::kAfterOpaque);
  // A frame without a fog pass (the pass never ends there; the scene copy or Present takes the
  // occlusion), then the one after it goes back to the first blended draw.
  t.OnDraw(Draw(3, C::kOpaque, kSceneColor, kSceneDepth, true));
  e = t.OnDraw(Draw(3, C::kTransparent, kSceneColor, kSceneDepth));
  CHECK(!e.opaque_complete);
  t.OnDraw(Draw(4, C::kOpaque, kSceneColor, kSceneDepth, true));
  CHECK(!t.expects_end());
  e = t.OnDraw(Draw(4, C::kTransparent, kSceneColor, kSceneDepth));
  CHECK(e.opaque_complete);
}

TEST(AoTracker_DepthValidity) {
  AoFrameTracker t;
  using C = AoDrawClass;
  t.OnDraw(Draw(1, C::kDepthOnly, kSceneColor, kSceneDepth, true));
  CHECK(t.depth_valid());
  // The game resolves the depth and clears it; the colour pass then tests against nothing it
  // wrote: the depth target is not this frame's scene any more.
  t.OnDepthCleared(1, kSceneDepth);
  CHECK(!t.depth_valid());
  t.OnDraw(Draw(1, C::kOpaque, kSceneColor, kSceneDepth, false));
  CHECK(!t.depth_valid());
  // A clear of another depth target changes nothing; a draw that writes depth makes it valid.
  t.OnDepthCleared(1, kShadowDepth);
  t.OnDraw(Draw(1, C::kOpaque, kSceneColor, kSceneDepth, true));
  CHECK(t.depth_valid());
  // Ignored draws never count.
  AoFrameTracker u;
  auto e = u.OnDraw(Draw(2, C::kIgnore, kSceneColor, kSceneDepth, true));
  CHECK(!e.in_scene);
  CHECK(u.phase() == AoFrameTracker::Phase::kNone);
}

namespace {

// Row-major 4x4 helpers (column vectors: clip = M * position).
struct M4 {
  float m[16] = {};
};
M4 Mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
  return r;
}
void Apply(const M4& a, const float v[4], float out[4]) {
  for (int i = 0; i < 4; ++i) out[i] = a.m[i * 4] * v[0] + a.m[i * 4 + 1] * v[1] + a.m[i * 4 + 2] * v[2] + a.m[i * 4 + 3] * v[3];
}
// D3D's left-handed perspective (z 0..1), with off-centre terms.
M4 Perspective(float fov_y, float aspect, float zn, float zf, float cx = 0, float cy = 0) {
  const float py = 1.0f / std::tan(fov_y / 2), px = py / aspect;
  M4 p;
  p.m[0] = px;
  p.m[2] = cx;
  p.m[5] = py;
  p.m[6] = cy;
  p.m[10] = zf / (zf - zn);
  p.m[11] = -zn * zf / (zf - zn);
  p.m[14] = 1;
  return p;
}
// A view: rotation about y then x, then a translation; and a world with a uniform scale.
M4 View(float yaw, float pitch, float tx, float ty, float tz) {
  const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
  M4 ry, rx, t;
  ry.m[0] = cy, ry.m[2] = sy, ry.m[5] = 1, ry.m[8] = -sy, ry.m[10] = cy, ry.m[15] = 1;
  rx.m[0] = 1, rx.m[5] = cp, rx.m[6] = -sp, rx.m[9] = sp, rx.m[10] = cp, rx.m[15] = 1;
  t.m[0] = t.m[5] = t.m[10] = t.m[15] = 1;
  t.m[3] = tx, t.m[7] = ty, t.m[11] = tz;
  return Mul(t, Mul(rx, ry));
}
M4 World(float sx, float sy, float sz, float x, float y, float z) {
  M4 w;
  w.m[0] = sx, w.m[5] = sy, w.m[10] = sz, w.m[15] = 1;
  w.m[3] = x, w.m[7] = y, w.m[11] = z;
  return w;
}
bool Close(float a, float b, float tol) { return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b)); }

}  // namespace

TEST(AoCamera_ProjectionFromTheMatrix) {
  const M4 p = Perspective(1.0f, 16.0f / 9.0f, 0.25f, 4000.0f);
  const M4 v = View(0.7f, -0.3f, 3.0f, -1.5f, 20.0f);
  for (float scale : {1.0f, 0.01f, 37.0f}) {
    const M4 wvp = Mul(p, Mul(v, World(scale, scale, scale, 5, -2, 9)));
    AoProjection got;
    CHECK(ExtractAoProjection(wvp.m, got));
    CHECK(Close(got.px, p.m[0], 1e-4f));
    CHECK(Close(got.py, p.m[5], 1e-4f));
    CHECK(std::fabs(got.cx) < 1e-4f && std::fabs(got.cy) < 1e-4f);
    CHECK(Close(got.a, p.m[10], 1e-5f));
    CHECK(Close(got.b, p.m[11], 1e-3f));
    CHECK(Close(got.near_plane(), 0.25f, 1e-3f));
    CHECK(Close(got.far_plane(), 4000.0f, 2e-2f));
  }
  // Off-centre.
  const M4 off = Perspective(1.2f, 1.0f, 1.0f, 100.0f, 0.1f, -0.05f);
  AoProjection got;
  CHECK(ExtractAoProjection(Mul(off, v).m, got));
  CHECK(Close(got.cx, 0.1f, 1e-4f) && Close(got.cy, -0.05f, 1e-4f));
  CHECK(Close(got.px, off.m[0], 1e-4f) && Close(got.py, off.m[5], 1e-4f));
  // Not a projection: an affine matrix, nothing.
  const M4 affine = Mul(v, World(2, 2, 2, 0, 0, 0));
  CHECK(!ExtractAoProjection(affine.m, got));
  const float zero[16] = {};
  CHECK(!ExtractAoProjection(zero, got));
}

TEST(AoCamera_FoundAmongTheConstants) {
  // Bone rows first, the matrix at c36, as the game's skinned meshes have it.
  std::vector<uint8_t> be(256 * 16, 0);
  auto put = [&](uint32_t reg, const float* v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) StoreBEFloat(be.data() + reg * 16 + i * 4, v[i]);
  };
  const M4 bones = World(1, 1, 1, 3, 4, 5);
  put(0, bones.m, 12);
  const M4 wvp = Mul(Perspective(0.9f, 1.5f, 0.5f, 900.0f), View(0.2f, 0.1f, 0, 0, 10));
  put(36, wvp.m, 16);
  float rows[16];
  CHECK_EQ(FindAoPerspectiveRegister(be.data(), rows), 36);
  CHECK(std::memcmp(rows, wvp.m, sizeof(rows)) == 0);
  std::vector<uint8_t> none(256 * 16, 0);
  CHECK_EQ(FindAoPerspectiveRegister(none.data(), rows), -1);
}

TEST(AoCamera_TheVoteKeepsTheSceneCamera) {
  const M4 p = Perspective(1.0f, 1.0f, 0.1f, 100.0f);
  const M4 v = View(0.3f, -0.2f, 0, 0, 5);
  AoScreen s;
  s.x_scale = 64, s.x_offset = 64, s.y_scale = -64, s.y_offset = 64;
  AoCameraFinder finder;
  AoProjection a, b;
  CHECK(ExtractAoProjection(Mul(p, v).m, a));
  // A mesh whose world stretches x: another px.
  CHECK(ExtractAoProjection(Mul(p, Mul(v, World(3, 1, 1, 0, 0, 0))).m, b));
  CHECK(!Close(a.px, b.px, 0.01f));
  finder.Offer(1, b, s);
  finder.Offer(1, a, s);
  finder.Offer(1, a, s);
  finder.Offer(1, b, s);
  finder.Offer(1, a, s);
  AoProjection got;
  AoScreen gs;
  CHECK(finder.Get(1, got, gs));
  CHECK(Close(got.px, a.px, 1e-6f));
  CHECK_EQ(finder.offers(1), 5u);
  // A frame without candidates keeps the last frame's camera.
  CHECK(finder.Get(2, got, gs));
  CHECK(Close(got.px, a.px, 1e-6f));
  finder.Offer(3, b, s);
  CHECK(finder.Get(3, got, gs));
  CHECK(Close(got.px, b.px, 1e-6f));
  AoCameraFinder empty;
  CHECK(!empty.Get(1, got, gs));
}

namespace {

// Projects a world point the way the game and the renderer do: the viewport transform in guest
// pixels (D3D9's half-pixel offset included), the render scale to host pixels, and the stored depth.
struct Projected {
  float host_x, host_y, depth, view[3];
};
Projected Project(const M4& proj, const M4& view, const AoScreen& s, const float world[3]) {
  const float pos[4] = {world[0], world[1], world[2], 1};
  float v[4], c[4];
  Apply(view, pos, v);
  Apply(Mul(proj, view), pos, c);
  Projected r;
  const float nx = c[0] / c[3], ny = c[1] / c[3], nz = c[2] / c[3];
  r.host_x = (nx * s.x_scale + s.x_offset + s.offset_x()) * s.host_scale_x;
  r.host_y = (ny * s.y_scale + s.y_offset + s.offset_y()) * s.host_scale_y;
  r.depth = nz * s.z_scale + s.z_offset;
  for (int i = 0; i < 3; ++i) r.view[i] = v[i];
  return r;
}

}  // namespace

TEST(AoCamera_ViewMappingRebuildsViewSpace) {
  // A 1280x720 guest frame, D3D9's half-pixel offset, render scales 1 and 2, full and half
  // resolution working images (padded to 16), a picture rectangle at the target's origin and one
  // inside it, an off-centre projection and a depth range.
  const M4 proj = Perspective(0.9f, 16.0f / 9.0f, 0.5f, 3000.0f, 0.02f, -0.01f);
  const M4 view = View(-0.4f, 0.25f, 2, -1, 30);
  AoProjection p;
  CHECK(ExtractAoProjection(Mul(proj, view).m, p));
  const float points[][3] = {{0, 0, 0}, {5, 2, -3}, {-8, 1, 6}, {1, -3, 20}};
  for (float scale : {1.0f, 2.0f, 1.5f}) {
    for (uint32_t step : {1u, 2u}) {
      for (int inner = 0; inner < 2; ++inner) {
        AoScreen s;
        const float w = inner ? 640.0f : 1280.0f, h = inner ? 360.0f : 720.0f;
        const float x0 = inner ? 100.0f : 0.0f, y0 = inner ? 50.0f : 0.0f;
        s.x_scale = w / 2, s.x_offset = x0 + w / 2, s.y_scale = -h / 2, s.y_offset = y0 + h / 2;
        s.z_scale = 0.9f, s.z_offset = 0.05f;
        s.half_pixel = 0.5f;
        s.host_scale_x = s.host_scale_y = scale;
        const int32_t ox = int32_t(std::lround(x0 * scale)), oy = int32_t(std::lround(y0 * scale));
        const uint32_t ww = uint32_t(std::lround(w * scale)) / step, wh = uint32_t(std::lround(h * scale)) / step;
        const uint32_t pw = (ww + 15) / 16 * 16, ph = (wh + 15) / 16 * 16;
        const AoViewMapping m = ComputeAoViewMapping(p, s, ox, oy, pw, ph, step);
        for (const auto& point : points) {
          const Projected pr = Project(proj, view, s, point);
          // The working pixel whose centre is that host position (fractional), as XeGTAO's uv.
          const float i = (pr.host_x - float(ox) - 0.5f) / float(step);
          const float j = (pr.host_y - float(oy) - 0.5f) / float(step);
          const float u = (i + 0.5f) / float(pw), v = (j + 0.5f) / float(ph);
          const float z = m.depth_mul / (m.depth_add - pr.depth);
          const float vx = (m.mul[0] * u + m.add[0]) * z, vy = (m.mul[1] * v + m.add[1]) * z;
          // XeGTAO's view space: x right, y up, z forward; the test's view is y up already.
          CHECK(Close(z, pr.view[2], 2e-3f));
          CHECK(std::fabs(vx - pr.view[0]) < 2e-3f * std::max(1.0f, pr.view[2]));
          CHECK(std::fabs(vy - pr.view[1]) < 2e-3f * std::max(1.0f, pr.view[2]));
        }
      }
    }
  }
}

// The lighting options (docs/lighting.md): names, defaults, the shadow scale a quality asks for, and the
// game's shaders they act on.
TEST(Lighting_SettingsAndShaders) {
  LightingConfig c;
  CHECK(!c.any() && !c.active());  // the defaults run nothing
  ShadowQuality q;
  CHECK(ParseShadowQuality("original", q) && q == ShadowQuality::kOriginal);
  CHECK(ParseShadowQuality("high", q) && q == ShadowQuality::kHigh);
  CHECK(ParseShadowQuality("ultra", q) && q == ShadowQuality::kUltra);
  CHECK(!ParseShadowQuality("max", q));
  CHECK_EQ(ShadowScaleForQuality(ShadowQuality::kOriginal), 0.0f);
  CHECK_EQ(ShadowScaleForQuality(ShadowQuality::kHigh), 2.0f);
  CHECK_EQ(ShadowScaleForQuality(ShadowQuality::kUltra), 4.0f);
  WaterReflections w;
  CHECK(ParseWaterReflections("ssr", w) && w == WaterReflections::kSsr);
  CHECK(!ParseWaterReflections("rt", w));
  Volumetrics v;
  CHECK(ParseVolumetrics("off", v) && v == Volumetrics::kOff);
  CHECK(ParseVolumetrics("high", v) && v == Volumetrics::kHigh);
  CHECK(!ParseVolumetrics("on", v));
  for (ShadowQuality s : {ShadowQuality::kOriginal, ShadowQuality::kHigh, ShadowQuality::kUltra})
    CHECK(ParseShadowQuality(ShadowQualityName(s), q) && q == s);
  c.census_every = 300;
  CHECK(!c.any() && c.active());
  c.volumetrics = Volumetrics::kLow;
  CHECK(c.any() && c.volumetrics_on());
  // The game's shaders.
  CHECK(LightFamilyOf(0x9A0FBB044BB1BD85ull) == LightFamily::kShadowQuad);
  CHECK_EQ(ShadowQuadLayoutOf(0x9A0FBB044BB1BD85ull).lights, 3u);
  CHECK_EQ(ShadowQuadLayoutOf(0x9F5E02F361E2A8A0ull).shadow_params, 5u);
  CHECK_EQ(ShadowQuadLayoutOf(0x6DC2B9F3C32C2394ull).shadow_color, 10u);
  CHECK(LightFamilyOf(0x1B65AC7429B67FE9ull) == LightFamily::kFog);
  CHECK(LightShaftModeOf(0x16C954AF5D9DEBD0ull) == LightShaftMode::kApply);
  CHECK(LightShaftModeOf(0xCA12F7AA224EF5C2ull) == LightShaftMode::kRender);
  CHECK(WaterLayoutOf(0xC28D09A8A208AD93ull).dynamic_reflection && WaterLayoutOf(0xC28D09A8A208AD93ull).environment);
  CHECK(WaterLayoutOf(0xAC2239A7CA9EDF1Cull).water && !WaterLayoutOf(0xAC2239A7CA9EDF1Cull).dynamic_reflection);
  CHECK(LightFamilyOf(0x0123456789ABCDEFull) == LightFamily::kNone);
}
NR_TEST_MAIN()
