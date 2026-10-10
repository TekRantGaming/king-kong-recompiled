#include "kknr/xenos.h"

#include <algorithm>

namespace kknr {

namespace {

#define F(name, kind, bw, bh, bits) {#name, FormatKind::kind, bw, bh, bits}
const FormatInfo kFormats[64] = {
    F(k_1_REVERSE, kOther, 1, 1, 1),
    F(k_1, kOther, 1, 1, 1),
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
