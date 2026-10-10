// Index and vertex buffers: the guest objects, the 360 vertex declaration and the byte swaps that turn the
// big-endian guest data into host buffers.
//
// Endianness is per vertex element, not per buffer: the 360 D3DDECLTYPE carries its own endian field (the
// game's SHORT4N elements are 8in16, its float and colour elements 8in32, in the same stream), so the D3D
// runtime fetches each element through a fetch constant with that element's endian. A host vertex buffer is
// therefore converted for a (buffer, stride, declaration stream) combination: every element's bytes are
// swapped with its own mode, so the host input layout reads the data the way the guest shader did.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kknr/xenos.h"

namespace kknr {

// CPU virtual -> GPU physical address (the XDK's GPU_CONVERT_CPU_TO_GPU_ADDRESS): the 0xE0000000 view is 4 KB
// ahead of physical memory.
inline uint32_t CpuToPhysical(uint32_t va) {
  return (va & 0x1FFFFFFFu) + ((((va >> 20) + 0x200) & 0x1000));
}

// Index buffer object (type 2): Common (+0, bit 31 = 32-bit indices), RefCount, Fence, Address (+12, a CPU
// address), Size (+16, bytes). Draws fetch 16-bit indices with 8in16 and 32-bit ones with 8in32.
struct IndexBufferInfo {
  uint32_t physical = 0;
  uint32_t size_bytes = 0;
  bool index32 = false;
  Endian endian = Endian::k8in16;
};
bool DecodeIndexBuffer(const uint8_t* guest_object, IndexBufferInfo& out);

// Vertex buffer object (type 1): Common, RefCount, Fence, then the 2-word vertex fetch constant (+12).
struct VertexBufferInfo {
  uint32_t physical = 0;
  uint32_t size_bytes = 0;
  Endian endian = Endian::k8in32;  // the buffer's default, used for bytes no element covers
};
bool DecodeVertexBuffer(const uint8_t* guest_object, VertexBufferInfo& out);

// Xenos vertex fetch formats (a subset of the texture format numbering).
enum class VertexFormat : uint8_t {
  kUndefined = 0,
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32 = 33,
  k_32_32 = 34,
  k_32_32_32_32 = 35,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_32_32_32_FLOAT = 57,
};
uint32_t VertexFormatBytes(VertexFormat format);  // 0 for unknown

// The 360 D3DDECLTYPE: bits 0-5 vertex format, 6-7 endian, 8 signed, 9 integer (not normalized), 10-21 the
// swizzle (3 bits per component, as the texture swizzle). E.g. FLOAT3 0x002A23B9, FLOAT2 0x002C23A5,
// FLOAT4 0x001A23A6, D3DCOLOR 0x00182886 (swizzle ZYXW), UBYTE4 0x001A2286, SHORT4N 0x001A215A (8in16).
struct DeclType {
  uint32_t value = 0;
  VertexFormat Format() const { return VertexFormat(value & 63); }
  Endian EndianMode() const { return Endian((value >> 6) & 3); }
  bool Signed() const { return ((value >> 8) & 1) != 0; }
  bool Integer() const { return ((value >> 9) & 1) != 0; }
  uint16_t Swizzle() const { return uint16_t((value >> 10) & 0xFFF); }
  bool Unused() const { return value == 0xFFFFFFFFu; }
};
// Builds a D3DDECLTYPE word from its fields (for declarations described on the host side and for tests).
constexpr DeclType MakeDeclType(VertexFormat format, Endian endian, bool is_signed, bool integer,
                                uint16_t swizzle = kSwizzleXYZW) {
  return DeclType{uint32_t(format) | uint32_t(endian) << 6 | uint32_t(is_signed) << 8 | uint32_t(integer) << 9 |
                  uint32_t(swizzle & 0xFFF) << 10};
}
// The 360 types the traces show (the constants named in the comment above).
constexpr DeclType kDeclFloat2{0x002C23A5};
constexpr DeclType kDeclFloat3{0x002A23B9};
constexpr DeclType kDeclFloat4{0x001A23A6};
constexpr DeclType kDeclColor{0x00182886};
constexpr DeclType kDeclUByte4{0x001A2286};
constexpr DeclType kDeclShort4N{0x001A215A};

// One D3DVERTEXELEMENT9 as the 360 stores it (12 bytes, big-endian): Stream, Offset, Type, Method, Usage,
// UsageIndex and a byte the runtime fills in.
struct VertexElement {
  uint16_t stream = 0;
  uint16_t offset = 0;
  DeclType type;
  uint8_t method = 0, usage = 0, usage_index = 0, extra = 0;
};

// The declaration object (from the census, brief 01 to confirm): element count at +8, elements from +36,
// ending with D3DDECL_END (stream 0xFF).
std::vector<VertexElement> DecodeVertexDeclaration(const uint8_t* guest_object, size_t max_bytes = 4096);

// Host input format for an element (nvrhi::Format names). R32_UINT with needs_unpack means the shader must
// unpack the bits itself (formats with no host equivalent).
enum class HostVertexFormat : uint8_t {
  UNKNOWN,
  RGBA8_UNORM, RGBA8_SNORM, RGBA8_UINT, RGBA8_SINT,
  R10G10B10A2_UNORM,
  RG16_UNORM, RG16_SNORM, RG16_UINT, RG16_SINT, RG16_FLOAT,
  RGBA16_UNORM, RGBA16_SNORM, RGBA16_UINT, RGBA16_SINT, RGBA16_FLOAT,
  R32_UINT, R32_SINT, R32_FLOAT,
  RG32_UINT, RG32_SINT, RG32_FLOAT,
  RGB32_FLOAT,
  RGBA32_UINT, RGBA32_SINT, RGBA32_FLOAT,
};
struct HostVertexElementFormat {
  HostVertexFormat format = HostVertexFormat::UNKNOWN;
  bool needs_unpack = false;  // read as raw 32-bit words; the shader decodes (10:11:11, signed 2:10:10:10, ...)
};
HostVertexElementFormat GetHostVertexFormat(DeclType type);
const char* HostVertexFormatName(HostVertexFormat format);

// Byte ranges of one vertex and how to swap them.
struct VertexSwapRange {
  uint32_t offset = 0, size = 0;
  Endian endian = Endian::kNone;
};
// The swap plan for one stream of a declaration: each element of that stream with its own endian; the rest
// of the stride (gaps, data no element reads) with default_endian.
// conflict (optional) is set when two elements of the stream overlap with different endian modes (the later
// element wins; the GPU would fetch the same bytes twice with different swaps, which one host buffer cannot
// reproduce).
std::vector<VertexSwapRange> PlanVertexSwap(const std::vector<VertexElement>& elements, uint32_t stream,
                                            uint32_t stride, Endian default_endian, bool* conflict = nullptr);
// Identifies a swap plan for the buffer cache (BufferCache::Bind's conversion_id): equal plans, equal ids.
uint64_t VertexConversionId(const std::vector<VertexSwapRange>& plan, uint32_t stride);
// The index buffer's conversion id.
inline uint64_t IndexConversionId(bool index32) { return index32 ? 0x1D32 : 0x1D16; }
// Converts vertex_count vertices (stride bytes each) from guest to host order.
void ConvertVertices(const uint8_t* guest, uint8_t* host, uint32_t vertex_count, uint32_t stride,
                     const std::vector<VertexSwapRange>& plan);
// Index data: count indices (16- or 32-bit) to host order.
void ConvertIndices(const uint8_t* guest, void* host, uint32_t count, bool index32);

}  // namespace kknr
