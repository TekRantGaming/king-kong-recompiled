#include <set>
#include <vector>

#include "kknr/tiling.h"
#include "test.h"

using namespace kknr;

namespace {

// An independent formulation of the 2D tiled address (row part and column part computed separately, as in
// crunch's crn_decomp.h and the SDK's legacy CPU untiler), to cross-check TiledOffset2D.
uint32_t RowPart(uint32_t y, uint32_t width, uint32_t log2_bpp) {
  const uint32_t macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  const uint32_t micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4);
}
uint32_t ColumnPart(uint32_t x, uint32_t y, uint32_t log2_bpp, uint32_t base) {
  const uint32_t macro = (x / 32) << (log2_bpp + 7);
  const uint32_t micro = (x & 7) << log2_bpp;
  const uint32_t offset = base + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

}  // namespace

TEST(tiled2d_matches_row_column_formulation) {
  for (uint32_t log2 = 0; log2 <= 4; ++log2) {
    const uint32_t pitch = 96;  // not a power of two on purpose
    for (uint32_t y = 0; y < 96; ++y)
      for (uint32_t x = 0; x < pitch; ++x) {
        const uint32_t a = uint32_t(TiledOffset2D(int32_t(x), int32_t(y), pitch, log2));
        const uint32_t b = ColumnPart(x, y, log2, RowPart(y, pitch, log2));
        if (a != b) {
          CHECK_EQ(a, b);
          return;
        }
      }
  }
}

TEST(tiled2d_is_a_permutation_of_whole_tiles) {
  for (uint32_t log2 = 0; log2 <= 4; ++log2) {
    // 128x128 blocks is a whole number of address periods for every block size.
    const uint32_t size = 128, bpb = 1u << log2;
    std::set<uint32_t> seen;
    bool aligned = true, in_range = true;
    for (uint32_t y = 0; y < size; ++y)
      for (uint32_t x = 0; x < size; ++x) {
        const uint32_t o = uint32_t(TiledOffset2D(int32_t(x), int32_t(y), size, log2));
        aligned &= (o % bpb) == 0;
        in_range &= o < size * size * bpb;
        seen.insert(o);
      }
    CHECK(aligned);
    CHECK(in_range);
    CHECK_EQ(seen.size(), size_t(size) * size);
  }
}

TEST(tiled2d_known_points) {
  // 16 bytes along X are contiguous; tile (32, 0) starts after a whole 32x32 tile for 4+ bytes per block.
  CHECK_EQ(TiledOffset2D(0, 0, 64, 2), 0);
  CHECK_EQ(TiledOffset2D(1, 0, 64, 2), 4);
  CHECK_EQ(TiledOffset2D(3, 0, 64, 2), 12);
  CHECK_EQ(TiledOffset2D(32, 0, 64, 2), 32 * 32 * 4);
  CHECK_EQ(TiledOffset2D(0, 32, 64, 2), 2 * 32 * 32 * 4);
  CHECK_EQ(TiledOffset2D(0, 0, 64, 3), 0);
  CHECK_EQ(TiledOffset2D(1, 0, 64, 3), 8);
}

TEST(tiled3d_is_a_permutation_of_whole_tiles) {
  for (uint32_t log2 = 0; log2 <= 4; ++log2) {
    const uint32_t w = 64, h = 32, d = 8, bpb = 1u << log2;
    std::set<uint32_t> seen;
    bool aligned = true;
    uint32_t max = 0;
    for (uint32_t z = 0; z < d; ++z)
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const uint32_t o = uint32_t(TiledOffset3D(int32_t(x), int32_t(y), int32_t(z), w, h, log2));
          aligned &= (o % bpb) == 0;
          max = std::max(max, o);
          seen.insert(o);
        }
    CHECK(aligned);
    CHECK_EQ(seen.size(), size_t(w) * h * d);
    CHECK(max < w * h * d * bpb);
  }
}

TEST(packed_mip_offsets_square) {
  // 32x32, one texel per block: the tail starts at level 1; 16x16 at x=16, 8x8 at x=8, 4x4 at x=4,
  // then 2x2 at y=8 and 1x1 at y=4 (the documented tail picture).
  CHECK_EQ(PackedMipLevel(32, 32), 1u);
  uint32_t x, y, z;
  CHECK(!PackedMipOffset(32, 32, 1, TextureFormat::k_8_8_8_8, 0, x, y, z));
  const uint32_t expect[6][2] = {{0, 0}, {16, 0}, {8, 0}, {4, 0}, {0, 8}, {0, 4}};
  for (uint32_t level = 1; level <= 5; ++level) {
    CHECK(PackedMipOffset(32, 32, 1, TextureFormat::k_8_8_8_8, level, x, y, z));
    CHECK_EQ(x, expect[level][0]);
    CHECK_EQ(y, expect[level][1]);
    CHECK_EQ(z, 0u);
  }
}

TEST(packed_mip_offsets_wide_and_blocks) {
  uint32_t x, y, z;
  // 128x32 (wider): the first packed levels go down the Y axis, the small ones along X.
  CHECK_EQ(PackedMipLevel(128, 32), 1u);
  CHECK(PackedMipOffset(128, 32, 1, TextureFormat::k_8_8_8_8, 1, x, y, z));
  CHECK_EQ(x, 0u);
  CHECK_EQ(y, 16u);
  CHECK(PackedMipOffset(128, 32, 1, TextureFormat::k_8_8_8_8, 4, x, y, z));
  CHECK_EQ(x, 32u);  // (1 << (7 - 1)) >> 1
  CHECK_EQ(y, 0u);
  // DXT: offsets are in 4x4 blocks.
  CHECK(PackedMipOffset(64, 64, 1, TextureFormat::k_DXT1, 2, x, y, z));
  CHECK_EQ(x, 4u);  // 16 texels
  CHECK_EQ(y, 0u);
}

TEST(layout_tiled_rgba8_with_mips) {
  // 256x256 k_8_8_8_8 tiled, all levels, packed tail from level 4 (16x16).
  const GuestLayout l = ComputeGuestLayout(Dimension::k2D, 256, 256, 256, 1, true, TextureFormat::k_8_8_8_8, true,
                                           true, 8);
  CHECK_EQ(l.packed_level, 4u);
  CHECK_EQ(l.max_level, 8u);
  CHECK_EQ(l.base.row_pitch_bytes, 1024u);
  CHECK_EQ(l.base.layer_stride_bytes, 256u * 1024);
  CHECK_EQ(l.mips[1].offset_bytes, 0u);
  CHECK_EQ(l.mips[2].offset_bytes, 128u * 128 * 4);
  CHECK_EQ(l.mips[3].offset_bytes, 128u * 128 * 4 + 64 * 64 * 4);
  CHECK_EQ(l.mips[4].offset_bytes, 128u * 128 * 4 + 64 * 64 * 4 + 32 * 32 * 4);
  CHECK_EQ(l.mips[4].pitch_blocks, 32u);  // 16 texels padded to a tile
  LevelSource s;
  CHECK(GetLevelSource(l, 6, 0, s));  // 4x4 inside the tail
  CHECK(s.storage == &l.mips[4]);
  CHECK_EQ(s.x_blocks, 4u);
  CHECK_EQ(s.width_blocks, 4u);
}

TEST(layout_linear_rows) {
  const GuestLayout l = ComputeGuestLayout(Dimension::k2D, 512, 512, 32, 1, false, TextureFormat::k_8_8_8_8, false,
                                           true, 0);
  CHECK_EQ(l.base.row_pitch_bytes, 2048u);
  CHECK_EQ(BlockOffset(l, l.base, 3, 2, 0), 2u * 2048 + 12);
  CHECK_EQ(l.base_extent_bytes, 2048u * 32);  // exactly the image, no padding rows read
  // Linear mips: rows 256-byte aligned.
  const GuestLayout m = ComputeGuestLayout(Dimension::k2D, 64, 64, 64, 1, false, TextureFormat::k_8, false, true, 2);
  CHECK_EQ(m.mips[1].row_pitch_bytes, 256u);
}

TEST(layout_cube_layers) {
  // A linear DXT1 cube, 128x128 with packed mips (as the game's environment map).
  const GuestLayout l = ComputeGuestLayout(Dimension::kCube, 128, 128, 128, 6, false, TextureFormat::k_DXT1, true,
                                           true, 7);
  CHECK_EQ(l.layers, 6u);
  CHECK_EQ(l.base.row_pitch_bytes, 32u * 8);       // 32 blocks (tile aligned) x 8 bytes
  CHECK_EQ(l.base.layer_stride_bytes, 256u * 32);  // 32 block rows, 4 KB aligned
  LevelSource s;
  CHECK(GetLevelSource(l, 0, 5, s));
  CHECK_EQ(s.layer_offset_bytes, 5u * 256 * 32);
}

TEST(fetch_constant_fields) {
  // A census fetch constant: DXT4_5 tiled 2D 1024x1024 with packed mips, levels 0-10.
  const uint32_t w[6] = {0x88000002, 0xFAB06054, 0x007FE3FF, 0x00000D10, 0x00000280, 0xFAC06A00};
  const TextureFetch f = TextureFetch::FromWords(w);
  CHECK_EQ(f.Type(), 2u);
  CHECK(f.Tiled());
  CHECK_EQ(uint32_t(f.Format()), uint32_t(TextureFormat::k_DXT4_5));
  CHECK_EQ(uint32_t(f.EndianMode()), uint32_t(Endian::k8in16));
  CHECK_EQ(f.Width(), 1024u);
  CHECK_EQ(f.Height(), 1024u);
  CHECK_EQ(f.PitchTexels(), 1024u);
  CHECK_EQ(f.BaseAddress(), 0x1AB06000u);
  CHECK_EQ(f.MipAddress(), 0x1AC06000u);
  CHECK(f.PackedMips());
  CHECK_EQ(uint32_t(f.Dim()), uint32_t(Dimension::k2D));
  CHECK_EQ(f.MipMaxLevel(), 10u);
  CHECK_EQ(f.Swizzle(), kSwizzleXYZW);
  const TextureLevels lv = GetTextureLevels(f);
  CHECK_EQ(lv.max_level, 10u);
}

TEST(d3dformat_words) {
  const D3DFormat argb{0x18280186};
  CHECK_EQ(uint32_t(argb.Format()), uint32_t(TextureFormat::k_8_8_8_8));
  CHECK_EQ(uint32_t(argb.EndianMode()), uint32_t(Endian::k8in32));
  CHECK(argb.Tiled());
  CHECK_EQ(argb.Swizzle(), MakeSwizzle(kSwzZ, kSwzY, kSwzX, kSwzW));
  const D3DFormat xrgb{0x28280186};
  CHECK_EQ(xrgb.Swizzle(), MakeSwizzle(kSwzZ, kSwzY, kSwzX, kSwz1));
  const D3DFormat r32f{0x2DA2ABA4};
  CHECK_EQ(uint32_t(r32f.Format()), uint32_t(TextureFormat::k_32_FLOAT));
  CHECK_EQ(r32f.Swizzle(), MakeSwizzle(kSwzX, kSwz1, kSwz1, kSwz1));
  const D3DFormat d24s8{0x2D200196};
  CHECK_EQ(uint32_t(d24s8.Format()), uint32_t(TextureFormat::k_24_8));
}
