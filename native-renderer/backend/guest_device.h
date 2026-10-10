// The game's Direct3D device struct (20,608 bytes at *(0x82D62324)) as the
// native renderer reads it at draw time: the register images the library's
// setters keep up to date, the constant shadows and the bound objects
// (docs/d3d-api-map.md, "Draw-time state recipe" and "The register shadow";
// docs/d3d-structs.md, "The device"). Everything is big-endian guest memory.
//
// The hooks run before the library's own draw, so these are exactly the
// values the GPU would have been sent for this draw (the setters write the
// images immediately; only the vertex fetch constants 95-s are built later,
// and those come from the streams instead).
#pragma once

#include <cstdint>
#include <cstring>

#include "backend/guest_memory.h"

namespace nr::dev {

// Constant shadows and fetch constants.
constexpr uint32_t kFetchConstants = 1152;  // 32 x 6 dwords
constexpr uint32_t kVsConstants = 1920;     // c0-c255, 16 bytes each
constexpr uint32_t kPsConstants = 6016;
constexpr uint32_t kBoolConstants = 10112;  // 8 dwords
constexpr uint32_t kLoopConstants = 10144;  // 32 dwords

// Register images.
constexpr uint32_t kRbSurfaceInfo = 11456;   // pitch 0-13, msaa 16-17
constexpr uint32_t kRbColorInfo = 11460;     // base 0-11, format 16-19, exp bias 20-25
constexpr uint32_t kRbDepthInfo = 11464;     // base 0-11, format bit 16
constexpr uint32_t kRbColor1Info = 11468;    // 1-3 follow
constexpr uint32_t kPaScWindowOffset = 11520;
constexpr uint32_t kPaScWindowScissorTl = 11524;
constexpr uint32_t kPaScWindowScissorBr = 11528;
constexpr uint32_t kVgtIndxOffset = 11540;
constexpr uint32_t kRbColorMask = 11548;
constexpr uint32_t kRbBlendRgba = 11552;     // 4 floats
constexpr uint32_t kRbStencilRefMaskBf = 11580;
constexpr uint32_t kRbStencilRefMask = 11584;
constexpr uint32_t kRbAlphaRef = 11588;      // float
constexpr uint32_t kPaClVport = 11592;       // XSCALE XOFFSET YSCALE YOFFSET ZSCALE ZOFFSET
constexpr uint32_t kRbDepthControl = 11636;
constexpr uint32_t kRbBlendControl0 = 11640;
constexpr uint32_t kRbColorControl = 11644;
constexpr uint32_t kPaClClipCntl = 11652;
constexpr uint32_t kPaSuScModeCntl = 11656;
constexpr uint32_t kPaClVteCntl = 11660;
constexpr uint32_t kRbBlendControl1 = 11672;  // 1-3 follow
constexpr uint32_t kPaSuVtxCntl = 11776;
constexpr uint32_t kPaSuPolyOffset = 11920;   // front scale, front offset, back scale, back offset

// D3D-level fields.
constexpr uint32_t kIndexBuffer = 12532;
constexpr uint32_t kRenderTargets = 12536;   // 4 surfaces
constexpr uint32_t kDepthStencil = 12552;
constexpr uint32_t kStreamOffset = 12556;    // + 8s
constexpr uint32_t kStreamBuffer = 12560;    // + 8s
constexpr uint32_t kTextures = 12704;        // + 4n
constexpr uint32_t kViewport = 12808;        // X Y W H (u32), MinZ MaxZ
constexpr uint32_t kClipPlanes = 12848;      // 6 x float4
constexpr uint32_t kFrontBuffer = 13960;
constexpr uint32_t kGammaRamp = 14060;      // D3DGAMMARAMP: WORD red[256], green[256], blue[256]
constexpr uint32_t kBackBuffer = 13964;
constexpr uint32_t kSize = 20608;

// A read-only view of the device at a guest address.
class DeviceView {
 public:
  DeviceView() = default;
  explicit DeviceView(const uint8_t* base) : base_(base) {}
  bool valid() const { return base_ != nullptr; }
  const uint8_t* at(uint32_t offset) const { return base_ + offset; }
  uint32_t u32(uint32_t offset) const { return LoadBE32(base_ + offset); }
  float f32(uint32_t offset) const { return LoadBEFloat(base_ + offset); }
  // Fetch constant n (6 words) in host order.
  void fetch_constant(uint32_t n, uint32_t out[6]) const {
    for (uint32_t i = 0; i < 6; ++i) out[i] = u32(kFetchConstants + n * 24 + i * 4);
  }

 private:
  const uint8_t* base_ = nullptr;
};

// Surface objects (CreateRenderTarget, 64 bytes): a fetch-constant image at
// +16 (word 2: width - 1 bits 0-12, height - 1 bits 13-25), RB_SURFACE_INFO at
// +48, RB_COLOR_INFO / RB_DEPTH_INFO at +52 (docs/d3d-structs.md).
struct SurfaceInfo {
  uint32_t width = 0, height = 0;
  uint32_t pitch = 0;       // pixels (RB_SURFACE_INFO)
  uint32_t edram_base = 0;  // tiles
  uint32_t format = 0;      // ColorRenderTargetFormat, or 0 / 1 for depth (D24S8 / D24FS8)
  uint32_t msaa = 0;        // 0 = 1x
};
inline bool DecodeSurface(const uint8_t* object, bool depth, SurfaceInfo& out) {
  if (!object) return false;
  const uint32_t size = LoadBE32(object + 16 + 8);
  out.width = (size & 0x1FFF) + 1;
  out.height = ((size >> 13) & 0x1FFF) + 1;
  const uint32_t surface_info = LoadBE32(object + 48);
  out.pitch = surface_info & 0x3FFF;
  out.msaa = (surface_info >> 16) & 3;
  const uint32_t info = LoadBE32(object + 52);
  out.edram_base = info & 0xFFF;
  out.format = depth ? (info >> 16) & 1 : (info >> 16) & 0xF;
  return true;
}

}  // namespace nr::dev
