// kknr_formats: prints the guest -> host format table of docs/formats.md from the library's own rules, so the
// document is regenerated rather than kept in step by hand: kknr_formats > table.md
#include <cstdio>
#include <string>

#include "kknr/texture_convert.h"

using namespace kknr;

namespace {

std::string Replicate(uint16_t r) {
  static const char kC[] = "XYZW01??";
  std::string t;
  for (int i = 0; i < 4; ++i) t += kC[SwizzleComponent(r, i)];
  return t;
}

// Why the choice is what it is, per guest format (empty: nothing beyond the columns).
const char* Note(TextureFormat f) {
  switch (f) {
    case TextureFormat::k_1_REVERSE:
    case TextureFormat::k_1: return "1 bpp treated as 8x1-texel byte blocks; bit order assumed (k_1 bit 0 first, REVERSE bit 7 first); no reference, SDK has no support; packed tails not supported";
    case TextureFormat::k_8_B: return "the SDK has no host format; same bits as k_8";
    case TextureFormat::k_1_5_5_5: case TextureFormat::k_5_6_5: case TextureFormat::k_4_4_4_4: return "X..W from bit 0 up; widened to RGBA8 (the 16-bit packed host formats differ in layout between D3D12 and NVRHI's Vulkan mapping)";
    case TextureFormat::k_6_5_5: return "X 5 bits, Y 5, Z 6; widened to 8 bits (the SDK packs it into B5G6R5 with an RBGA view instead)";
    case TextureFormat::k_8_8_8_8_A: return "SDK: unknown; read as k_8_8_8_8";
    case TextureFormat::k_2_10_10_10: case TextureFormat::k_2_10_10_10_AS_16_16_16_16: return "signed: no host SNORM 10:10:10:2, widened to RGBA16_SNORM";
    case TextureFormat::k_Cr_Y1_Cb_Y0_REP: case TextureFormat::k_Y1_Cr_Y0_Cb_REP: return "4:2:2 pairs expanded per texel (R = Cr, G = Y, B = Cb); no colour conversion, the shader does it as on the 360";
    case TextureFormat::k_16_16_EDRAM: case TextureFormat::k_16_16_16_16_EDRAM: return "EDRAM fixed point -32..32 (snorm16 x 32, the SDK's convention) as half floats; the SDK does not sample these";
    case TextureFormat::k_10_11_11: case TextureFormat::k_11_11_10: case TextureFormat::k_10_11_11_AS_16_16_16_16: case TextureFormat::k_11_11_10_AS_16_16_16_16: return "no host unorm / snorm 11:11:10; widened to 16 bits by bit replication";
    case TextureFormat::k_DXT1: case TextureFormat::k_DXT2_3: case TextureFormat::k_DXT4_5: case TextureFormat::k_DXT1_AS_16_16_16_16: case TextureFormat::k_DXT2_3_AS_16_16_16_16: case TextureFormat::k_DXT4_5_AS_16_16_16_16: return "BC blocks after the 8in16 swap are byte-identical to the PC's; decoded to RGBA8 when level 0 is not a multiple of 4";
    case TextureFormat::k_DXN: return "signed: BC5_SNORM (the SDK does not support signed DXN; unverified)";
    case TextureFormat::k_DXT5A: return "signed: BC4_SNORM (unverified, as DXN)";
    case TextureFormat::k_DXT3A: return "4-bit values x 17 (R8 has no 4x4 alignment rule; as the SDK)";
    case TextureFormat::k_DXT3A_AS_1_1_1_1: return "bit k of each 4-bit value -> component k (the SDK uses B4G4R4A4)";
    case TextureFormat::k_CTX1: return "two 8:8 end points (G in the low byte), thirds truncated as the SDK's CPU decoder";
    case TextureFormat::k_24_8: case TextureFormat::k_24_8_FLOAT: return "depth as a texture: top 24 bits to float (stencil dropped; the SDK does the same)";
    case TextureFormat::k_16_MPEG: case TextureFormat::k_16_16_MPEG: case TextureFormat::k_16_INTERLACED: case TextureFormat::k_16_MPEG_INTERLACED: case TextureFormat::k_16_16_MPEG_INTERLACED: case TextureFormat::k_8_INTERLACED: return "video formats: read as the plain format (no reference; the SDK has no support)";
    case TextureFormat::k_32: case TextureFormat::k_32_32: case TextureFormat::k_32_32_32_32: return "32-bit fixed: integer data; with the fraction number format the shader normalises (kShaderNorm32)";
    case TextureFormat::k_32_32_32_FLOAT: return "no RGB32 texture format on every host; alpha 1 added";
    case TextureFormat::k_32_AS_8: case TextureFormat::k_32_AS_8_INTERLACED: case TextureFormat::k_32_AS_8_8: return "a 32-bit block of 4 (2) texels, X in the low byte after the swap (unverified)";
    case TextureFormat::k_32_AS_8_8_INTERLACED: return "the SDK's table gives it a 1x1 16-bit block; followed";
    case TextureFormat::k_8_8_8_8_GAMMA_EDRAM: return "EDRAM gamma format: data as is, the shader degammas (kShaderGamma)";
    case TextureFormat::k_2_10_10_10_FLOAT_EDRAM: return "7e3 RGB (0..31.875) and 2-bit alpha to half floats";
    default: return "";
  }
}

}  // namespace

int main() {
  std::printf("| # | Guest format | Block | Bytes | Host format | Conversion | Signed host | Signed conversion | "
              "Unaligned BC fallback | Channels | Notes |\n");
  std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
  for (uint32_t i = 0; i < 64; ++i) {
    const TextureFormat f = TextureFormat(i);
    const FormatInfo& info = GetFormatInfo(f);
    const FormatChoice c = GetFormatChoice(f);
    std::string fallback = "-";
    if (c.decompressed_format != HostFormat::UNKNOWN) {
      fallback = std::string(GetHostFormatInfo(c.decompressed_format).name) + " (" + ConversionName(c.decompress) + ")";
      if (c.decompressed_signed_format != HostFormat::UNKNOWN)
        fallback += ", signed " + std::string(GetHostFormatInfo(c.decompressed_signed_format).name);
    }
    const bool has_signed = c.signed_format != HostFormat::UNKNOWN;
    std::printf("| %u | `%s` | %ux%u | %u | %s | %s | %s | %s | %s | %s | %s |\n", i, info.name, info.block_width,
                info.block_height, info.BytesPerBlock(), GetHostFormatInfo(c.format).name, ConversionName(c.conversion),
                has_signed ? GetHostFormatInfo(c.signed_format).name : "shader fix-up",
                has_signed ? ConversionName(c.signed_conversion) : "-", fallback.c_str(), Replicate(c.replicate).c_str(),
                Note(f));
  }
  return 0;
}
