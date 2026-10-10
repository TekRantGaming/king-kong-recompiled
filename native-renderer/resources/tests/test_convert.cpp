// Converter tests with synthetic data: for each conversion a texel or block written into guest memory the way
// the 360 stores it (big-endian units, the fetch constant's endian mode), converted, and compared with values
// worked out by hand from the format's definition. Then the plan rules (host format, signs, swizzle,
// fallbacks) and an end-to-end pass over all 64 formats.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "kknr/endian.h"
#include "kknr/texture_convert.h"
#include "test.h"

using namespace kknr;
using kknr_test::FakeGuest;
using kknr_test::Rng;

namespace {

constexpr uint32_t kBase = 0x00100000, kMips = 0x00800000;

struct RowOptions {
  TextureSign sign = TextureSign::kUnsigned;
  uint16_t swizzle = kSwizzleXYZW;
  uint32_t height = 1;
  bool integer = false;
};

// A linear 2D texture whose base level's first row starts with `memory` (guest memory order).
HostTextureData ConvertRow(TextureFormat format, Endian endian, uint32_t width, const std::vector<uint8_t>& memory,
                           const RowOptions& o = RowOptions()) {
  static FakeGuest guest;
  std::fill_n(guest.bytes.begin() + kBase, 1u << 16, 0);
  guest.Put(kBase, memory);
  TextureFetchDesc d;
  d.format = format;
  d.endian = endian;
  d.width = width;
  d.height = o.height;
  d.base_address = kBase;
  d.swizzle = o.swizzle;
  d.integer = o.integer;
  for (TextureSign& s : d.signs) s = o.sign;
  HostTextureData out;
  std::string why;
  if (!ConvertTexture(MakeTextureFetch(d), GuestMemory{guest.bytes.data(), guest.bytes.size()}, out, &why))
    kknr_test::Fail(__FILE__, __LINE__, std::string("conversion failed: ") + why);
  return out;
}

const uint8_t* Texel(const HostTextureData& t, uint32_t x, uint32_t y = 0) {
  const HostSubresource* s = t.Find(0, 0);
  if (!s) return nullptr;
  const uint32_t bpb = GetHostFormatInfo(t.plan.format).bytes_per_block;
  return t.bytes.data() + s->offset + size_t(y) * s->row_pitch + size_t(x) * bpb;
}

uint16_t U16(const uint8_t* p, int i = 0) {
  uint16_t v;
  std::memcpy(&v, p + 2 * i, 2);
  return v;
}
uint32_t U32(const uint8_t* p, int i = 0) {
  uint32_t v;
  std::memcpy(&v, p + 4 * i, 4);
  return v;
}
float F32(const uint8_t* p, int i = 0) {
  float v;
  std::memcpy(&v, p + 4 * i, 4);
  return v;
}
float Half(const uint8_t* p, int i) { return HalfToFloat(U16(p, i)); }

// 32-bit value v as the 360 stores it with 8in32 (big-endian).
std::vector<uint8_t> BE32(std::initializer_list<uint32_t> values) {
  std::vector<uint8_t> out;
  for (uint32_t v : values)
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
  return out;
}
std::vector<uint8_t> BE16(std::initializer_list<uint16_t> values) {
  std::vector<uint8_t> out;
  for (uint16_t v : values) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
  }
  return out;
}

}  // namespace

// ---- Endian swaps for 8, 16 and 32-bit element layouts ----

TEST(endian_swap_units) {
  const uint8_t src[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  uint8_t d[8];
  CopySwap(Endian::kNone, src, d, 8);
  CHECK(std::memcmp(d, src, 8) == 0);
  CopySwap(Endian::k8in16, src, d, 8);
  const uint8_t e16[8] = {1, 0, 3, 2, 5, 4, 7, 6};
  CHECK(std::memcmp(d, e16, 8) == 0);
  CopySwap(Endian::k8in32, src, d, 8);
  const uint8_t e32[8] = {3, 2, 1, 0, 7, 6, 5, 4};
  CHECK(std::memcmp(d, e32, 8) == 0);
  CopySwap(Endian::k16in32, src, d, 8);
  const uint8_t e1632[8] = {2, 3, 0, 1, 6, 7, 4, 5};
  CHECK(std::memcmp(d, e1632, 8) == 0);
  // In place, and each mode undoes itself.
  for (int m = 0; m < 4; ++m) {
    uint8_t v[8];
    std::memcpy(v, src, 8);
    CopySwap(Endian(m), v, v, 8);
    CopySwap(Endian(m), v, v, 8);
    CHECK(std::memcmp(v, src, 8) == 0);
  }
  CHECK_EQ(ApplyEndian32(0x11223344u, Endian::k8in16), 0x22114433u);
  CHECK_EQ(ApplyEndian32(0x11223344u, Endian::k8in32), 0x44332211u);
  CHECK_EQ(ApplyEndian32(0x11223344u, Endian::k16in32), 0x33441122u);
}

TEST(endian_8bit_elements) {
  // 8-bit texels inside 32-bit units: with 8in32 the texel at x = 0 is the last byte of each word in memory.
  HostTextureData t = ConvertRow(TextureFormat::k_8, Endian::k8in32, 8, {1, 2, 3, 4, 5, 6, 7, 8});
  const uint8_t expect32[8] = {4, 3, 2, 1, 8, 7, 6, 5};
  for (int x = 0; x < 8; ++x) CHECK_EQ(Texel(t, x)[0], expect32[x]);
  t = ConvertRow(TextureFormat::k_8, Endian::kNone, 4, {1, 2, 3, 4});
  for (int x = 0; x < 4; ++x) CHECK_EQ(Texel(t, x)[0], x + 1);
  t = ConvertRow(TextureFormat::k_8_8, Endian::k8in16, 2, {0x11, 0x22, 0x33, 0x44});
  CHECK_EQ(Texel(t, 0)[0], 0x22);  // X is the low byte of the 16-bit unit
  CHECK_EQ(Texel(t, 0)[1], 0x11);
  CHECK_EQ(Texel(t, 1)[0], 0x44);
}

TEST(endian_16bit_elements) {
  HostTextureData t = ConvertRow(TextureFormat::k_16, Endian::k8in16, 2, BE16({0x1234, 0xABCD}));
  CHECK_EQ(U16(Texel(t, 0)), 0x1234);
  CHECK_EQ(U16(Texel(t, 1)), 0xABCD);
  // 16:16 with 8in32: the big-endian 32-bit word holds Y in the high half, X in the low half.
  t = ConvertRow(TextureFormat::k_16_16, Endian::k8in32, 1, BE32({0xBBBBAAAAu}));
  CHECK_EQ(U16(Texel(t, 0), 0), 0xAAAA);
  CHECK_EQ(U16(Texel(t, 0), 1), 0xBBBB);
  // The same texel stored as two big-endian halves in X, Y order reads right with 8in16.
  t = ConvertRow(TextureFormat::k_16_16, Endian::k8in16, 1, BE16({0xAAAA, 0xBBBB}));
  CHECK_EQ(U16(Texel(t, 0), 0), 0xAAAA);
  CHECK_EQ(U16(Texel(t, 0), 1), 0xBBBB);
  // 16in32 swaps the halves of each 32-bit unit only.
  t = ConvertRow(TextureFormat::k_16_16, Endian::k16in32, 1, {0x22, 0x22, 0x11, 0x11});
  CHECK_EQ(U16(Texel(t, 0), 0), 0x1111);
  CHECK_EQ(U16(Texel(t, 0), 1), 0x2222);
  // 64-bit texels: two 32-bit units, each swapped on its own (X, Y in the first; Z, W in the second).
  t = ConvertRow(TextureFormat::k_16_16_16_16, Endian::k8in32, 1, BE32({0x00020001u, 0x00040003u}));
  for (int i = 0; i < 4; ++i) CHECK_EQ(U16(Texel(t, 0), i), i + 1);
}

TEST(endian_32bit_elements) {
  HostTextureData t = ConvertRow(TextureFormat::k_8_8_8_8, Endian::k8in32, 1, {0xAA, 0x11, 0x22, 0x33});
  // A8R8G8B8 word 0xAA112233: X = B = 0x33, Y = G, Z = R, W = A.
  CHECK_EQ(Texel(t, 0)[0], 0x33);
  CHECK_EQ(Texel(t, 0)[1], 0x22);
  CHECK_EQ(Texel(t, 0)[2], 0x11);
  CHECK_EQ(Texel(t, 0)[3], 0xAA);
  uint32_t one, two;
  const float f1 = 1.0f, f2 = -2.5f;
  std::memcpy(&one, &f1, 4);
  std::memcpy(&two, &f2, 4);
  t = ConvertRow(TextureFormat::k_32_32_FLOAT, Endian::k8in32, 1, BE32({one, two}));
  CHECK_EQ(F32(Texel(t, 0), 0), 1.0f);
  CHECK_EQ(F32(Texel(t, 0), 1), -2.5f);
  t = ConvertRow(TextureFormat::k_32_32_32_32_FLOAT, Endian::k8in32, 1, BE32({one, two, one, two}));
  CHECK_EQ(F32(Texel(t, 0), 3), -2.5f);
  t = ConvertRow(TextureFormat::k_32, Endian::k8in32, 1, BE32({0xDEADBEEFu}));
  CHECK_EQ(U32(Texel(t, 0)), 0xDEADBEEFu);
}

// ---- Packed formats ----

TEST(convert_565_1555_4444_655) {
  HostTextureData t = ConvertRow(TextureFormat::k_5_6_5, Endian::k8in16, 2, BE16({0x001F, 0x07E0}));
  CHECK_EQ(int(t.plan.format), int(HostFormat::B5G6R5_UNORM));
  CHECK_EQ(U16(Texel(t, 0)), 0xF800);  // X (bits 0-4) -> host R (bits 11-15)
  CHECK_EQ(U16(Texel(t, 1)), 0x07E0);
  CHECK_EQ(t.plan.view_swizzle, MakeSwizzle(0, 1, 2, 2));  // no W: reads Z (the SDK's RGBB)
  t = ConvertRow(TextureFormat::k_1_5_5_5, Endian::k8in16, 2, BE16({0x801F, 0x7C00}));
  CHECK_EQ(U16(Texel(t, 0)), 0xFC00);
  CHECK_EQ(U16(Texel(t, 1)), 0x001F);
  t = ConvertRow(TextureFormat::k_4_4_4_4, Endian::k8in16, 2, BE16({0x000F, 0xF0A0}));
  CHECK_EQ(U16(Texel(t, 0)), 0x0F00);
  CHECK_EQ(U16(Texel(t, 1)), 0xF0A0);
  t = ConvertRow(TextureFormat::k_6_5_5, Endian::k8in16, 2, BE16({0xFC1F, 0x03E0}));
  const uint8_t* p = Texel(t, 0);
  CHECK(p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255);
  p = Texel(t, 1);
  CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0);
}

TEST(convert_10_11_11_and_11_11_10) {
  // k_10_11_11: X 11 bits (0-10), Y 11 (11-21), Z 10 (22-31).
  HostTextureData t = ConvertRow(TextureFormat::k_10_11_11, Endian::k8in32, 2, BE32({0x7FFu, 0x3FFu << 22}));
  CHECK_EQ(int(t.plan.format), int(HostFormat::RGBA16_UNORM));
  CHECK_EQ(U16(Texel(t, 0), 0), 0xFFFF);
  CHECK_EQ(U16(Texel(t, 0), 1), 0);
  CHECK_EQ(U16(Texel(t, 0), 3), 0xFFFF);
  CHECK_EQ(U16(Texel(t, 1), 2), 0xFFFF);
  // k_11_11_10: X 10 bits, Y 11, Z 11.
  t = ConvertRow(TextureFormat::k_11_11_10, Endian::k8in32, 2, BE32({0x3FFu, 0x7FFu << 10}));
  CHECK_EQ(U16(Texel(t, 0), 0), 0xFFFF);
  CHECK_EQ(U16(Texel(t, 0), 1), 0);
  CHECK_EQ(U16(Texel(t, 1), 1), 0xFFFF);
  // Mid values replicate bits: 0x200 of 10 bits -> 0x8020.
  t = ConvertRow(TextureFormat::k_11_11_10, Endian::k8in32, 1, BE32({0x200u}));
  CHECK_EQ(U16(Texel(t, 0), 0), 0x8020);
  // Signed: two's complement fields, the most negative clamping to -1.
  RowOptions s;
  s.sign = TextureSign::kSigned;
  t = ConvertRow(TextureFormat::k_10_11_11, Endian::k8in32, 2, BE32({0x3FFu | 0x400u << 11, 0x1FFu << 22}), s);
  CHECK_EQ(int(t.plan.format), int(HostFormat::RGBA16_SNORM));
  CHECK_EQ(int16_t(U16(Texel(t, 0), 0)), 32767);
  CHECK_EQ(int16_t(U16(Texel(t, 0), 1)), -32767);
  CHECK_EQ(int16_t(U16(Texel(t, 1), 2)), 32767);
}

TEST(convert_2_10_10_10) {
  HostTextureData t = ConvertRow(TextureFormat::k_2_10_10_10, Endian::k8in32, 1, BE32({0xC00003FFu}));
  CHECK_EQ(int(t.plan.format), int(HostFormat::R10G10B10A2_UNORM));
  CHECK_EQ(int(t.plan.conversion), int(Conversion::kCopy));
  CHECK_EQ(U32(Texel(t, 0)), 0xC00003FFu);
  RowOptions s;
  s.sign = TextureSign::kSigned;
  // X = 511 (+1), Y = -512 (-1), Z = 0, W = 01 (+1).
  t = ConvertRow(TextureFormat::k_2_10_10_10, Endian::k8in32, 1, BE32({0x1FFu | 0x200u << 10 | 1u << 30}), s);
  CHECK_EQ(int(t.plan.format), int(HostFormat::RGBA16_SNORM));
  CHECK_EQ(int16_t(U16(Texel(t, 0), 0)), 32767);
  CHECK_EQ(int16_t(U16(Texel(t, 0), 1)), -32767);
  CHECK_EQ(int16_t(U16(Texel(t, 0), 2)), 0);
  CHECK_EQ(int16_t(U16(Texel(t, 0), 3)), 32767);
}

TEST(convert_edram_formats) {
  // k_16_16_EDRAM: fixed point -32..32 (snorm16 * 32).
  HostTextureData t = ConvertRow(TextureFormat::k_16_16_EDRAM, Endian::k8in32, 2, BE32({0x80007FFFu, 0x00004000u}));
  CHECK_EQ(int(t.plan.format), int(HostFormat::RG16_FLOAT));
  CHECK_EQ(Half(Texel(t, 0), 0), 32.0f);
  CHECK_EQ(Half(Texel(t, 0), 1), -32.0f);
  CHECK_NEAR(Half(Texel(t, 1), 0), 16.0, 0.02);
  t = ConvertRow(TextureFormat::k_16_16_16_16_EDRAM, Endian::k8in32, 1, BE32({0x00007FFFu, 0x7FFF0000u}));
  CHECK_EQ(int(t.plan.format), int(HostFormat::RGBA16_FLOAT));
  CHECK_EQ(Half(Texel(t, 0), 0), 32.0f);
  CHECK_EQ(Half(Texel(t, 0), 3), 32.0f);
  // k_2_10_10_10_FLOAT_EDRAM: 7e3 (1.0 = 0x180), alpha 2 bits.
  t = ConvertRow(TextureFormat::k_2_10_10_10_FLOAT_EDRAM, Endian::k8in32, 1,
                 BE32({0x180u | 0x3FFu << 10 | 0u << 20 | 3u << 30}));
  CHECK_EQ(Half(Texel(t, 0), 0), 1.0f);
  CHECK_EQ(Half(Texel(t, 0), 1), 31.875f);
  CHECK_EQ(Half(Texel(t, 0), 2), 0.0f);
  CHECK_EQ(Half(Texel(t, 0), 3), 1.0f);
  // k_8_8_8_8_GAMMA_EDRAM: data as is, the shader degammas.
  t = ConvertRow(TextureFormat::k_8_8_8_8_GAMMA_EDRAM, Endian::k8in32, 1, {4, 3, 2, 1});
  CHECK_EQ(Texel(t, 0)[0], 1);
  CHECK(t.plan.shader_flags & kShaderGamma);
}

TEST(convert_depth) {
  HostTextureData t = ConvertRow(TextureFormat::k_24_8, Endian::k8in32, 3, BE32({0xFFFFFF00u, 0x80000055u, 0}));
  CHECK_EQ(int(t.plan.format), int(HostFormat::R32_FLOAT));
  CHECK_EQ(F32(Texel(t, 0)), 1.0f);
  // The SDK's n / 2^24 with the top bit added back in (what dividing by 2^24 - 1 rounds to); stencil ignored.
  CHECK_EQ(F32(Texel(t, 1)), 0.5f + std::ldexp(1.0f, -24));
  CHECK_EQ(F32(Texel(t, 2)), 0.0f);
  // 20e4: 1.0 = exponent 15, 0.5 = 14; a denormal 0x000001 = 2^-35.
  t = ConvertRow(TextureFormat::k_24_8_FLOAT, Endian::k8in32, 3, BE32({0xF0000000u, 0xE00000FFu, 0x00000100u}));
  CHECK_EQ(F32(Texel(t, 0)), 1.0f);
  CHECK_EQ(F32(Texel(t, 1)), 0.5f);
  CHECK_EQ(F32(Texel(t, 2)), std::ldexp(1.0f, -34));  // 2^-20 * 2^(1 - 15)
  CHECK_EQ(Float20e4ToFloat(0xFFFFFF), std::ldexp(2.0f - std::ldexp(1.0f, -20), 0));
}

TEST(convert_rgb32_float) {
  uint32_t a, b, c;
  const float fa = 0.25f, fb = 2.0f, fc = -1.0f;
  std::memcpy(&a, &fa, 4);
  std::memcpy(&b, &fb, 4);
  std::memcpy(&c, &fc, 4);
  HostTextureData t = ConvertRow(TextureFormat::k_32_32_32_FLOAT, Endian::k8in32, 1, BE32({a, b, c}));
  CHECK_EQ(int(t.plan.format), int(HostFormat::RGBA32_FLOAT));
  CHECK_EQ(F32(Texel(t, 0), 0), 0.25f);
  CHECK_EQ(F32(Texel(t, 0), 1), 2.0f);
  CHECK_EQ(F32(Texel(t, 0), 2), -1.0f);
  CHECK_EQ(F32(Texel(t, 0), 3), 1.0f);
}

TEST(convert_yuv422) {
  // Y1_Cr_Y0_Cb: host-order bytes Cb, Y0, Cr, Y1 (byte 0 lowest).
  HostTextureData t = ConvertRow(TextureFormat::k_Y1_Cr_Y0_Cb_REP, Endian::kNone, 2, {10, 20, 30, 40});
  const uint8_t* p = Texel(t, 0);
  CHECK(p[0] == 30 && p[1] == 20 && p[2] == 10 && p[3] == 255);
  p = Texel(t, 1);
  CHECK(p[0] == 30 && p[1] == 40 && p[2] == 10);
  // Cr_Y1_Cb_Y0: bytes Y0, Cb, Y1, Cr.
  t = ConvertRow(TextureFormat::k_Cr_Y1_Cb_Y0_REP, Endian::kNone, 2, {10, 20, 30, 40});
  p = Texel(t, 0);
  CHECK(p[0] == 40 && p[1] == 10 && p[2] == 20);
  p = Texel(t, 1);
  CHECK(p[0] == 40 && p[1] == 30 && p[2] == 20);
  // An odd width keeps only the first texel of the last block.
  t = ConvertRow(TextureFormat::k_Y1_Cr_Y0_Cb_REP, Endian::k8in32, 3, {40, 30, 20, 10, 41, 31, 21, 11});
  CHECK_EQ(t.Find(0, 0)->width, 3u);
  CHECK_EQ(Texel(t, 2)[1], 21);
}

TEST(convert_1bpp_and_split_blocks) {
  HostTextureData t = ConvertRow(TextureFormat::k_1, Endian::kNone, 8, {0x05});
  const uint8_t e1[8] = {255, 0, 255, 0, 0, 0, 0, 0};
  for (int x = 0; x < 8; ++x) CHECK_EQ(Texel(t, x)[0], e1[x]);
  t = ConvertRow(TextureFormat::k_1_REVERSE, Endian::kNone, 8, {0x05});
  for (int x = 0; x < 8; ++x) CHECK_EQ(Texel(t, x)[0], e1[7 - x]);
  // k_32_AS_8: four 8-bit texels per 32-bit unit, X in the low byte after the swap.
  t = ConvertRow(TextureFormat::k_32_AS_8, Endian::k8in32, 4, {4, 3, 2, 1});
  for (int x = 0; x < 4; ++x) CHECK_EQ(Texel(t, x)[0], x + 1);
  t = ConvertRow(TextureFormat::k_32_AS_8_8, Endian::k8in32, 2, {4, 3, 2, 1});
  CHECK_EQ(int(t.plan.format), int(HostFormat::RG8_UNORM));
  CHECK(Texel(t, 0)[0] == 1 && Texel(t, 0)[1] == 2 && Texel(t, 1)[0] == 3 && Texel(t, 1)[1] == 4);
}

// ---- Block formats ----

namespace {
// The DXT1 test block: red / blue end points, indices 0, 1, 2, 3 on the first row, in host order.
const uint8_t kDxt1Block[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00};
std::vector<uint8_t> Swap16(const uint8_t* b, size_t n) {
  std::vector<uint8_t> v(b, b + n);
  for (size_t i = 0; i + 1 < n; i += 2) std::swap(v[i], v[i + 1]);
  return v;
}
}  // namespace

TEST(convert_dxt1_decoded) {
  // 4x1 is not a multiple of 4 in height: decoded to RGBA8 (the BC path is tested below).
  HostTextureData t = ConvertRow(TextureFormat::k_DXT1, Endian::k8in16, 4, Swap16(kDxt1Block, 8));
  CHECK_EQ(int(t.plan.format), int(HostFormat::RGBA8_UNORM));
  CHECK(t.plan.decompressed);
  const uint8_t e[4][4] = {{255, 0, 0, 255}, {0, 0, 255, 255}, {170, 0, 85, 255}, {85, 0, 170, 255}};
  for (int x = 0; x < 4; ++x) CHECK(std::memcmp(Texel(t, x), e[x], 4) == 0);
  // c0 <= c1: three colours and transparent black.
  const uint8_t three[8] = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0, 0, 0};
  t = ConvertRow(TextureFormat::k_DXT1, Endian::k8in16, 4, Swap16(three, 8));
  const uint8_t e3[4][4] = {{0, 0, 255, 255}, {255, 0, 0, 255}, {128, 0, 128, 255}, {0, 0, 0, 0}};
  for (int x = 0; x < 4; ++x) CHECK(std::memcmp(Texel(t, x), e3[x], 4) == 0);
}

TEST(convert_dxt3_dxt5) {
  uint8_t b3[16] = {0x0F, 0xF0, 0, 0, 0, 0, 0, 0};
  std::memcpy(b3 + 8, kDxt1Block, 8);
  HostTextureData t = ConvertRow(TextureFormat::k_DXT2_3, Endian::k8in16, 4, Swap16(b3, 16));
  CHECK_EQ(Texel(t, 0)[3], 255);
  CHECK_EQ(Texel(t, 1)[3], 0);
  CHECK_EQ(Texel(t, 2)[3], 0);
  CHECK_EQ(Texel(t, 3)[3], 255);
  CHECK_EQ(Texel(t, 2)[0], 170);
  // DXT5 alpha: 255 / 0 end points, 8-value mode; indices 0, 1, 2, 7 -> 255, 0, 219, 36.
  uint8_t b5[16] = {255, 0};
  const uint64_t idx = 0 | 1u << 3 | 2u << 6 | 7u << 9;
  for (int k = 0; k < 6; ++k) b5[2 + k] = uint8_t(idx >> (8 * k));
  std::memcpy(b5 + 8, kDxt1Block, 8);
  t = ConvertRow(TextureFormat::k_DXT4_5, Endian::k8in16, 4, Swap16(b5, 16));
  CHECK_EQ(Texel(t, 0)[3], 255);
  CHECK_EQ(Texel(t, 1)[3], 0);
  CHECK_EQ(Texel(t, 2)[3], 219);
  CHECK_EQ(Texel(t, 3)[3], 36);
  CHECK_EQ(Texel(t, 1)[2], 255);  // DXT5 colour is always four-colour
}

TEST(convert_dxn_dxt5a_dxt3a) {
  uint8_t n[16] = {200, 100, 0, 0, 0, 0, 0, 0, 10, 20, 0x09, 0, 0, 0, 0, 0};  // G: indices 1, 1 for texels 0, 1
  HostTextureData t = ConvertRow(TextureFormat::k_DXN, Endian::k8in16, 4, Swap16(n, 16));
  CHECK_EQ(int(t.plan.format), int(HostFormat::RG8_UNORM));
  CHECK_EQ(t.plan.view_swizzle, MakeSwizzle(0, 1, 1, 1));
  CHECK_EQ(Texel(t, 0)[0], 200);
  CHECK_EQ(Texel(t, 0)[1], 20);
  CHECK_EQ(Texel(t, 2)[1], 10);
  // DXT5A: a BC4 block; 6-value mode (e0 <= e1) gives 0 and 255 for indices 6, 7.
  uint8_t a[8] = {50, 100};
  const uint64_t idx = 6 | 7u << 3 | 2u << 6;
  for (int k = 0; k < 6; ++k) a[2 + k] = uint8_t(idx >> (8 * k));
  t = ConvertRow(TextureFormat::k_DXT5A, Endian::k8in16, 4, Swap16(a, 8));
  CHECK_EQ(int(t.plan.format), int(HostFormat::R8_UNORM));
  CHECK_EQ(Texel(t, 0)[0], 0);
  CHECK_EQ(Texel(t, 1)[0], 255);
  CHECK_EQ(Texel(t, 2)[0], 60);  // (4 * 50 + 100 + 2) / 5
  // Signed DXT5A (BC4_SNORM rules): 127 / -127 in 8-value mode, index 2 = round((6 * 127 - 127) / 7) = 91.
  RowOptions s;
  s.sign = TextureSign::kSigned;
  const uint8_t sa[8] = {0x7F, 0x81, uint8_t(1 | 2 << 3)};  // indices 1, 2, 0
  t = ConvertRow(TextureFormat::k_DXT5A, Endian::k8in16, 4, Swap16(sa, 8), s);
  CHECK_EQ(int(t.plan.format), int(HostFormat::R8_SNORM));
  CHECK_EQ(int8_t(Texel(t, 0)[0]), -127);
  CHECK_EQ(int8_t(Texel(t, 1)[0]), 91);
  CHECK_EQ(int8_t(Texel(t, 2)[0]), 127);
  // DXT3A: 4-bit values * 17.
  uint8_t d3[8] = {0x3A, 0xF0};
  t = ConvertRow(TextureFormat::k_DXT3A, Endian::k8in16, 4, Swap16(d3, 8));
  CHECK_EQ(Texel(t, 0)[0], 0xA * 17);
  CHECK_EQ(Texel(t, 1)[0], 3 * 17);
  CHECK_EQ(Texel(t, 3)[0], 255);
  // DXT3A as 1:1:1:1: bit k of each 4-bit value -> component k.
  t = ConvertRow(TextureFormat::k_DXT3A_AS_1_1_1_1, Endian::k8in16, 4, Swap16(d3, 8));
  const uint8_t* p = Texel(t, 0);  // 0xA = 1010b
  CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255);
}

TEST(convert_ctx1) {
  // Host order: g0, r0, g1, r1, then 2-bit indices 0, 1, 2, 3.
  const uint8_t b[8] = {30, 90, 0, 0, 0xE4, 0, 0, 0};
  HostTextureData t = ConvertRow(TextureFormat::k_CTX1, Endian::k8in16, 4, Swap16(b, 8));
  CHECK_EQ(int(t.plan.format), int(HostFormat::RG8_UNORM));
  CHECK(Texel(t, 0)[0] == 90 && Texel(t, 0)[1] == 30);
  CHECK(Texel(t, 1)[0] == 0 && Texel(t, 1)[1] == 0);
  CHECK(Texel(t, 2)[0] == 60 && Texel(t, 2)[1] == 20);
  CHECK(Texel(t, 3)[0] == 30 && Texel(t, 3)[1] == 10);
}

TEST(convert_bc_copy_with_mips) {
  // An 8x8 tiled DXT1 with a packed tail: host BC1 blocks equal the guest blocks, level by level.
  TextureFetchDesc d;
  d.format = TextureFormat::k_DXT1;
  d.endian = Endian::k8in16;
  d.tiled = true;
  d.packed_mips = true;
  d.width = d.height = 8;
  d.base_address = kBase;
  d.mip_address = kMips;
  d.max_level = 3;
  const TextureFetch f = MakeTextureFetch(d);
  Rng rng(9);
  std::vector<std::vector<uint8_t>> blocks(4);
  const uint32_t counts[4] = {4, 1, 1, 1};
  for (int l = 0; l < 4; ++l) {
    blocks[l].resize(counts[l] * 8);
    rng.Fill(blocks[l].data(), blocks[l].size());
  }
  GuestTextureImage image;
  CHECK(EncodeGuestTexture(f, [&](uint32_t level, uint32_t) { return blocks[level].data(); }, image));
  FakeGuest guest;
  guest.Put(kBase, image.base);
  guest.Put(kMips, image.mips);
  HostTextureData t;
  CHECK(ConvertTexture(f, GuestMemory{guest.bytes.data(), guest.bytes.size()}, t));
  CHECK_EQ(int(t.plan.format), int(HostFormat::BC1_UNORM));
  CHECK_EQ(t.subresources.size(), size_t(4));
  for (uint32_t l = 0; l < 4; ++l) {
    const HostSubresource* s = t.Find(l, 0);
    CHECK(s != nullptr);
    if (!s) continue;
    CHECK_EQ(s->size, blocks[l].size());
    CHECK(std::memcmp(t.bytes.data() + s->offset, blocks[l].data(), s->size) == 0);
  }
}

// ---- Scalars ----

TEST(half_float_conversion) {
  CHECK_EQ(FloatToHalf(1.0f), 0x3C00);
  CHECK_EQ(FloatToHalf(-2.0f), 0xC000);
  CHECK_EQ(FloatToHalf(65504.0f), 0x7BFF);
  CHECK_EQ(FloatToHalf(65520.0f), 0x7C00);
  CHECK_EQ(FloatToHalf(std::ldexp(1.0f, -24)), 0x0001);
  CHECK_EQ(FloatToHalf(std::ldexp(1.0f, -26)), 0x0000);
  CHECK_EQ(FloatToHalf(1.0f + std::ldexp(1.0f, -11)), 0x3C00);  // tie to even
  bool all = true;
  for (uint32_t h = 0; h < 0x10000; ++h) {
    if ((h & 0x7C00) == 0x7C00 && (h & 0x3FF)) continue;  // NaN
    all &= FloatToHalf(HalfToFloat(uint16_t(h))) == h;
  }
  CHECK(all);
}

TEST(float_7e3_and_20e4) {
  CHECK_EQ(Float7e3ToFloat(0x180), 1.0f);
  CHECK_EQ(Float7e3ToFloat(0x3FF), 31.875f);
  CHECK_EQ(Float7e3ToFloat(0x001), std::ldexp(1.0f, -9));  // denormal: 2^-7 * 2^(1 - 3)
  CHECK_EQ(Float20e4ToFloat(0xF00000), 1.0f);
  CHECK_EQ(UNorm24ToFloat(0xFFFFFF), 1.0f);
  CHECK_EQ(UNorm24ToFloat(0xC00000), 0.75f + std::ldexp(1.0f, -24));
  CHECK_EQ(UNorm24ToFloat(0x400000), 0.25f);
}

// ---- Plan rules ----

TEST(plan_signs_and_flags) {
  RowOptions o;
  o.sign = TextureSign::kSigned;
  HostTextureData t = ConvertRow(TextureFormat::k_8_8_8_8, Endian::k8in32, 1, {0x80, 0x7F, 0, 1}, o);
  CHECK_EQ(int(t.plan.format), int(HostFormat::RGBA8_SNORM));
  CHECK_EQ(t.plan.shader_flags, uint8_t(kShaderNone));
  CHECK_EQ(int8_t(Texel(t, 0)[3]), -128);  // data unchanged; SNORM reads -128 as -1 like the 360
  o.sign = TextureSign::kUnsignedBiased;
  t = ConvertRow(TextureFormat::k_8_8_8_8, Endian::k8in32, 1, {0, 0, 0, 0}, o);
  CHECK_EQ(int(t.plan.format), int(HostFormat::RGBA8_UNORM));
  CHECK(t.plan.shader_flags & kShaderBias);
  o.sign = TextureSign::kGamma;
  t = ConvertRow(TextureFormat::k_8_8_8_8, Endian::k8in32, 1, {0, 0, 0, 0}, o);
  CHECK(t.plan.shader_flags & kShaderGamma);
  // Signed with no signed host format: unsigned data, the shader converts.
  o.sign = TextureSign::kSigned;
  t = ConvertRow(TextureFormat::k_5_6_5, Endian::k8in16, 1, {0, 0}, o);
  CHECK_EQ(int(t.plan.format), int(HostFormat::B5G6R5_UNORM));
  CHECK(t.plan.shader_flags & kShaderMixedSigns);
  // 32-bit fixed point: integer data the shader normalises unless the number format is integer.
  t = ConvertRow(TextureFormat::k_32_32, Endian::k8in32, 1, BE32({1, 2}));
  CHECK(t.plan.shader_flags & kShaderNorm32);
  RowOptions i;
  i.integer = true;
  t = ConvertRow(TextureFormat::k_32_32, Endian::k8in32, 1, BE32({1, 2}), i);
  CHECK(!(t.plan.shader_flags & kShaderNorm32));
  CHECK(t.plan.shader_flags & kShaderInteger);
  i.sign = TextureSign::kSigned;
  t = ConvertRow(TextureFormat::k_32, Endian::k8in32, 1, BE32({uint32_t(-5)}), i);
  CHECK_EQ(int(t.plan.format), int(HostFormat::R32_SINT));
}

TEST(plan_view_swizzle) {
  RowOptions o;
  o.swizzle = kSwizzleXYZW;
  HostTextureData t = ConvertRow(TextureFormat::k_8, Endian::kNone, 4, {1, 2, 3, 4}, o);
  CHECK_EQ(t.plan.view_swizzle, MakeSwizzle(0, 0, 0, 0));  // single component replicates
  o.swizzle = MakeSwizzle(kSwzY, kSwzX, kSwz0, kSwz1);
  t = ConvertRow(TextureFormat::k_8_8, Endian::kNone, 2, {1, 2, 3, 4}, o);
  CHECK_EQ(t.plan.view_swizzle, MakeSwizzle(1, 0, 4, 5));
  o.swizzle = MakeSwizzle(kSwzW, kSwzW, kSwzW, kSwzW);
  t = ConvertRow(TextureFormat::k_16_16, Endian::k8in32, 1, {0, 0, 0, 0}, o);
  CHECK_EQ(t.plan.view_swizzle, MakeSwizzle(1, 1, 1, 1));  // two components: W reads Y
}

TEST(plan_bc_alignment_fallback) {
  auto plan = [](TextureFormat f, uint32_t w, uint32_t h, bool is_signed) {
    TextureFetchDesc d;
    d.format = f;
    d.width = w;
    d.height = h;
    d.base_address = kBase;
    if (is_signed)
      for (TextureSign& s : d.signs) s = TextureSign::kSigned;
    HostTexturePlan p;
    PlanHostTexture(MakeTextureFetch(d), p);
    return p;
  };
  CHECK_EQ(int(plan(TextureFormat::k_DXT1, 64, 64, false).format), int(HostFormat::BC1_UNORM));
  CHECK_EQ(int(plan(TextureFormat::k_DXT1, 30, 64, false).format), int(HostFormat::RGBA8_UNORM));
  CHECK_EQ(int(plan(TextureFormat::k_DXT4_5, 64, 6, false).format), int(HostFormat::RGBA8_UNORM));
  CHECK_EQ(int(plan(TextureFormat::k_DXN, 64, 64, true).format), int(HostFormat::BC5_SNORM));
  CHECK_EQ(int(plan(TextureFormat::k_DXN, 6, 6, true).format), int(HostFormat::RG8_SNORM));
  CHECK_EQ(int(plan(TextureFormat::k_DXT5A, 6, 6, false).format), int(HostFormat::R8_UNORM));
  CHECK(plan(TextureFormat::k_DXT1, 30, 64, false).decompressed);
  CHECK(!plan(TextureFormat::k_DXT1, 64, 64, false).decompressed);
}

TEST(every_format_converts_end_to_end) {
  // All 64 formats: a 64x32 texture with mips (tiled where the block tiles), random data, converted. The
  // host data has the size the host format implies, and every kCopy format's level 0 equals its blocks.
  int converted = 0;
  for (uint32_t fi = 0; fi < 64; ++fi) {
    const TextureFormat format = TextureFormat(fi);
    const FormatChoice choice = GetFormatChoice(format);
    if (choice.format == HostFormat::UNKNOWN) {
      kknr_test::Fail(__FILE__, __LINE__, std::string("no host format for ") + GetFormatInfo(format).name);
      continue;
    }
    TextureFetchDesc d;
    d.format = format;
    d.endian = Endian::k8in32;
    d.width = 64;
    d.height = 32;
    d.tiled = GetFormatInfo(format).BytesPerBlock() != 12;
    d.packed_mips = format != TextureFormat::k_1 && format != TextureFormat::k_1_REVERSE;
    d.base_address = kBase;
    d.mip_address = kMips;
    d.max_level = 6;
    const TextureFetch f = MakeTextureFetch(d);
    const GuestLayout layout = ComputeGuestLayout(f);
    const uint32_t bpb = GetFormatInfo(format).BytesPerBlock();
    Rng rng(fi + 100);
    std::vector<std::vector<uint8_t>> blocks(7);
    for (uint32_t l = 0; l <= 6; ++l) {
      blocks[l].resize(GetLevelBlockExtent(layout, l).Count() * bpb);
      rng.Fill(blocks[l].data(), blocks[l].size());
    }
    GuestTextureImage image;
    EncodeGuestTexture(f, [&](uint32_t level, uint32_t) { return blocks[level].data(); }, image);
    static FakeGuest guest;
    guest.Put(kBase, image.base);
    guest.Put(kMips, image.mips);
    HostTextureData t;
    std::string why;
    if (!ConvertTexture(f, GuestMemory{guest.bytes.data(), guest.bytes.size()}, t, &why)) {
      kknr_test::Fail(__FILE__, __LINE__, std::string(GetFormatInfo(format).name) + ": " + why);
      continue;
    }
    const HostFormatInfo& host = GetHostFormatInfo(t.plan.format);
    bool sizes = t.subresources.size() == 7;
    for (const HostSubresource& s : t.subresources) {
      const uint32_t bw = (s.width + host.block_size - 1) / host.block_size;
      const uint32_t bh = (s.height + host.block_size - 1) / host.block_size;
      sizes &= s.size == size_t(bw) * bh * host.bytes_per_block;
    }
    if (!sizes) kknr_test::Fail(__FILE__, __LINE__, std::string("sizes: ") + GetFormatInfo(format).name);
    if (t.plan.conversion == Conversion::kCopy) {
      const HostSubresource* s = t.Find(0, 0);
      if (!s || s->size != blocks[0].size() || std::memcmp(t.bytes.data() + s->offset, blocks[0].data(), s->size))
        kknr_test::Fail(__FILE__, __LINE__, std::string("copy differs: ") + GetFormatInfo(format).name);
    }
    ++converted;
  }
  CHECK_EQ(converted, 64);
}
