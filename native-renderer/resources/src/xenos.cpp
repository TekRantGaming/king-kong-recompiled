#include "kknr/xenos.h"

#include <algorithm>

namespace kknr {

namespace {

#define F(name, kind, bw, bh, bits) {#name, FormatKind::kind, bw, bh, bits}
const FormatInfo kFormats[64] = {
    // 1 bpp: the SDK's table has 1x1 blocks of 1 bit (0 bytes, unusable); we treat 8 texels as one byte.
    F(k_1_REVERSE, kOther, 8, 1, 1),
    F(k_1, kOther, 8, 1, 1),
    F(k_8, kUncompressed, 1, 1, 8),
    F(k_1_5_5_5, kUncompressed, 1, 1, 16),
    F(k_5_6_5, kUncompressed, 1, 1, 16),
    F(k_6_5_5, kUncompressed, 1, 1, 16),
    F(k_8_8_8_8, kUncompressed, 1, 1, 32),
    F(k_2_10_10_10, kUncompressed, 1, 1, 32),
    F(k_8_A, kUncompressed, 1, 1, 8),
    F(k_8_B, kUncompressed, 1, 1, 8),
    F(k_8_8, kUncompressed, 1, 1, 16),
    F(k_Cr_Y1_Cb_Y0_REP, kPacked422, 2, 1, 16),
    F(k_Y1_Cr_Y0_Cb_REP, kPacked422, 2, 1, 16),
    F(k_16_16_EDRAM, kUncompressed, 1, 1, 32),
    F(k_8_8_8_8_A, kUncompressed, 1, 1, 32),
    F(k_4_4_4_4, kUncompressed, 1, 1, 16),
    F(k_10_11_11, kUncompressed, 1, 1, 32),
    F(k_11_11_10, kUncompressed, 1, 1, 32),
    F(k_DXT1, kBlock4x4, 4, 4, 4),
    F(k_DXT2_3, kBlock4x4, 4, 4, 8),
    F(k_DXT4_5, kBlock4x4, 4, 4, 8),
    F(k_16_16_16_16_EDRAM, kUncompressed, 1, 1, 64),
    F(k_24_8, kUncompressed, 1, 1, 32),
    F(k_24_8_FLOAT, kUncompressed, 1, 1, 32),
    F(k_16, kUncompressed, 1, 1, 16),
    F(k_16_16, kUncompressed, 1, 1, 32),
    F(k_16_16_16_16, kUncompressed, 1, 1, 64),
    F(k_16_EXPAND, kUncompressed, 1, 1, 16),
    F(k_16_16_EXPAND, kUncompressed, 1, 1, 32),
    F(k_16_16_16_16_EXPAND, kUncompressed, 1, 1, 64),
    F(k_16_FLOAT, kUncompressed, 1, 1, 16),
    F(k_16_16_FLOAT, kUncompressed, 1, 1, 32),
    F(k_16_16_16_16_FLOAT, kUncompressed, 1, 1, 64),
    F(k_32, kUncompressed, 1, 1, 32),
    F(k_32_32, kUncompressed, 1, 1, 64),
    F(k_32_32_32_32, kUncompressed, 1, 1, 128),
    F(k_32_FLOAT, kUncompressed, 1, 1, 32),
    F(k_32_32_FLOAT, kUncompressed, 1, 1, 64),
    F(k_32_32_32_32_FLOAT, kUncompressed, 1, 1, 128),
    F(k_32_AS_8, kOther, 4, 1, 8),
    F(k_32_AS_8_8, kOther, 2, 1, 16),
    F(k_16_MPEG, kOther, 1, 1, 16),
    F(k_16_16_MPEG, kOther, 1, 1, 32),
    F(k_8_INTERLACED, kOther, 1, 1, 8),
    F(k_32_AS_8_INTERLACED, kOther, 4, 1, 8),
    F(k_32_AS_8_8_INTERLACED, kOther, 1, 1, 16),
    F(k_16_INTERLACED, kOther, 1, 1, 16),
    F(k_16_MPEG_INTERLACED, kOther, 1, 1, 16),
    F(k_16_16_MPEG_INTERLACED, kOther, 1, 1, 32),
    F(k_DXN, kBlock4x4, 4, 4, 8),
    F(k_8_8_8_8_AS_16_16_16_16, kUncompressed, 1, 1, 32),
    F(k_DXT1_AS_16_16_16_16, kBlock4x4, 4, 4, 4),
    F(k_DXT2_3_AS_16_16_16_16, kBlock4x4, 4, 4, 8),
    F(k_DXT4_5_AS_16_16_16_16, kBlock4x4, 4, 4, 8),
    F(k_2_10_10_10_AS_16_16_16_16, kUncompressed, 1, 1, 32),
    F(k_10_11_11_AS_16_16_16_16, kUncompressed, 1, 1, 32),
    F(k_11_11_10_AS_16_16_16_16, kUncompressed, 1, 1, 32),
    F(k_32_32_32_FLOAT, kUncompressed, 1, 1, 96),
    F(k_DXT3A, kBlock4x4, 4, 4, 4),
    F(k_DXT5A, kBlock4x4, 4, 4, 4),
    F(k_CTX1, kBlock4x4, 4, 4, 4),
    F(k_DXT3A_AS_1_1_1_1, kBlock4x4, 4, 4, 4),
    F(k_8_8_8_8_GAMMA_EDRAM, kOther, 1, 1, 32),
    F(k_2_10_10_10_FLOAT_EDRAM, kOther, 1, 1, 32),
};
#undef F

uint32_t Log2Floor(uint32_t v) {
  uint32_t r = 0;
  while (v >>= 1) ++r;
  return r;
}

}  // namespace

const FormatInfo& GetFormatInfo(TextureFormat format) { return kFormats[uint32_t(format) & 63]; }

const char* EndianName(Endian endian) {
  static const char* const kNames[] = {"none", "8in16", "8in32", "16in32"};
  return kNames[uint32_t(endian) & 3];
}

const char* DimensionName(Dimension dimension) {
  static const char* const kNames[] = {"1D", "2D", "3D", "cube"};
  return kNames[uint32_t(dimension) & 3];
}

uint32_t BytesPerBlockLog2(TextureFormat format) { return Log2Floor(GetFormatInfo(format).BytesPerBlock()); }

TextureFetch TextureFetch::FromGuest(const uint8_t* be_words) {
  TextureFetch f;
  for (int i = 0; i < 6; ++i) f.words[i] = LoadBE32(be_words + 4 * i);
  return f;
}

TextureFetch TextureFetch::FromWords(const uint32_t words[6]) {
  TextureFetch f;
  for (int i = 0; i < 6; ++i) f.words[i] = words[i];
  return f;
}

uint32_t TextureFetch::Width() const {
  switch (Dim()) {
    case Dimension::k1D:
      return (words[2] & 0xFFFFFF) + 1;
    case Dimension::k3D:
      return (words[2] & 0x7FF) + 1;
    default:
      return (words[2] & 0x1FFF) + 1;
  }
}

uint32_t TextureFetch::Height() const {
  switch (Dim()) {
    case Dimension::k1D:
      return 1;
    case Dimension::k3D:
      return ((words[2] >> 11) & 0x7FF) + 1;
    default:
      return ((words[2] >> 13) & 0x1FFF) + 1;
  }
}

uint32_t TextureFetch::DepthOrLayers() const {
  switch (Dim()) {
    case Dimension::k3D:
      return (words[2] >> 22) + 1;
    case Dimension::kCube:
      return 6;
    case Dimension::k2D:
      return Stacked() ? (words[2] >> 26) + 1 : 1;
    default:
      return 1;
  }
}

uint32_t DefaultPitchTexels(TextureFormat format, uint32_t width, bool tiled) {
  const FormatInfo& info = GetFormatInfo(format);
  const uint32_t bpb = std::max(info.BytesPerBlock(), 1u);
  uint32_t blocks = (width + info.block_width - 1) / info.block_width;
  // Tiled: whole 32-block tiles. Linear: rows of whole 256-byte units, and a pitch the field can hold
  // (texels / 32).
  for (;; ++blocks) {
    const uint32_t texels = blocks * info.block_width;
    if (texels % 32) continue;
    if (tiled ? blocks % 32 == 0 : (blocks * bpb) % 256 == 0) return texels;
  }
}

TextureFetch MakeTextureFetch(const TextureFetchDesc& d) {
  TextureFetch f;
  uint32_t* w = f.words;
  const uint32_t pitch = d.pitch_texels ? d.pitch_texels : DefaultPitchTexels(d.format, d.width, d.tiled);
  w[0] = 2;  // texture
  for (int i = 0; i < 4; ++i) w[0] |= uint32_t(d.signs[i]) << (2 + 2 * i);
  w[0] |= ((pitch >> 5) & 0x1FF) << 22;
  w[0] |= uint32_t(d.tiled) << 31;
  const bool stacked = d.dimension == Dimension::k2D && d.stacked;
  w[1] = uint32_t(d.format) | uint32_t(d.endian) << 6 | uint32_t(stacked) << 10 | (d.base_address & 0x1FFFF000u);
  switch (d.dimension) {
    case Dimension::k1D:
      w[2] = (d.width - 1) & 0xFFFFFF;
      break;
    case Dimension::k3D:
      w[2] = ((d.width - 1) & 0x7FF) | ((d.height - 1) & 0x7FF) << 11 | ((d.depth_or_layers - 1) & 0x3FF) << 22;
      break;
    case Dimension::kCube:
      w[2] = ((d.width - 1) & 0x1FFF) | ((d.height - 1) & 0x1FFF) << 13 | 5u << 26;
      break;
    default:
      w[2] = ((d.width - 1) & 0x1FFF) | ((d.height - 1) & 0x1FFF) << 13 |
             (stacked ? ((d.depth_or_layers - 1) & 63) << 26 : 0);
      break;
  }
  w[3] = uint32_t(d.integer) | uint32_t(d.swizzle & 0xFFF) << 1 | (uint32_t(d.exp_adjust) & 63) << 13;
  w[4] = (d.min_level & 15) << 2 | (d.max_level & 15) << 6;
  w[5] = uint32_t(d.dimension) << 9 | uint32_t(d.packed_mips) << 11 | (d.mip_address & 0x1FFFF000u);
  return f;
}

TextureLevels GetTextureLevels(const TextureFetch& fetch) {
  TextureLevels l;
  l.width = fetch.Width();
  l.height = fetch.Height();
  l.depth_or_layers = fetch.DepthOrLayers();
  uint32_t longest = std::max(l.width, l.height);
  if (fetch.Dim() == Dimension::k3D) longest = std::max(longest, l.depth_or_layers);
  const uint32_t size_max_level = Log2Floor(longest);
  uint32_t base = fetch.BaseAddress(), mip = fetch.MipAddress();
  uint32_t min_level = 0, max_level = 0;
  if (mip) {
    min_level = std::min(fetch.MipMinLevel(), size_max_level);
    max_level = std::max(std::min(fetch.MipMaxLevel(), size_max_level), min_level);
  }
  if (max_level != 0) {
    if (base == 0) min_level = std::max(min_level, 1u);
    if (min_level != 0) base = 0;
  } else {
    mip = 0;
  }
  l.base_address = base;
  l.mip_address = mip;
  l.min_level = min_level;
  l.max_level = max_level;
  return l;
}

VertexFetch VertexFetch::FromGuest(const uint8_t* be_words) {
  VertexFetch f;
  f.words[0] = LoadBE32(be_words);
  f.words[1] = LoadBE32(be_words + 4);
  return f;
}

}  // namespace kknr
