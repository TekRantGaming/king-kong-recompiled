// Resolve conversions with synthetic EDRAM data. For every pair the game uses (and more), two paths must
// agree texel for texel:
//   A: the 360's path: ResolveToGuest packs the EDRAM pixels into the destination texture in guest memory
//      (swap, rounding, endian, tiling), then ConvertTexture reads it as any texture.
//   B: the renderer's path: the pixel as the host render target holds it, through PlanResolveConversion's
//      copy or blit (channels, scale) into the destination's host format.
// And sampling the result through the texture's view swizzle must give back the colour that was drawn.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "kknr/resolve.h"
#include "test.h"

using namespace kknr;
using kknr_test::FakeGuest;
using kknr_test::Rng;

namespace {

constexpr uint32_t kBase = 0x00400000, kMips = 0x00C00000;

struct Case {
  const char* name;
  ResolveSource source;
  TextureFetchDesc dest;
  ResolveMethod method;
  float tolerance;  // per channel, in the decoded host value
  bool sample_check;
};

TextureFetchDesc Dest(TextureFormat f, Endian e, uint16_t swizzle, uint32_t w, uint32_t h,
                      TextureSign sign = TextureSign::kUnsigned) {
  TextureFetchDesc d;
  d.format = f;
  d.endian = e;
  d.swizzle = swizzle;
  d.tiled = true;
  d.width = w;
  d.height = h;
  d.base_address = kBase;
  for (TextureSign& s : d.signs) s = sign;
  return d;
}

ResolveSource Color(EdramColorFormat f) { return ResolveSource{false, uint32_t(f)}; }
ResolveSource Depth(EdramDepthFormat f) { return ResolveSource{true, uint32_t(f)}; }

uint16_t RandomHalf(Rng& rng) {
  for (;;) {
    const uint16_t h = uint16_t(rng.Next() >> 48);
    if (((h >> 10) & 31) != 31) return h;  // finite
  }
}

ResolveSourceImage RandomSource(const ResolveSource& s, uint32_t w, uint32_t h, uint64_t seed) {
  ResolveSourceImage img;
  img.source = s;
  img.width = w;
  img.height = h;
  img.raw.resize(size_t(w) * h * 2);
  Rng rng(seed);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    uint32_t* p = &img.raw[i * 2];
    p[0] = uint32_t(rng.Next() >> 32);
    p[1] = uint32_t(rng.Next() >> 32);
    if (s.depth) continue;
    switch (EdramColorFormat(s.format)) {
      case EdramColorFormat::k_16_16_FLOAT:
      case EdramColorFormat::k_16_16_16_16_FLOAT:
        p[0] = RandomHalf(rng) | uint32_t(RandomHalf(rng)) << 16;
        p[1] = RandomHalf(rng) | uint32_t(RandomHalf(rng)) << 16;
        break;
      case EdramColorFormat::k_32_FLOAT:
      case EdramColorFormat::k_32_32_FLOAT:
        for (int k = 0; k < 2; ++k) {
          const float f = float(int32_t(rng.Next() >> 40)) / 65536.0f;
          std::memcpy(&p[k], &f, 4);
        }
        break;
      default:
        break;
    }
  }
  return img;
}

// Runs both paths for one case and one rectangle; reports the first difference.
bool RunCase(const Case& c, const ResolveRect& rect, uint64_t seed) {
  const TextureFetch dest = MakeTextureFetch(c.dest);
  const ResolveConversion plan = PlanResolveConversion(c.source, dest);
  if (plan.method != c.method) {
    kknr_test::Fail(__FILE__, __LINE__, std::string(c.name) + ": unexpected method (" + plan.why + ")");
    return false;
  }
  const ResolveSourceImage src = RandomSource(c.source, 48, 40, seed);

  // A: through guest memory.
  GuestTextureImage image;
  EncodeGuestTexture(dest, [](uint32_t, uint32_t) -> const uint8_t* { return nullptr; }, image);
  const GuestTextureImage untouched = image;
  if (!ResolveToGuest(src, rect, dest, image)) {
    kknr_test::Fail(__FILE__, __LINE__, std::string(c.name) + ": ResolveToGuest failed");
    return false;
  }
  static FakeGuest guest;
  auto load = [&](const GuestTextureImage& im, HostTextureData& out) {
    std::fill_n(guest.bytes.begin() + kBase, kMips - kBase, 0);
    std::fill_n(guest.bytes.begin() + kMips, 4u << 20, 0);
    guest.Put(kBase, im.base);
    guest.Put(kMips, im.mips);
    std::string why;
    return ConvertTexture(dest, GuestMemory{guest.bytes.data(), guest.bytes.size()}, out, &why);
  };
  HostTextureData a, before;
  if (!load(image, a) || !load(untouched, before)) {
    kknr_test::Fail(__FILE__, __LINE__, std::string(c.name) + ": ConvertTexture failed");
    return false;
  }
  CHECK_EQ(int(a.plan.format), int(plan.dest_format));
  const HostSubresource* sub = a.Find(rect.level, rect.slice);
  const HostSubresource* sub0 = before.Find(rect.level, rect.slice);
  if (!sub || !sub0) return false;
  const uint32_t bpp = GetHostFormatInfo(plan.dest_format).bytes_per_block;
  const int32_t w = std::min({rect.width, int32_t(src.width) - rect.src_x, int32_t(sub->width) - rect.dst_x});
  const int32_t h = std::min({rect.height, int32_t(src.height) - rect.src_y, int32_t(sub->height) - rect.dst_y});

  for (uint32_t y = 0; y < sub->height; ++y)
    for (uint32_t x = 0; x < sub->width; ++x) {
      const uint8_t* got = a.bytes.data() + sub->offset + size_t(y) * sub->row_pitch + size_t(x) * bpp;
      const bool inside = int32_t(x) >= rect.dst_x && int32_t(x) < rect.dst_x + w && int32_t(y) >= rect.dst_y &&
                          int32_t(y) < rect.dst_y + h;
      if (!inside) {
        // Untouched texels keep what the memory held.
        const uint8_t* old = before.bytes.data() + sub0->offset + size_t(y) * sub0->row_pitch + size_t(x) * bpp;
        if (std::memcmp(got, old, bpp) != 0) {
          kknr_test::Fail(__FILE__, __LINE__, std::string(c.name) + ": texel outside the rectangle changed");
          return false;
        }
        continue;
      }
      // B: the host render target's texel through the plan.
      const uint32_t* raw =
          &src.raw[(size_t(rect.src_y + int32_t(y) - rect.dst_y) * src.width + size_t(rect.src_x + int32_t(x) - rect.dst_x)) * 2];
      float target[4];
      EdramPixelToHostTarget(c.source, raw, target);
      uint8_t expect[16];
      if (!EmulateResolveTexel(plan, target, expect)) {
        kknr_test::Fail(__FILE__, __LINE__, std::string(c.name) + ": no GPU path");
        return false;
      }
      float ga[4], gb[4];
      DecodeHostTexel(plan.dest_format, got, ga);
      DecodeHostTexel(plan.dest_format, expect, gb);
      for (int k = 0; k < 4; ++k)
        if (!(std::fabs(ga[k] - gb[k]) <= c.tolerance)) {
          kknr_test::Fail(__FILE__, __LINE__,
                          std::string(c.name) + ": texel " + std::to_string(x) + "," + std::to_string(y) +
                              " channel " + std::to_string(k) + ": memory path " + std::to_string(ga[k]) +
                              ", GPU path " + std::to_string(gb[k]));
          return false;
        }
      if (c.sample_check) {
        // What a shader sampling the texture sees, against what was drawn.
        float seen[4];
        for (int k = 0; k < 4; ++k) {
          const uint8_t s = SwizzleComponent(a.plan.view_swizzle, k);
          seen[k] = s < 4 ? ga[s] : (s == 5 ? 1.0f : 0.0f);
        }
        float want[4] = {target[0], target[1], target[2], target[3]};
        // The view swizzle's constants and replicated components stand for themselves.
        for (int k = 0; k < 4; ++k) {
          const uint8_t fs = SwizzleComponent(dest.Swizzle(), k);
          if (fs >= 4) want[k] = fs == 5 ? 1.0f : 0.0f;
        }
        if (dest.Format() == TextureFormat::k_8) want[3] = target[0];  // 000X: the mask reads red in W
        // Half-float destinations round what was drawn to 11 significant bits.
        const HostFormat df = plan.dest_format;
        const bool half = df == HostFormat::R16_FLOAT || df == HostFormat::RG16_FLOAT || df == HostFormat::RGBA16_FLOAT;
        for (int k = 0; k < 4; ++k)
          if (!(std::fabs(seen[k] - want[k]) <= c.tolerance + 1e-6f + (half ? std::fabs(want[k]) / 1024.0f : 0.0f))) {
            kknr_test::Fail(__FILE__, __LINE__, std::string(c.name) + ": sampled channel " + std::to_string(k) +
                                                    " " + std::to_string(seen[k]) + " vs drawn " +
                                                    std::to_string(want[k]));
            return false;
          }
      }
    }
  return true;
}

constexpr uint16_t kARGB = MakeSwizzle(kSwzZ, kSwzY, kSwzX, kSwzW);
constexpr uint16_t kXRGB = MakeSwizzle(kSwzZ, kSwzY, kSwzX, kSwz1);
constexpr uint16_t kMask = MakeSwizzle(kSwz0, kSwz0, kSwz0, kSwzX);
constexpr uint16_t kX111 = MakeSwizzle(kSwzX, kSwz1, kSwz1, kSwz1);

}  // namespace

TEST(resolve_game_pairs) {
  // The census's resolves: A8R8G8B8 targets into 8in32 A8R8G8B8 textures, the endian-none X8R8G8B8 one, the
  // k_8 mask (endian none), the R32F shadow maps (signed, integer number format, as bound), depth into
  // k_24_8.
  TextureFetchDesc shadow = Dest(TextureFormat::k_32_FLOAT, Endian::k8in32, kX111, 64, 48, TextureSign::kSigned);
  shadow.integer = true;
  const Case cases[] = {
      {"A8R8G8B8 8in32", Color(EdramColorFormat::k_8_8_8_8),
       Dest(TextureFormat::k_8_8_8_8, Endian::k8in32, kARGB, 64, 48), ResolveMethod::kBlit, 0.0f, true},
      {"X8R8G8B8 endian none", Color(EdramColorFormat::k_8_8_8_8),
       Dest(TextureFormat::k_8_8_8_8, Endian::kNone, kXRGB, 64, 48), ResolveMethod::kBlit, 0.0f, true},
      {"k_8 mask endian none", Color(EdramColorFormat::k_8_8_8_8),
       Dest(TextureFormat::k_8, Endian::kNone, kMask, 64, 48), ResolveMethod::kBlit, 0.0f, true},
      {"R32F shadow map", Color(EdramColorFormat::k_32_FLOAT), shadow, ResolveMethod::kCopy, 0.0f, true},
      {"D24S8 into k_24_8", Depth(EdramDepthFormat::kD24S8),
       Dest(TextureFormat::k_24_8, Endian::k8in32, kX111, 64, 48), ResolveMethod::kBlit, 0.0f, false},
  };
  for (const Case& c : cases) {
    ResolveRect r;
    r.src_x = 3, r.src_y = 5, r.width = 30, r.height = 20, r.dst_x = 7, r.dst_y = 9;
    CHECK(RunCase(c, r, 1));
    // The whole surface at the origin, clipped by the destination.
    ResolveRect whole;
    whole.width = 48, whole.height = 40;
    CHECK(RunCase(c, whole, 2));
  }
}

TEST(resolve_other_pairs) {
  const Case cases[] = {
      {"8888 XYZW copy", Color(EdramColorFormat::k_8_8_8_8),
       Dest(TextureFormat::k_8_8_8_8, Endian::k8in32, kSwizzleXYZW, 64, 48), ResolveMethod::kCopy, 0.0f, true},
      {"gamma target into 8888", Color(EdramColorFormat::k_8_8_8_8_GAMMA),
       Dest(TextureFormat::k_8_8_8_8, Endian::k8in32, kSwizzleXYZW, 64, 48), ResolveMethod::kCopy, 0.0f, true},
      {"D24FS8 into k_24_8_FLOAT", Depth(EdramDepthFormat::kD24FS8),
       Dest(TextureFormat::k_24_8_FLOAT, Endian::k8in32, kX111, 64, 48), ResolveMethod::kBlit, 0.0f, false},
      {"2:10:10:10", Color(EdramColorFormat::k_2_10_10_10),
       Dest(TextureFormat::k_2_10_10_10, Endian::k8in32, kSwizzleXYZW, 64, 48), ResolveMethod::kCopy, 0.0f, true},
      {"2:10:10:10 as A2R10G10B10", Color(EdramColorFormat::k_2_10_10_10),
       Dest(TextureFormat::k_2_10_10_10, Endian::k8in32, kARGB, 64, 48), ResolveMethod::kBlit, 0.0f, true},
      {"16:16:16:16 float", Color(EdramColorFormat::k_16_16_16_16_FLOAT),
       Dest(TextureFormat::k_16_16_16_16_FLOAT, Endian::k8in32, kSwizzleXYZW, 64, 48), ResolveMethod::kCopy, 0.0f,
       true},
      {"16:16 float", Color(EdramColorFormat::k_16_16_FLOAT),
       Dest(TextureFormat::k_16_16_FLOAT, Endian::k8in32, MakeSwizzle(0, 1, 5, 5), 64, 48), ResolveMethod::kCopy,
       0.0f, true},
      {"7e3 into half floats", Color(EdramColorFormat::k_2_10_10_10_FLOAT),
       Dest(TextureFormat::k_16_16_16_16_FLOAT, Endian::k8in32, kSwizzleXYZW, 64, 48), ResolveMethod::kCopy, 0.0f,
       true},
      {"32:32 float", Color(EdramColorFormat::k_32_32_FLOAT),
       Dest(TextureFormat::k_32_32_FLOAT, Endian::k8in32, MakeSwizzle(0, 1, 5, 5), 64, 48), ResolveMethod::kCopy,
       0.0f, true},
      {"8888 into half floats", Color(EdramColorFormat::k_8_8_8_8),
       Dest(TextureFormat::k_16_16_16_16_FLOAT, Endian::k8in32, kSwizzleXYZW, 64, 48), ResolveMethod::kBlit, 0.0f,
       true},
      {"8888 into 8:8", Color(EdramColorFormat::k_8_8_8_8),
       Dest(TextureFormat::k_8_8, Endian::k8in16, MakeSwizzle(0, 1, 5, 5), 64, 48), ResolveMethod::kBlit, 0.0f,
       true},
      {"8888 into 16-bit unorm", Color(EdramColorFormat::k_8_8_8_8),
       Dest(TextureFormat::k_16_16_16_16, Endian::k8in32, kSwizzleXYZW, 64, 48), ResolveMethod::kBlit, 0.0f, true},
      // The destination keeps 11:11:10 bits in memory, the blit writes 16 bits: within half an 11-bit step.
      {"8888 into 10:11:11", Color(EdramColorFormat::k_8_8_8_8),
       Dest(TextureFormat::k_10_11_11, Endian::k8in32, MakeSwizzle(0, 1, 2, 5), 64, 48), ResolveMethod::kBlit,
       1.0f / 1023.0f, false},
      // Fixed point bits copied; read as signed normalized: value / 32 (the GPU path rounds through a half).
      {"16:16:16:16 fixed into signed 16-bit", Color(EdramColorFormat::k_16_16_16_16),
       Dest(TextureFormat::k_16_16_16_16, Endian::k8in32, kSwizzleXYZW, 64, 48, TextureSign::kSigned),
       ResolveMethod::kBlit, 1.0f / 1024.0f, false},
  };
  for (const Case& c : cases) {
    ResolveRect r;
    r.src_x = 1, r.src_y = 2, r.width = 33, r.height = 17, r.dst_x = 30, r.dst_y = 31;
    CHECK(RunCase(c, r, 7));
  }
}

TEST(resolve_into_mips_and_cube_faces) {
  // A level of a texture with a mip chain (the data lands in the mip region) and a cube face.
  TextureFetchDesc d = Dest(TextureFormat::k_8_8_8_8, Endian::k8in32, kARGB, 64, 64);
  d.mip_address = kMips;
  d.packed_mips = true;
  d.max_level = 6;
  const Case mips{"8888 level 1", Color(EdramColorFormat::k_8_8_8_8), d, ResolveMethod::kBlit, 0.0f, true};
  ResolveRect r;
  r.width = 20, r.height = 12, r.dst_x = 2, r.dst_y = 3, r.level = 1;
  CHECK(RunCase(mips, r, 11));
  r.level = 4;  // inside the packed tail
  r.dst_x = r.dst_y = 0;
  CHECK(RunCase(mips, r, 12));
  TextureFetchDesc cube = Dest(TextureFormat::k_8_8_8_8, Endian::k8in32, kARGB, 32, 32);
  cube.dimension = Dimension::kCube;
  const Case face{"8888 cube face", Color(EdramColorFormat::k_8_8_8_8), cube, ResolveMethod::kBlit, 0.0f, true};
  r = ResolveRect();
  r.width = 32, r.height = 32, r.slice = 3;
  CHECK(RunCase(face, r, 13));
}

TEST(resolve_known_values) {
  // One pixel by hand: an A8R8G8B8 target pixel drawn as (r, g, b, a) = (0x11, 0x22, 0x33, 0x44) lands in
  // memory as the big-endian word 0x44112233 (A, R, G, B) for an 8in32 A8R8G8B8 texture, and as the single
  // byte 0x11 for the k_8 mask.
  ResolveSourceImage src;
  src.source = Color(EdramColorFormat::k_8_8_8_8);
  src.width = src.height = 1;
  src.raw = {0x44332211u, 0};
  ResolveRect r;
  r.width = r.height = 1;
  TextureFetchDesc d = Dest(TextureFormat::k_8_8_8_8, Endian::k8in32, kARGB, 32, 32);
  d.tiled = false;
  GuestTextureImage image;
  CHECK(ResolveToGuest(src, r, MakeTextureFetch(d), image));
  CHECK(image.base.size() >= 4);
  if (image.base.size() >= 4) CHECK(image.base[0] == 0x44 && image.base[1] == 0x11 && image.base[2] == 0x22 && image.base[3] == 0x33);
  d = Dest(TextureFormat::k_8, Endian::kNone, kMask, 32, 32);
  d.tiled = false;
  image = GuestTextureImage();
  CHECK(ResolveToGuest(src, r, MakeTextureFetch(d), image));
  if (!image.base.empty()) CHECK_EQ(image.base[0], 0x11);
  // The endian-none X8R8G8B8 texture: the same word stored little-endian (B, G, R, X in memory order).
  d = Dest(TextureFormat::k_8_8_8_8, Endian::kNone, kXRGB, 32, 32);
  d.tiled = false;
  image = GuestTextureImage();
  CHECK(ResolveToGuest(src, r, MakeTextureFetch(d), image));
  if (image.base.size() >= 4) CHECK(image.base[0] == 0x33 && image.base[1] == 0x22 && image.base[2] == 0x11 && image.base[3] == 0x44);
  // Depth: the raw word, big-endian with 8in32.
  src.source = Depth(EdramDepthFormat::kD24S8);
  src.raw = {0xC0000012u, 0};
  d = Dest(TextureFormat::k_24_8, Endian::k8in32, kX111, 32, 32);
  d.tiled = false;
  image = GuestTextureImage();
  CHECK(ResolveToGuest(src, r, MakeTextureFetch(d), image));
  if (image.base.size() >= 4) CHECK(image.base[0] == 0xC0 && image.base[3] == 0x12);
}

TEST(resolve_unsupported_pairs) {
  auto method = [](ResolveSource s, TextureFetchDesc d) { return PlanResolveConversion(s, MakeTextureFetch(d)).method; };
  CHECK(method(Color(EdramColorFormat::k_8_8_8_8), Dest(TextureFormat::k_DXT1, Endian::k8in16, kSwizzleXYZW, 64, 64)) ==
        ResolveMethod::kUnsupported);
  CHECK(method(Color(EdramColorFormat::k_8_8_8_8),
               Dest(TextureFormat::k_8_8_8_8, Endian::k8in32, kSwizzleXYZW, 64, 64, TextureSign::kSigned)) ==
        ResolveMethod::kUnsupported);
  CHECK(method(Color(EdramColorFormat::k_16_16), Dest(TextureFormat::k_16_16, Endian::k8in32, kSwizzleXYZW, 64, 64)) ==
        ResolveMethod::kUnsupported);  // fixed-point bits read unsigned
  CHECK(method(Depth(EdramDepthFormat::kD24S8), Dest(TextureFormat::k_32_FLOAT, Endian::k8in32, kX111, 64, 64)) ==
        ResolveMethod::kUnsupported);
  CHECK(method(Depth(EdramDepthFormat::kD24FS8), Dest(TextureFormat::k_24_8, Endian::k8in32, kX111, 64, 64)) ==
        ResolveMethod::kUnsupported);
  CHECK(method(Color(EdramColorFormat::k_8_8_8_8), Dest(TextureFormat::k_32, Endian::k8in32, kX111, 64, 64)) ==
        ResolveMethod::kUnsupported);
  // The plan for the shadow-map pair needs nothing but a copy; a 16-bit fixed pair needs a scale.
  const ResolveConversion fixed =
      PlanResolveConversion(Color(EdramColorFormat::k_16_16_16_16),
                            MakeTextureFetch(Dest(TextureFormat::k_16_16_16_16, Endian::k8in32, kSwizzleXYZW, 64, 64,
                                                  TextureSign::kSigned)));
  CHECK(fixed.scale == 1.0f / 32.0f);
}

TEST(edram_host_formats) {
  CHECK_EQ(int(EdramColorHostFormat(0)), int(HostFormat::RGBA8_UNORM));
  CHECK_EQ(int(EdramColorHostFormat(2)), int(HostFormat::R10G10B10A2_UNORM));
  CHECK_EQ(int(EdramColorHostFormat(3)), int(HostFormat::RGBA16_FLOAT));
  CHECK_EQ(int(EdramColorHostFormat(4)), int(HostFormat::RG16_FLOAT));
  CHECK_EQ(int(EdramColorHostFormat(14)), int(HostFormat::R32_FLOAT));
  CHECK_EQ(int(EdramColorHostFormat(15)), int(HostFormat::RG32_FLOAT));
  CHECK(IsEdramColorFormat64bpp(5) && IsEdramColorFormat64bpp(7) && IsEdramColorFormat64bpp(15));
  CHECK(!IsEdramColorFormat64bpp(0));
}
