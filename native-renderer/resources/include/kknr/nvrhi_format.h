// kknr host formats -> NVRHI. Header-only and the only place the library meets NVRHI, so the converters
// build and test without it. tests/test_nvrhi_format.cpp checks every entry against NVRHI's own format table
// (name, bytes per block, block size).
#pragma once

#include <nvrhi/nvrhi.h>

#include "kknr/buffers.h"
#include "kknr/render_targets.h"
#include "kknr/texture_convert.h"

namespace kknr {

inline nvrhi::Format ToNvrhi(HostFormat f) {
  using N = nvrhi::Format;
  switch (f) {
    case HostFormat::R8_UNORM: return N::R8_UNORM;
    case HostFormat::R8_SNORM: return N::R8_SNORM;
    case HostFormat::RG8_UNORM: return N::RG8_UNORM;
    case HostFormat::RG8_SNORM: return N::RG8_SNORM;
    case HostFormat::R16_UNORM: return N::R16_UNORM;
    case HostFormat::R16_SNORM: return N::R16_SNORM;
    case HostFormat::R16_FLOAT: return N::R16_FLOAT;
    case HostFormat::BGRA4_UNORM: return N::BGRA4_UNORM;
    case HostFormat::B5G6R5_UNORM: return N::B5G6R5_UNORM;
    case HostFormat::B5G5R5A1_UNORM: return N::B5G5R5A1_UNORM;
    case HostFormat::RGBA8_UNORM: return N::RGBA8_UNORM;
    case HostFormat::RGBA8_SNORM: return N::RGBA8_SNORM;
    case HostFormat::R10G10B10A2_UNORM: return N::R10G10B10A2_UNORM;
    case HostFormat::RG16_UNORM: return N::RG16_UNORM;
    case HostFormat::RG16_SNORM: return N::RG16_SNORM;
    case HostFormat::RG16_FLOAT: return N::RG16_FLOAT;
    case HostFormat::R32_UINT: return N::R32_UINT;
    case HostFormat::R32_SINT: return N::R32_SINT;
    case HostFormat::R32_FLOAT: return N::R32_FLOAT;
    case HostFormat::RGBA16_FLOAT: return N::RGBA16_FLOAT;
    case HostFormat::RGBA16_UNORM: return N::RGBA16_UNORM;
    case HostFormat::RGBA16_SNORM: return N::RGBA16_SNORM;
    case HostFormat::RG32_UINT: return N::RG32_UINT;
    case HostFormat::RG32_SINT: return N::RG32_SINT;
    case HostFormat::RG32_FLOAT: return N::RG32_FLOAT;
    case HostFormat::RGBA32_UINT: return N::RGBA32_UINT;
    case HostFormat::RGBA32_SINT: return N::RGBA32_SINT;
    case HostFormat::RGBA32_FLOAT: return N::RGBA32_FLOAT;
    case HostFormat::BC1_UNORM: return N::BC1_UNORM;
    case HostFormat::BC2_UNORM: return N::BC2_UNORM;
    case HostFormat::BC3_UNORM: return N::BC3_UNORM;
    case HostFormat::BC4_UNORM: return N::BC4_UNORM;
    case HostFormat::BC4_SNORM: return N::BC4_SNORM;
    case HostFormat::BC5_UNORM: return N::BC5_UNORM;
    case HostFormat::BC5_SNORM: return N::BC5_SNORM;
    default: return N::UNKNOWN;
  }
}

inline nvrhi::Format ToNvrhi(HostRtFormat f) {
  using N = nvrhi::Format;
  switch (f) {
    case HostRtFormat::RGBA8_UNORM: return N::RGBA8_UNORM;
    case HostRtFormat::R10G10B10A2_UNORM: return N::R10G10B10A2_UNORM;
    case HostRtFormat::RGBA16_FLOAT: return N::RGBA16_FLOAT;
    case HostRtFormat::RG16_FLOAT: return N::RG16_FLOAT;
    case HostRtFormat::RGBA16_SNORM: return N::RGBA16_SNORM;
    case HostRtFormat::RG16_SNORM: return N::RG16_SNORM;
    case HostRtFormat::R32_FLOAT: return N::R32_FLOAT;
    case HostRtFormat::RG32_FLOAT: return N::RG32_FLOAT;
    case HostRtFormat::D24S8: return N::D24S8;
    case HostRtFormat::D32S8: return N::D32S8;
    default: return N::UNKNOWN;
  }
}

inline nvrhi::Format ToNvrhi(HostVertexFormat f) {
  using N = nvrhi::Format;
  using V = HostVertexFormat;
  switch (f) {
    case V::RGBA8_UNORM: return N::RGBA8_UNORM;
    case V::RGBA8_SNORM: return N::RGBA8_SNORM;
    case V::RGBA8_UINT: return N::RGBA8_UINT;
    case V::RGBA8_SINT: return N::RGBA8_SINT;
    case V::R10G10B10A2_UNORM: return N::R10G10B10A2_UNORM;
    case V::RG16_UNORM: return N::RG16_UNORM;
    case V::RG16_SNORM: return N::RG16_SNORM;
    case V::RG16_UINT: return N::RG16_UINT;
    case V::RG16_SINT: return N::RG16_SINT;
    case V::RG16_FLOAT: return N::RG16_FLOAT;
    case V::RGBA16_UNORM: return N::RGBA16_UNORM;
    case V::RGBA16_SNORM: return N::RGBA16_SNORM;
    case V::RGBA16_UINT: return N::RGBA16_UINT;
    case V::RGBA16_SINT: return N::RGBA16_SINT;
    case V::RGBA16_FLOAT: return N::RGBA16_FLOAT;
    case V::R32_UINT: return N::R32_UINT;
    case V::R32_SINT: return N::R32_SINT;
    case V::R32_FLOAT: return N::R32_FLOAT;
    case V::RG32_UINT: return N::RG32_UINT;
    case V::RG32_SINT: return N::RG32_SINT;
    case V::RG32_FLOAT: return N::RG32_FLOAT;
    case V::RGB32_FLOAT: return N::RGB32_FLOAT;
    case V::RGBA32_UINT: return N::RGBA32_UINT;
    case V::RGBA32_SINT: return N::RGBA32_SINT;
    case V::RGBA32_FLOAT: return N::RGBA32_FLOAT;
    default: return N::UNKNOWN;
  }
}

// The plan's view swizzle as an SRV component mapping (the same 3-bit encoding).
inline nvrhi::ComponentMapping ToNvrhiMapping(uint16_t view_swizzle) {
  nvrhi::ComponentMapping m;
  m.r = nvrhi::ComponentSwizzle(SwizzleComponent(view_swizzle, 0));
  m.g = nvrhi::ComponentSwizzle(SwizzleComponent(view_swizzle, 1));
  m.b = nvrhi::ComponentSwizzle(SwizzleComponent(view_swizzle, 2));
  m.a = nvrhi::ComponentSwizzle(SwizzleComponent(view_swizzle, 3));
  return m;
}

}  // namespace kknr
