// Where a Xenos texture's texels are in guest memory: the tiled address functions, the packed mip tail and
// the per-level layout (strides, offsets, extents). Behaviour follows the SDK's texture_util (Xenia, BSD:
// GetTiledOffset2D / 3D, GetPackedMipOffset, GetGuestTextureLayout); the code here is our own.
//
// Summary of the storage rules (details in docs/formats.md):
// - Level 0 is at the base address with the fetch constant's row pitch; levels 1+ are at the mip address,
//   one after another, each with pitch and height max(next_pow2(size) >> level, 1).
// - Rows and slices are padded to 32x32 blocks (32x32x4 for 3D); every array layer / level is 4 KB aligned.
//   Linear rows are 256-byte aligned.
// - With packed mips, levels whose shorter side is 16 texels or less share one "tail" stored like the level
//   where packing starts, each at a fixed block offset inside it.
#pragma once

#include <cstdint>

#include "kknr/xenos.h"

namespace kknr {

constexpr uint32_t kTileSize = 32;           // blocks per tile side
constexpr uint32_t kTileDepth = 4;           // 3D tiles are 32x32x4
constexpr uint32_t kSubresourceAlign = 4096; // array layers and levels
constexpr uint32_t kLinearRowAlign = 256;    // bytes per linear row of blocks
constexpr uint32_t kMaxLevels = 14;

// Byte offset of block (x, y) in a tiled 2D surface whose row pitch is pitch_blocks (aligned to 32 inside).
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch_blocks, uint32_t bytes_per_block_log2);
// Byte offset of block (x, y, z) in a tiled 3D surface (pitch and height in blocks, aligned to 32 inside).
int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch_blocks, uint32_t height_blocks,
                      uint32_t bytes_per_block_log2);

// First level stored in the packed tail for a width x height texture (when the fetch constant has packed mips).
uint32_t PackedMipLevel(uint32_t width, uint32_t height);
// Block offset of a level inside the packed tail; false (and zeros) if that level is not packed.
bool PackedMipOffset(uint32_t width, uint32_t height, uint32_t depth, TextureFormat format, uint32_t level,
                     uint32_t& x_blocks, uint32_t& y_blocks, uint32_t& z);

struct LevelStorage {
  uint32_t offset_bytes = 0;          // from the base address (level 0) or the mip address (levels 1+)
  uint32_t row_pitch_bytes = 0;       // between rows of blocks
  uint32_t pitch_blocks = 0;          // the tiled functions' pitch (row pitch / bytes per block)
  uint32_t height_blocks = 0;         // block rows per depth slice (aligned to 32)
  uint32_t layer_stride_bytes = 0;    // between array layers (4 KB aligned; whole volume for 3D)
  uint32_t extent_blocks_x = 0;       // blocks actually used (the whole tail's extent for a packed tail)
  uint32_t extent_blocks_y = 0;
  uint32_t extent_z = 0;
  uint32_t data_extent_bytes = 0;     // bytes from offset_bytes the GPU may read, all layers included
};

struct GuestLayout {
  Dimension dimension = Dimension::k2D;
  TextureFormat format = TextureFormat::k_8_8_8_8;
  bool tiled = false;
  uint32_t width = 0, height = 0, depth = 1, layers = 1;
  uint32_t max_level = 0;
  uint32_t packed_level = UINT32_MAX;  // UINT32_MAX: no packed tail
  bool has_base = false;
  bool mips_in_base_tail = false;      // levels 1+ inside the base's tail (TextureOptions::mips_from_base_tail)
  LevelStorage base;                   // level 0 (or the base's packed tail if packed_level == 0)
  LevelStorage mips[kMaxLevels];       // levels 1..min(max_level, packed_level); [0] = the mips' tail when
                                       // packed_level == 0
  uint32_t base_extent_bytes = 0;
  uint32_t mips_extent_bytes = 0;
};

GuestLayout ComputeGuestLayout(Dimension dimension, uint32_t base_pitch_texels, uint32_t width, uint32_t height,
                               uint32_t depth_or_layers, bool tiled, TextureFormat format, bool packed_mips,
                               bool has_base, uint32_t max_level, bool mips_in_base_tail = false);
GuestLayout ComputeGuestLayout(const TextureFetch& fetch, const TextureOptions& options = TextureOptions());

// Where one host level of one layer comes from: the storage (base or a mip, possibly a tail) and the block /
// slice offset inside it.
struct LevelSource {
  bool from_mips = false;
  const LevelStorage* storage = nullptr;
  uint32_t layer_offset_bytes = 0;  // storage offset + layer * stride
  uint32_t x_blocks = 0, y_blocks = 0, z = 0;
  uint32_t width_blocks = 0, height_blocks = 0, depth = 0;  // this level's size
};
bool GetLevelSource(const GuestLayout& layout, uint32_t level, uint32_t layer, LevelSource& out);

// Byte offset (from the region start) of block (x, y, z) of a storage level.
uint32_t BlockOffset(const GuestLayout& layout, const LevelStorage& storage, uint32_t x, uint32_t y, uint32_t z);

}  // namespace kknr
