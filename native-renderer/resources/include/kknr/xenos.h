// Xbox 360 (Xenos) resource descriptions as the game's Direct3D library hands them over: the 6-word GPU
// texture fetch constant (texture object +16), the 2-word vertex fetch constant (vertex buffer object +12),
// the 360 D3DFORMAT word (CreateTexture / CreateRenderTarget argument) and the per-format block sizes.
// Field layouts follow the SDK's include/rex/graphics/xenos.h (Xenia research, BSD); this is our own
// standalone copy with no SDK dependency.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kknr {

enum class TextureFormat : uint8_t {
  k_1_REVERSE = 0,
  k_1 = 1,
  k_8 = 2,
  k_1_5_5_5 = 3,
  k_5_6_5 = 4,
  k_6_5_5 = 5,
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_8_A = 8,
  k_8_B = 9,
  k_8_8 = 10,
  k_Cr_Y1_Cb_Y0_REP = 11,
  k_Y1_Cr_Y0_Cb_REP = 12,
  k_16_16_EDRAM = 13,
  k_8_8_8_8_A = 14,
  k_4_4_4_4 = 15,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_DXT1 = 18,
  k_DXT2_3 = 19,
  k_DXT4_5 = 20,
  k_16_16_16_16_EDRAM = 21,
  k_24_8 = 22,
  k_24_8_FLOAT = 23,
  k_16 = 24,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_EXPAND = 27,
  k_16_16_EXPAND = 28,
  k_16_16_16_16_EXPAND = 29,
  k_16_FLOAT = 30,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32 = 33,
  k_32_32 = 34,
  k_32_32_32_32 = 35,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_32_AS_8 = 39,
  k_32_AS_8_8 = 40,
  k_16_MPEG = 41,
  k_16_16_MPEG = 42,
  k_8_INTERLACED = 43,
  k_32_AS_8_INTERLACED = 44,
  k_32_AS_8_8_INTERLACED = 45,
  k_16_INTERLACED = 46,
  k_16_MPEG_INTERLACED = 47,
  k_16_16_MPEG_INTERLACED = 48,
  k_DXN = 49,
  k_8_8_8_8_AS_16_16_16_16 = 50,
  k_DXT1_AS_16_16_16_16 = 51,
  k_DXT2_3_AS_16_16_16_16 = 52,
  k_DXT4_5_AS_16_16_16_16 = 53,
  k_2_10_10_10_AS_16_16_16_16 = 54,
  k_10_11_11_AS_16_16_16_16 = 55,
  k_11_11_10_AS_16_16_16_16 = 56,
  k_32_32_32_FLOAT = 57,
  k_DXT3A = 58,
  k_DXT5A = 59,
  k_CTX1 = 60,
  k_DXT3A_AS_1_1_1_1 = 61,
  k_8_8_8_8_GAMMA_EDRAM = 62,
  k_2_10_10_10_FLOAT_EDRAM = 63,
};

// Byte order of the data as the GPU reads it: the swap applied to each 16- or 32-bit unit of memory.
enum class Endian : uint8_t { kNone = 0, k8in16 = 1, k8in32 = 2, k16in32 = 3 };

enum class Dimension : uint8_t { k1D = 0, k2D = 1, k3D = 2, kCube = 3 };  // k2D includes stacked (arrays)

// Per data component (before the swizzle).
enum class TextureSign : uint8_t { kUnsigned = 0, kSigned = 1, kUnsignedBiased = 2, kGamma = 3 };

// Swizzle selectors (3 bits each, X in bits 0-2): 0-3 = source X..W, 4 = constant 0, 5 = constant 1. The same
// values as nvrhi::ComponentSwizzle (R, G, B, A, Zero, One), so a fetch swizzle is directly an SRV mapping.
enum Swizzle : uint8_t { kSwzX = 0, kSwzY = 1, kSwzZ = 2, kSwzW = 3, kSwz0 = 4, kSwz1 = 5 };
constexpr uint16_t MakeSwizzle(uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
  return uint16_t(x | y << 3 | z << 6 | w << 9);
}
constexpr uint16_t kSwizzleXYZW = MakeSwizzle(kSwzX, kSwzY, kSwzZ, kSwzW);
constexpr uint8_t SwizzleComponent(uint16_t swizzle, int i) { return uint8_t((swizzle >> (3 * i)) & 7); }

enum class FormatKind : uint8_t {
  kUncompressed,  // one texel per block
  kBlock4x4,      // DXT / DXN / CTX1 style 4x4 blocks
  kPacked422,     // two texels per 32-bit block (Y'CbCr 4:2:2)
  kOther,         // rare layouts: 1 bpp, 32-bit blocks split in texels, MPEG and interlaced variants
};

struct FormatInfo {
  const char* name;
  FormatKind kind;
  uint8_t block_width;
  uint8_t block_height;
  uint8_t bits_per_texel;  // per texel, so a 4x4 block of 4 bpp is 8 bytes
  uint32_t BytesPerBlock() const { return uint32_t(block_width) * block_height * bits_per_texel / 8; }
};

const FormatInfo& GetFormatInfo(TextureFormat format);
const char* EndianName(Endian endian);
const char* DimensionName(Dimension dimension);

// log2 of the bytes per block as the tiling functions use it (floor; 12-byte blocks never tile).
uint32_t BytesPerBlockLog2(TextureFormat format);

// The 6-word texture fetch constant (SQ_TEX_*), decoded.
struct TextureFetch {
  uint32_t words[6] = {};

  // From the guest object: big-endian words at texture+16.
  static TextureFetch FromGuest(const uint8_t* be_words);
  static TextureFetch FromWords(const uint32_t words[6]);

  uint32_t Type() const { return words[0] & 3; }  // 2 = texture
  TextureSign Sign(int component) const { return TextureSign((words[0] >> (2 + 2 * component)) & 3); }
  uint32_t ClampX() const { return (words[0] >> 10) & 7; }
  uint32_t ClampY() const { return (words[0] >> 13) & 7; }
  uint32_t ClampZ() const { return (words[0] >> 16) & 7; }
  // Row pitch of the base level in texels (the field is texels / 32).
  uint32_t PitchTexels() const { return ((words[0] >> 22) & 0x1FF) << 5; }
  bool Tiled() const { return (words[0] >> 31) != 0; }

  TextureFormat Format() const { return TextureFormat(words[1] & 63); }
  Endian EndianMode() const { return Endian((words[1] >> 6) & 3); }
  bool Stacked() const { return ((words[1] >> 10) & 1) != 0; }
  // Physical addresses (the 512 MB physical space; the guest may give them through any of its views).
  uint32_t BaseAddress() const { return ((words[1] >> 12) & 0x1FFFF) << 12; }
  uint32_t MipAddress() const { return ((words[5] >> 12) & 0x1FFFF) << 12; }

  Dimension Dim() const { return Dimension((words[5] >> 9) & 3); }
  uint32_t Width() const;
  uint32_t Height() const;
  // Depth of a 3D texture, layers of a stacked 2D texture, 6 for a cube, else 1.
  uint32_t DepthOrLayers() const;

  bool NumFormatInteger() const { return (words[3] & 1) != 0; }
  uint16_t Swizzle() const { return uint16_t((words[3] >> 1) & 0xFFF); }
  int32_t ExpAdjust() const { return int32_t(words[3] << 13) >> 26; }
  uint32_t MagFilter() const { return (words[3] >> 19) & 3; }
  uint32_t MinFilter() const { return (words[3] >> 21) & 3; }
  uint32_t MipFilter() const { return (words[3] >> 23) & 3; }
  uint32_t AnisoFilter() const { return (words[3] >> 25) & 7; }

  uint32_t MipMinLevel() const { return (words[4] >> 2) & 15; }
  uint32_t MipMaxLevel() const { return (words[4] >> 6) & 15; }
  int32_t LodBiasFixed() const { return int32_t(words[4] << 10) >> 22; }  // 5 fractional bits

  uint32_t BorderColor() const { return words[5] & 3; }
  bool PackedMips() const { return ((words[5] >> 11) & 1) != 0; }
};

// Builds a fetch constant (tests, tools and host-side descriptions; the game's own come from its objects).
struct TextureFetchDesc {
  TextureFormat format = TextureFormat::k_8_8_8_8;
  Endian endian = Endian::kNone;
  Dimension dimension = Dimension::k2D;
  bool tiled = false;
  bool packed_mips = false;
  bool stacked = false;            // 2D only: an array of depth_or_layers layers
  uint32_t width = 1, height = 1;
  uint32_t depth_or_layers = 1;    // 3D depth or stacked layers (cubes are always 6)
  uint32_t pitch_texels = 0;       // 0: what the XDK would pick (32-block tiles, or 256-byte linear rows)
  uint32_t base_address = 0;       // physical, 4 KB aligned; 0 = no base level
  uint32_t mip_address = 0;        // physical, 4 KB aligned; 0 = no mips
  uint32_t min_level = 0, max_level = 0;
  uint16_t swizzle = kSwizzleXYZW;
  TextureSign signs[4] = {};
  bool integer = false;
  int32_t exp_adjust = 0;
};
TextureFetch MakeTextureFetch(const TextureFetchDesc& desc);
// The base pitch the XDK gives a texture (texels, a multiple of 32).
uint32_t DefaultPitchTexels(TextureFormat format, uint32_t width, bool tiled);

// Levels actually stored, as the SDK's GetSubresourcesFromFetchConstant decides: the base exists if its
// address is set and mips start at 1 only when the mip address is set.
struct TextureLevels {
  uint32_t width = 0, height = 0, depth_or_layers = 0;
  uint32_t base_address = 0, mip_address = 0;  // 0 when that region is absent
  uint32_t min_level = 0, max_level = 0;
};
TextureLevels GetTextureLevels(const TextureFetch& fetch);

// The 360 D3DFORMAT word (MAKED3DFMT): the texture format, endian, tiling, signs, number format and
// swizzle that become the fetch constant. Examples: 0x18280186 A8R8G8B8, 0x28280186 X8R8G8B8,
// 0x2DA2ABA4 R32F, 0x2D200196 D24S8, 0x1A200152 DXT1.
struct D3DFormat {
  uint32_t value = 0;
  TextureFormat Format() const { return TextureFormat(value & 63); }
  Endian EndianMode() const { return Endian((value >> 6) & 3); }
  bool Tiled() const { return ((value >> 8) & 1) != 0; }
  TextureSign Sign(int component) const { return TextureSign((value >> (9 + 2 * component)) & 3); }
  bool NumFormatInteger() const { return ((value >> 17) & 1) != 0; }
  uint16_t Swizzle() const { return uint16_t((value >> 18) & 0xFFF); }
};

// The 2-word vertex fetch constant (vertex buffer object +12).
struct VertexFetch {
  uint32_t words[2] = {};
  static VertexFetch FromGuest(const uint8_t* be_words);
  uint32_t Type() const { return words[0] & 3; }        // 3 = vertex
  uint32_t Address() const { return words[0] & ~3u; }   // guest address (physical, any view)
  Endian EndianMode() const { return Endian(words[1] & 3); }
  uint32_t SizeBytes() const { return ((words[1] >> 2) & 0xFFFFFF) * 4; }
};

// Big-endian loads from guest memory.
inline uint16_t LoadBE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

}  // namespace kknr
