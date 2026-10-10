// Render-target pool and resolve planning with the surfaces the census shows (832x832 R32F shadow maps,
// 320x180 / 640x360 / 640x480 A8R8G8B8 and X8R8G8B8 post-effect targets, a 1280x720 back buffer).
#include <vector>

#include "kknr/render_targets.h"
#include "test.h"

using namespace kknr;

namespace {

SurfaceDesc Surface(uint32_t w, uint32_t h, uint32_t d3d_format, uint32_t ms = 0, uint32_t base = 0) {
  SurfaceDesc d;
  DecodeSurfaceDesc(w, h, d3d_format, ms, base, d);
  return d;
}

TextureFetch DestTexture(TextureFormat format, uint32_t w, uint32_t h, uint16_t swizzle = kSwizzleXYZW) {
  TextureFetchDesc d;
  d.format = format;
  d.width = w;
  d.height = h;
  d.tiled = true;
  d.base_address = 0x100000;
  d.swizzle = swizzle;
  return MakeTextureFetch(d);
}

}  // namespace

TEST(surface_descriptions) {
  SurfaceDesc d;
  CHECK(DecodeSurfaceDesc(640, 360, 0x18280186, 0, 0, d));
  CHECK_EQ(int(d.host), int(HostRtFormat::RGBA8_UNORM));
  CHECK(!d.depth);
  CHECK(DecodeSurfaceDesc(832, 832, 0x2DA2ABA4, 0, 0, d));
  CHECK_EQ(int(d.host), int(HostRtFormat::R32_FLOAT));
  CHECK(DecodeSurfaceDesc(1280, 720, 0x2D200196, 0, 0, d));
  CHECK_EQ(int(d.host), int(HostRtFormat::D24S8));
  CHECK(d.depth);
  CHECK(DecodeSurfaceDesc(640, 480, 0x28280186, 2, 0, d));
  CHECK_EQ(d.msaa, 4u);
  CHECK(!DecodeSurfaceDesc(64, 64, uint32_t(TextureFormat::k_DXT1), 0, 0, d));  // not renderable
  CHECK(!DecodeSurfaceDesc(0, 64, 0x18280186, 0, 0, d));
}

TEST(surface_edram_tiles) {
  // 80x16-sample tiles: 1280x720 = 16 x 45; 4x MSAA doubles both axes; 64 bpp doubles the count.
  CHECK_EQ(SurfaceEdramTiles(Surface(1280, 720, 0x18280186)), 720u);
  CHECK_EQ(SurfaceEdramTiles(Surface(1280, 720, 0x18280186, 2)), 2880u);
  CHECK_EQ(SurfaceEdramTiles(Surface(1280, 720, 0x18280186, 1)), 1440u);
  CHECK_EQ(SurfaceEdramTiles(Surface(832, 832, 0x2DA2ABA4)), 11u * 52);
  SurfaceDesc f16 = Surface(320, 180, uint32_t(TextureFormat::k_16_16_16_16_FLOAT));
  CHECK_EQ(SurfaceEdramTiles(f16), 4u * 12 * 2);
}

TEST(pool_reuses_targets_across_frames) {
  // 12 transient surfaces per frame, created and released every frame: the pool stops growing after frame 1.
  RenderTargetPool pool;
  const SurfaceDesc frame_set[4] = {Surface(832, 832, 0x2DA2ABA4), Surface(320, 180, 0x18280186),
                                    Surface(640, 360, 0x28280186), Surface(640, 480, 0x18280186)};
  int created_total = 0;
  for (uint64_t frame = 1; frame <= 5; ++frame) {
    std::vector<size_t> held;
    for (int i = 0; i < 12; ++i) {
      bool created = false;
      const size_t h = pool.Acquire(frame_set[i % 4], frame, &created);
      if (created) pool.Get(h).host = reinterpret_cast<void*>(uintptr_t(h + 1));  // the renderer creates it
      created_total += created;
      // Every third one is released at once (draw, resolve, release), the rest at the end of the frame.
      if (i % 3 == 0)
        pool.Release(h);
      else
        held.push_back(h);
    }
    CHECK_EQ(pool.InUse(), held.size());
    for (size_t h : held) pool.Release(h);
  }
  CHECK_EQ(created_total, int(pool.size()));
  CHECK(pool.size() <= 12u);
  // Nothing is created after the first frame.
  bool created = true;
  pool.Acquire(frame_set[0], 6, &created);
  CHECK(!created);
}

TEST(pool_prefers_same_edram_place) {
  RenderTargetPool pool;
  const SurfaceDesc at0 = Surface(320, 180, 0x18280186, 0, 0);
  const SurfaceDesc at100 = Surface(320, 180, 0x18280186, 0, 100);
  const size_t a = pool.Acquire(at0, 1);
  const size_t b = pool.Acquire(at100, 1);
  pool.Release(b);
  pool.Release(a);  // released last
  // Asking for base 100 returns b (same place) even though a was released more recently.
  CHECK_EQ(pool.Acquire(at100, 2), b);
  // With b in use, base 0 gets a; a third surface at base 200 takes neither and is created.
  CHECK_EQ(pool.Acquire(at0, 2), a);
  bool created = false;
  pool.Acquire(Surface(320, 180, 0x18280186, 0, 200), 2, &created);
  CHECK(created);
  // Once everything is free, a new base reuses a same-shape slot instead of growing.
  pool.Release(0);
  pool.Release(1);
  pool.Release(2);
  const size_t before = pool.size();
  pool.Acquire(Surface(320, 180, 0x18280186, 0, 300), 3, &created);
  CHECK(!created);
  CHECK_EQ(pool.size(), before);
  // Different shape: never shared.
  pool.Acquire(Surface(320, 180, 0x2DA2ABA4, 0, 300), 3, &created);
  CHECK(created);
}

TEST(pool_trim) {
  RenderTargetPool pool;
  const size_t a = pool.Acquire(Surface(64, 64, 0x18280186), 1);
  const size_t b = pool.Acquire(Surface(128, 64, 0x18280186), 1);
  pool.Release(a);
  pool.Release(b);
  pool.Acquire(Surface(128, 64, 0x18280186), 50);
  pool.Release(b);
  const std::vector<RenderTargetPool::Slot> freed = pool.Trim(100, 60);
  CHECK_EQ(freed.size(), size_t(1));
  if (!freed.empty()) CHECK_EQ(freed[0].desc.width, 64u);
  bool created = false;
  pool.Acquire(Surface(256, 256, 0x18280186), 101, &created);
  CHECK(created);
  CHECK_EQ(pool.size(), size_t(2));  // the trimmed slot was reused, not a third one
}

TEST(resolve_plans) {
  const SurfaceDesc rt = Surface(640, 360, 0x18280186);
  // Whole surface into an A8R8G8B8 texture of the same size: a copy with the R / B order noted.
  TextureFetch dest = DestTexture(TextureFormat::k_8_8_8_8, 640, 360, MakeSwizzle(kSwzZ, kSwzY, kSwzX, kSwzW));
  ResolveOp op = PlanResolve(0, rt, nullptr, &dest, nullptr, 0, 0);
  CHECK(op.kind == ResolveKind::kCopy);
  CHECK(op.swap_red_blue);
  CHECK_EQ(op.width, 640u);
  CHECK_EQ(op.height, 360u);
  // A rect and destination point, clipped to the destination.
  const uint32_t rect[4] = {100, 50, 500, 300};
  const uint32_t point[2] = {300, 200};
  dest = DestTexture(TextureFormat::k_8_8_8_8, 320, 180);
  op = PlanResolve(0x100, rt, rect, &dest, point, 0, 0);
  CHECK(op.kind == ResolveKind::kCopy);
  CHECK(op.clear_color);
  CHECK(!op.clear_depth);
  CHECK(!op.swap_red_blue);
  CHECK_EQ(op.src_x, 100u);
  CHECK_EQ(op.width, 20u);
  CHECK_EQ(op.height, 0u);
  // Another colour format: a converting blit.
  dest = DestTexture(TextureFormat::k_16_16_16_16_FLOAT, 640, 360);
  CHECK(PlanResolve(0, rt, nullptr, &dest, nullptr, 0, 0).kind == ResolveKind::kConvert);
  // Depth into a float texture (shadow maps).
  const SurfaceDesc depth = Surface(832, 832, 0x2D200196);
  dest = DestTexture(TextureFormat::k_32_FLOAT, 832, 832);
  op = PlanResolve(0x4 | 0x200, depth, nullptr, &dest, nullptr, 0, 0);
  CHECK(op.kind == ResolveKind::kDepthToFloat);
  CHECK(op.clear_depth);
  // No destination: clears only.
  op = PlanResolve(0x300, rt, nullptr, nullptr, nullptr, 0, 0);
  CHECK(op.kind == ResolveKind::kNone);
  CHECK(op.clear_color && op.clear_depth);
  // Resolve into a mip level clips to that level's size.
  dest = DestTexture(TextureFormat::k_8_8_8_8, 640, 360);
  op = PlanResolve(0, rt, nullptr, &dest, nullptr, 1, 0);
  CHECK_EQ(op.width, 320u);
  CHECK_EQ(op.height, 180u);
  CHECK_EQ(op.dst_level, 1u);
}
