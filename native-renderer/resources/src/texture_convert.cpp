#include "kknr/texture_convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "kknr/endian.h"
#include "kknr/tiling.h"

namespace kknr {

namespace {

const HostFormatInfo kHostFormats[] = {
    {"UNKNOWN", 1, 0},          {"R8_UNORM", 1, 1},       {"R8_SNORM", 1, 1},        {"RG8_UNORM", 1, 2},
    {"RG8_SNORM", 1, 2},        {"R16_UNORM", 1, 2},      {"R16_SNORM", 1, 2},       {"R16_FLOAT", 1, 2},
    {"BGRA4_UNORM", 1, 2},      {"B5G6R5_UNORM", 1, 2},   {"B5G5R5A1_UNORM", 1, 2},  {"RGBA8_UNORM", 1, 4},
    {"RGBA8_SNORM", 1, 4},      {"R10G10B10A2_UNORM", 1, 4}, {"RG16_UNORM", 1, 4},   {"RG16_SNORM", 1, 4},
    {"RG16_FLOAT", 1, 4},       {"R32_UINT", 1, 4},       {"R32_SINT", 1, 4},        {"R32_FLOAT", 1, 4},
    {"RGBA16_FLOAT", 1, 8},     {"RGBA16_UNORM", 1, 8},   {"RGBA16_SNORM", 1, 8},    {"RG32_UINT", 1, 8},
    {"RG32_SINT", 1, 8},        {"RG32_FLOAT", 1, 8},     {"RGBA32_UINT", 1, 16},    {"RGBA32_SINT", 1, 16},
    {"RGBA32_FLOAT", 1, 16},    {"BC1_UNORM", 4, 8},      {"BC2_UNORM", 4, 16},      {"BC3_UNORM", 4, 16},
    {"BC4_UNORM", 4, 8},        {"BC4_SNORM", 4, 8},      {"BC5_UNORM", 4, 16},      {"BC5_SNORM", 4, 16},
};
static_assert(sizeof(kHostFormats) / sizeof(kHostFormats[0]) == size_t(HostFormat::COUNT), "host format table");

constexpr uint16_t kRRRR = MakeSwizzle(0, 0, 0, 0);
constexpr uint16_t kRGGG = MakeSwizzle(0, 1, 1, 1);
constexpr uint16_t kRGBB = MakeSwizzle(0, 1, 2, 2);
constexpr uint16_t kRGBA = MakeSwizzle(0, 1, 2, 3);

using C = Conversion;
using H = HostFormat;
using T = TextureFormat;

FormatChoice Choice(H format, C conversion, H signed_format, C signed_conversion, uint16_t replicate,
                    H decompressed = H::UNKNOWN, C decompress = C::kCopy, H decompressed_signed = H::UNKNOWN,
                    C decompress_signed = C::kCopy, uint8_t flags = kShaderNone) {
  FormatChoice c;
  c.format = format;
  c.conversion = conversion;
  c.signed_format = signed_format;
  c.signed_conversion = signed_conversion;
  c.replicate = replicate;
  c.decompressed_format = decompressed;
  c.decompress = decompress;
  c.decompressed_signed_format = decompressed_signed;
  c.decompress_signed = decompress_signed;
  c.extra_shader_flags = flags;
  return c;
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

void DecodeSignedAlphaBlock(const uint8_t* b, int8_t out[16]) {
  // BC4_SNORM: signed end points (-128 reads as -127), 6 interpolated values when e0 > e1, else 4 plus -1, +1.
  const int32_t e0 = std::max(int32_t(int8_t(b[0])), -127), e1 = std::max(int32_t(int8_t(b[1])), -127);
  int32_t a[8] = {e0, e1};
  auto lerp = [](int32_t x, int32_t y, int k, int n) {
    return int32_t(std::lround(double((n - k) * x + k * y) / n));
  };
  if (int8_t(b[0]) > int8_t(b[1])) {
    for (int k = 1; k < 7; ++k) a[k + 1] = lerp(e0, e1, k, 7);
  } else {
    for (int k = 1; k < 5; ++k) a[k + 1] = lerp(e0, e1, k, 5);
    a[6] = -127;
    a[7] = 127;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
  for (int i = 0; i < 16; ++i) out[i] = int8_t(a[(bits >> (3 * i)) & 7]);
}

// Bytes per decoded texel for the block-decoding conversions.
uint32_t DecodedTexelBytes(Conversion c, TextureFormat guest) {
  switch (c) {
    case C::kDXT1ToRGBA8:
    case C::kDXT3ToRGBA8:
    case C::kDXT5ToRGBA8:
    case C::kDXT3AAs1111ToRGBA8:
    case C::kCrY1CbY0ToRGBA8:
    case C::kY1CrY0CbToRGBA8:
      return 4;
    case C::kDXNToRG8:
    case C::kDXNToRG8S:
    case C::kCTX1ToRG8:
      return 2;
    case C::kSplitBlockToTexels: {
      const FormatInfo& info = GetFormatInfo(guest);
      return info.BytesPerBlock() / (info.block_width * info.block_height);
    }
    default:
      return 1;
  }
}

// One guest block -> its texels (row-major over the block's footprint), DecodedTexelBytes each.
void DecodeBlockTexels(Conversion c, TextureFormat guest, const uint8_t* s, uint8_t* out) {
  switch (c) {
    case C::kDXT1ToRGBA8:
      DecodeBC1(s, out);
      return;
    case C::kDXT3ToRGBA8:
      DecodeBC2(s, out);
      return;
    case C::kDXT5ToRGBA8:
      DecodeBC3(s, out);
      return;
    case C::kDXNToRG8:
      DecodeBC5(s, out);
      return;
    case C::kDXNToRG8S:
      DecodeBC5Signed(s, reinterpret_cast<int8_t*>(out));
      return;
    case C::kDXT5AToR8:
      DecodeBC4(s, out);
      return;
    case C::kDXT5AToR8S:
      DecodeBC4Signed(s, reinterpret_cast<int8_t*>(out));
      return;
    case C::kDXT3AToR8:
      DecodeDXT3A(s, out);
      return;
    case C::kDXT3AAs1111ToRGBA8: {
      // Each 4-bit value spreads over the four components: bit 0 -> X, 1 -> Y, 2 -> Z, 3 -> W.
      for (int i = 0; i < 16; ++i) {
        const uint32_t v = (s[i / 2] >> (4 * (i & 1))) & 15;
        for (int k = 0; k < 4; ++k) out[i * 4 + k] = ((v >> k) & 1) ? 255 : 0;
      }
      return;
    }
    case C::kCTX1ToRG8:
      DecodeCTX1(s, out);
      return;
    case C::kCrY1CbY0ToRGBA8:
    case C::kY1CrY0CbToRGBA8:
      // Two texels per 32-bit block (bytes in host order, byte 0 lowest). Cr_Y1_Cb_Y0: byte 3 Cr, 2 Y1, 1 Cb,
      // 0 Y0. Y1_Cr_Y0_Cb: byte 3 Y1, 2 Cr, 1 Y0, 0 Cb. Host R = Cr, G = the texel's Y, B = Cb, A = 1.
      for (int t = 0; t < 2; ++t) {
        uint8_t* d = out + 4 * t;
        if (c == C::kCrY1CbY0ToRGBA8) {
          d[0] = s[3], d[1] = t ? s[2] : s[0], d[2] = s[1];
        } else {
          d[0] = s[2], d[1] = t ? s[3] : s[1], d[2] = s[0];
        }
        d[3] = 255;
      }
      return;
    case C::k1ToR8:
    case C::k1ReverseToR8:
      for (int t = 0; t < 8; ++t) {
        const int bit = c == C::k1ToR8 ? t : 7 - t;
        out[t] = ((s[0] >> bit) & 1) ? 255 : 0;
      }
      return;
    case C::kSplitBlockToTexels: {
      std::memcpy(out, s, GetFormatInfo(guest).BytesPerBlock());
      return;
    }
    default:
      return;
  }
}

// One guest block -> one host block of the same footprint.
void ConvertTexel(Conversion c, const uint8_t* s, uint8_t* d, uint32_t guest_bpb) {
  switch (c) {
    case C::kCopy:
      std::memcpy(d, s, guest_bpb);
      return;
    case C::k565ToRGBA8:
    case C::k1555ToRGBA8:
    case C::k4444ToRGBA8: {
      // Fields from bit 0 up in X, Y, Z, W order; widened by bit replication (exact at 0 and the maximum).
      static const uint8_t kBits[3][4] = {{5, 6, 5, 0}, {5, 5, 5, 1}, {4, 4, 4, 4}};
      const uint8_t* bits = kBits[c == C::k565ToRGBA8 ? 0 : (c == C::k1555ToRGBA8 ? 1 : 2)];
      uint16_t v;
      std::memcpy(&v, s, 2);
      uint32_t shift = 0;
      for (int i = 0; i < 4; ++i) {
        if (!bits[i]) {
          d[i] = 255;
          continue;
        }
        d[i] = uint8_t(Expand((v >> shift) & ((1u << bits[i]) - 1), bits[i], 8));
        shift += bits[i];
      }
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
      uint32_t bits[3], fields[3];
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
    case C::kFixed16ToHalf: {
      // Fixed point -32..32: the SDK's convention, snorm16 scaled by 32.
      for (uint32_t i = 0; i < guest_bpb / 2; ++i) {
        int16_t v;
        std::memcpy(&v, s + 2 * i, 2);
        const float f = float(std::max<int32_t>(v, -32767)) * (32.0f / 32767.0f);
        const uint16_t h = FloatToHalf(f);
        std::memcpy(d + 2 * i, &h, 2);
      }
      return;
    }
    case C::k7e3ToRGBA16F: {
      uint32_t v;
      std::memcpy(&v, s, 4);
      uint16_t out[4];
      for (int i = 0; i < 3; ++i) out[i] = FloatToHalf(Float7e3ToFloat(v >> (10 * i)));
      out[3] = FloatToHalf(float(v >> 30) / 3.0f);
      Store16x4(d, out);
      return;
    }
    default:
      std::memset(d, 0, guest_bpb);
      return;
  }
}

}  // namespace

const HostFormatInfo& GetHostFormatInfo(HostFormat format) {
  return kHostFormats[std::min(size_t(format), size_t(HostFormat::COUNT) - 1)];
}

const char* ConversionName(Conversion conversion) {
  static const char* const kNames[] = {
      "copy",
      "565_to_rgba8",
      "1555_to_rgba8",
      "4444_to_rgba8",
      "655_to_rgba8",
      "11_11_10_to_rgba16",
      "10_11_11_to_rgba16",
      "11_11_10_to_rgba16s",
      "10_11_11_to_rgba16s",
      "2_10_10_10_to_rgba16s",
      "dxt1_to_rgba8",
      "dxt3_to_rgba8",
      "dxt5_to_rgba8",
      "dxn_to_rg8",
      "dxn_to_rg8s",
      "dxt5a_to_r8",
      "dxt5a_to_r8s",
      "dxt3a_to_r8",
      "dxt3a_as_1111_to_rgba8",
      "ctx1_to_rg8",
      "cr_y1_cb_y0_to_rgba8",
      "y1_cr_y0_cb_to_rgba8",
      "depth24_to_float",
      "depth20e4_to_float",
      "rgb32_to_rgba32f",
      "1_to_r8",
      "1_reverse_to_r8",
      "split_block_to_texels",
      "fixed16_to_half",
      "7e3_to_rgba16f",
  };
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == size_t(Conversion::COUNT), "conversion names");
  return kNames[std::min(size_t(conversion), size_t(Conversion::COUNT) - 1)];
}

bool ConversionDecodesBlocks(Conversion c) {
  switch (c) {
    case C::kDXT1ToRGBA8:
    case C::kDXT3ToRGBA8:
    case C::kDXT5ToRGBA8:
    case C::kDXNToRG8:
    case C::kDXNToRG8S:
    case C::kDXT5AToR8:
    case C::kDXT5AToR8S:
    case C::kDXT3AToR8:
    case C::kDXT3AAs1111ToRGBA8:
    case C::kCTX1ToRG8:
    case C::kCrY1CbY0ToRGBA8:
    case C::kY1CrY0CbToRGBA8:
    case C::k1ToR8:
    case C::k1ReverseToR8:
    case C::kSplitBlockToTexels:
      return true;
    default:
      return false;
  }
}

FormatChoice GetFormatChoice(TextureFormat format) {
  switch (format) {
    case T::k_1_REVERSE:
      return Choice(H::R8_UNORM, C::k1ReverseToR8, H::UNKNOWN, C::kCopy, kRRRR);
    case T::k_1:
      return Choice(H::R8_UNORM, C::k1ToR8, H::UNKNOWN, C::kCopy, kRRRR);
    case T::k_8:
    case T::k_8_A:
    case T::k_8_B:
    case T::k_8_INTERLACED:
      return Choice(H::R8_UNORM, C::kCopy, H::R8_SNORM, C::kCopy, kRRRR);
    case T::k_1_5_5_5:
      return Choice(H::RGBA8_UNORM, C::k1555ToRGBA8, H::UNKNOWN, C::kCopy, kRGBA);
    case T::k_5_6_5:
      return Choice(H::RGBA8_UNORM, C::k565ToRGBA8, H::UNKNOWN, C::kCopy, kRGBB);
    case T::k_6_5_5:
      return Choice(H::RGBA8_UNORM, C::k655ToRGBA8, H::UNKNOWN, C::kCopy, kRGBB);
    case T::k_8_8_8_8:
    case T::k_8_8_8_8_A:
    case T::k_8_8_8_8_AS_16_16_16_16:
      return Choice(H::RGBA8_UNORM, C::kCopy, H::RGBA8_SNORM, C::kCopy, kRGBA);
    case T::k_8_8_8_8_GAMMA_EDRAM:
      return Choice(H::RGBA8_UNORM, C::kCopy, H::UNKNOWN, C::kCopy, kRGBA, H::UNKNOWN, C::kCopy, H::UNKNOWN,
                    C::kCopy, kShaderGamma);
    case T::k_2_10_10_10:
    case T::k_2_10_10_10_AS_16_16_16_16:
      return Choice(H::R10G10B10A2_UNORM, C::kCopy, H::RGBA16_SNORM, C::k2_10_10_10ToRGBA16S, kRGBA);
    case T::k_2_10_10_10_FLOAT_EDRAM:
      return Choice(H::RGBA16_FLOAT, C::k7e3ToRGBA16F, H::UNKNOWN, C::kCopy, kRGBA);
    case T::k_8_8:
      return Choice(H::RG8_UNORM, C::kCopy, H::RG8_SNORM, C::kCopy, kRGGG);
    case T::k_Cr_Y1_Cb_Y0_REP:
      return Choice(H::RGBA8_UNORM, C::kCrY1CbY0ToRGBA8, H::UNKNOWN, C::kCopy, kRGBB);
    case T::k_Y1_Cr_Y0_Cb_REP:
      return Choice(H::RGBA8_UNORM, C::kY1CrY0CbToRGBA8, H::UNKNOWN, C::kCopy, kRGBB);
    case T::k_16_16_EDRAM:
      return Choice(H::RG16_FLOAT, C::kFixed16ToHalf, H::RG16_FLOAT, C::kFixed16ToHalf, kRGGG);
    case T::k_16_16_16_16_EDRAM:
      return Choice(H::RGBA16_FLOAT, C::kFixed16ToHalf, H::RGBA16_FLOAT, C::kFixed16ToHalf, kRGBA);
    case T::k_4_4_4_4:
      return Choice(H::RGBA8_UNORM, C::k4444ToRGBA8, H::UNKNOWN, C::kCopy, kRGBA);
    case T::k_10_11_11:
    case T::k_10_11_11_AS_16_16_16_16:
      return Choice(H::RGBA16_UNORM, C::k11_11_10ToRGBA16, H::RGBA16_SNORM, C::k11_11_10ToRGBA16S, kRGBB);
    case T::k_11_11_10:
    case T::k_11_11_10_AS_16_16_16_16:
      return Choice(H::RGBA16_UNORM, C::k10_11_11ToRGBA16, H::RGBA16_SNORM, C::k10_11_11ToRGBA16S, kRGBB);
    case T::k_DXT1:
    case T::k_DXT1_AS_16_16_16_16:
      return Choice(H::BC1_UNORM, C::kCopy, H::UNKNOWN, C::kCopy, kRGBA, H::RGBA8_UNORM, C::kDXT1ToRGBA8);
    case T::k_DXT2_3:
    case T::k_DXT2_3_AS_16_16_16_16:
      return Choice(H::BC2_UNORM, C::kCopy, H::UNKNOWN, C::kCopy, kRGBA, H::RGBA8_UNORM, C::kDXT3ToRGBA8);
    case T::k_DXT4_5:
    case T::k_DXT4_5_AS_16_16_16_16:
      return Choice(H::BC3_UNORM, C::kCopy, H::UNKNOWN, C::kCopy, kRGBA, H::RGBA8_UNORM, C::kDXT5ToRGBA8);
    case T::k_DXN:
      return Choice(H::BC5_UNORM, C::kCopy, H::BC5_SNORM, C::kCopy, kRGGG, H::RG8_UNORM, C::kDXNToRG8,
                    H::RG8_SNORM, C::kDXNToRG8S);
    case T::k_DXT5A:
      return Choice(H::BC4_UNORM, C::kCopy, H::BC4_SNORM, C::kCopy, kRRRR, H::R8_UNORM, C::kDXT5AToR8,
                    H::R8_SNORM, C::kDXT5AToR8S);
    case T::k_DXT3A:
      return Choice(H::R8_UNORM, C::kDXT3AToR8, H::UNKNOWN, C::kCopy, kRRRR);
    case T::k_DXT3A_AS_1_1_1_1:
      return Choice(H::RGBA8_UNORM, C::kDXT3AAs1111ToRGBA8, H::UNKNOWN, C::kCopy, kRGBA);
    case T::k_CTX1:
      return Choice(H::RG8_UNORM, C::kCTX1ToRG8, H::UNKNOWN, C::kCopy, kRGGG);
    case T::k_24_8:
      return Choice(H::R32_FLOAT, C::kDepth24ToFloat, H::R32_FLOAT, C::kDepth24ToFloat, kRRRR);
    case T::k_24_8_FLOAT:
      return Choice(H::R32_FLOAT, C::kDepth20e4ToFloat, H::R32_FLOAT, C::kDepth20e4ToFloat, kRRRR);
    case T::k_16:
    case T::k_16_MPEG:
    case T::k_16_INTERLACED:
    case T::k_16_MPEG_INTERLACED:
      return Choice(H::R16_UNORM, C::kCopy, H::R16_SNORM, C::kCopy, kRRRR);
    case T::k_16_16:
    case T::k_16_16_MPEG:
    case T::k_16_16_MPEG_INTERLACED:
      return Choice(H::RG16_UNORM, C::kCopy, H::RG16_SNORM, C::kCopy, kRGGG);
    case T::k_16_16_16_16:
      return Choice(H::RGBA16_UNORM, C::kCopy, H::RGBA16_SNORM, C::kCopy, kRGBA);
    case T::k_16_EXPAND:
    case T::k_16_FLOAT:
      return Choice(H::R16_FLOAT, C::kCopy, H::R16_FLOAT, C::kCopy, kRRRR);
    case T::k_16_16_EXPAND:
    case T::k_16_16_FLOAT:
      return Choice(H::RG16_FLOAT, C::kCopy, H::RG16_FLOAT, C::kCopy, kRGGG);
    case T::k_16_16_16_16_EXPAND:
    case T::k_16_16_16_16_FLOAT:
      return Choice(H::RGBA16_FLOAT, C::kCopy, H::RGBA16_FLOAT, C::kCopy, kRGBA);
    case T::k_32:
      return Choice(H::R32_UINT, C::kCopy, H::R32_SINT, C::kCopy, kRRRR);
    case T::k_32_32:
      return Choice(H::RG32_UINT, C::kCopy, H::RG32_SINT, C::kCopy, kRGGG);
    case T::k_32_32_32_32:
      return Choice(H::RGBA32_UINT, C::kCopy, H::RGBA32_SINT, C::kCopy, kRGBA);
    case T::k_32_FLOAT:
      return Choice(H::R32_FLOAT, C::kCopy, H::R32_FLOAT, C::kCopy, kRRRR);
    case T::k_32_32_FLOAT:
      return Choice(H::RG32_FLOAT, C::kCopy, H::RG32_FLOAT, C::kCopy, kRGGG);
    case T::k_32_32_32_32_FLOAT:
      return Choice(H::RGBA32_FLOAT, C::kCopy, H::RGBA32_FLOAT, C::kCopy, kRGBA);
    case T::k_32_32_32_FLOAT:
      return Choice(H::RGBA32_FLOAT, C::kRGB32ToRGBA32F, H::RGBA32_FLOAT, C::kRGB32ToRGBA32F, kRGBB);
    case T::k_32_AS_8:
    case T::k_32_AS_8_INTERLACED:
      return Choice(H::R8_UNORM, C::kSplitBlockToTexels, H::R8_SNORM, C::kSplitBlockToTexels, kRRRR);
    case T::k_32_AS_8_8:
      return Choice(H::RG8_UNORM, C::kSplitBlockToTexels, H::RG8_SNORM, C::kSplitBlockToTexels, kRGGG);
    case T::k_32_AS_8_8_INTERLACED:
      // The SDK's table gives this one a 1x1 block of 16 bits (unlike k_32_AS_8_8): one 8:8 texel per block.
      return Choice(H::RG8_UNORM, C::kCopy, H::RG8_SNORM, C::kCopy, kRGGG);
  }
  return FormatChoice();
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

void DecodeBC4Signed(const uint8_t* b, int8_t r[16]) { DecodeSignedAlphaBlock(b, r); }

void DecodeBC5(const uint8_t* b, uint8_t rg[32]) {
  uint8_t r[16], g[16];
  DecodeAlphaBlock(b, r);
  DecodeAlphaBlock(b + 8, g);
  for (int i = 0; i < 16; ++i) {
    rg[2 * i] = r[i];
    rg[2 * i + 1] = g[i];
  }
}

void DecodeBC5Signed(const uint8_t* b, int8_t rg[32]) {
  int8_t r[16], g[16];
  DecodeSignedAlphaBlock(b, r);
  DecodeSignedAlphaBlock(b + 8, g);
  for (int i = 0; i < 16; ++i) {
    rg[2 * i] = r[i];
    rg[2 * i + 1] = g[i];
  }
}

void DecodeDXT3A(const uint8_t* b, uint8_t r[16]) {
  for (int i = 0; i < 16; ++i) r[i] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
}

void DecodeCTX1(const uint8_t* b, uint8_t rg[32]) {
  // Two 8:8 end points (green in the low byte, red in the high one, as the SDK reads it) and 2-bit indices.
  // The in-between values are truncated thirds, like the SDK's CPU decoder.
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
  // As the SDK: n / 2^24 with the top value nudged to exactly 1.
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

float Float7e3ToFloat(uint32_t f10) {
  f10 &= 0x3FF;
  if (!f10) return 0.0f;
  uint32_t mantissa = f10 & 0x7F, exponent = f10 >> 7;
  if (!exponent) {
    uint32_t shift = 0;
    while (!(mantissa & 0x80)) {
      mantissa <<= 1;
      ++shift;
    }
    exponent = uint32_t(1 - int32_t(shift));
    mantissa &= 0x7F;
  }
  const uint32_t bits = ((exponent + 124) << 23) | (mantissa << 16);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint16_t FloatToHalf(float value) {
  uint32_t f;
  std::memcpy(&f, &value, 4);
  const uint32_t sign = (f >> 16) & 0x8000;
  const uint32_t abs = f & 0x7FFFFFFF;
  if (abs >= 0x7F800000) return uint16_t(sign | 0x7C00 | (abs > 0x7F800000 ? 0x200 : 0));  // inf / NaN
  if (abs >= 0x477FF000) return uint16_t(sign | 0x7C00);                                    // overflow
  if (abs < 0x38800000) {
    // Denormal half (or zero): round to nearest even on the shifted mantissa.
    if (abs < 0x33000000) return uint16_t(sign);
    const uint32_t shift = 126 - (abs >> 23);  // 14..24
    const uint32_t mantissa = (abs & 0x7FFFFF) | 0x800000;
    uint32_t h = mantissa >> shift;
    const uint32_t rest = mantissa & ((1u << shift) - 1), half = 1u << (shift - 1);
    if (rest > half || (rest == half && (h & 1))) ++h;
    return uint16_t(sign | h);
  }
  uint32_t h = ((abs >> 13) - (112u << 10));
  const uint32_t rest = abs & 0x1FFF;
  if (rest > 0x1000 || (rest == 0x1000 && (h & 1))) ++h;
  return uint16_t(sign | h);
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exponent = (h >> 10) & 31, mantissa = h & 0x3FF, bits;
  if (exponent == 31) {
    bits = sign | 0x7F800000 | (mantissa << 13);
  } else if (exponent) {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  } else if (mantissa) {
    exponent = 113;
    while (!(mantissa & 0x400)) {
      mantissa <<= 1;
      --exponent;
    }
    bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
  } else {
    bits = sign;
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

bool PlanHostTexture(const TextureFetch& fetch, HostTexturePlan& plan, std::string* why,
                     const TextureOptions& options) {
  plan = HostTexturePlan();
  if (fetch.Type() != 2) {
    if (why) *why = "not a texture fetch constant (type " + std::to_string(fetch.Type()) + ")";
    return false;
  }
  const TextureFormat format = fetch.Format();
  const FormatChoice rule = GetFormatChoice(format);
  if (rule.format == HostFormat::UNKNOWN) {
    if (why) *why = std::string("unsupported format ") + GetFormatInfo(format).name;
    return false;
  }
  const TextureLevels levels = GetTextureLevels(fetch, options);
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
  plan.format = use_signed ? rule.signed_format : rule.format;
  plan.conversion = use_signed ? rule.signed_conversion : rule.conversion;
  plan.shader_flags = rule.extra_shader_flags;
  if (any_bias) plan.shader_flags |= kShaderBias;
  if (any_gamma) plan.shader_flags |= kShaderGamma;
  if (any_signed && !use_signed) plan.shader_flags |= kShaderMixedSigns;
  if (fetch.ExpAdjust() != 0) plan.shader_flags |= kShaderExpAdjust;
  const bool is_float = plan.format == HostFormat::R32_FLOAT || plan.format == HostFormat::RG32_FLOAT ||
                        plan.format == HostFormat::RGBA32_FLOAT || plan.format == HostFormat::R16_FLOAT ||
                        plan.format == HostFormat::RG16_FLOAT || plan.format == HostFormat::RGBA16_FLOAT;
  if (fetch.NumFormatInteger() && !is_float) plan.shader_flags |= kShaderInteger;
  if (!fetch.NumFormatInteger() &&
      (format == TextureFormat::k_32 || format == TextureFormat::k_32_32 || format == TextureFormat::k_32_32_32_32))
    plan.shader_flags |= kShaderNorm32;

  // Host BC textures need a level 0 that is a multiple of 4 (D3D12); otherwise decode to texels.
  if (GetHostFormatInfo(plan.format).block_size == 4 && ((plan.width & 3) || (plan.height & 3))) {
    if (use_signed && rule.decompressed_signed_format != HostFormat::UNKNOWN) {
      plan.format = rule.decompressed_signed_format;
      plan.conversion = rule.decompress_signed;
    } else if (rule.decompressed_format != HostFormat::UNKNOWN) {
      plan.format = rule.decompressed_format;
      plan.conversion = rule.decompress;
    } else {
      if (why) *why = "block format with an unaligned size and no decoder";
      return false;
    }
  }
  plan.decompressed = ConversionDecodesBlocks(plan.conversion) && GetFormatInfo(format).kind == FormatKind::kBlock4x4;

  // SRV mapping: each output takes the fetch swizzle's source component, through the format's replication.
  uint16_t view = 0;
  for (int i = 0; i < 4; ++i) {
    uint8_t s = SwizzleComponent(swizzle, i);
    if (s < 4)
      s = SwizzleComponent(rule.replicate, s);
    else if (s > 5)
      s = 4;
    view |= uint16_t(s) << (3 * i);
  }
  plan.view_swizzle = view;
  return true;
}

TextureRanges GetTextureRanges(const TextureFetch& fetch, const TextureOptions& options) {
  TextureRanges r;
  const GuestLayout layout = ComputeGuestLayout(fetch, options);
  const TextureLevels levels = GetTextureLevels(fetch, options);
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

void ConvertBlocks(Conversion conversion, TextureFormat guest_format, HostFormat host_format, const uint8_t* blocks,
                   const BlockExtent& extent, uint32_t width, uint32_t height, uint8_t* dst, uint32_t row_pitch,
                   uint32_t depth_pitch) {
  const FormatInfo& guest = GetFormatInfo(guest_format);
  const uint32_t guest_bpb = guest.BytesPerBlock();
  const uint32_t host_bpb = GetHostFormatInfo(host_format).bytes_per_block;
  const uint8_t* src = blocks;
  if (ConversionDecodesBlocks(conversion)) {
    const uint32_t bw = guest.block_width, bh = guest.block_height;
    const uint32_t tb = DecodedTexelBytes(conversion, guest_format);
    uint8_t texels[128];
    for (uint32_t z = 0; z < extent.depth; ++z) {
      uint8_t* slice = dst + size_t(z) * depth_pitch;
      for (uint32_t by = 0; by < extent.height_blocks; ++by)
        for (uint32_t bx = 0; bx < extent.width_blocks; ++bx, src += guest_bpb) {
          DecodeBlockTexels(conversion, guest_format, src, texels);
          for (uint32_t ty = 0; ty < bh && by * bh + ty < height; ++ty)
            for (uint32_t tx = 0; tx < bw && bx * bw + tx < width; ++tx)
              std::memcpy(slice + size_t(by * bh + ty) * row_pitch + size_t(bx * bw + tx) * tb,
                          texels + (ty * bw + tx) * tb, tb);
        }
    }
    return;
  }
  for (uint32_t z = 0; z < extent.depth; ++z) {
    uint8_t* slice = dst + size_t(z) * depth_pitch;
    for (uint32_t by = 0; by < extent.height_blocks; ++by)
      for (uint32_t bx = 0; bx < extent.width_blocks; ++bx, src += guest_bpb)
        ConvertTexel(conversion, src, slice + size_t(by) * row_pitch + size_t(bx) * host_bpb, guest_bpb);
  }
}

bool ConvertTexture(const TextureFetch& fetch, const GuestMemory& memory, HostTextureData& out, std::string* why,
                    const TextureOptions& options) {
  out = HostTextureData();
  if (!PlanHostTexture(fetch, out.plan, why, options)) return false;
  const HostTexturePlan& plan = out.plan;
  const GuestLayout layout = ComputeGuestLayout(fetch, options);
  const TextureRanges ranges = GetTextureRanges(fetch, options);
  const HostFormatInfo& host = GetHostFormatInfo(plan.format);

  std::vector<uint8_t> base, mips;
  if ((ranges.base_bytes && !LoadSwappedRegion(memory, ranges.base, ranges.base_bytes, fetch.EndianMode(), base)) ||
      (ranges.mip_bytes && !LoadSwappedRegion(memory, ranges.mip, ranges.mip_bytes, fetch.EndianMode(), mips))) {
    if (why) *why = "texture memory outside guest memory";
    return false;
  }
  GuestRegions regions;
  regions.base = base.data();
  regions.base_size = base.size();
  regions.mips = mips.data();
  regions.mips_size = mips.size();

  std::vector<uint8_t> blocks;
  for (uint32_t level = plan.min_level; level < plan.levels; ++level) {
    if (level == 0 && !ranges.base_bytes) continue;
    if (level > 0 && !ranges.mip_bytes && !layout.mips_in_base_tail) break;
    for (uint32_t layer = 0; layer < plan.layers; ++layer) {
      BlockExtent extent;
      if (!ReadGuestBlocks(layout, regions, level, layer, blocks, extent)) continue;
      HostSubresource sub;
      sub.level = level;
      sub.layer = layer;
      sub.width = std::max(plan.width >> level, 1u);
      sub.height = std::max(plan.height >> level, 1u);
      sub.depth = extent.depth;
      sub.row_pitch = DivUp(sub.width, host.block_size) * host.bytes_per_block;
      sub.depth_pitch = sub.row_pitch * DivUp(sub.height, host.block_size);
      sub.size = size_t(sub.depth_pitch) * sub.depth;
      sub.offset = out.bytes.size();
      out.bytes.resize(sub.offset + sub.size, 0);
      ConvertBlocks(plan.conversion, fetch.Format(), plan.format, blocks.data(), extent, sub.width, sub.height,
                    out.bytes.data() + sub.offset, sub.row_pitch, sub.depth_pitch);
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
