#include "kknr/tiling.h"

#include <algorithm>
#include <cstring>

namespace kknr {

namespace {

uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint32_t DivUp(uint32_t v, uint32_t d) { return (v + d - 1) / d; }

uint32_t Log2Floor(uint32_t v) {
  uint32_t r = 0;
  while (v >>= 1) ++r;
  return r;
}

uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : Log2Floor(v - 1) + 1; }

uint32_t NextPow2(uint32_t v) { return v <= 1 ? 1 : 1u << Log2Ceil(v); }

// Upper bound of the bytes a tiled region touches when blocks [0, right) x [0, bottom) (x [0, back)) are used.
uint32_t TiledExtent2D(uint32_t right, uint32_t bottom, uint32_t pitch, uint32_t bpb_log2) {
  if (!right || !bottom) return 0;
  // Origin of the 32x32 tile holding the last block, plus how far addresses inside one tile reach.
  uint32_t end = uint32_t(TiledOffset2D(int32_t((right - 1) & ~(kTileSize - 1)),
                                        int32_t((bottom - 1) & ~(kTileSize - 1)), pitch, bpb_log2));
  if (bpb_log2 == 0)
    end += 0xA00;
  else if (bpb_log2 == 1)
    end += 0xC00;
  else
    end += 0x400u << bpb_log2;
  return end;
}

uint32_t TiledExtent3D(uint32_t right, uint32_t bottom, uint32_t back, uint32_t pitch, uint32_t height,
                       uint32_t bpb_log2) {
  if (!right || !bottom || !back) return 0;
  uint32_t end = uint32_t(TiledOffset3D(int32_t((right - 1) & ~(kTileSize - 1)),
                                        int32_t((bottom - 1) & ~(kTileSize - 1)),
                                        int32_t((back - 1) & ~(kTileDepth - 1)), pitch, height, bpb_log2));
  const uint32_t pitch_aligned = Align(pitch, kTileSize);
  if (bpb_log2 == 0)
    end += ((pitch_aligned >> 6) << 12) + 0xC00 + ((pitch_aligned & 32) << 5);
  else
    end += ((pitch_aligned << 6) + 0x800) << bpb_log2;
  return end;
}

}  // namespace

int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch_blocks, uint32_t bpb_log2) {
  // Xenos 2D tiling: 32x32-block tiles in row-major order, with the bits of x and y inside a tile mixed
  // (the same function as XGAddress2DTiledOffset; public in UModel and crunch).
  const int32_t pitch = int32_t(Align(pitch_blocks, kTileSize));
  const int32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch_blocks, uint32_t height_blocks,
                      uint32_t bpb_log2) {
  // XGAddress3DTiledOffset (as reconstructed in the SDK from XGRAPHICS::TileVolume).
  const int32_t pitch = int32_t(Align(pitch_blocks, kTileSize));
  const int32_t height = int32_t(Align(height_blocks, kTileSize));
  const int32_t macro_outer = ((y >> 4) + (z >> 2) * (height >> 4)) * (pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_outer) << (bpb_log2 + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bpb_log2 + 6)) >> 6;
  const int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  const int32_t offset2 =
      ((macro + (micro & ~15)) << 1) + (micro & 15) + ((z & 3) << (bpb_log2 + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

uint32_t PackedMipLevel(uint32_t width, uint32_t height) {
  const uint32_t log2_size = Log2Ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

bool PackedMipOffset(uint32_t width, uint32_t height, uint32_t depth, TextureFormat format, uint32_t level,
                     uint32_t& x_blocks, uint32_t& y_blocks, uint32_t& z) {
  // Levels whose shorter side is 16 texels or less share a 32x32 tail: the first three packed levels (16,
  // 8, 4 texels) sit at 16 >> n texels along the shorter axis, the smaller ones at decreasing offsets along
  // the longer axis.
  x_blocks = y_blocks = z = 0;
  const uint32_t log2_w = Log2Ceil(width), log2_h = Log2Ceil(height);
  const uint32_t log2_size = std::min(log2_w, log2_h);
  if (log2_size > 4 + level) return false;
  const uint32_t packed_base = log2_size > 4 ? log2_size - 4 : 0;
  const uint32_t packed = level - packed_base;
  uint32_t x = 0, y = 0;
  if (packed < 3) {
    if (log2_w > log2_h)
      y = 16 >> packed;
    else
      x = 16 >> packed;
  } else {
    uint32_t offset;
    if (log2_w > log2_h) {
      offset = (1u << (log2_w - packed_base)) >> (packed - 2);
      x = offset;
    } else {
      offset = (1u << (log2_h - packed_base)) >> (packed - 2);
      y = offset;
    }
    if (offset < 4) {
      // 1x1 levels of volumes are packed along Z (not reached for 2D).
      const uint32_t log2_d = Log2Ceil(depth);
      z = log2_d > 1 + packed ? (log2_d - packed) * 4 : 4;
    }
  }
  const FormatInfo& info = GetFormatInfo(format);
  x_blocks = x / info.block_width;
  y_blocks = y / info.block_height;
  return true;
}

GuestLayout ComputeGuestLayout(Dimension dimension, uint32_t base_pitch_texels, uint32_t width, uint32_t height,
                               uint32_t depth_or_layers, bool tiled, TextureFormat format, bool packed_mips,
                               bool has_base, uint32_t max_level) {
  GuestLayout layout;
  layout.dimension = dimension;
  layout.format = format;
  layout.tiled = tiled;
  layout.has_base = has_base;
  if (dimension == Dimension::k1D) height = 1;
  layout.width = width;
  layout.height = height;
  layout.depth = dimension == Dimension::k3D ? depth_or_layers : 1;
  layout.layers = dimension == Dimension::k3D ? 1 : (dimension == Dimension::kCube ? 6 : depth_or_layers);
  if (!width || !height || !layout.depth || !layout.layers) return layout;

  max_level = std::min(max_level, Log2Floor(std::max({width, height, layout.depth})));
  max_level = std::min(max_level, kMaxLevels - 1);
  layout.max_level = max_level;
  layout.packed_level = packed_mips ? PackedMipLevel(width, height) : UINT32_MAX;

  const FormatInfo& info = GetFormatInfo(format);
  const uint32_t bpb = info.BytesPerBlock();
  const uint32_t bpb_log2 = Log2Floor(bpb);

  // With the tail at level 0, iteration 0 is the base's tail and 1 the mips' tail; otherwise the iteration
  // is the level.
  const uint32_t last = layout.packed_level == 0 ? uint32_t(max_level != 0)
                                                 : std::min(max_level, layout.packed_level);
  uint32_t mip_offset = 0;
  for (uint32_t it = has_base ? 0 : 1; it <= last; ++it) {
    const bool is_base = it == 0;
    const uint32_t level = layout.packed_level == 0 ? 0 : it;
    LevelStorage& s = is_base ? layout.base : layout.mips[level];

    uint32_t pitch_texels, rows_texels;
    if (is_base) {
      pitch_texels = base_pitch_texels;
      rows_texels = height;
    } else {
      pitch_texels = std::max(NextPow2(width) >> level, 1u);
      rows_texels = std::max(NextPow2(height) >> level, 1u);
    }
    s.pitch_blocks = Align(DivUp(pitch_texels, info.block_width), kTileSize);
    s.row_pitch_bytes = s.pitch_blocks * bpb;
    if (!tiled && !is_base) s.row_pitch_bytes = Align(s.row_pitch_bytes, kLinearRowAlign);
    s.height_blocks = dimension == Dimension::k1D ? 1 : Align(DivUp(rows_texels, info.block_height), kTileSize);
    const uint32_t slice_bytes = s.row_pitch_bytes * s.height_blocks;
    s.layer_stride_bytes = slice_bytes;
    if (dimension == Dimension::k3D) s.layer_stride_bytes *= Align(layout.depth, kTileDepth);
    s.layer_stride_bytes = Align(s.layer_stride_bytes, kSubresourceAlign);

    if (level == layout.packed_level) {
      // The used part of the tail: the union of the packed levels stored in it.
      const uint32_t sub_last = is_base ? 0 : max_level;
      for (uint32_t sub = layout.packed_level; sub <= sub_last; ++sub) {
        uint32_t xb, yb, z;
        PackedMipOffset(width, height, layout.depth, format, sub, xb, yb, z);
        s.extent_blocks_x = std::max(s.extent_blocks_x, xb + DivUp(std::max(width >> sub, 1u), info.block_width));
        s.extent_blocks_y = std::max(s.extent_blocks_y, yb + DivUp(std::max(height >> sub, 1u), info.block_height));
        s.extent_z = std::max(s.extent_z, z + std::max(layout.depth >> sub, 1u));
      }
    } else {
      s.extent_blocks_x = DivUp(std::max(width >> level, 1u), info.block_width);
      s.extent_blocks_y = DivUp(std::max(height >> level, 1u), info.block_height);
      s.extent_z = std::max(layout.depth >> level, 1u);
    }
    uint32_t layer_extent;
    if (tiled) {
      layer_extent = dimension == Dimension::k3D
                         ? TiledExtent3D(s.extent_blocks_x, s.extent_blocks_y, s.extent_z, s.pitch_blocks,
                                         s.extent_blocks_y, bpb_log2)
                         : TiledExtent2D(s.extent_blocks_x, s.extent_blocks_y, s.pitch_blocks, bpb_log2);
    } else {
      layer_extent = slice_bytes * (s.extent_z - 1) + s.row_pitch_bytes * (s.extent_blocks_y - 1) +
                     bpb * s.extent_blocks_x;
    }
    s.data_extent_bytes = s.layer_stride_bytes * (layout.layers - 1) + layer_extent;
    if (is_base) {
      s.offset_bytes = 0;
      layout.base_extent_bytes = s.data_extent_bytes;
    } else {
      s.offset_bytes = mip_offset;
      layout.mips_extent_bytes = std::max(layout.mips_extent_bytes, mip_offset + s.data_extent_bytes);
      mip_offset += s.layer_stride_bytes * layout.layers;
    }
  }
  return layout;
}

GuestLayout ComputeGuestLayout(const TextureFetch& fetch) {
  const TextureLevels levels = GetTextureLevels(fetch);
  return ComputeGuestLayout(fetch.Dim(), fetch.PitchTexels(), levels.width, levels.height, levels.depth_or_layers,
                            fetch.Tiled(), fetch.Format(), fetch.PackedMips(), levels.base_address != 0,
                            levels.max_level);
}

bool GetLevelSource(const GuestLayout& layout, uint32_t level, uint32_t layer, LevelSource& out) {
  out = LevelSource();
  if (level > layout.max_level || layer >= layout.layers) return false;
  const FormatInfo& info = GetFormatInfo(layout.format);
  if (level == 0) {
    if (!layout.has_base) return false;
    out.storage = &layout.base;
  } else {
    out.from_mips = true;
    const uint32_t stored = layout.packed_level == 0 ? 0 : std::min(level, layout.packed_level);
    out.storage = &layout.mips[stored];
  }
  if (level >= layout.packed_level)
    PackedMipOffset(layout.width, layout.height, layout.depth, layout.format, level, out.x_blocks, out.y_blocks,
                    out.z);
  out.layer_offset_bytes = out.storage->offset_bytes + layer * out.storage->layer_stride_bytes;
  out.width_blocks = DivUp(std::max(layout.width >> level, 1u), info.block_width);
  out.height_blocks = DivUp(std::max(layout.height >> level, 1u), info.block_height);
  out.depth = std::max(layout.depth >> level, 1u);
  return true;
}

uint32_t BlockOffset(const GuestLayout& layout, const LevelStorage& s, uint32_t x, uint32_t y, uint32_t z) {
  const uint32_t bpb = GetFormatInfo(layout.format).BytesPerBlock();
  if (layout.tiled) {
    const uint32_t bpb_log2 = Log2Floor(bpb);
    if (layout.dimension == Dimension::k3D)
      return uint32_t(TiledOffset3D(int32_t(x), int32_t(y), int32_t(z), s.pitch_blocks, s.height_blocks, bpb_log2));
    return uint32_t(TiledOffset2D(int32_t(x), int32_t(y), s.pitch_blocks, bpb_log2));
  }
  return (z * s.height_blocks + y) * s.row_pitch_bytes + x * bpb;
}

}  // namespace kknr
