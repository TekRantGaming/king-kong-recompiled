#include "kknr/buffers.h"

#include <algorithm>
#include <cstring>

#include "kknr/endian.h"

namespace kknr {

bool DecodeIndexBuffer(const uint8_t* object, IndexBufferInfo& out) {
  out = IndexBufferInfo();
  const uint32_t common = LoadBE32(object);
  if (((common >> 16) & 7) != 2) return false;  // not an index buffer
  out.index32 = (common >> 31) != 0;
  out.endian = out.index32 ? Endian::k8in32 : Endian::k8in16;
  out.physical = CpuToPhysical(LoadBE32(object + 12));
  out.size_bytes = LoadBE32(object + 16);
  return true;
}

bool DecodeVertexBuffer(const uint8_t* object, VertexBufferInfo& out) {
  out = VertexBufferInfo();
  const uint32_t common = LoadBE32(object);
  if (((common >> 16) & 7) != 1) return false;  // not a vertex buffer
  const VertexFetch fetch = VertexFetch::FromGuest(object + 12);
  if (fetch.Type() != 3) return false;
  out.physical = fetch.Address() & 0x1FFFFFFFu;
  out.size_bytes = fetch.SizeBytes();
  out.endian = fetch.EndianMode();
  return true;
}

uint32_t VertexFormatBytes(VertexFormat format) {
  switch (format) {
    case VertexFormat::k_8_8_8_8:
    case VertexFormat::k_2_10_10_10:
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
    case VertexFormat::k_16_16:
    case VertexFormat::k_16_16_FLOAT:
    case VertexFormat::k_32:
    case VertexFormat::k_32_FLOAT:
      return 4;
    case VertexFormat::k_16_16_16_16:
    case VertexFormat::k_16_16_16_16_FLOAT:
    case VertexFormat::k_32_32:
    case VertexFormat::k_32_32_FLOAT:
      return 8;
    case VertexFormat::k_32_32_32_FLOAT:
      return 12;
    case VertexFormat::k_32_32_32_32:
    case VertexFormat::k_32_32_32_32_FLOAT:
      return 16;
    default:
      return 0;
  }
}

std::vector<VertexElement> DecodeVertexDeclaration(const uint8_t* object, size_t max_bytes) {
  std::vector<VertexElement> elements;
  const uint32_t count = LoadBE32(object + 8);
  for (uint32_t i = 0; i < count && 36 + 12 * (i + 1) <= max_bytes && i < 64; ++i) {
    const uint8_t* e = object + 36 + 12 * i;
    VertexElement el;
    el.stream = LoadBE16(e);
    if (el.stream == 0xFF) break;  // D3DDECL_END
    el.offset = LoadBE16(e + 2);
    el.type.value = LoadBE32(e + 4);
    el.method = e[8];
    el.usage = e[9];
    el.usage_index = e[10];
    el.extra = e[11];
    if (el.type.Unused()) break;
    elements.push_back(el);
  }
  return elements;
}

HostVertexElementFormat GetHostVertexFormat(DeclType type) {
  using V = VertexFormat;
  using H = HostVertexFormat;
  const bool s = type.Signed(), i = type.Integer();
  auto pick = [&](H unorm, H snorm, H uint, H sint) {
    return HostVertexElementFormat{i ? (s ? sint : uint) : (s ? snorm : unorm), false};
  };
  switch (type.Format()) {
    case V::k_8_8_8_8:
      return pick(H::RGBA8_UNORM, H::RGBA8_SNORM, H::RGBA8_UINT, H::RGBA8_SINT);
    case V::k_2_10_10_10:
      if (!s && !i) return {H::R10G10B10A2_UNORM, false};
      return {H::R32_UINT, true};
    case V::k_10_11_11:
    case V::k_11_11_10:
      return {H::R32_UINT, true};
    case V::k_16_16:
      return pick(H::RG16_UNORM, H::RG16_SNORM, H::RG16_UINT, H::RG16_SINT);
    case V::k_16_16_16_16:
      return pick(H::RGBA16_UNORM, H::RGBA16_SNORM, H::RGBA16_UINT, H::RGBA16_SINT);
    case V::k_16_16_FLOAT:
      return {H::RG16_FLOAT, false};
    case V::k_16_16_16_16_FLOAT:
      return {H::RGBA16_FLOAT, false};
    case V::k_32:
      return {i ? (s ? H::R32_SINT : H::R32_UINT) : H::R32_UINT, !i};
    case V::k_32_32:
      return {i ? (s ? H::RG32_SINT : H::RG32_UINT) : H::RG32_UINT, !i};
    case V::k_32_32_32_32:
      return {i ? (s ? H::RGBA32_SINT : H::RGBA32_UINT) : H::RGBA32_UINT, !i};
    case V::k_32_FLOAT:
      return {H::R32_FLOAT, false};
    case V::k_32_32_FLOAT:
      return {H::RG32_FLOAT, false};
    case V::k_32_32_32_FLOAT:
      return {H::RGB32_FLOAT, false};
    case V::k_32_32_32_32_FLOAT:
      return {H::RGBA32_FLOAT, false};
    default:
      return {H::UNKNOWN, false};
  }
}

const char* HostVertexFormatName(HostVertexFormat format) {
  static const char* const kNames[] = {
      "UNKNOWN",     "RGBA8_UNORM", "RGBA8_SNORM", "RGBA8_UINT",   "RGBA8_SINT",   "R10G10B10A2_UNORM",
      "RG16_UNORM",  "RG16_SNORM",  "RG16_UINT",   "RG16_SINT",    "RG16_FLOAT",   "RGBA16_UNORM",
      "RGBA16_SNORM", "RGBA16_UINT", "RGBA16_SINT", "RGBA16_FLOAT", "R32_UINT",     "R32_SINT",
      "R32_FLOAT",   "RG32_UINT",   "RG32_SINT",   "RG32_FLOAT",   "RGB32_FLOAT",  "RGBA32_UINT",
      "RGBA32_SINT", "RGBA32_FLOAT"};
  return kNames[size_t(format)];
}

std::vector<VertexSwapRange> PlanVertexSwap(const std::vector<VertexElement>& elements, uint32_t stream,
                                            uint32_t stride, Endian default_endian) {
  // Mark each 4-byte unit of the stride with the endian of the element covering it (elements and strides are
  // multiples of 4 bytes on the 360), then merge runs.
  const uint32_t units = stride / 4;
  std::vector<Endian> unit_endian(units, default_endian);
  for (const VertexElement& e : elements) {
    if (e.stream != stream) continue;
    const uint32_t bytes = VertexFormatBytes(e.type.Format());
    for (uint32_t b = e.offset; b < e.offset + bytes && b / 4 < units; b += 4) unit_endian[b / 4] = e.type.EndianMode();
  }
  std::vector<VertexSwapRange> plan;
  for (uint32_t u = 0; u < units; ++u) {
    if (!plan.empty() && plan.back().endian == unit_endian[u] && plan.back().offset + plan.back().size == u * 4)
      plan.back().size += 4;
    else
      plan.push_back({u * 4, 4, unit_endian[u]});
  }
  if (stride % 4) plan.push_back({units * 4, stride % 4, Endian::kNone});
  return plan;
}

void ConvertVertices(const uint8_t* guest, uint8_t* host, uint32_t vertex_count, uint32_t stride,
                     const std::vector<VertexSwapRange>& plan) {
  if (plan.size() == 1 && plan[0].offset == 0 && plan[0].size == stride) {
    CopySwap(plan[0].endian, guest, host, size_t(vertex_count) * stride);
    return;
  }
  for (uint32_t v = 0; v < vertex_count; ++v) {
    const uint8_t* s = guest + size_t(v) * stride;
    uint8_t* d = host + size_t(v) * stride;
    for (const VertexSwapRange& r : plan) CopySwap(r.endian, s + r.offset, d + r.offset, r.size);
  }
}

void ConvertIndices(const uint8_t* guest, void* host, uint32_t count, bool index32) {
  if (index32)
    CopySwapIndices32(guest, host, count);
  else
    CopySwapIndices16(guest, host, count);
}

}  // namespace kknr
