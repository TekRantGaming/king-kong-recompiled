// Layouts of the guest Direct3D objects the draw path reads, and the decoders
// for them. Sources:
// - docs/d3d-api-map.md (device fields, D3DVIEWPORT, the per-draw sequence);
// - the brief 03 draft on branch nr-03-textures (resources/include/kknr/
//   buffers.h, from a census of the game's objects): the vertex buffer's
//   fetch constant at +12, the index buffer's address at +12 and size at +16,
//   bit 31 of Common = 32-bit indices, the resource type in Common bits 16-18,
//   the declaration's element count at +8 and elements from +36;
// - the SDK's xenos.h (xe_gpu_vertex_fetch_t).
// Note: d3d-api-map.md gives the type field as Common bits 13-15 (from the
// XDK headers' D3DCOMMON); the census says bits 16-18. The decoders below do
// not check the type, so either is fine for now. Brief 01 settles it.
#pragma once

#include <cstdint>

#include "backend/guest_memory.h"

namespace nr {

// CPU virtual -> GPU physical address (the XDK's GPU_CONVERT_CPU_TO_GPU_ADDRESS):
// the 0xE0000000 view is 4 KB ahead of physical memory.
inline uint32_t CpuToPhysical(uint32_t va) {
  return (va & 0x1FFFFFFFu) + ((((va >> 20) + 0x200) & 0x1000));
}

// Index buffer object: Common (+0, bit 31 = 32-bit indices), RefCount, Fence,
// Address (+12, a CPU address), Size (+16, bytes).
struct IndexBufferInfo {
  uint32_t physical = 0;
  uint32_t size_bytes = 0;
  bool index32 = false;
};
inline bool DecodeIndexBuffer(const uint8_t* object, IndexBufferInfo& out) {
  if (!object) return false;
  const uint32_t common = LoadBE32(object);
  out.index32 = (common >> 31) != 0;
  out.physical = CpuToPhysical(LoadBE32(object + 12));
  out.size_bytes = LoadBE32(object + 16);
  return true;
}

// Vertex buffer object: Common, RefCount, Fence, then the 2-word vertex fetch
// constant at +12: dword 0 = type (bits 0-1, 3 = vertex) | address (bytes,
// bits 2-31), dword 1 = endian (bits 0-1) | size in dwords (bits 2-25).
struct VertexBufferInfo {
  uint32_t physical = 0;
  uint32_t size_bytes = 0;
  uint32_t endian = 2;  // 2 = 8in32
};
inline bool DecodeVertexBuffer(const uint8_t* object, VertexBufferInfo& out) {
  if (!object) return false;
  const uint32_t w0 = LoadBE32(object + 12);
  const uint32_t w1 = LoadBE32(object + 16);
  if ((w0 & 3) != 3) return false;
  out.physical = (w0 & ~3u) & 0x1FFFFFFFu;
  out.size_bytes = ((w1 >> 2) & 0xFFFFFF) * 4;
  out.endian = w1 & 3;
  return true;
}

// Vertex declaration object: element count at +8, 12-byte D3DVERTEXELEMENT9s
// from +36 (Stream u16, Offset u16, Type u32, Method u8, Usage u8, UsageIndex
// u8, one byte), ending with stream 0xFF.
constexpr uint32_t kDeclCountOffset = 8;
constexpr uint32_t kDeclElementsOffset = 36;
constexpr uint32_t kDeclElementSize = 12;
constexpr uint8_t kDeclUsagePosition = 0;

struct DeclElement {
  uint16_t stream = 0;
  uint16_t offset = 0;
  uint32_t type = 0;
  uint8_t usage = 0;
  uint8_t usage_index = 0;
};

// The 360 D3DDECLTYPE: bits 0-5 are the vertex format. 57 = 32_32_32_FLOAT
// (FLOAT3), 38 = 32_32_32_32_FLOAT (FLOAT4).
inline uint32_t DeclTypeFormat(uint32_t type) { return type & 63; }

// Finds the position element of `stream` (usage POSITION, index 0). Returns
// false when the declaration has none.
inline bool FindPositionElement(const uint8_t* decl, uint32_t stream, DeclElement& out) {
  if (!decl) return false;
  uint32_t count = LoadBE32(decl + kDeclCountOffset);
  if (count > 64) count = 64;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = decl + kDeclElementsOffset + i * kDeclElementSize;
    DeclElement el;
    el.stream = LoadBE16(e);
    if (el.stream == 0xFF) break;
    el.offset = LoadBE16(e + 2);
    el.type = LoadBE32(e + 4);
    el.usage = e[9];
    el.usage_index = e[10];
    if (el.stream == stream && el.usage == kDeclUsagePosition && el.usage_index == 0) {
      out = el;
      return true;
    }
  }
  return false;
}

// D3DVIEWPORT9 as the engine passes it to SetViewport: X, Y, Width, Height
// (u32), MinZ, MaxZ (float).
struct Viewport {
  uint32_t x = 0, y = 0, width = 0, height = 0;
  float min_z = 0.0f, max_z = 1.0f;
  bool operator==(const Viewport&) const = default;
};
inline Viewport DecodeViewport(const uint8_t* p) {
  Viewport v;
  v.x = LoadBE32(p);
  v.y = LoadBE32(p + 4);
  v.width = LoadBE32(p + 8);
  v.height = LoadBE32(p + 12);
  v.min_z = LoadBEFloat(p + 16);
  v.max_z = LoadBEFloat(p + 20);
  return v;
}

// D3DPRIMITIVETYPE on the 360 (the Xenos primitive numbering).
enum class GuestPrimitive : uint32_t {
  kPointList = 1,
  kLineList = 2,
  kLineStrip = 3,
  kTriangleList = 4,
  kTriangleFan = 5,
  kTriangleStrip = 6,
  kRectList = 8,
  kQuadList = 13,
};

// D3DRENDERSTATETYPE values (byte offsets into the device's setter table, see
// d3d-api-map.md) for the states the traces show.
namespace rs {
constexpr uint32_t kZEnable = 40;
constexpr uint32_t kZFunc = 44;
constexpr uint32_t kZWriteEnable = 48;
constexpr uint32_t kFillMode = 52;
constexpr uint32_t kCullMode = 56;
constexpr uint32_t kAlphaTestEnable = 96;
constexpr uint32_t kAlphaRef = 100;
constexpr uint32_t kAlphaFunc = 104;
constexpr uint32_t kStencilEnable = 108;
constexpr uint32_t kStencilRef = 132;
constexpr uint32_t kClipPlaneEnable = 172;
constexpr uint32_t kColorWriteEnable = 212;
constexpr uint32_t kCount = 97 * 4;  // values 0..384
}  // namespace rs

// D3DSAMPLERSTATETYPE values (byte offsets after the render states).
namespace ss {
constexpr uint32_t kAddressU = 0;
constexpr uint32_t kAddressV = 4;
constexpr uint32_t kAddressW = 8;
constexpr uint32_t kBorderColor = 12;
constexpr uint32_t kMagFilter = 16;
constexpr uint32_t kMinFilter = 20;
constexpr uint32_t kMipFilter = 24;
constexpr uint32_t kMipMapLodBias = 28;
constexpr uint32_t kCount = 20 * 4;  // values 0..76
}  // namespace ss

// D3DCLEAR flags as the game passes them (traced: 1, 0xF, 0x3F, 0x30). Taken
// as: bit 0 or bits 4-7 (the 360's per-target bits) = colour, bit 1 = z,
// bit 2 = stencil. To be confirmed by brief 01 against the clear path
// (sub_82114D10).
namespace clear_flags {
constexpr uint32_t kTargetMask = 0xF1;
constexpr uint32_t kZBuffer = 0x2;
constexpr uint32_t kStencil = 0x4;
}  // namespace clear_flags

}  // namespace nr
