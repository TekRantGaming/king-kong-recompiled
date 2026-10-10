// Render targets: EDRAM surfaces (CreateRenderTarget) become pooled host render targets, and Resolve becomes
// a copy (or a clear) into the destination texture.
//
// The game creates its render-to-texture surfaces every frame (12 switches per gameplay frame: 832x832 R32F
// shadow maps, 320x180 / 640x360 / 640x480 A8R8G8B8 / X8R8G8B8 post-effect targets) with EDRAM base 0, draws,
// resolves into a texture and releases them. The pool hands back the host target released most recently
// for the same description, so a surface re-created at the same EDRAM place sees the same pixels, as it
// would on the 360, and nothing is allocated per frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "kknr/xenos.h"

namespace kknr {

// Host render-target formats (nvrhi::Format names).
enum class HostRtFormat : uint8_t {
  UNKNOWN,
  RGBA8_UNORM,        // k_8_8_8_8 (and its gamma variant: the shader / view applies the curve)
  R10G10B10A2_UNORM,  // k_2_10_10_10
  RGBA16_FLOAT,       // k_2_10_10_10_FLOAT (7e3) and k_16_16_16_16_FLOAT
  RG16_FLOAT,
  RGBA16_SNORM,       // k_16_16_16_16 (fixed -32..32: the shader scales)
  RG16_SNORM,
  R32_FLOAT,
  RG32_FLOAT,
  D24S8,              // depth k_24_8
  D32S8,              // depth k_24_8_FLOAT (20e4 kept in a float depth buffer)
};
const char* HostRtFormatName(HostRtFormat format);

struct SurfaceDesc {
  uint32_t width = 0, height = 0;
  uint32_t d3d_format = 0;          // the 360 D3DFORMAT word
  TextureFormat format = TextureFormat::k_8_8_8_8;
  uint32_t msaa = 1;                // samples: 1, 2, 4
  uint32_t edram_base = 0;          // tiles (D3DSURFACE_PARAMETERS::Base)
  bool depth = false;
  HostRtFormat host = HostRtFormat::UNKNOWN;
  bool operator==(const SurfaceDesc& o) const {
    return width == o.width && height == o.height && host == o.host && msaa == o.msaa &&
           edram_base == o.edram_base && depth == o.depth;
  }
};
// From the CreateRenderTarget arguments: width, height, D3DFORMAT, D3DMULTISAMPLE_TYPE (0 none, 1 2x, 2 4x)
// and the parameters' EDRAM base.
bool DecodeSurfaceDesc(uint32_t width, uint32_t height, uint32_t d3d_format, uint32_t multisample,
                       uint32_t edram_base, SurfaceDesc& out);
// EDRAM tiles a surface occupies (80x16-sample tiles of 32 bits; 64 bpp formats take two).
uint32_t SurfaceEdramTiles(const SurfaceDesc& desc);

// Pool of host render targets. Handles are indices; host is the renderer's resource.
class RenderTargetPool {
 public:
  struct Slot {
    SurfaceDesc desc;
    void* host = nullptr;
    bool in_use = false;
    uint64_t last_used_frame = 0;
    uint64_t released_order = 0;  // larger = released more recently
  };
  // A free slot with the same description (the most recently released), or a new one (host == nullptr: the
  // renderer must create it). created reports which.
  size_t Acquire(const SurfaceDesc& desc, uint64_t frame, bool* created = nullptr);
  void Release(size_t handle);
  Slot& Get(size_t handle) { return slots_[handle]; }
  size_t size() const { return slots_.size(); }
  size_t InUse() const;
  // Frees (returns) slots unused for more than max_age frames; the renderer destroys their host resources.
  std::vector<Slot> Trim(uint64_t frame, uint64_t max_age);

 private:
  std::vector<Slot> slots_;
  uint64_t release_counter_ = 0;
};

// Resolve (sub_82116178) flags, as the XDK's D3DRESOLVE_*.
struct ResolveFlags {
  uint32_t value = 0;
  bool DepthStencil() const { return (value & 4) != 0; }           // source is the depth buffer
  uint32_t RenderTargetIndex() const { return value & 3; }         // source colour target 0-3
  uint32_t Fragments() const { return (value >> 4) & 15; }         // 0 = all samples
  bool ClearRenderTarget() const { return (value & 0x100) != 0; }  // clear the source colour after copying
  bool ClearDepthStencil() const { return (value & 0x200) != 0; }
};

enum class ResolveKind : uint8_t {
  kCopy,          // same format family: copy (rect / offset applied)
  kConvert,       // colour formats differ: a blit converts
  kDepthToFloat,  // depth buffer -> k_24_8 / k_32_FLOAT texture: read depth, write R32_FLOAT
  kNone,          // nothing to copy (no destination), clears only
};

struct ResolveOp {
  ResolveKind kind = ResolveKind::kNone;
  bool swap_red_blue = false;  // destination swizzle reads X from the source's blue (A8R8G8B8 style)
  bool clear_color = false, clear_depth = false;
  uint32_t src_x = 0, src_y = 0, width = 0, height = 0;  // source rectangle
  uint32_t dst_x = 0, dst_y = 0;
  uint32_t dst_level = 0, dst_slice = 0;
};
// source: the surface being resolved; rect / point may be null (whole surface, origin); dest may be null.
ResolveOp PlanResolve(uint32_t flags, const SurfaceDesc& source, const uint32_t* rect_ltrb, const TextureFetch* dest,
                      const uint32_t* point_xy, uint32_t level, uint32_t slice);

}  // namespace kknr
