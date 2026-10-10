// Index and vertex buffers: guest objects, declarations and the per-element endian swaps, with synthetic
// big-endian vertices written element by element the way the engine's data sits in guest memory.
#include <cstring>
#include <vector>

#include "kknr/buffers.h"
#include "test.h"

using namespace kknr;

namespace {

void PutBE32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (24 - 8 * i));
}
void PutBE16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
  v[at] = uint8_t(x >> 8);
  v[at + 1] = uint8_t(x);
}
uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// A declaration object as brief 01 describes it: count at +8, 12-byte elements from +36, D3DDECL_END.
std::vector<uint8_t> MakeDeclObject(const std::vector<VertexElement>& elements) {
  std::vector<uint8_t> o(36 + 12 * (elements.size() + 1), 0);
  PutBE32(o, 8, uint32_t(elements.size() + 1));
  for (size_t i = 0; i <= elements.size(); ++i) {
    const size_t at = 36 + 12 * i;
    if (i == elements.size()) {
      PutBE16(o, at, 0xFF);
      PutBE32(o, at + 4, 0xFFFFFFFFu);
      continue;
    }
    const VertexElement& e = elements[i];
    PutBE16(o, at, e.stream);
    PutBE16(o, at + 2, e.offset);
    PutBE32(o, at + 4, e.type.value);
    o[at + 8] = e.method;
    o[at + 9] = e.usage;
    o[at + 10] = e.usage_index;
  }
  return o;
}

VertexElement El(uint16_t stream, uint16_t offset, DeclType type, uint8_t usage = 0) {
  VertexElement e;
  e.stream = stream;
  e.offset = offset;
  e.type = type;
  e.usage = usage;
  return e;
}

}  // namespace

TEST(decl_type_fields) {
  CHECK_EQ(MakeDeclType(VertexFormat::k_32_32_32_FLOAT, Endian::k8in32, true, true, MakeSwizzle(0, 1, 2, 5)).value,
           kDeclFloat3.value);
  CHECK_EQ(MakeDeclType(VertexFormat::k_32_32_FLOAT, Endian::k8in32, true, true, MakeSwizzle(0, 1, 4, 5)).value,
           kDeclFloat2.value);
  CHECK_EQ(MakeDeclType(VertexFormat::k_8_8_8_8, Endian::k8in32, false, false, MakeSwizzle(2, 1, 0, 3)).value,
           kDeclColor.value);
  CHECK_EQ(uint32_t(kDeclShort4N.Format()), uint32_t(VertexFormat::k_16_16_16_16));
  CHECK_EQ(uint32_t(kDeclShort4N.EndianMode()), uint32_t(Endian::k8in16));
  CHECK(kDeclShort4N.Signed());
  CHECK(!kDeclShort4N.Integer());
  CHECK(kDeclUByte4.Integer());
  CHECK(!kDeclUByte4.Signed());
}

TEST(host_vertex_formats) {
  CHECK_EQ(int(GetHostVertexFormat(kDeclFloat3).format), int(HostVertexFormat::RGB32_FLOAT));
  CHECK_EQ(int(GetHostVertexFormat(kDeclFloat2).format), int(HostVertexFormat::RG32_FLOAT));
  CHECK_EQ(int(GetHostVertexFormat(kDeclFloat4).format), int(HostVertexFormat::RGBA32_FLOAT));
  CHECK_EQ(int(GetHostVertexFormat(kDeclColor).format), int(HostVertexFormat::RGBA8_UNORM));
  CHECK_EQ(int(GetHostVertexFormat(kDeclUByte4).format), int(HostVertexFormat::RGBA8_UINT));
  CHECK_EQ(int(GetHostVertexFormat(kDeclShort4N).format), int(HostVertexFormat::RGBA16_SNORM));
  const HostVertexElementFormat dec3 =
      GetHostVertexFormat(MakeDeclType(VertexFormat::k_10_11_11, Endian::k8in32, false, false));
  CHECK_EQ(int(dec3.format), int(HostVertexFormat::R32_UINT));
  CHECK(dec3.needs_unpack);
  CHECK_EQ(VertexFormatBytes(VertexFormat::k_32_32_32_FLOAT), 12u);
  CHECK_EQ(VertexFormatBytes(VertexFormat::k_16_16), 4u);
}

TEST(decode_buffer_objects) {
  std::vector<uint8_t> ib(32, 0);
  PutBE32(ib, 0, 2u << 16 | 1);  // type 2, refcount 1, 16-bit
  PutBE32(ib, 12, 0xA0012000u);
  PutBE32(ib, 16, 600);
  IndexBufferInfo i;
  CHECK(DecodeIndexBuffer(ib.data(), i));
  CHECK(!i.index32);
  CHECK_EQ(i.physical, 0x12000u);
  CHECK_EQ(i.size_bytes, 600u);
  CHECK_EQ(uint32_t(i.endian), uint32_t(Endian::k8in16));
  PutBE32(ib, 0, 0x80000000u | 2u << 16 | 1);
  PutBE32(ib, 12, 0xE0012000u);  // the 0xE0000000 view is 4 KB ahead
  CHECK(DecodeIndexBuffer(ib.data(), i));
  CHECK(i.index32);
  CHECK_EQ(i.physical, 0x13000u);
  CHECK_EQ(uint32_t(i.endian), uint32_t(Endian::k8in32));

  std::vector<uint8_t> vb(32, 0);
  PutBE32(vb, 0, 1u << 16 | 1);
  PutBE32(vb, 12, 0x00345000u | 3);         // address, type 3 (vertex)
  PutBE32(vb, 16, (1024u / 4) << 2 | 2);    // size in dwords, 8in32
  VertexBufferInfo v;
  CHECK(DecodeVertexBuffer(vb.data(), v));
  CHECK_EQ(v.physical, 0x345000u);
  CHECK_EQ(v.size_bytes, 1024u);
  CHECK_EQ(uint32_t(v.endian), uint32_t(Endian::k8in32));
  CHECK(!DecodeIndexBuffer(vb.data(), i));
  CHECK(!DecodeVertexBuffer(ib.data(), v));
}

TEST(decode_declaration_object) {
  const std::vector<VertexElement> in = {El(0, 0, kDeclFloat3, 0), El(0, 12, kDeclColor, 10), El(1, 0, kDeclShort4N, 3)};
  const std::vector<uint8_t> obj = MakeDeclObject(in);
  const std::vector<VertexElement> out = DecodeVertexDeclaration(obj.data(), obj.size());
  CHECK_EQ(out.size(), size_t(3));
  if (out.size() != 3) return;
  CHECK_EQ(out[1].offset, 12);
  CHECK_EQ(out[1].type.value, kDeclColor.value);
  CHECK_EQ(out[1].usage, 10);
  CHECK_EQ(out[2].stream, 1);
}

TEST(vertex_swap_plan_and_conversion) {
  // Stream 0, stride 32: FLOAT3 position (8in32), D3DCOLOR (8in32), SHORT4N normal (8in16), 4 unused bytes.
  const std::vector<VertexElement> decl = {El(0, 0, kDeclFloat3), El(0, 12, kDeclColor), El(0, 16, kDeclShort4N),
                                           El(1, 0, kDeclFloat2)};
  bool conflict = true;
  const std::vector<VertexSwapRange> plan = PlanVertexSwap(decl, 0, 28, Endian::k8in32, &conflict);
  CHECK(!conflict);
  CHECK_EQ(plan.size(), size_t(3));
  if (plan.size() != 3) return;
  CHECK(plan[0].offset == 0 && plan[0].size == 16 && plan[0].endian == Endian::k8in32);
  CHECK(plan[1].offset == 16 && plan[1].size == 8 && plan[1].endian == Endian::k8in16);
  CHECK(plan[2].offset == 24 && plan[2].size == 4 && plan[2].endian == Endian::k8in32);

  // Two vertices, big-endian as the engine writes them.
  std::vector<uint8_t> guest(56, 0);
  for (int v = 0; v < 2; ++v) {
    const size_t b = size_t(v) * 28;
    PutBE32(guest, b + 0, FloatBits(1.5f + v));
    PutBE32(guest, b + 4, FloatBits(-2.0f));
    PutBE32(guest, b + 8, FloatBits(100.0f));
    PutBE32(guest, b + 12, 0xFF102030u);  // A8R8G8B8
    PutBE16(guest, b + 16, 0x7FFF);
    PutBE16(guest, b + 18, 0x8001);
    PutBE16(guest, b + 20, 0x0000);
    PutBE16(guest, b + 22, 0x1234);
    PutBE32(guest, b + 24, 0xCAFEF00Du);
  }
  std::vector<uint8_t> host(56, 0);
  ConvertVertices(guest.data(), host.data(), 2, 28, plan);
  for (int v = 0; v < 2; ++v) {
    const uint8_t* h = host.data() + v * 28;
    float f[3];
    std::memcpy(f, h, 12);
    CHECK_EQ(f[0], 1.5f + v);
    CHECK_EQ(f[1], -2.0f);
    CHECK_EQ(f[2], 100.0f);
    // RGBA8 input: X = blue (low byte of the word) ... the declaration's ZYXW swizzle is the shader's job.
    CHECK(h[12] == 0x30 && h[13] == 0x20 && h[14] == 0x10 && h[15] == 0xFF);
    int16_t s[4];
    std::memcpy(s, h + 16, 8);
    CHECK(s[0] == 0x7FFF && s[1] == int16_t(0x8001) && s[2] == 0 && s[3] == 0x1234);
    uint32_t pad;
    std::memcpy(&pad, h + 24, 4);
    CHECK_EQ(pad, 0xCAFEF00Du);
  }
  // A plan covering the whole stride with one mode takes the single-run path.
  const std::vector<VertexSwapRange> single = PlanVertexSwap({El(0, 0, kDeclFloat4)}, 0, 16, Endian::k8in32);
  CHECK_EQ(single.size(), size_t(1));
  std::vector<uint8_t> g4(16), h4(16);
  for (int i = 0; i < 4; ++i) PutBE32(g4, 4 * i, FloatBits(float(i)));
  ConvertVertices(g4.data(), h4.data(), 1, 16, single);
  float f4[4];
  std::memcpy(f4, h4.data(), 16);
  CHECK(f4[0] == 0 && f4[3] == 3);
}

TEST(vertex_swap_conflicts_and_ids) {
  bool conflict = false;
  // A SHORT4N (8in16) read over a FLOAT2 (8in32): the same bytes with two swaps.
  PlanVertexSwap({El(0, 0, kDeclFloat2), El(0, 0, kDeclShort4N)}, 0, 8, Endian::k8in32, &conflict);
  CHECK(conflict);
  PlanVertexSwap({El(0, 0, kDeclFloat2), El(0, 0, kDeclFloat2)}, 0, 8, Endian::k8in32, &conflict);
  CHECK(!conflict);
  const auto a = PlanVertexSwap({El(0, 0, kDeclFloat3)}, 0, 12, Endian::k8in32);
  const auto b = PlanVertexSwap({El(0, 0, kDeclFloat3)}, 0, 12, Endian::k8in32);
  const auto c = PlanVertexSwap({El(0, 0, kDeclShort4N), El(0, 8, kDeclColor)}, 0, 12, Endian::k8in32);
  CHECK_EQ(VertexConversionId(a, 12), VertexConversionId(b, 12));
  CHECK(VertexConversionId(a, 12) != VertexConversionId(c, 12));
  CHECK(VertexConversionId(a, 12) != VertexConversionId(a, 16));
  CHECK(IndexConversionId(true) != IndexConversionId(false));
}

TEST(index_conversion) {
  const uint8_t g16[6] = {0x00, 0x01, 0x12, 0x34, 0xFF, 0xFE};
  uint16_t h16[3];
  ConvertIndices(g16, h16, 3, false);
  CHECK(h16[0] == 1 && h16[1] == 0x1234 && h16[2] == 0xFFFE);
  const uint8_t g32[8] = {0x00, 0x01, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 0xFF};
  uint32_t h32[2];
  ConvertIndices(g32, h32, 2, true);
  CHECK(h32[0] == 0x00010002u && h32[1] == 0xFFFFFFFFu);
}
