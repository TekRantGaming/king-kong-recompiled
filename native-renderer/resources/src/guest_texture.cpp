#include "kknr/guest_texture.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

#include "kknr/endian.h"

namespace kknr {

namespace {

uint32_t DivUp(uint32_t v, uint32_t d) { return (v + d - 1) / d; }

}  // namespace

bool LoadSwappedRegion(const GuestMemory& memory, uint32_t address, uint32_t bytes, Endian endian,
                       std::vector<uint8_t>& out) {
  // The GPU reads whole 32-bit units: round the region up so its last bytes get their swap partners.
  const uint32_t rounded = (bytes + 3) & ~3u;
  out.assign(rounded, 0);
  if (!bytes) return true;
  const uint64_t available = memory.Available(address);
  if (!available) return false;
  const uint32_t readable = uint32_t(std::min<uint64_t>(rounded, available));
  // Whole swap units only; a partial unit at the very end of guest memory stays zero.
  CopySwap(endian, memory.At(address, 0), out.data(), readable & ~3u);
  return true;
}

BlockExtent GetLevelBlockExtent(const GuestLayout& layout, uint32_t level) {
  const FormatInfo& info = GetFormatInfo(layout.format);
  BlockExtent e;
  e.width_blocks = DivUp(std::max(layout.width >> level, 1u), info.block_width);
  e.height_blocks = DivUp(std::max(layout.height >> level, 1u), info.block_height);
  e.depth = std::max(layout.depth >> level, 1u);
  return e;
}

// Calls fn(byte offset in the region, index of the block in the packed level) for every block of a level.
// The offsets are BlockOffset's, with the format's block size worked out once per level instead of
// once per block (task 085: untiling a chapter's textures held up its first frames).
template <typename Fn>
static bool WalkLevel(const GuestLayout& layout, uint32_t level, uint32_t layer, LevelSource& src, Fn&& fn,
                      uint32_t first_row = 0, uint32_t row_count = UINT32_MAX) {
  if (!GetLevelSource(layout, level, layer, src)) return false;
  const LevelStorage& s = *src.storage;
  const uint32_t bpb = GetFormatInfo(layout.format).BytesPerBlock();
  uint32_t bpb_log2 = 0;
  while ((2u << bpb_log2) <= bpb) ++bpb_log2;  // Log2Floor (bpb >= 1)
  const size_t base = src.layer_offset_bytes;
  size_t index = 0;
  const uint32_t end_row = uint32_t(std::min(uint64_t(first_row) + row_count,
                                           uint64_t(src.height_blocks) * src.depth));
  for (uint32_t flat_row = first_row; flat_row < end_row; ++flat_row) {
    const uint32_t z = flat_row / src.height_blocks, y = flat_row % src.height_blocks;
    const uint32_t gy = y + src.y_blocks, gz = z + src.z;
    if (!layout.tiled) {
      const uint32_t row = (gz * s.height_blocks + gy) * s.row_pitch_bytes;
      for (uint32_t x = 0; x < src.width_blocks; ++x, ++index)
        fn(base + uint32_t(row + (x + src.x_blocks) * bpb), index);
    } else if (layout.dimension == Dimension::k3D) {
      for (uint32_t x = 0; x < src.width_blocks; ++x, ++index)
        fn(base + uint32_t(TiledOffset3D(int32_t(x + src.x_blocks), int32_t(gy), int32_t(gz), s.pitch_blocks,
                                         s.height_blocks, bpb_log2)),
           index);
    } else {
      for (uint32_t x = 0; x < src.width_blocks; ++x, ++index)
        fn(base + uint32_t(TiledOffset2D(int32_t(x + src.x_blocks), int32_t(gy), s.pitch_blocks, bpb_log2)), index);
    }
  }
  return true;
}

bool ReadGuestBlockRows(const GuestLayout& layout, const GuestRegions& regions, uint32_t level, uint32_t layer,
                        uint32_t first_row, uint32_t row_count, uint8_t* out) {
  LevelSource src;
  if (!GetLevelSource(layout, level, layer, src) ||
      uint64_t(first_row) + row_count > uint64_t(src.height_blocks) * src.depth) return false;
  if (!row_count) return true;
  const uint32_t bpb = GetFormatInfo(layout.format).BytesPerBlock();
  const uint8_t* region = src.from_mips ? regions.mips : regions.base;
  const size_t region_size = src.from_mips ? regions.mips_size : regions.base_size;
  std::memset(out, 0, size_t(row_count) * src.width_blocks * bpb);
  auto read = [&](auto block_bytes) {
    constexpr size_t kBytes = decltype(block_bytes)::value;
    return WalkLevel(layout, level, layer, src, [&](size_t offset, size_t index) {
      if (region && offset + kBytes <= region_size) std::memcpy(out + index * kBytes, region + offset, kBytes);
    }, first_row, row_count);
  };
  switch (bpb) {
    case 4: return read(std::integral_constant<size_t, 4>());
    case 8: return read(std::integral_constant<size_t, 8>());
    case 16: return read(std::integral_constant<size_t, 16>());
    default:
      return WalkLevel(layout, level, layer, src, [&](size_t offset, size_t index) {
        if (region && offset + bpb <= region_size) std::memcpy(out + index * bpb, region + offset, bpb);
      }, first_row, row_count);
  }
}

bool ReadGuestBlocks(const GuestLayout& layout, const GuestRegions& regions, uint32_t level, uint32_t layer,
                     std::vector<uint8_t>& out, BlockExtent& extent) {
  const uint32_t bpb = GetFormatInfo(layout.format).BytesPerBlock();
  extent = GetLevelBlockExtent(layout, level);
  LevelSource src;
  if (!GetLevelSource(layout, level, layer, src)) return false;
  const uint8_t* region = src.from_mips ? regions.mips : regions.base;
  const size_t region_size = src.from_mips ? regions.mips_size : regions.base_size;
  out.assign(extent.Count() * bpb, 0);
  // The usual block sizes as constants: a copy of a run-time size was a library call per block.
  uint8_t* dst = out.data();
  auto read = [&](auto block_bytes) {
    constexpr size_t kBytes = decltype(block_bytes)::value;
    return WalkLevel(layout, level, layer, src, [&](size_t offset, size_t index) {
      if (region && offset + kBytes <= region_size) std::memcpy(dst + index * kBytes, region + offset, kBytes);
    });
  };
  switch (bpb) {
    case 4: return read(std::integral_constant<size_t, 4>());
    case 8: return read(std::integral_constant<size_t, 8>());
    case 16: return read(std::integral_constant<size_t, 16>());
    default:
      return WalkLevel(layout, level, layer, src, [&](size_t offset, size_t index) {
        if (region && offset + bpb <= region_size) std::memcpy(dst + index * bpb, region + offset, bpb);
      });
  }
}

bool WriteGuestBlocks(const GuestLayout& layout, uint8_t* base, size_t base_size, uint8_t* mips, size_t mips_size,
                      uint32_t level, uint32_t layer, const uint8_t* blocks) {
  const uint32_t bpb = GetFormatInfo(layout.format).BytesPerBlock();
  LevelSource src;
  if (!GetLevelSource(layout, level, layer, src)) return false;
  uint8_t* region = src.from_mips ? mips : base;
  const size_t region_size = src.from_mips ? mips_size : base_size;
  bool fits = true;
  WalkLevel(layout, level, layer, src, [&](size_t offset, size_t index) {
    if (region && offset + bpb <= region_size)
      std::memcpy(region + offset, blocks + index * bpb, bpb);
    else
      fits = false;
  });
  return fits;
}

bool EncodeGuestTexture(const TextureFetch& fetch,
                        const std::function<const uint8_t*(uint32_t level, uint32_t layer)>& blocks,
                        GuestTextureImage& out, const TextureOptions& options) {
  out = GuestTextureImage();
  const GuestLayout layout = ComputeGuestLayout(fetch, options);
  const TextureLevels levels = GetTextureLevels(fetch, options);
  if (!layout.width) return false;
  // Whole 32-bit units, so the swap covers every byte (see LoadSwappedRegion).
  out.base.assign(levels.base_address ? (layout.base_extent_bytes + 3) & ~3u : 0, 0);
  out.mips.assign(levels.mip_address && levels.max_level ? (layout.mips_extent_bytes + 3) & ~3u : 0, 0);
  bool ok = true;
  for (uint32_t level = levels.min_level; level <= layout.max_level; ++level) {
    if (level == 0 && out.base.empty()) continue;
    if (level > 0 && out.mips.empty() && !layout.mips_in_base_tail) break;
    for (uint32_t layer = 0; layer < layout.layers; ++layer) {
      const uint8_t* data = blocks(level, layer);
      if (!data) continue;
      ok &= WriteGuestBlocks(layout, out.base.data(), out.base.size(), out.mips.data(), out.mips.size(), level,
                             layer, data);
    }
  }
  // The swaps are their own inverse.
  const Endian endian = fetch.EndianMode();
  CopySwap(endian, out.base.data(), out.base.data(), out.base.size());
  CopySwap(endian, out.mips.data(), out.mips.data(), out.mips.size());
  return ok;
}

}  // namespace kknr
