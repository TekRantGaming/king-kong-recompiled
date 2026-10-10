#include "kknr/texture_convert.h"

#include <algorithm>
#include <cstring>

#include "kknr/endian.h"
#include "kknr/tiling.h"

namespace kknr {

namespace {

const HostFormatInfo kHostFormats[] = {
    {"UNKNOWN", 1, 0},         {"R8_UNORM", 1, 1},      {"R8_SNORM", 1, 1},        {"RG8_UNORM", 1, 2},
    {"RG8_SNORM", 1, 2},       {"R16_UNORM", 1, 2},     {"R16_SNORM", 1, 2},       {"R16_FLOAT", 1, 2},
    {"BGRA4_UNORM", 1, 2},     {"B5G6R5_UNORM", 1, 2},  {"B5G5R5A1_UNORM", 1, 2},  {"RGBA8_UNORM", 1, 4},
    {"RGBA8_SNORM", 1, 4},     {"R10G10B10A2_UNORM", 1, 4}, {"RG16_UNORM", 1, 4},  {"RG16_SNORM", 1, 4},
    {"RG16_FLOAT", 1, 4},      {"R32_UINT", 1, 4},      {"R32_FLOAT", 1, 4},       {"RGBA16_FLOAT", 1, 8},
    {"RGBA16_UNORM", 1, 8},    {"RGBA16_SNORM", 1, 8},  {"RG32_UINT", 1, 8},       {"RG32_FLOAT", 1, 8},
    {"RGBA32_UINT", 1, 16},    {"RGBA32_FLOAT", 1, 16}, {"BC1_UNORM", 4, 8},       {"BC2_UNORM", 4, 16},
    {"BC3_UNORM", 4, 16},      {"BC4_UNORM", 4, 8},     {"BC4_SNORM", 4, 8},       {"BC5_UNORM", 4, 16},
    {"BC5_SNORM", 4, 16},
};
static_assert(sizeof(kHostFormats) / sizeof(kHostFormats[0]) == size_t(HostFormat::COUNT), "host format table");

// What each guest format becomes. replicate: how the guest's components fill a 4-component read before the
// fetch swizzle (single-component formats replicate X, two-component ones Y), in host channels.
struct FormatRule {
  HostFormat unsigned_format;
  HostFormat signed_format;  // UNKNOWN: no signed host format (shader fix-up)
  Conversion conversion;
  Conversion signed_conversion;
  uint16_t replicate;
  HostFormat decompressed_format;  // for block formats: the texel format when decoding
  Conversion decompress;
};

constexpr uint16_t kRRRR = MakeSwizzle(0, 0, 0, 0);
constexpr uint16_t kRGGG = MakeSwizzle(0, 1, 1, 1);
constexpr uint16_t kRGBB = MakeSwizzle(0, 1, 2, 2);
constexpr uint16_t kRGBA = MakeSwizzle(0, 1, 2, 3);

using C = Conversion;
using H = HostFormat;

FormatRule Rule(TextureFormat format) {
  switch (format) {
    case TextureFormat::k_8:
    case TextureFormat::k_8_A:
    case TextureFormat::k_8_B:
      return {H::R8_UNORM, H::R8_SNORM, C::kCopy, C::kCopy, kRRRR};
    case TextureFormat::k_1_5_5_5:
      return {H::B5G5R5A1_UNORM, H::UNKNOWN, C::kSwapRB1555, C::kSwapRB1555, kRGBA};
    case TextureFormat::k_5_6_5:
      return {H::B5G6R5_UNORM, H::UNKNOWN, C::kSwapRB565, C::kSwapRB565, kRGBB};
    case TextureFormat::k_6_5_5:
      return {H::RGBA8_UNORM, H::UNKNOWN, C::k655ToRGBA8, C::k655ToRGBA8, kRGBB};
    case TextureFormat::k_8_8_8_8:
    case TextureFormat::k_8_8_8_8_A:
    case TextureFormat::k_8_8_8_8_AS_16_16_16_16:
      return {H::RGBA8_UNORM, H::RGBA8_SNORM, C::kCopy, C::kCopy, kRGBA};
    case TextureFormat::k_2_10_10_10:
    case TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      return {H::R10G10B10A2_UNORM, H::RGBA16_SNORM, C::kCopy, C::k2_10_10_10ToRGBA16S, kRGBA};
    case TextureFormat::k_8_8:
      return {H::RG8_UNORM, H::RG8_SNORM, C::kCopy, C::kCopy, kRGGG};
    case TextureFormat::k_Cr_Y1_Cb_Y0_REP:
    case TextureFormat::k_Y1_Cr_Y0_Cb_REP:
      return {H::RGBA8_UNORM, H::UNKNOWN, C::kYUV422ToRGBA8, C::kYUV422ToRGBA8, kRGBB};
    case TextureFormat::k_4_4_4_4:
      return {H::BGRA4_UNORM, H::UNKNOWN, C::kSwapRB4444, C::kSwapRB4444, kRGBA};
    case TextureFormat::k_10_11_11:
    case TextureFormat::k_10_11_11_AS_16_16_16_16:
      return {H::RGBA16_UNORM, H::RGBA16_SNORM, C::k11_11_10ToRGBA16, C::k11_11_10ToRGBA16S, kRGBB};
    case TextureFormat::k_11_11_10:
    case TextureFormat::k_11_11_10_AS_16_16_16_16:
      return {H::RGBA16_UNORM, H::RGBA16_SNORM, C::k10_11_11ToRGBA16, C::k10_11_11ToRGBA16S, kRGBB};
    case TextureFormat::k_DXT1:
    case TextureFormat::k_DXT1_AS_16_16_16_16:
      return {H::BC1_UNORM, H::UNKNOWN, C::kCopy, C::kCopy, kRGBA, H::RGBA8_UNORM, C::kDXT1ToRGBA8};
    case TextureFormat::k_DXT2_3:
    case TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return {H::BC2_UNORM, H::UNKNOWN, C::kCopy, C::kCopy, kRGBA, H::RGBA8_UNORM, C::kDXT3ToRGBA8};
    case TextureFormat::k_DXT4_5:
    case TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return {H::BC3_UNORM, H::UNKNOWN, C::kCopy, C::kCopy, kRGBA, H::RGBA8_UNORM, C::kDXT5ToRGBA8};
    case TextureFormat::k_24_8:
      return {H::R32_FLOAT, H::R32_FLOAT, C::kDepth24ToFloat, C::kDepth24ToFloat, kRRRR};
    case TextureFormat::k_24_8_FLOAT:
      return {H::R32_FLOAT, H::R32_FLOAT, C::kDepth20e4ToFloat, C::kDepth20e4ToFloat, kRRRR};
    case TextureFormat::k_16:
      return {H::R16_UNORM, H::R16_SNORM, C::kCopy, C::kCopy, kRRRR};
    case TextureFormat::k_16_16:
      return {H::RG16_UNORM, H::RG16_SNORM, C::kCopy, C::kCopy, kRGGG};
    case TextureFormat::k_16_16_16_16:
      return {H::RGBA16_UNORM, H::RGBA16_SNORM, C::kCopy, C::kCopy, kRGBA};
    case TextureFormat::k_16_EXPAND:
    case TextureFormat::k_16_FLOAT:
      return {H::R16_FLOAT, H::R16_FLOAT, C::kCopy, C::kCopy, kRRRR};
    case TextureFormat::k_16_16_EXPAND:
    case TextureFormat::k_16_16_FLOAT:
      return {H::RG16_FLOAT, H::RG16_FLOAT, C::kCopy, C::kCopy, kRGGG};
    case TextureFormat::k_16_16_16_16_EXPAND:
    case TextureFormat::k_16_16_16_16_FLOAT:
      return {H::RGBA16_FLOAT, H::RGBA16_FLOAT, C::kCopy, C::kCopy, kRGBA};
    case TextureFormat::k_32:
      return {H::R32_UINT, H::R32_UINT, C::kCopy, C::kCopy, kRRRR};
    case TextureFormat::k_32_32:
      return {H::RG32_UINT, H::RG32_UINT, C::kCopy, C::kCopy, kRGGG};
    case TextureFormat::k_32_32_32_32:
      return {H::RGBA32_UINT, H::RGBA32_UINT, C::kCopy, C::kCopy, kRGBA};
    case TextureFormat::k_32_FLOAT:
      return {H::R32_FLOAT, H::R32_FLOAT, C::kCopy, C::kCopy, kRRRR};
    case TextureFormat::k_32_32_FLOAT:
      return {H::RG32_FLOAT, H::RG32_FLOAT, C::kCopy, C::kCopy, kRGGG};
    case TextureFormat::k_32_32_32_32_FLOAT:
      return {H::RGBA32_FLOAT, H::RGBA32_FLOAT, C::kCopy, C::kCopy, kRGBA};
    case TextureFormat::k_32_32_32_FLOAT:
      return {H::RGBA32_FLOAT, H::RGBA32_FLOAT, C::kRGB32ToRGBA32F, C::kRGB32ToRGBA32F, kRGBB};
    case TextureFormat::k_DXN:
      return {H::BC5_UNORM, H::BC5_SNORM, C::kCopy, C::kCopy, kRGGG, H::RG8_UNORM, C::kDXNToRG8};
    case TextureFormat::k_DXT3A:
      return {H::R8_UNORM, H::UNKNOWN, C::kDXT3AToR8, C::kDXT3AToR8, kRRRR};
    case TextureFormat::k_DXT5A:
      return {H::BC4_UNORM, H::BC4_SNORM, C::kCopy, C::kCopy, kRRRR, H::R8_UNORM, C::kDXT5AToR8};
    case TextureFormat::k_CTX1:
      return {H::RG8_UNORM, H::UNKNOWN, C::kCTX1ToRG8, C::kCTX1ToRG8, kRGGG};
    case TextureFormat::k_DXT3A_AS_1_1_1_1:
      return {H::RGBA8_UNORM, H::UNKNOWN, C::kDXT3AAs1111ToRGBA8, C::kDXT3AAs1111ToRGBA8, kRGBA};
    default:
      return {H::UNKNOWN, H::UNKNOWN, C::kCopy, C::kCopy, kRGBA};
  }
}

uint32_t DivUp(uint32_t v, uint32_t d) { return (v + d - 1) / d; }

uint32_t Expand(uint32_t v, uint32_t bits, uint32_t to_bits) {
  // Bit replication (exact for 0 and the maximum).
  uint32_t r = 0;
  int shift = int(to_bits) - int(bits);
  while (shift > 0) {
    r |= v << shift;
    shift -= int(bits);
  }
  return r | (v >> -shift);
}

uint16_t ToUnorm16(uint32_t v, uint32_t bits) { return uint16_t(Expand(v, bits, 16)); }

int16_t ToSnorm16(uint32_t v, uint32_t bits) {
  // Two's complement field -> snorm16, the most negative value clamping to -1.
  const int32_t max = (1 << (bits - 1)) - 1;
  int32_t s = int32_t(v << (32 - bits)) >> (32 - bits);
  s = std::max(s, -max);
  return int16_t((s * 32767 + (s >= 0 ? max / 2 : -max / 2)) / max);
}

void Store16x4(uint8_t* dst, const uint16_t c[4]) { std::memcpy(dst, c, 8); }

bool BlockDecode(Conversion c) {
  switch (c) {
    case C::kDXT1ToRGBA8:
    case C::kDXT3ToRGBA8:
    case C::kDXT5ToRGBA8:
    case C::kDXNToRG8:
    case C::kDXT5AToR8:
    case C::kDXT3AToR8:
    case C::kDXT3AAs1111ToRGBA8:
    case C::kCTX1ToRG8:
      return true;
    default:
      return false;
  }
}

// One guest texel (block) -> one host texel (block), for the per-texel conversions.
void ConvertTexel(Conversion c, const uint8_t* s, uint8_t* d, uint32_t bpb) {
  switch (c) {
    case C::kCopy:
      std::memcpy(d, s, bpb);
      return;
    case C::kSwapRB565: {
      uint16_t v;
      std::memcpy(&v, s, 2);
      v = uint16_t((v & 0x07E0) | (v >> 11) | (v << 11));
      std::memcpy(d, &v, 2);
      return;
    }
    case C::kSwapRB1555: {
      uint16_t v;
      std::memcpy(&v, s, 2);
      v = uint16_t((v & 0x83E0) | ((v >> 10) & 0x1F) | ((v & 0x1F) << 10));
      std::memcpy(d, &v, 2);
      return;
    }
    case C::kSwapRB4444: {
      uint16_t v;
      std::memcpy(&v, s, 2);
      v = uint16_t((v & 0xF0F0) | ((v >> 8) & 0xF) | ((v & 0xF) << 8));
      std::memcpy(d, &v, 2);
      return;
    }
    case C::k655ToRGBA8: {
      uint16_t v;
      std::memcpy(&v, s, 2);
      d[0] = uint8_t(Expand(v & 31, 5, 8));
      d[1] = uint8_t(Expand((v >> 5) & 31, 5, 8));
      d[2] = uint8_t(Expand(v >> 10, 6, 8));
      d[3] = 255;
      return;
    }
    case C::k11_11_10ToRGBA16:
    case C::k10_11_11ToRGBA16:
    case C::k11_11_10ToRGBA16S:
    case C::k10_11_11ToRGBA16S:
    case C::k2_10_10_10ToRGBA16S: {
      uint32_t v;
      std::memcpy(&v, s, 4);
      uint32_t bits[4], fields[4];
      if (c == C::k11_11_10ToRGBA16 || c == C::k11_11_10ToRGBA16S) {
        bits[0] = 11, bits[1] = 11, bits[2] = 10;
        fields[0] = v & 0x7FF, fields[1] = (v >> 11) & 0x7FF, fields[2] = v >> 22;
      } else if (c == C::k2_10_10_10ToRGBA16S) {
        bits[0] = bits[1] = bits[2] = 10;
        fields[0] = v & 0x3FF, fields[1] = (v >> 10) & 0x3FF, fields[2] = (v >> 20) & 0x3FF;
      } else {
        bits[0] = 10, bits[1] = 11, bits[2] = 11;
        fields[0] = v & 0x3FF, fields[1] = (v >> 10) & 0x7FF, fields[2] = v >> 21;
      }
      uint16_t out[4];
      const bool is_signed = c != C::k11_11_10ToRGBA16 && c != C::k10_11_11ToRGBA16;
      for (int i = 0; i < 3; ++i)
        out[i] = is_signed ? uint16_t(ToSnorm16(fields[i], bits[i])) : ToUnorm16(fields[i], bits[i]);
      if (c == C::k2_10_10_10ToRGBA16S)
        out[3] = uint16_t(ToSnorm16(v >> 30, 2));
      else
        out[3] = is_signed ? 0x7FFF : 0xFFFF;
      Store16x4(d, out);
      return;
    }
    case C::kDepth24ToFloat:
    case C::kDepth20e4ToFloat: {
      uint32_t v;
      std::memcpy(&v, s, 4);
      const float f = c == C::kDepth24ToFloat ? UNorm24ToFloat(v >> 8) : Float20e4ToFloat(v >> 8);
      std::memcpy(d, &f, 4);
      return;
    }
    case C::kRGB32ToRGBA32F: {
      std::memcpy(d, s, 12);
      const float one = 1.0f;
      std::memcpy(d + 12, &one, 4);
      return;
    }
    default:
      std::memset(d, 0, 4);
      return;
  }
}

// A 4x4 guest block -> texels (texel_bytes each, 4x4 row-major in out).
void DecodeBlock(Conversion c, const uint8_t* s, uint8_t* out, uint32_t& texel_bytes) {
  switch (c) {
    case C::kDXT1ToRGBA8:
      texel_bytes = 4;
      DecodeBC1(s, out);
      return;
    case C::kDXT3ToRGBA8:
      texel_bytes = 4;
      DecodeBC2(s, out);
      return;
    case C::kDXT5ToRGBA8:
      texel_bytes = 4;
      DecodeBC3(s, out);
      return;
    case C::kDXNToRG8:
      texel_bytes = 2;
      DecodeBC5(s, out);
      return;
    case C::kDXT5AToR8:
      texel_bytes = 1;
      DecodeBC4(s, out);
      return;
    case C::kDXT3AToR8:
      texel_bytes = 1;
      DecodeDXT3A(s, out);
      return;
    case C::kDXT3AAs1111ToRGBA8: {
      texel_bytes = 4;
      uint8_t a[16];
      DecodeDXT3A(s, a);
      for (int i = 0; i < 16; ++i) {
        const uint32_t v = a[i] / 17;  // back to the 4-bit value
        for (int k = 0; k < 4; ++k) out[i * 4 + k] = ((v >> k) & 1) ? 255 : 0;
      }
      return;
    }
    case C::kCTX1ToRG8:
      texel_bytes = 2;
      DecodeCTX1(s, out);
      return;
    default:
      texel_bytes = 4;
      std::memset(out, 0, 64);
      return;
  }
}

uint32_t Rgb565To888(uint16_t c, uint8_t rgb[3]) {
  rgb[0] = uint8_t(Expand(c >> 11, 5, 8));
  rgb[1] = uint8_t(Expand((c >> 5) & 63, 6, 8));
  rgb[2] = uint8_t(Expand(c & 31, 5, 8));
  return 0;
}

void DecodeAlphaBlock(const uint8_t* b, uint8_t out[16]) {
  uint8_t a[8] = {b[0], b[1]};
  if (a[0] > a[1]) {
    for (int k = 1; k < 7; ++k) a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1] + 3) / 7);
  } else {
    for (int k = 1; k < 5; ++k) a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1] + 2) / 5);
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
  for (int i = 0; i < 16; ++i) out[i] = a[(bits >> (3 * i)) & 7];
}

}  // namespace

const HostFormatInfo& GetHostFormatInfo(HostFormat format) {
  return kHostFormats[std::min(size_t(format), size_t(HostFormat::COUNT) - 1)];
}

const char* ConversionName(Conversion conversion) {
  static const char* const kNames[] = {
      "copy",          "swap_rb_565",    "swap_rb_1555",   "swap_rb_4444",     "655_to_rgba8",
      "11_11_10_to_rgba16", "10_11_11_to_rgba16", "11_11_10_to_rgba16s", "10_11_11_to_rgba16s",
      "2_10_10_10_to_rgba16s", "dxt1_to_rgba8", "dxt3_to_rgba8", "dxt5_to_rgba8", "dxn_to_rg8",
      "dxt5a_to_r8",   "dxt3a_to_r8",    "dxt3a_as_1111_to_rgba8", "ctx1_to_rg8", "yuv422_to_rgba8",
      "depth24_to_float", "depth20e4_to_float", "rgb32_to_rgba32f"};
  return kNames[size_t(conversion)];
}

const HostSubresource* HostTextureData::Find(uint32_t level, uint32_t layer) const {
  for (const HostSubresource& s : subresources)
    if (s.level == level && s.layer == layer) return &s;
  return nullptr;
}

void DecodeBC1(const uint8_t* b, uint8_t rgba[64], bool force_four_colors) {
  const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
  uint8_t pal[4][4];
  Rgb565To888(c0, pal[0]);
  Rgb565To888(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || force_four_colors) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k] + 1) / 2);
    pal[2][3] = 255;
    pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
  }
  const uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
  for (int i = 0; i < 16; ++i) std::memcpy(rgba + 4 * i, pal[(bits >> (2 * i)) & 3], 4);
}

void DecodeBC2(const uint8_t* b, uint8_t rgba[64]) {
  DecodeBC1(b + 8, rgba, true);
  for (int i = 0; i < 16; ++i) rgba[4 * i + 3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
}

void DecodeBC3(const uint8_t* b, uint8_t rgba[64]) {
  DecodeBC1(b + 8, rgba, true);
  uint8_t a[16];
  DecodeAlphaBlock(b, a);
  for (int i = 0; i < 16; ++i) rgba[4 * i + 3] = a[i];
}

void DecodeBC4(const uint8_t* b, uint8_t r[16]) { DecodeAlphaBlock(b, r); }

void DecodeBC5(const uint8_t* b, uint8_t rg[32]) {
  uint8_t r[16], g[16];
  DecodeAlphaBlock(b, r);
  DecodeAlphaBlock(b + 8, g);
  for (int i = 0; i < 16; ++i) {
    rg[2 * i] = r[i];
    rg[2 * i + 1] = g[i];
  }
}

void DecodeDXT3A(const uint8_t* b, uint8_t r[16]) {
  for (int i = 0; i < 16; ++i) r[i] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
}

void DecodeCTX1(const uint8_t* b, uint8_t rg[32]) {
  // Two 8:8 end points (green first, then red, as the SDK reads it) and 2-bit indices.
  const uint8_t g0 = b[0], r0 = b[1], g1 = b[2], r1 = b[3];
  const uint8_t r[4] = {r0, r1, uint8_t((2 * r0 + r1) / 3), uint8_t((r0 + 2 * r1) / 3)};
  const uint8_t g[4] = {g0, g1, uint8_t((2 * g0 + g1) / 3), uint8_t((g0 + 2 * g1) / 3)};
  const uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
  for (int i = 0; i < 16; ++i) {
    const uint32_t k = (bits >> (2 * i)) & 3;
    rg[2 * i] = r[k];
    rg[2 * i + 1] = g[k];
  }
}

float UNorm24ToFloat(uint32_t n24) {
  n24 &= 0xFFFFFF;
  return float(n24 + (n24 >> 23)) * (1.0f / float(1 << 24));
}

float Float20e4ToFloat(uint32_t f24) {
  f24 &= 0xFFFFFF;
  if (!f24) return 0.0f;
  uint32_t mantissa = f24 & 0xFFFFF, exponent = f24 >> 20;
  if (!exponent) {
    // Denormal: normalise into the float's exponent.
    uint32_t shift = 0;
    while (!(mantissa & 0x100000)) {
      mantissa <<= 1;
      ++shift;
    }
    exponent = uint32_t(1 - int32_t(shift));
    mantissa &= 0xFFFFF;
  }
  const uint32_t bits = ((exponent + 112) << 23) | (mantissa << 3);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

bool PlanHostTexture(const TextureFetch& fetch, HostTexturePlan& plan, std::string* why) {
  plan = HostTexturePlan();
  if (fetch.Type() != 2) {
    if (why) *why = "not a texture fetch constant (type " + std::to_string(fetch.Type()) + ")";
    return false;
  }
  const TextureFormat format = fetch.Format();
  const FormatRule rule = Rule(format);
  if (rule.unsigned_format == HostFormat::UNKNOWN) {
    if (why) *why = std::string("unsupported format ") + GetFormatInfo(format).name;
    return false;
  }
  const TextureLevels levels = GetTextureLevels(fetch);
  plan.dimension = fetch.Dim();
  plan.width = levels.width;
  plan.height = levels.height;
  plan.depth = plan.dimension == Dimension::k3D ? levels.depth_or_layers : 1;
  plan.layers = plan.dimension == Dimension::k3D ? 1 : levels.depth_or_layers;
  plan.levels = levels.max_level + 1;
  plan.min_level = levels.min_level;
  if (!levels.base_address && !levels.mip_address) {
    if (why) *why = "no base or mip address";
    return false;
  }

  // Signs of the components the swizzle reads.
  const uint16_t swizzle = fetch.Swizzle();
  bool any_signed = false, any_unsigned = false, any_bias = false, any_gamma = false;
  for (int i = 0; i < 4; ++i) {
    const uint8_t s = SwizzleComponent(swizzle, i);
    if (s >= 4) continue;
    switch (fetch.Sign(s)) {
      case TextureSign::kSigned:
        any_signed = true;
        break;
      case TextureSign::kUnsignedBiased:
        any_bias = true;
        any_unsigned = true;
        break;
      case TextureSign::kGamma:
        any_gamma = true;
        any_unsigned = true;
        break;
      default:
        any_unsigned = true;
        break;
    }
  }
  const bool use_signed = any_signed && !any_unsigned && rule.signed_format != HostFormat::UNKNOWN;
  plan.format = use_signed ? rule.signed_format : rule.unsigned_format;
  plan.conversion = use_signed ? rule.signed_conversion : rule.conversion;
  if (any_bias) plan.shader_flags |= kShaderBias;
  if (any_gamma) plan.shader_flags |= kShaderGamma;
  if (any_signed && !use_signed) plan.shader_flags |= kShaderMixedSigns;
  if (fetch.ExpAdjust() != 0) plan.shader_flags |= kShaderExpAdjust;
  if (fetch.NumFormatInteger() && plan.format != HostFormat::R32_FLOAT && plan.format != HostFormat::RG32_FLOAT &&
      plan.format != HostFormat::RGBA32_FLOAT && plan.format != HostFormat::R16_FLOAT &&
      plan.format != HostFormat::RG16_FLOAT && plan.format != HostFormat::RGBA16_FLOAT)
    plan.shader_flags |= kShaderInteger;

  // Host BC textures need a level 0 that is a multiple of 4 (D3D12); otherwise decode to texels.
  if (GetHostFormatInfo(plan.format).block_size == 4 && ((plan.width & 3) || (plan.height & 3))) {
    if (rule.decompressed_format == HostFormat::UNKNOWN) {
      if (why) *why = "block format with an unaligned size and no decoder";
      return false;
    }
    plan.format = rule.decompressed_format;
    plan.conversion = rule.decompress;
    plan.decompressed = true;
  }
  if (BlockDecode(plan.conversion)) plan.decompressed = true;

  // SRV mapping: each output takes the fetch swizzle's source component, through the format's replication.
  uint16_t view = 0;
  for (int i = 0; i < 4; ++i) {
    uint8_t s = SwizzleComponent(swizzle, i);
    if (s < 4) s = SwizzleComponent(rule.replicate, s);
    else if (s > 5) s = 4;
    view |= uint16_t(s) << (3 * i);
  }
  plan.view_swizzle = view;
  return true;
}

TextureRanges GetTextureRanges(const TextureFetch& fetch) {
  TextureRanges r;
  const GuestLayout layout = ComputeGuestLayout(fetch);
  const TextureLevels levels = GetTextureLevels(fetch);
  if (levels.base_address && layout.has_base) {
    r.base = levels.base_address;
    r.base_bytes = layout.base_extent_bytes;
  }
  if (levels.mip_address && levels.max_level > 0) {
    r.mip = levels.mip_address;
    r.mip_bytes = layout.mips_extent_bytes;
  }
  return r;
}

bool ConvertTexture(const TextureFetch& fetch, const GuestMemory& memory, HostTextureData& out, std::string* why) {
  out = HostTextureData();
  if (!PlanHostTexture(fetch, out.plan, why)) return false;
  const HostTexturePlan& plan = out.plan;
  const GuestLayout layout = ComputeGuestLayout(fetch);
  const TextureRanges ranges = GetTextureRanges(fetch);
  const FormatInfo& guest = GetFormatInfo(fetch.Format());
  const uint32_t guest_bpb = guest.BytesPerBlock();
  const HostFormatInfo& host = GetHostFormatInfo(plan.format);
  const Endian endian = fetch.EndianMode();

  // Endian-swapped copies of the base and mip regions (both start 4 KB aligned, so the 16- and 32-bit units
  // of the swap line up with memory as the GPU reads it).
  std::vector<uint8_t> regions[2];
  const uint32_t region_address[2] = {ranges.base, ranges.mip};
  const uint32_t region_bytes[2] = {ranges.base_bytes, ranges.mip_bytes};
  for (int r = 0; r < 2; ++r) {
    if (!region_bytes[r]) continue;
    uint32_t bytes = region_bytes[r];
    const uint8_t* src = memory.At(region_address[r], bytes);
    if (!src) {
      // Clamp to the end of guest memory; missing bytes read as zero.
      if (region_address[r] >= memory.size || !memory.base) {
        if (why) *why = "texture memory outside guest memory";
        return false;
      }
      bytes = uint32_t(memory.size - region_address[r]);
      src = memory.base + region_address[r];
    }
    regions[r].assign(size_t(region_bytes[r]) + 16, 0);
    CopySwap(endian, src, regions[r].data(), bytes & ~3u);
  }

  const bool decode_blocks = BlockDecode(plan.conversion);
  const bool yuv = plan.conversion == C::kYUV422ToRGBA8;
  for (uint32_t level = plan.min_level; level < plan.levels; ++level) {
    for (uint32_t layer = 0; layer < plan.layers; ++layer) {
      LevelSource src;
      if (!GetLevelSource(layout, level, layer, src)) continue;
      const std::vector<uint8_t>& region = regions[src.from_mips ? 1 : 0];
      if (region.empty()) continue;

      HostSubresource sub;
      sub.level = level;
      sub.layer = layer;
      sub.width = std::max(plan.width >> level, 1u);
      sub.height = std::max(plan.height >> level, 1u);
      sub.depth = src.depth;
      const uint32_t host_blocks_x = DivUp(sub.width, host.block_size);
      const uint32_t host_blocks_y = DivUp(sub.height, host.block_size);
      sub.row_pitch = host_blocks_x * host.bytes_per_block;
      sub.depth_pitch = sub.row_pitch * host_blocks_y;
      sub.size = size_t(sub.depth_pitch) * sub.depth;
      sub.offset = out.bytes.size();
      out.bytes.resize(sub.offset + sub.size, 0);
      uint8_t* dst = out.bytes.data() + sub.offset;

      const uint8_t zero_block[16] = {};
      auto guest_block = [&](uint32_t bx, uint32_t by, uint32_t z) -> const uint8_t* {
        const size_t o = size_t(src.layer_offset_bytes) +
                         BlockOffset(layout, *src.storage, bx + src.x_blocks, by + src.y_blocks, z + src.z);
        return o + guest_bpb <= region.size() - 16 ? region.data() + o : zero_block;
      };

      for (uint32_t z = 0; z < sub.depth; ++z) {
        uint8_t* slice = dst + size_t(z) * sub.depth_pitch;
        for (uint32_t by = 0; by < src.height_blocks; ++by) {
          for (uint32_t bx = 0; bx < src.width_blocks; ++bx) {
            const uint8_t* g = guest_block(bx, by, z);
            if (decode_blocks) {
              uint8_t texels[64];
              uint32_t tb = 4;
              DecodeBlock(plan.conversion, g, texels, tb);
              for (uint32_t ty = 0; ty < 4 && by * 4 + ty < sub.height; ++ty)
                for (uint32_t tx = 0; tx < 4 && bx * 4 + tx < sub.width; ++tx)
                  std::memcpy(slice + size_t(by * 4 + ty) * sub.row_pitch + size_t(bx * 4 + tx) * tb,
                              texels + (ty * 4 + tx) * tb, tb);
            } else if (yuv) {
              // Two texels per 32-bit block; host R = byte 2 (or 3), G = the texel's luma, B = byte 0 (or 1).
              const bool y1_cr_y0_cb = fetch.Format() == TextureFormat::k_Y1_Cr_Y0_Cb_REP;
              for (uint32_t t = 0; t < 2 && bx * 2 + t < sub.width; ++t) {
                uint8_t* d = slice + size_t(by) * sub.row_pitch + size_t(bx * 2 + t) * 4;
                if (y1_cr_y0_cb) {
                  d[0] = g[2], d[1] = t ? g[3] : g[1], d[2] = g[0];
                } else {
                  d[0] = g[3], d[1] = t ? g[2] : g[0], d[2] = g[1];
                }
                d[3] = 255;
              }
            } else {
              ConvertTexel(plan.conversion, g,
                           slice + size_t(by) * sub.row_pitch + size_t(bx) * host.bytes_per_block, guest_bpb);
            }
          }
        }
      }
      out.subresources.push_back(sub);
    }
  }
  if (out.subresources.empty()) {
    if (why) *why = "no level could be read";
    return false;
  }
  return true;
}

}  // namespace kknr
