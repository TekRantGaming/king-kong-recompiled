// Raw guest texture blocks <-> guest memory, in both directions: reading one level / layer out of the tiled or
// linear storage (untiling) and writing it back (tiling). The two share one address walk (GetLevelSource +
// BlockOffset), so a round trip through them checks the layout code against itself and, with the
// permutation tests in test_tiling.cpp, the address functions against the documented ones.
//
// "Blocks" are the guest format's blocks (one texel for uncompressed formats, 4x4 texels for DXT, 2x1 for the
// 4:2:2 formats) in host byte order, that is after the fetch constant's endian swap. Converting blocks to
// host texels is texture_convert.h's job.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "kknr/tiling.h"
#include "kknr/xenos.h"

namespace kknr {

// Physical guest memory: base points at physical address `origin` (in hooks: base + 0xA0000000 with origin 0;
// tools that load a dump of just a texture's pages set origin to the first dumped address).
struct GuestMemory {
  const uint8_t* base = nullptr;
  uint64_t size = 0x20000000;
  uint32_t origin = 0;
  const uint8_t* At(uint32_t physical, uint32_t length) const {
    if (!base || physical < origin || uint64_t(physical - origin) + length > size) return nullptr;
    return base + (physical - origin);
  }
  // Bytes readable from physical (0 if outside).
  uint64_t Available(uint32_t physical) const {
    if (!base || physical < origin || physical - origin >= size) return 0;
    return size - (physical - origin);
  }
};

// A copy of [address, address + bytes) of guest memory with the endian swap applied (bytes past the end of
// guest memory read as zero). Regions start 4 KB aligned, so the swap's 16- and 32-bit units line up with
// memory as the GPU reads it. False if the region starts outside guest memory.
bool LoadSwappedRegion(const GuestMemory& memory, uint32_t address, uint32_t bytes, Endian endian,
                       std::vector<uint8_t>& out);

// The two storage regions of a texture in host byte order: base (level 0) and mips (levels 1+).
struct GuestRegions {
  const uint8_t* base = nullptr;
  size_t base_size = 0;
  const uint8_t* mips = nullptr;
  size_t mips_size = 0;
};

// Size of one level / layer in guest blocks.
struct BlockExtent {
  uint32_t width_blocks = 0, height_blocks = 0, depth = 0;
  size_t Count() const { return size_t(width_blocks) * height_blocks * depth; }
};

// Reads the blocks of one level / layer, tightly packed (rows of width_blocks, then slices). Blocks outside
// the regions read as zero. False if the level / layer is not stored (no base, level above max_level ...).
bool ReadGuestBlocks(const GuestLayout& layout, const GuestRegions& regions, uint32_t level, uint32_t layer,
                     std::vector<uint8_t>& out, BlockExtent& extent);

// Writes tightly packed blocks of one level / layer into regions (host byte order) sized by the layout.
bool WriteGuestBlocks(const GuestLayout& layout, uint8_t* base, size_t base_size, uint8_t* mips, size_t mips_size,
                      uint32_t level, uint32_t layer, const uint8_t* blocks);

BlockExtent GetLevelBlockExtent(const GuestLayout& layout, uint32_t level);

// A whole texture as it would sit in guest memory: the base and mip regions (guest byte order, endian swap
// applied), each to be placed at the fetch constant's base / mip address.
struct GuestTextureImage {
  std::vector<uint8_t> base, mips;
};
// Lays out every stored level / layer of the fetch constant from blocks(level, layer), which returns tightly
// packed host-order blocks (GetLevelBlockExtent sizes) or nullptr to leave that subresource zero. The inverse
// of LoadSwappedRegion + ReadGuestBlocks; tests and replacement textures use it.
bool EncodeGuestTexture(const TextureFetch& fetch,
                        const std::function<const uint8_t*(uint32_t level, uint32_t layer)>& blocks,
                        GuestTextureImage& out, const TextureOptions& options = TextureOptions());

}  // namespace kknr
