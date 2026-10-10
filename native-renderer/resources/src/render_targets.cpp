#include "kknr/render_targets.h"

#include <algorithm>

namespace kknr {

const char* HostRtFormatName(HostRtFormat format) {
  static const char* const kNames[] = {"UNKNOWN",      "RGBA8_UNORM", "R10G10B10A2_UNORM", "RGBA16_FLOAT",
                                       "RG16_FLOAT",   "RGBA16_SNORM", "RG16_SNORM",       "R32_FLOAT",
                                       "RG32_FLOAT",   "D24S8",        "D32S8"};
  return kNames[size_t(format)];
}

bool DecodeSurfaceDesc(uint32_t width, uint32_t height, uint32_t d3d_format, uint32_t multisample,
                       uint32_t edram_base, SurfaceDesc& out) {
  out = SurfaceDesc();
  out.width = width;
  out.height = height;
  out.d3d_format = d3d_format;
  out.edram_base = edram_base;
  out.msaa = multisample == 2 ? 4 : (multisample == 1 ? 2 : 1);
  const D3DFormat f{d3d_format};
  out.format = f.Format();
  switch (out.format) {
    case TextureFormat::k_8_8_8_8:
    case TextureFormat::k_8_8_8_8_A:
    case TextureFormat::k_8_8_8_8_AS_16_16_16_16:
    case TextureFormat::k_8_8_8_8_GAMMA_EDRAM:
      out.host = HostRtFormat::RGBA8_UNORM;
      break;
    case TextureFormat::k_2_10_10_10:
    case TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      out.host = HostRtFormat::R10G10B10A2_UNORM;
      break;
    case TextureFormat::k_2_10_10_10_FLOAT_EDRAM:
    case TextureFormat::k_16_16_16_16_FLOAT:
      out.host = HostRtFormat::RGBA16_FLOAT;
      break;
    case TextureFormat::k_16_16_FLOAT:
      out.host = HostRtFormat::RG16_FLOAT;
      break;
    case TextureFormat::k_16_16_16_16:
    case TextureFormat::k_16_16_16_16_EDRAM:
      out.host = HostRtFormat::RGBA16_SNORM;
      break;
    case TextureFormat::k_16_16:
    case TextureFormat::k_16_16_EDRAM:
      out.host = HostRtFormat::RG16_SNORM;
      break;
    case TextureFormat::k_32_FLOAT:
      out.host = HostRtFormat::R32_FLOAT;
      break;
    case TextureFormat::k_32_32_FLOAT:
      out.host = HostRtFormat::RG32_FLOAT;
      break;
    case TextureFormat::k_24_8:
      out.host = HostRtFormat::D24S8;
      out.depth = true;
      break;
    case TextureFormat::k_24_8_FLOAT:
      out.host = HostRtFormat::D32S8;
      out.depth = true;
      break;
    default:
      return false;
  }
  return width && height;
}

uint32_t SurfaceEdramTiles(const SurfaceDesc& d) {
  // XGSurfaceSize: double the height for 2x / 4x, the width for 4x, align to 80x16 samples, x2 for 64 bpp.
  const uint32_t w = d.width * (d.msaa >= 4 ? 2 : 1), h = d.height * (d.msaa >= 2 ? 2 : 1);
  const bool is64 = d.host == HostRtFormat::RGBA16_FLOAT || d.host == HostRtFormat::RGBA16_SNORM ||
                    d.host == HostRtFormat::RG32_FLOAT;
  return ((w + 79) / 80) * ((h + 15) / 16) * (is64 ? 2 : 1);
}

size_t RenderTargetPool::Acquire(const SurfaceDesc& desc, uint64_t frame, bool* created) {
  size_t best = SIZE_MAX;
  for (size_t i = 0; i < slots_.size(); ++i) {
    const Slot& s = slots_[i];
    if (s.in_use || !(s.desc == desc)) continue;
    if (best == SIZE_MAX || s.released_order > slots_[best].released_order) best = i;
  }
  if (created) *created = best == SIZE_MAX;
  if (best == SIZE_MAX) {
    // Reuse an emptied slot (host == nullptr, not in use) before growing.
    for (size_t i = 0; i < slots_.size(); ++i)
      if (!slots_[i].in_use && !slots_[i].host && slots_[i].desc.width == 0) {
        best = i;
        break;
      }
    if (best == SIZE_MAX) {
      best = slots_.size();
      slots_.emplace_back();
    }
    slots_[best] = Slot();
    slots_[best].desc = desc;
  }
  Slot& s = slots_[best];
  s.in_use = true;
  s.last_used_frame = frame;
  return best;
}

void RenderTargetPool::Release(size_t handle) {
  if (handle >= slots_.size()) return;
  slots_[handle].in_use = false;
  slots_[handle].released_order = ++release_counter_;
}

size_t RenderTargetPool::InUse() const {
  return size_t(std::count_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.in_use; }));
}

std::vector<RenderTargetPool::Slot> RenderTargetPool::Trim(uint64_t frame, uint64_t max_age) {
  std::vector<Slot> freed;
  for (Slot& s : slots_) {
    if (s.in_use || s.desc.width == 0 || frame <= s.last_used_frame + max_age) continue;
    freed.push_back(s);
    s = Slot();  // empty, reusable
  }
  return freed;
}

ResolveOp PlanResolve(uint32_t flags_value, const SurfaceDesc& source, const uint32_t* rect, const TextureFetch* dest,
                      const uint32_t* point, uint32_t level, uint32_t slice) {
  const ResolveFlags flags{flags_value};
  ResolveOp op;
  op.clear_color = flags.ClearRenderTarget();
  op.clear_depth = flags.ClearDepthStencil();
  op.dst_level = level;
  op.dst_slice = slice;
  if (rect) {
    op.src_x = rect[0];
    op.src_y = rect[1];
    op.width = rect[2] > rect[0] ? rect[2] - rect[0] : 0;
    op.height = rect[3] > rect[1] ? rect[3] - rect[1] : 0;
  } else {
    op.width = source.width;
    op.height = source.height;
  }
  if (point) {
    op.dst_x = point[0];
    op.dst_y = point[1];
  }
  if (!dest) {
    op.kind = ResolveKind::kNone;
    return op;
  }
  // Clip to the destination level.
  const uint32_t dw = std::max(dest->Width() >> level, 1u), dh = std::max(dest->Height() >> level, 1u);
  op.width = std::min(op.width, dw > op.dst_x ? dw - op.dst_x : 0);
  op.height = std::min(op.height, dh > op.dst_y ? dh - op.dst_y : 0);

  const TextureFormat df = dest->Format();
  if (flags.DepthStencil() || source.depth) {
    op.kind = ResolveKind::kDepthToFloat;
    return op;
  }
  const uint16_t swizzle = dest->Swizzle();
  const bool four = df == TextureFormat::k_8_8_8_8 || df == TextureFormat::k_8_8_8_8_A ||
                    df == TextureFormat::k_8_8_8_8_AS_16_16_16_16 || df == TextureFormat::k_2_10_10_10 ||
                    df == TextureFormat::k_2_10_10_10_AS_16_16_16_16 || df == TextureFormat::k_16_16_16_16 ||
                    df == TextureFormat::k_16_16_16_16_FLOAT;
  op.swap_red_blue = four && SwizzleComponent(swizzle, 0) == kSwzZ && SwizzleComponent(swizzle, 2) == kSwzX;
  // Same host family: a plain copy (with the swap done by the view or a blit, see docs/formats.md).
  SurfaceDesc as_target;
  const bool same = DecodeSurfaceDesc(source.width, source.height, D3DFormat{uint32_t(df)}.value, 0, 0, as_target) &&
                    as_target.host == source.host;
  op.kind = same ? ResolveKind::kCopy : ResolveKind::kConvert;
  return op;
}

}  // namespace kknr
