// Tiling round trips: random guest blocks for every level / layer are laid out into guest memory
// (EncodeGuestTexture: tiling and the endian swap) and read back (LoadSwappedRegion + ReadGuestBlocks: the
// path ConvertTexture uses). Also checks that no two blocks of a texture share bytes and that every block
// lies inside the extent the cache watches.
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "kknr/guest_texture.h"
#include "kknr/texture_convert.h"
#include "kknr/tiling.h"
#include "test.h"

using namespace kknr;
using kknr_test::FakeGuest;
using kknr_test::Rng;

namespace {

constexpr uint32_t kBase = 0x00100000, kMips = 0x01000000;

struct Shape {
  Dimension dim;
  uint32_t w, h, d;
  bool stacked;
};

std::string Describe(const TextureFetchDesc& d) {
  return std::string(GetFormatInfo(d.format).name) + " " + DimensionName(d.dimension) + " " + std::to_string(d.width) +
         "x" + std::to_string(d.height) + "x" + std::to_string(d.depth_or_layers) + (d.tiled ? " tiled" : " linear") +
         (d.packed_mips ? " packed" : "") + " levels " + std::to_string(d.min_level) + "-" +
         std::to_string(d.max_level) + " endian " + EndianName(d.endian) + (d.base_address ? "" : " no-base");
}

// Runs one round trip; returns false (and reports) on the first mismatch.
bool RoundTrip(const TextureFetchDesc& desc, uint64_t seed) {
  const TextureFetch fetch = MakeTextureFetch(desc);
  const GuestLayout layout = ComputeGuestLayout(fetch);
  const TextureLevels levels = GetTextureLevels(fetch);
  const uint32_t bpb = GetFormatInfo(desc.format).BytesPerBlock();
  Rng rng(seed);

  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> data;
  for (uint32_t level = levels.min_level; level <= layout.max_level; ++level) {
    if (level == 0 && !levels.base_address) continue;
    for (uint32_t layer = 0; layer < layout.layers; ++layer) {
      std::vector<uint8_t>& v = data[{level, layer}];
      v.resize(GetLevelBlockExtent(layout, level).Count() * bpb);
      rng.Fill(v.data(), v.size());
    }
  }
  GuestTextureImage image;
  const bool encoded = EncodeGuestTexture(
      fetch,
      [&](uint32_t level, uint32_t layer) -> const uint8_t* {
        auto it = data.find({level, layer});
        return it == data.end() ? nullptr : it->second.data();
      },
      image);
  if (!encoded) {
    kknr_test::Fail(__FILE__, __LINE__, "a block falls outside the computed extent: " + Describe(desc));
    return false;
  }

  // No block shares bytes with another one (the packed tail included).
  for (int region = 0; region < 2; ++region) {
    std::vector<uint32_t> owner(region ? image.mips.size() : image.base.size(), 0);
    uint32_t id = 0;
    for (const auto& [key, blocks] : data) {
      ++id;
      LevelSource src;
      if (!GetLevelSource(layout, key.first, key.second, src) || src.from_mips != (region == 1)) continue;
      for (uint32_t z = 0; z < src.depth; ++z)
        for (uint32_t y = 0; y < src.height_blocks; ++y)
          for (uint32_t x = 0; x < src.width_blocks; ++x) {
            const size_t o = size_t(src.layer_offset_bytes) +
                             BlockOffset(layout, *src.storage, x + src.x_blocks, y + src.y_blocks, z + src.z);
            for (uint32_t b = 0; b < bpb; ++b) {
              if (owner[o + b] != 0) {
                kknr_test::Fail(__FILE__, __LINE__,
                                "level " + std::to_string(key.first) + " layer " + std::to_string(key.second) +
                                    " overlaps another block: " + Describe(desc));
                return false;
              }
              owner[o + b] = id;
            }
          }
    }
  }

  static FakeGuest guest;  // shared: 64 MB is too much to clear per case
  static size_t last_base = kMips - kBase, last_mips = (64u << 20) - kMips;  // clear what the last case wrote
  std::fill_n(guest.bytes.begin() + kBase, last_base, 0);
  std::fill_n(guest.bytes.begin() + kMips, last_mips, 0);
  last_base = image.base.size() + 4096;
  last_mips = image.mips.size() + 4096;
  guest.Put(kBase, image.base);
  guest.Put(kMips, image.mips);
  GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
  const TextureRanges ranges = GetTextureRanges(fetch);
  std::vector<uint8_t> base, mips;
  CHECK(LoadSwappedRegion(memory, ranges.base, ranges.base_bytes, fetch.EndianMode(), base));
  CHECK(LoadSwappedRegion(memory, ranges.mip, ranges.mip_bytes, fetch.EndianMode(), mips));
  const GuestRegions regions{base.data(), base.size(), mips.data(), mips.size()};
  for (const auto& [key, blocks] : data) {
    std::vector<uint8_t> back;
    BlockExtent extent;
    if (!ReadGuestBlocks(layout, regions, key.first, key.second, back, extent) || back != blocks) {
      kknr_test::Fail(__FILE__, __LINE__,
                      "level " + std::to_string(key.first) + " layer " + std::to_string(key.second) +
                          " differs after the round trip: " + Describe(desc));
      return false;
    }
  }
  return true;
}

const TextureFormat kFormats[] = {
    TextureFormat::k_8,           TextureFormat::k_8_8,          TextureFormat::k_8_8_8_8,
    TextureFormat::k_16_16_16_16, TextureFormat::k_32_32_32_32_FLOAT, TextureFormat::k_DXT1,
    TextureFormat::k_DXT4_5,      TextureFormat::k_Y1_Cr_Y0_Cb_REP,   TextureFormat::k_32_AS_8,
    TextureFormat::k_1,           TextureFormat::k_32_32_32_FLOAT,
};

const Shape kShapes[] = {
    {Dimension::k2D, 256, 256, 1, false}, {Dimension::k2D, 100, 60, 1, false},  {Dimension::k2D, 1024, 16, 1, false},
    {Dimension::k2D, 16, 128, 1, false},  {Dimension::k2D, 1, 1, 1, false},     {Dimension::k2D, 3, 5, 1, false},
    {Dimension::k2D, 64, 32, 3, true},    {Dimension::kCube, 64, 64, 6, false}, {Dimension::kCube, 8, 8, 6, false},
    {Dimension::k3D, 32, 32, 8, false},   {Dimension::k3D, 17, 9, 5, false},    {Dimension::k3D, 4, 4, 16, false},
    {Dimension::k1D, 300, 1, 1, false},
};

uint32_t MaxLevel(const Shape& s) {
  uint32_t longest = std::max(s.w, s.h);
  if (s.dim == Dimension::k3D) longest = std::max(longest, s.d);
  uint32_t l = 0;
  while (longest >>= 1) ++l;
  return l;
}

}  // namespace

TEST(roundtrip_every_shape_format_tiling_and_packing) {
  uint64_t seed = 1;
  int cases = 0, failed = 0;
  for (TextureFormat format : kFormats) {
    for (const Shape& shape : kShapes) {
      for (int tiled = 0; tiled < 2; ++tiled) {
        if (tiled && (shape.dim == Dimension::k1D || format == TextureFormat::k_32_32_32_FLOAT)) continue;
        for (int packed = 0; packed < 2; ++packed) {
          if (packed && shape.dim == Dimension::k1D) continue;
          // Packed tails the layout cannot hold without overlap (see packed_tail_known_overlaps): 1 bpp
          // (8-texel blocks are wider than the tail's small offsets) and volumes thinner than a 4x4 block.
          if (packed && format == TextureFormat::k_1) continue;
          if (packed && shape.dim == Dimension::k3D && shape.w <= 4 &&
              GetFormatInfo(format).kind == FormatKind::kBlock4x4)
            continue;
          TextureFetchDesc d;
          d.format = format;
          d.dimension = shape.dim;
          d.width = shape.w;
          d.height = shape.h;
          d.depth_or_layers = shape.d;
          d.stacked = shape.stacked;
          d.tiled = tiled;
          d.packed_mips = packed;
          d.base_address = kBase;
          d.mip_address = kMips;
          d.max_level = MaxLevel(shape);
          // Exercise every swap; 8in16 needs 16-bit units, the others 32-bit (all blocks here are >= 1 byte
          // and the regions are 4 KB aligned, so any mode is a valid layout).
          d.endian = Endian(seed % 4);
          ++cases;
          if (!RoundTrip(d, seed++)) ++failed;
        }
      }
    }
  }
  CHECK_EQ(failed, 0);
  CHECK(cases > 400);
}

TEST(roundtrip_mips_without_base_and_min_level) {
  // Base address 0: only the mips are stored (min level forced to 1).
  TextureFetchDesc d;
  d.format = TextureFormat::k_DXT1;
  d.width = d.height = 128;
  d.tiled = true;
  d.packed_mips = true;
  d.mip_address = kMips;
  d.max_level = 7;
  d.endian = Endian::k8in16;
  CHECK(RoundTrip(d, 77));
  const TextureLevels l = GetTextureLevels(MakeTextureFetch(d));
  CHECK_EQ(l.min_level, 1u);
  // A min level above 0 drops the base even when its address is set.
  d.base_address = kBase;
  d.min_level = 2;
  CHECK(RoundTrip(d, 78));
  CHECK_EQ(GetTextureLevels(MakeTextureFetch(d)).base_address, 0u);
  // Mip address set but max level 0: no mips.
  d.min_level = 0;
  d.max_level = 0;
  CHECK_EQ(GetTextureLevels(MakeTextureFetch(d)).mip_address, 0u);
  CHECK(RoundTrip(d, 79));
}

TEST(roundtrip_non_default_pitch) {
  // A linear k_DXT4_5 1280-wide texture with a 1408-texel pitch (the SDK's example of pitch > width).
  TextureFetchDesc d;
  d.format = TextureFormat::k_DXT4_5;
  d.width = 1280;
  d.height = 64;
  d.pitch_texels = 1408;
  d.base_address = kBase;
  d.endian = Endian::k8in16;
  CHECK(RoundTrip(d, 5));
  const GuestLayout l = ComputeGuestLayout(MakeTextureFetch(d));
  CHECK_EQ(l.base.row_pitch_bytes, 1408u / 4 * 16);
}

TEST(untile_matches_independent_formula_8888) {
  // End to end against an independent writer: a 64x48 tiled A8R8G8B8 texture written the way the port's
  // launcher_art.cpp reads one (big-endian ARGB words at the crunch-style tiled offset), converted, and compared
  // texel by texel with the expected RGBA8 host data (host R = guest X = the word's low byte = blue).
  const uint32_t w = 64, h = 48;
  FakeGuest guest;
  auto tiled_offset = [](uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2_bpp) {
    const uint32_t macro = ((x >> 5) + (y >> 5) * (((pitch + 31) & ~31u) >> 5)) << (log2_bpp + 7);
    const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpp;
    const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
  };
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* p = guest.bytes.data() + kBase + tiled_offset(x, y, w, 2);
      p[0] = uint8_t(255 - x);  // A
      p[1] = uint8_t(x);        // R
      p[2] = uint8_t(y);        // G
      p[3] = uint8_t(x ^ y);    // B
    }
  TextureFetchDesc d;
  d.format = TextureFormat::k_8_8_8_8;
  d.endian = Endian::k8in32;
  d.tiled = true;
  d.width = w;
  d.height = h;
  d.base_address = kBase;
  d.swizzle = MakeSwizzle(kSwzZ, kSwzY, kSwzX, kSwzW);  // A8R8G8B8
  HostTextureData out;
  std::string why;
  CHECK(ConvertTexture(MakeTextureFetch(d), GuestMemory{guest.bytes.data(), guest.bytes.size()}, out, &why));
  CHECK_EQ(int(out.plan.format), int(HostFormat::RGBA8_UNORM));
  CHECK_EQ(out.plan.view_swizzle, MakeSwizzle(kSwzZ, kSwzY, kSwzX, kSwzW));
  const HostSubresource* s = out.Find(0, 0);
  CHECK(s != nullptr);
  if (!s) return;
  bool same = true;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* t = out.bytes.data() + s->offset + y * s->row_pitch + x * 4;
      same &= t[0] == uint8_t(x ^ y) && t[1] == uint8_t(y) && t[2] == uint8_t(x) && t[3] == uint8_t(255 - x);
    }
  CHECK(same);
}

TEST(layout_extents_cover_reads) {
  // GetTextureRanges must cover every byte ReadGuestBlocks touches, for the cache's write watches.
  for (const Shape& shape : kShapes) {
    for (int tiled = 0; tiled < 2; ++tiled) {
      if (tiled && shape.dim == Dimension::k1D) continue;
      TextureFetchDesc d;
      d.format = TextureFormat::k_DXT1;
      d.dimension = shape.dim;
      d.width = shape.w;
      d.height = shape.h;
      d.depth_or_layers = shape.d;
      d.stacked = shape.stacked;
      d.tiled = tiled;
      d.packed_mips = shape.dim != Dimension::k1D;
      d.base_address = kBase;
      d.mip_address = kMips;
      d.max_level = MaxLevel(shape);
      const TextureFetch f = MakeTextureFetch(d);
      const GuestLayout layout = ComputeGuestLayout(f);
      const TextureRanges r = GetTextureRanges(f);
      for (uint32_t level = 0; level <= layout.max_level; ++level)
        for (uint32_t layer = 0; layer < layout.layers; ++layer) {
          LevelSource src;
          if (!GetLevelSource(layout, level, layer, src)) continue;
          const uint32_t limit = src.from_mips ? r.mip_bytes : r.base_bytes;
          const uint32_t last = src.layer_offset_bytes +
                                BlockOffset(layout, *src.storage, src.x_blocks + src.width_blocks - 1,
                                            src.y_blocks + src.height_blocks - 1, src.z + src.depth - 1);
          if (last + 8 > limit) {
            kknr_test::Fail(__FILE__, __LINE__, "range too small: " + Describe(d));
            return;
          }
        }
    }
  }
}

TEST(packed_tail_known_overlaps) {
  // The SDK's tail formula (which we follow) puts the 1x1x2 and 1x1x1 levels of a 4x4x16 volume both at z = 4,
  // and with 4x4 blocks their x / y offsets round to 0: they share a block. Kept as the SDK does it (no
  // hardware reference here); docs/formats.md lists it.
  uint32_t x3, y3, z3, x4, y4, z4;
  CHECK(PackedMipOffset(4, 4, 16, TextureFormat::k_DXT1, 3, x3, y3, z3));
  CHECK(PackedMipOffset(4, 4, 16, TextureFormat::k_DXT1, 4, x4, y4, z4));
  CHECK_EQ(z3, 4u);
  CHECK_EQ(z4, 4u);
  CHECK_EQ(x3 + y3 + x4 + y4, 0u);
}
