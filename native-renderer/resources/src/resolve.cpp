#include "kknr/resolve.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "kknr/endian.h"
#include "kknr/tiling.h"

namespace kknr {

namespace {

using T = TextureFormat;
using H = HostFormat;

uint32_t Unorm(float v, uint32_t max) {
  return uint32_t(std::floor(std::clamp(std::isnan(v) ? 0.0f : v, 0.0f, 1.0f) * float(max) + 0.5f));
}
int32_t Snorm(float v, int32_t max) {
  return int32_t(std::lround(std::clamp(std::isnan(v) ? 0.0f : v, -1.0f, 1.0f) * float(max)));
}
void Put16(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 2); }
void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void PutF(uint8_t* p, float v) { std::memcpy(p, &v, 4); }
uint16_t Get16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}
uint32_t Get32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// The SDK's fast path: a fixed-point target into the 16-bit texture of the same layout is copied as bits.
bool FixedBitCopy(const ResolveSource& s, TextureFormat dest) {
  return !s.depth && ((s.format == uint32_t(EdramColorFormat::k_16_16) && dest == T::k_16_16) ||
                      (s.format == uint32_t(EdramColorFormat::k_16_16_16_16) && dest == T::k_16_16_16_16));
}

bool IsSnormHost(HostFormat f) {
  return f == H::R8_SNORM || f == H::RG8_SNORM || f == H::RGBA8_SNORM || f == H::R16_SNORM || f == H::RG16_SNORM ||
         f == H::RGBA16_SNORM || f == H::BC4_SNORM || f == H::BC5_SNORM;
}

// Formats a resolve can write (1x1 blocks the resolve shaders pack).
bool IsResolveColorDest(TextureFormat f) {
  switch (f) {
    case T::k_8: case T::k_8_A: case T::k_8_B: case T::k_8_8:
    case T::k_8_8_8_8: case T::k_8_8_8_8_A: case T::k_8_8_8_8_AS_16_16_16_16:
    case T::k_2_10_10_10: case T::k_2_10_10_10_AS_16_16_16_16:
    case T::k_10_11_11: case T::k_10_11_11_AS_16_16_16_16: case T::k_11_11_10: case T::k_11_11_10_AS_16_16_16_16:
    case T::k_16: case T::k_16_16: case T::k_16_16_16_16:
    case T::k_16_FLOAT: case T::k_16_16_FLOAT: case T::k_16_16_16_16_FLOAT:
    case T::k_32_FLOAT: case T::k_32_32_FLOAT: case T::k_32_32_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool IsEdramColorFormat64bpp(uint32_t f) {
  return f == uint32_t(EdramColorFormat::k_16_16_16_16) || f == uint32_t(EdramColorFormat::k_16_16_16_16_FLOAT) ||
         f == uint32_t(EdramColorFormat::k_32_32_FLOAT);
}

HostFormat EdramColorHostFormat(uint32_t format) {
  switch (EdramColorFormat(format)) {
    case EdramColorFormat::k_8_8_8_8:
    case EdramColorFormat::k_8_8_8_8_GAMMA:
      return H::RGBA8_UNORM;
    case EdramColorFormat::k_2_10_10_10:
    case EdramColorFormat::k_2_10_10_10_AS_10_10_10_10:
      return H::R10G10B10A2_UNORM;
    case EdramColorFormat::k_2_10_10_10_FLOAT:
    case EdramColorFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case EdramColorFormat::k_16_16_16_16:
    case EdramColorFormat::k_16_16_16_16_FLOAT:
      return H::RGBA16_FLOAT;
    case EdramColorFormat::k_16_16:
    case EdramColorFormat::k_16_16_FLOAT:
      return H::RG16_FLOAT;
    case EdramColorFormat::k_32_FLOAT:
      return H::R32_FLOAT;
    case EdramColorFormat::k_32_32_FLOAT:
      return H::RG32_FLOAT;
  }
  return H::RGBA8_UNORM;
}

bool ResolveDestSwap(const TextureFetch& dest) {
  const TextureFormat f = dest.Format();
  const bool four = f == T::k_8_8_8_8 || f == T::k_8_8_8_8_A || f == T::k_8_8_8_8_AS_16_16_16_16 ||
                    f == T::k_2_10_10_10 || f == T::k_2_10_10_10_AS_16_16_16_16 || f == T::k_16_16_16_16 ||
                    f == T::k_16_16_16_16_FLOAT || f == T::k_32_32_32_32_FLOAT;
  const uint16_t s = dest.Swizzle();
  return four && SwizzleComponent(s, 0) == kSwzZ && SwizzleComponent(s, 2) == kSwzX;
}

// ---------------------------------------------------------------- host texels

bool EncodeHostTexel(HostFormat f, const float c[4], uint8_t* out) {
  switch (f) {
    case H::R8_UNORM: case H::RG8_UNORM: case H::RGBA8_UNORM: {
      const int n = f == H::R8_UNORM ? 1 : f == H::RG8_UNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) out[i] = uint8_t(Unorm(c[i], 255));
      return true;
    }
    case H::R8_SNORM: case H::RG8_SNORM: case H::RGBA8_SNORM: {
      const int n = f == H::R8_SNORM ? 1 : f == H::RG8_SNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) out[i] = uint8_t(int8_t(Snorm(c[i], 127)));
      return true;
    }
    case H::R16_UNORM: case H::RG16_UNORM: case H::RGBA16_UNORM: {
      const int n = f == H::R16_UNORM ? 1 : f == H::RG16_UNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) Put16(out + 2 * i, Unorm(c[i], 65535));
      return true;
    }
    case H::R16_SNORM: case H::RG16_SNORM: case H::RGBA16_SNORM: {
      const int n = f == H::R16_SNORM ? 1 : f == H::RG16_SNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) Put16(out + 2 * i, uint16_t(int16_t(Snorm(c[i], 32767))));
      return true;
    }
    case H::R16_FLOAT: case H::RG16_FLOAT: case H::RGBA16_FLOAT: {
      const int n = f == H::R16_FLOAT ? 1 : f == H::RG16_FLOAT ? 2 : 4;
      for (int i = 0; i < n; ++i) Put16(out + 2 * i, FloatToHalf(c[i]));
      return true;
    }
    case H::R32_FLOAT: case H::RG32_FLOAT: case H::RGBA32_FLOAT: {
      const int n = f == H::R32_FLOAT ? 1 : f == H::RG32_FLOAT ? 2 : 4;
      for (int i = 0; i < n; ++i) PutF(out + 4 * i, c[i]);
      return true;
    }
    case H::R10G10B10A2_UNORM:
      Put32(out, Unorm(c[0], 1023) | Unorm(c[1], 1023) << 10 | Unorm(c[2], 1023) << 20 | Unorm(c[3], 3) << 30);
      return true;
    default:
      return false;
  }
}

bool DecodeHostTexel(HostFormat f, const uint8_t* in, float c[4]) {
  c[0] = c[1] = c[2] = 0.0f;
  c[3] = 1.0f;
  switch (f) {
    case H::R8_UNORM: case H::RG8_UNORM: case H::RGBA8_UNORM: {
      const int n = f == H::R8_UNORM ? 1 : f == H::RG8_UNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = in[i] / 255.0f;
      return true;
    }
    case H::R8_SNORM: case H::RG8_SNORM: case H::RGBA8_SNORM: {
      const int n = f == H::R8_SNORM ? 1 : f == H::RG8_SNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = std::max(int8_t(in[i]) / 127.0f, -1.0f);
      return true;
    }
    case H::R16_UNORM: case H::RG16_UNORM: case H::RGBA16_UNORM: {
      const int n = f == H::R16_UNORM ? 1 : f == H::RG16_UNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = Get16(in + 2 * i) / 65535.0f;
      return true;
    }
    case H::R16_SNORM: case H::RG16_SNORM: case H::RGBA16_SNORM: {
      const int n = f == H::R16_SNORM ? 1 : f == H::RG16_SNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = std::max(int16_t(Get16(in + 2 * i)) / 32767.0f, -1.0f);
      return true;
    }
    case H::R16_FLOAT: case H::RG16_FLOAT: case H::RGBA16_FLOAT: {
      const int n = f == H::R16_FLOAT ? 1 : f == H::RG16_FLOAT ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = HalfToFloat(Get16(in + 2 * i));
      return true;
    }
    case H::R32_FLOAT: case H::RG32_FLOAT: case H::RGBA32_FLOAT: {
      const int n = f == H::R32_FLOAT ? 1 : f == H::RG32_FLOAT ? 2 : 4;
      for (int i = 0; i < n; ++i) std::memcpy(&c[i], in + 4 * i, 4);
      return true;
    }
    case H::R10G10B10A2_UNORM: {
      const uint32_t v = Get32(in);
      for (int i = 0; i < 3; ++i) c[i] = ((v >> (10 * i)) & 1023) / 1023.0f;
      c[3] = (v >> 30) / 3.0f;
      return true;
    }
    default:
      return false;
  }
}

// ---------------------------------------------------------------- the GPU path

ResolveConversion PlanResolveConversion(const ResolveSource& source, const TextureFetch& dest,
                                        const TextureOptions& options) {
  ResolveConversion c;
  c.depth = source.depth;
  c.source_format = source.depth ? H::R32_FLOAT : EdramColorHostFormat(source.format);
  HostTexturePlan plan;
  if (!PlanHostTexture(dest, plan, nullptr, options)) {
    c.why = "destination texture not convertible";
    return c;
  }
  c.dest_format = plan.format;
  const TextureFormat df = dest.Format();
  if (source.depth) {
    // The resolve writes the raw depth word; it reads back as depth only through the matching format.
    const bool match = (source.format == uint32_t(EdramDepthFormat::kD24S8) && df == T::k_24_8) ||
                       (source.format == uint32_t(EdramDepthFormat::kD24FS8) && df == T::k_24_8_FLOAT);
    if (!match) {
      c.why = (df == T::k_24_8 || df == T::k_24_8_FLOAT) ? "depth format differs from the texture's"
                                                        : "depth into a non-depth texture";
      return c;
    }
    c.method = ResolveMethod::kBlit;
    c.channels[0] = 0;
    c.channels[1] = c.channels[2] = 4;
    c.channels[3] = 5;
    return c;
  }
  if (!IsResolveColorDest(df)) {
    c.why = "not a resolvable texture format";
    return c;
  }
  uint8_t probe[16];
  const float zero[4] = {};
  if (!EncodeHostTexel(plan.format, zero, probe)) {
    c.why = "destination host format cannot be drawn into";
    return c;
  }
  if (FixedBitCopy(source, df)) {
    // The 360 copies the fixed-point bits; read as signed normalized they are value / 32.
    if (!IsSnormHost(plan.format)) {
      c.why = "fixed-point bits read as unsigned";
      return c;
    }
    c.scale = 1.0f / 32.0f;
  } else if (IsSnormHost(plan.format)) {
    c.why = "signed destination reinterprets the colour bits";
    return c;
  }
  c.swap_red_blue = ResolveDestSwap(dest);
  if (c.swap_red_blue) {
    c.channels[0] = 2;
    c.channels[2] = 0;
  }
  // Three-component formats widened to a four-channel host format: the host alpha reads 1, as ConvertTexture
  // fills it (the view never reads it, but the texel stays the same either way).
  if (df == T::k_10_11_11 || df == T::k_10_11_11_AS_16_16_16_16 || df == T::k_11_11_10 ||
      df == T::k_11_11_10_AS_16_16_16_16)
    c.channels[3] = 5;
  const bool copy = !c.swap_red_blue && c.scale == 1.0f && plan.conversion == Conversion::kCopy &&
                    plan.format == c.source_format;
  c.method = copy ? ResolveMethod::kCopy : ResolveMethod::kBlit;
  return c;
}

bool EmulateResolveTexel(const ResolveConversion& plan, const float s[4], uint8_t* out) {
  if (plan.method == ResolveMethod::kCopy) return EncodeHostTexel(plan.source_format, s, out);
  if (plan.method != ResolveMethod::kBlit) return false;
  float v[4];
  for (int i = 0; i < 4; ++i) {
    const uint8_t ch = plan.channels[i];
    v[i] = ch < 4 ? s[ch] * plan.scale : (ch == 5 ? 1.0f : 0.0f);
  }
  return EncodeHostTexel(plan.dest_format, v, out);
}

// ---------------------------------------------------------------- the CPU reference

void UnpackEdramPixel(const ResolveSource& s, const uint32_t raw[2], float c[4]) {
  c[0] = c[1] = c[2] = 0.0f;
  c[3] = 1.0f;
  const uint32_t v = raw[0];
  if (s.depth) {
    c[0] = s.format == uint32_t(EdramDepthFormat::kD24FS8) ? Float20e4ToFloat(v >> 8) : UNorm24ToFloat(v >> 8);
    return;
  }
  switch (EdramColorFormat(s.format)) {
    case EdramColorFormat::k_8_8_8_8:
    case EdramColorFormat::k_8_8_8_8_GAMMA:
      for (int i = 0; i < 4; ++i) c[i] = ((v >> (8 * i)) & 255) / 255.0f;
      break;
    case EdramColorFormat::k_2_10_10_10:
    case EdramColorFormat::k_2_10_10_10_AS_10_10_10_10:
      for (int i = 0; i < 3; ++i) c[i] = ((v >> (10 * i)) & 1023) / 1023.0f;
      c[3] = (v >> 30) / 3.0f;
      break;
    case EdramColorFormat::k_2_10_10_10_FLOAT:
    case EdramColorFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      for (int i = 0; i < 3; ++i) c[i] = Float7e3ToFloat(v >> (10 * i));
      c[3] = (v >> 30) / 3.0f;
      break;
    case EdramColorFormat::k_16_16:
    case EdramColorFormat::k_16_16_16_16:
      // Fixed point -32..32: the SDK's resolve divides by 1024.
      c[0] = int16_t(v & 0xFFFF) / 1024.0f;
      c[1] = int16_t(v >> 16) / 1024.0f;
      if (s.format == uint32_t(EdramColorFormat::k_16_16_16_16)) {
        c[2] = int16_t(raw[1] & 0xFFFF) / 1024.0f;
        c[3] = int16_t(raw[1] >> 16) / 1024.0f;
      }
      break;
    case EdramColorFormat::k_16_16_FLOAT:
    case EdramColorFormat::k_16_16_16_16_FLOAT:
      c[0] = HalfToFloat(uint16_t(v));
      c[1] = HalfToFloat(uint16_t(v >> 16));
      if (s.format == uint32_t(EdramColorFormat::k_16_16_16_16_FLOAT)) {
        c[2] = HalfToFloat(uint16_t(raw[1]));
        c[3] = HalfToFloat(uint16_t(raw[1] >> 16));
      }
      break;
    case EdramColorFormat::k_32_FLOAT:
      std::memcpy(&c[0], &raw[0], 4);
      break;
    case EdramColorFormat::k_32_32_FLOAT:
      std::memcpy(&c[0], &raw[0], 4);
      std::memcpy(&c[1], &raw[1], 4);
      break;
  }
}

void EdramPixelToHostTarget(const ResolveSource& s, const uint32_t raw[2], float c[4]) {
  UnpackEdramPixel(s, raw, c);
  if (s.depth) return;  // a float depth buffer holds the value
  uint8_t t[16];
  const HostFormat f = EdramColorHostFormat(s.format);
  if (EncodeHostTexel(f, c, t)) DecodeHostTexel(f, t, c);
}

bool PackResolveTexel(TextureFormat dest, bool swap, const float rgba[4], const ResolveSource& source,
                      const uint32_t raw[2], uint8_t* out) {
  if (source.depth) {
    if (dest != T::k_24_8 && dest != T::k_24_8_FLOAT) return false;
    Put32(out, raw[0]);
    return true;
  }
  float c[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
  if (swap) std::swap(c[0], c[2]);
  // The fast path: bitwise-equivalent formats are copied as bits (fixed point into 16-bit textures).
  if (FixedBitCopy(source, dest)) {
    uint16_t f[4] = {uint16_t(raw[0]), uint16_t(raw[0] >> 16), uint16_t(raw[1]), uint16_t(raw[1] >> 16)};
    if (swap) std::swap(f[0], f[2]);
    std::memcpy(out, f, dest == T::k_16_16 ? 4 : 8);
    return true;
  }
  switch (dest) {
    case T::k_8: case T::k_8_A: case T::k_8_B:
      out[0] = uint8_t(Unorm(c[0], 255));
      return true;
    case T::k_8_8:
      Put16(out, Unorm(c[0], 255) | Unorm(c[1], 255) << 8);
      return true;
    case T::k_8_8_8_8: case T::k_8_8_8_8_A: case T::k_8_8_8_8_AS_16_16_16_16:
      Put32(out, Unorm(c[0], 255) | Unorm(c[1], 255) << 8 | Unorm(c[2], 255) << 16 | Unorm(c[3], 255) << 24);
      return true;
    case T::k_2_10_10_10: case T::k_2_10_10_10_AS_16_16_16_16:
      Put32(out, Unorm(c[0], 1023) | Unorm(c[1], 1023) << 10 | Unorm(c[2], 1023) << 20 | Unorm(c[3], 3) << 30);
      return true;
    case T::k_10_11_11: case T::k_10_11_11_AS_16_16_16_16:  // X 11, Y 11, Z 10 bits
      Put32(out, Unorm(c[0], 2047) | Unorm(c[1], 2047) << 11 | Unorm(c[2], 1023) << 22);
      return true;
    case T::k_11_11_10: case T::k_11_11_10_AS_16_16_16_16:  // X 10, Y 11, Z 11 bits
      Put32(out, Unorm(c[0], 1023) | Unorm(c[1], 2047) << 10 | Unorm(c[2], 2047) << 21);
      return true;
    case T::k_16:
      Put16(out, Unorm(c[0], 65535));
      return true;
    case T::k_16_16:
      Put32(out, Unorm(c[0], 65535) | Unorm(c[1], 65535) << 16);
      return true;
    case T::k_16_16_16_16:
      for (int i = 0; i < 4; ++i) Put16(out + 2 * i, Unorm(c[i], 65535));
      return true;
    case T::k_16_FLOAT: case T::k_16_16_FLOAT: case T::k_16_16_16_16_FLOAT: {
      const int n = dest == T::k_16_FLOAT ? 1 : dest == T::k_16_16_FLOAT ? 2 : 4;
      for (int i = 0; i < n; ++i) Put16(out + 2 * i, FloatToHalf(c[i]));
      return true;
    }
    case T::k_32_FLOAT: case T::k_32_32_FLOAT: case T::k_32_32_32_32_FLOAT: {
      const int n = dest == T::k_32_FLOAT ? 1 : dest == T::k_32_32_FLOAT ? 2 : 4;
      for (int i = 0; i < n; ++i) PutF(out + 4 * i, c[i]);
      return true;
    }
    default:
      return false;
  }
}

bool ResolveToGuest(const ResolveSourceImage& src, const ResolveRect& r, const TextureFetch& dest,
                    GuestTextureImage& image, const TextureOptions& options) {
  const GuestLayout layout = ComputeGuestLayout(dest, options);
  const TextureRanges ranges = GetTextureRanges(dest, options);
  const FormatInfo& info = GetFormatInfo(dest.Format());
  if (info.block_width != 1 || info.block_height != 1 || layout.dimension == Dimension::k3D) return false;
  LevelSource where;
  if (!GetLevelSource(layout, r.level, r.slice, where)) return false;
  std::vector<uint8_t>& region = where.from_mips ? image.mips : image.base;
  const size_t needed = ((where.from_mips ? ranges.mip_bytes : ranges.base_bytes) + 3) & ~size_t(3);
  if (region.size() < needed) region.resize(needed, 0);
  const Endian endian = dest.EndianMode();
  CopySwap(endian, region.data(), region.data(), region.size() & ~size_t(3));  // to host order

  const int32_t level_w = int32_t(std::max(layout.width >> r.level, 1u));
  const int32_t level_h = int32_t(std::max(layout.height >> r.level, 1u));
  int32_t w = std::min({r.width, int32_t(src.width) - r.src_x, level_w - r.dst_x});
  int32_t h = std::min({r.height, int32_t(src.height) - r.src_y, level_h - r.dst_y});
  bool ok = r.src_x >= 0 && r.src_y >= 0 && r.dst_x >= 0 && r.dst_y >= 0;
  const bool swap = ResolveDestSwap(dest);
  const uint32_t bpb = info.BytesPerBlock();
  for (int32_t y = 0; ok && y < h; ++y)
    for (int32_t x = 0; ok && x < w; ++x) {
      const uint32_t* raw = &src.raw[(size_t(r.src_y + y) * src.width + size_t(r.src_x + x)) * 2];
      float c[4];
      UnpackEdramPixel(src.source, raw, c);
      uint8_t texel[16];
      if (!PackResolveTexel(dest.Format(), swap, c, src.source, raw, texel)) {
        ok = false;
        break;
      }
      const size_t o = size_t(where.layer_offset_bytes) +
                       BlockOffset(layout, *where.storage, uint32_t(r.dst_x + x) + where.x_blocks,
                                   uint32_t(r.dst_y + y) + where.y_blocks, where.z);
      if (o + bpb > region.size()) {
        ok = false;
        break;
      }
      std::memcpy(region.data() + o, texel, bpb);
    }
  CopySwap(endian, region.data(), region.data(), region.size() & ~size_t(3));  // back to guest order
  return ok;
}

}  // namespace kknr
