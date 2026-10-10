// Guest texture -> host texture data. PlanHostTexture picks the host format, the conversion and the SRV
// component mapping for a fetch constant; ConvertTexture reads the guest memory (endian swap, untiling,
// unpacking the mip tail, decoding where the host has no equivalent) and returns one tightly packed image per
// host subresource, ready for nvrhi::ICommandList::writeTexture.
//
// Conventions (see docs/formats.md):
// - Host channel R holds the guest's component X, G holds Y and so on, for every format. The fetch
//   constant's swizzle is applied by the SRV (view_swizzle, nvrhi::ComponentMapping encoding), not baked
//   into the data, so one host texture serves every swizzle the engine binds it with.
// - All-signed components pick the SNORM host format where one exists; unsigned-biased, gamma and mixed
//   signs keep UNORM data and set a shader flag (the translated shader applies them, as the hardware does
//   before filtering).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kknr/guest_texture.h"
#include "kknr/xenos.h"

namespace kknr {

// Host formats used for textures. Names (and meaning) are nvrhi::Format's; kknr/nvrhi_format.h maps them.
enum class HostFormat : uint8_t {
  UNKNOWN,
  R8_UNORM,
  R8_SNORM,
  RG8_UNORM,
  RG8_SNORM,
  R16_UNORM,
  R16_SNORM,
  R16_FLOAT,
  BGRA4_UNORM,
  B5G6R5_UNORM,
  B5G5R5A1_UNORM,
  RGBA8_UNORM,
  RGBA8_SNORM,
  R10G10B10A2_UNORM,
  RG16_UNORM,
  RG16_SNORM,
  RG16_FLOAT,
  R32_UINT,
  R32_SINT,
  R32_FLOAT,
  RGBA16_FLOAT,
  RGBA16_UNORM,
  RGBA16_SNORM,
  RG32_UINT,
  RG32_SINT,
  RG32_FLOAT,
  RGBA32_UINT,
  RGBA32_SINT,
  RGBA32_FLOAT,
  BC1_UNORM,
  BC2_UNORM,
  BC3_UNORM,
  BC4_UNORM,
  BC4_SNORM,
  BC5_UNORM,
  BC5_SNORM,
  COUNT,
};

struct HostFormatInfo {
  const char* name;
  uint8_t block_size;       // 1, or 4 for BC formats
  uint8_t bytes_per_block;  // bytes per texel, or per 4x4 block
};
const HostFormatInfo& GetHostFormatInfo(HostFormat format);

// How guest blocks become host blocks / texels.
enum class Conversion : uint8_t {
  kCopy,                 // bytes unchanged after the endian swap (block for block)
  k565ToRGBA8,           // k_5_6_5: X 5, Y 6, Z 5 bits from the bottom, widened by bit replication
  k1555ToRGBA8,          // k_1_5_5_5: X 5, Y 5, Z 5, W 1
  k4444ToRGBA8,          // k_4_4_4_4: X 4, Y 4, Z 4, W 4
  k655ToRGBA8,           // k_6_5_5: X 5, Y 5, Z 6 bits from the bottom
  k11_11_10ToRGBA16,     // k_10_11_11: X 11, Y 11, Z 10 bits from the bottom
  k10_11_11ToRGBA16,     // k_11_11_10: X 10, Y 11, Z 11 bits from the bottom
  k11_11_10ToRGBA16S,    // the same, signed components
  k10_11_11ToRGBA16S,
  k2_10_10_10ToRGBA16S,  // signed k_2_10_10_10 (no host SNORM 10:10:10:2)
  kDXT1ToRGBA8,
  kDXT3ToRGBA8,
  kDXT5ToRGBA8,
  kDXNToRG8,
  kDXNToRG8S,            // signed DXN (BC5_SNORM rules) decoded
  kDXT5AToR8,
  kDXT5AToR8S,           // signed DXT5A (BC4_SNORM rules) decoded
  kDXT3AToR8,
  kDXT3AAs1111ToRGBA8,
  kCTX1ToRG8,
  kCrY1CbY0ToRGBA8,      // k_Cr_Y1_Cb_Y0_REP: components per texel (R = Cr, G = Y, B = Cb), no colour conversion
  kY1CrY0CbToRGBA8,      // k_Y1_Cr_Y0_Cb_REP: the same
  kDepth24ToFloat,       // k_24_8: depth in the top 24 bits, unorm
  kDepth20e4ToFloat,     // k_24_8_FLOAT: depth in the top 24 bits, 20e4 float
  kRGB32ToRGBA32F,       // k_32_32_32_FLOAT, alpha 1
  k1ToR8,                // k_1: 8 texels per byte, bit 0 first
  k1ReverseToR8,         // k_1_REVERSE: 8 texels per byte, bit 7 first
  kSplitBlockToTexels,   // k_32_AS_8 / k_32_AS_8_8: a 32-bit block of 4 (2) texels, byte order kept
  kFixed16ToHalf,        // k_16_16_EDRAM / k_16_16_16_16_EDRAM: fixed -32..32 -> half float
  k7e3ToRGBA16F,         // k_2_10_10_10_FLOAT_EDRAM: 7e3 RGB, 2-bit alpha -> half float
  COUNT,
};
const char* ConversionName(Conversion conversion);
// True for conversions that turn a multi-texel guest block into texels.
bool ConversionDecodesBlocks(Conversion conversion);

enum ShaderFlags : uint8_t {
  kShaderNone = 0,
  kShaderBias = 1,        // some component is unsigned-biased: sample * 2 - 1
  kShaderGamma = 2,       // some component is gamma (or the format is gamma): PWL degamma after sampling
  kShaderMixedSigns = 4,  // signed components in unsigned host data: the shader converts them
  kShaderExpAdjust = 8,   // exp_adjust != 0: multiply by 2^exp_adjust
  kShaderInteger = 16,    // number format integer (data read as integers, not normalized)
  kShaderNorm32 = 32,     // k_32 / k_32_32 / k_32_32_32_32 fraction: UINT / SINT data the shader normalizes
};

// The host side of one guest format (no fetch-constant specifics).
struct FormatChoice {
  HostFormat format = HostFormat::UNKNOWN;
  Conversion conversion = Conversion::kCopy;
  HostFormat signed_format = HostFormat::UNKNOWN;  // UNKNOWN: signed data needs the shader fix-up
  Conversion signed_conversion = Conversion::kCopy;
  uint16_t replicate = 0;                           // guest component each host channel read returns
  HostFormat decompressed_format = HostFormat::UNKNOWN;  // block formats: texels when BC is not usable
  Conversion decompress = Conversion::kCopy;
  HostFormat decompressed_signed_format = HostFormat::UNKNOWN;
  Conversion decompress_signed = Conversion::kCopy;
  uint8_t extra_shader_flags = kShaderNone;          // flags the format always needs (gamma EDRAM ...)
};
FormatChoice GetFormatChoice(TextureFormat format);

struct HostTexturePlan {
  HostFormat format = HostFormat::UNKNOWN;
  Conversion conversion = Conversion::kCopy;
  Dimension dimension = Dimension::k2D;
  uint32_t width = 0, height = 0, depth = 1, layers = 1, levels = 1;
  uint32_t min_level = 0;     // levels below min_level are not stored (the view's most detailed mip)
  uint16_t view_swizzle = kSwizzleXYZW;  // fetch swizzle composed with the format's own mapping
  uint8_t shader_flags = kShaderNone;
  bool decompressed = false;  // a block format decoded to texels (no host equivalent / unaligned size)
};

struct HostSubresource {
  uint32_t level = 0, layer = 0;
  uint32_t width = 0, height = 0, depth = 1;  // texels
  uint32_t row_pitch = 0;    // bytes between rows of blocks
  uint32_t depth_pitch = 0;  // bytes between depth slices
  size_t offset = 0;         // into HostTextureData::bytes
  size_t size = 0;
};

struct HostTextureData {
  HostTexturePlan plan;
  std::vector<uint8_t> bytes;
  std::vector<HostSubresource> subresources;  // level-major: for each level, every layer
  const HostSubresource* Find(uint32_t level, uint32_t layer) const;
};

bool PlanHostTexture(const TextureFetch& fetch, HostTexturePlan& plan, std::string* why = nullptr);
bool ConvertTexture(const TextureFetch& fetch, const GuestMemory& memory, HostTextureData& out,
                    std::string* why = nullptr);

// Guest blocks of one subresource (host byte order, as ReadGuestBlocks returns them) -> host data.
// width / height / depth are the level's size in texels; dst has row_pitch bytes per row of host blocks and
// depth_pitch per slice.
void ConvertBlocks(Conversion conversion, TextureFormat guest_format, HostFormat host_format, const uint8_t* blocks,
                   const BlockExtent& extent, uint32_t width, uint32_t height, uint8_t* dst, uint32_t row_pitch,
                   uint32_t depth_pitch);

// Guest regions a texture reads (for residency and write watching): [base, base + base_bytes) and
// [mip, mip + mip_bytes), physical addresses; sizes 0 when absent.
struct TextureRanges {
  uint32_t base = 0, base_bytes = 0, mip = 0, mip_bytes = 0;
};
TextureRanges GetTextureRanges(const TextureFetch& fetch);

// Single-block decoders (also used by the tests). Inputs are host-order (already endian-swapped) blocks;
// outputs are 4x4 texels, row-major, 4 bytes (RGBA8) / 2 bytes (RG8) / 1 byte (R8) per texel.
void DecodeBC1(const uint8_t* block, uint8_t rgba[64], bool force_four_colors = false);
void DecodeBC2(const uint8_t* block, uint8_t rgba[64]);
void DecodeBC3(const uint8_t* block, uint8_t rgba[64]);
void DecodeBC4(const uint8_t* block, uint8_t r[16]);
void DecodeBC4Signed(const uint8_t* block, int8_t r[16]);
void DecodeBC5(const uint8_t* block, uint8_t rg[32]);
void DecodeBC5Signed(const uint8_t* block, int8_t rg[32]);
void DecodeDXT3A(const uint8_t* block, uint8_t r[16]);
void DecodeCTX1(const uint8_t* block, uint8_t rg[32]);

// Scalar conversions.
float UNorm24ToFloat(uint32_t depth24);
float Float20e4ToFloat(uint32_t depth24);
float Float7e3ToFloat(uint32_t f10);
uint16_t FloatToHalf(float f);
float HalfToFloat(uint16_t h);

}  // namespace kknr
