#include "kknr/guest_texture.h"

#include <algorithm>
#include <cstring>

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
  if (!memory.base || address >= memory.size) return false;
  const uint32_t readable = uint32_t(std::min<uint64_t>(rounded, memory.size - address));
  // Whole swap units only; a partial unit at the very end of guest memory stays zero.
  CopySwap(endian, memory.base + address, out.data(), readable & ~3u);
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
template <typename Fn>
static bool WalkLevel(const GuestLayout& layout, uint32_t level, uint32_t layer, LevelSource& src, Fn&& fn) {
  if (!GetLevelSource(layout, level, layer, src)) return false;
  size_t index = 0;
  for (uint32_t z = 0; z < src.depth; ++z)
    for (uint32_t y = 0; y < src.height_blocks; ++y)
      for (uint32_t x = 0; x < src.width_blocks; ++x, ++index)
        fn(size_t(src.layer_offset_bytes) +
               BlockOffset(layout, *src.storage, x + src.x_blocks, y + src.y_blocks, z + src.z),
           index);
  return true;
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
  return WalkLevel(layout, level, layer, src, [&](size_t offset, size_t index) {
    if (region && offset + bpb <= region_size) std::memcpy(out.data() + index * bpb, region + offset, bpb);
  });
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
                        GuestTextureImage& out) {
  out = GuestTextureImage();
  const GuestLayout layout = ComputeGuestLayout(fetch);
  const TextureLevels levels = GetTextureLevels(fetch);
  if (!layout.width) return false;
  // Whole 32-bit units, so the swap covers every byte (see LoadSwappedRegion).
  out.base.assign(levels.base_address ? (layout.base_extent_bytes + 3) & ~3u : 0, 0);
  out.mips.assign(levels.mip_address && levels.max_level ? (layout.mips_extent_bytes + 3) & ~3u : 0, 0);
  bool ok = true;
  for (uint32_t level = levels.min_level; level <= layout.max_level; ++level) {
    if (level == 0 && out.base.empty()) continue;
    if (level > 0 && out.mips.empty()) break;
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
