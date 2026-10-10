// Resolve conversions: an EDRAM render target (colour or depth) copied into a texture.
//
// Two halves:
// - PlanResolveConversion: what the renderer does on the GPU. The render target lives in a host texture
//   (EdramColorHostFormat), the destination texture in the host format PlanHostTexture picks for its fetch
//   constant; the plan says whether a raw copy is enough or a blit with a channel mapping is needed, or that
//   the pair is not handled.
// - ResolveToGuest: the CPU reference, what the 360 writes into guest memory (the SDK's resolve shaders:
//   unpack the EDRAM pixel to floats, exchange red and blue when the copy's swap is set, saturate and round
//   to the destination format, pack with the first component in the low bits, swap to the destination's
//   endian, store tiled). Reading that memory back through ConvertTexture must give what the GPU path puts in
//   the host texture; tests/test_resolve.cpp checks it for every pair the game uses.
//
// The D3D-level Resolve call gives the destination texture, not the GPU's copy registers; the copy's format
// is the texture's, and its red / blue swap is derived from the texture's swizzle (ResolveDestSwap), as the
// XDK does to make an A8R8G8B8 texture read back the colours that were drawn.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kknr/guest_texture.h"
#include "kknr/texture_convert.h"
#include "kknr/xenos.h"

namespace kknr {

// RB_COLOR_INFO's colour format (xenos::ColorRenderTargetFormat).
enum class EdramColorFormat : uint8_t {
  k_8_8_8_8 = 0,
  k_8_8_8_8_GAMMA = 1,
  k_2_10_10_10 = 2,
  k_2_10_10_10_FLOAT = 3,  // 7e3 RGB, 2-bit alpha
  k_16_16 = 4,             // fixed point -32..32
  k_16_16_16_16 = 5,       // fixed point -32..32
  k_16_16_FLOAT = 6,
  k_16_16_16_16_FLOAT = 7,
  k_2_10_10_10_AS_10_10_10_10 = 10,
  k_2_10_10_10_FLOAT_AS_16_16_16_16 = 12,
  k_32_FLOAT = 14,
  k_32_32_FLOAT = 15,
};
// RB_DEPTH_INFO's depth format.
enum class EdramDepthFormat : uint8_t { kD24S8 = 0, kD24FS8 = 1 };

bool IsEdramColorFormat64bpp(uint32_t color_format);
// The host render target format for an EDRAM colour format (the game renderer's targets: 8:8:8:8 as RGBA8,
// 2:10:10:10 as R10G10B10A2, the 7e3 and 16-bit fixed formats as half floats, the float formats as such).
HostFormat EdramColorHostFormat(uint32_t color_format);

struct ResolveSource {
  bool depth = false;
  uint32_t format = 0;  // EdramColorFormat, or EdramDepthFormat when depth
};

// The copy's red / blue swap for a destination texture: set when its swizzle reads X as the blue of a
// four-component colour (A8R8G8B8 ZYXW, X8R8G8B8 ZYX1), so sampling returns what was drawn.
bool ResolveDestSwap(const TextureFetch& dest);

enum class ResolveMethod : uint8_t {
  kCopy,         // texel for texel, same host format and channel order
  kBlit,         // a draw reading the source and writing the destination through `channels`
  kUnsupported,  // the destination format cannot be written this way (see why)
};

struct ResolveConversion {
  ResolveMethod method = ResolveMethod::kUnsupported;
  bool swap_red_blue = false;
  bool depth = false;
  // Destination host channel i takes source host channel channels[i] (0-3 R G B A, 4 zero, 5 one); the
  // same encoding as the backend's blit constants.
  uint8_t channels[4] = {0, 1, 2, 3};
  // Multiplies the value before the destination's own rounding (1/32 when a 16-bit fixed -32..32 target goes
  // into the 16-bit texture of the same layout: the 360 copies the bits and a signed texture reads them as
  // -1..1). The backend's blit has no scale yet, so it treats scale != 1 as unsupported.
  float scale = 1.0f;
  HostFormat source_format = HostFormat::UNKNOWN;  // the host render target's (R32_FLOAT stands for depth)
  HostFormat dest_format = HostFormat::UNKNOWN;    // the destination texture's host format
  const char* why = "";
};
ResolveConversion PlanResolveConversion(const ResolveSource& source, const TextureFetch& dest,
                                        const TextureOptions& options = TextureOptions());

// ---- CPU reference ----

// An EDRAM pixel as the resolve reads it, to floats in the shader's r, g, b, a order. raw[0] holds 32-bit
// pixels (depth: depth << 8 | stencil); 64-bit formats use raw[0] (r, g) and raw[1] (b, a).
void UnpackEdramPixel(const ResolveSource& source, const uint32_t raw[2], float rgba[4]);
// What the host render target holds for that pixel (the value rounded to its host format).
void EdramPixelToHostTarget(const ResolveSource& source, const uint32_t raw[2], float rgba[4]);
// Packs a colour into one block of a resolvable texture format (host byte order, before the endian swap):
// components in X, Y, Z, W order from the low bits, swap exchanging red and blue first. Depth sources go
// into k_24_8 / k_24_8_FLOAT as the raw EDRAM word. False for formats a resolve cannot write.
bool PackResolveTexel(TextureFormat dest, bool swap, const float rgba[4], const ResolveSource& source,
                      const uint32_t raw[2], uint8_t* out);

// Source pixels: raw EDRAM words, width x height, two words per pixel (the second unused for 32 bpp).
struct ResolveSourceImage {
  ResolveSource source;
  uint32_t width = 0, height = 0;
  std::vector<uint32_t> raw;  // (y * width + x) * 2
};
struct ResolveRect {
  int32_t src_x = 0, src_y = 0, width = 0, height = 0;
  int32_t dst_x = 0, dst_y = 0;
  uint32_t level = 0, slice = 0;
};
// Writes the rectangle into `image` (the destination texture's base and mip regions in guest byte order,
// sized by EncodeGuestTexture or by the caller from GetTextureRanges); texels outside the rectangle keep
// their bytes. The rectangle is clipped to the source and the destination level.
bool ResolveToGuest(const ResolveSourceImage& src, const ResolveRect& rect, const TextureFetch& dest,
                    GuestTextureImage& image, const TextureOptions& options = TextureOptions());

// Emulates the GPU path for one texel: a source host texel (EdramPixelToHostTarget) through the plan's
// channels and scale, encoded in the destination host format (round to nearest for normalized formats, as
// a render target write does). For kCopy the source texel's own encoding.
bool EmulateResolveTexel(const ResolveConversion& plan, const float source_texel[4], uint8_t* out);

// Host texel encoding / decoding for the uncompressed host formats (floats in, as a shader would write them).
bool EncodeHostTexel(HostFormat format, const float rgba[4], uint8_t* out);
bool DecodeHostTexel(HostFormat format, const uint8_t* in, float rgba[4]);

}  // namespace kknr
