// kknr_sdkref: checks the library's texture conversion against today's renderer, texture by texture.
//
//   kknr_sdkref [--out DIR] [--csv FILE] [--png] [--quiet] [--gpu] FILE_OR_DIR...
//
// The reference side is the SDK's own texture code (rexglue-sdk, the D3D12 texture cache the game renders
// with today), compiled from its sources: texture_util::GetSubresourcesFromFetchConstant decides which levels
// exist, texture_util::GetGuestTextureLayout gives every level's storage, texture_util::GetTiledOffset2D / 3D
// and GetPackedMipOffset place the blocks. The rest of D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl
// and its load shaders is emulated here on the CPU, step for step: per stored level (or packed tail) and array
// slice, every guest block is read at the address the shader computes, endian-swapped per 16- or 32-bit memory
// unit, and written as the host block (a copy for the formats whose host format keeps the guest bits, the
// depth_unorm shader's arithmetic for k_24_8); then the host level is cut out of a packed tail with the box
// CopyTextureRegion uses. The host formats are those of the SDK's D3D12 host_formats_ table.
// With --gpu (Windows), the SDK's own compiled load shaders (the bytecode in its source tree, the same the game's
// renderer dispatches) also run on the GPU through D3D12 with the cache's constants, dispatch sizes and copy
// buffer layout (sdkref_gpu.cpp); a texture then passes only when ours matches both.
//
// Our side is kknr::ConvertTexture on the same bytes. Every subresource is compared block for block; a texture
// passes when every byte matches. Formats whose SDK load is not emulated here are reported as "skipped".
// Input: KKTX files (see kknr_texdump). With --png, <name>.ref.png is the reference's level 0 (layers stacked,
// view swizzle applied, decoded with the library's own decoders) next to kknr_texdump's <name>.png.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "image_io.h"
#include "kknr/texture_convert.h"
#include "kktx.h"
#include "sdkref.h"

// The one SDK runtime function texture_util needs (src/core/math_*.cpp; not built here).
namespace rex {
uint8_t lzcnt(uint32_t v) { return uint8_t(std::countl_zero(v)); }
}  // namespace rex

namespace fs = std::filesystem;
namespace sdk = rex::graphics;
namespace tu = rex::graphics::texture_util;

namespace {

// How the SDK's D3D12 texture cache turns a guest block into a host block for the formats it loads with a
// plain copy shader (texture_load_8bpb / 16bpb / 32bpb / 64bpb / 128bpb) or the depth_unorm shader.
enum class RefLoad { kUnsupported, kCopy, kDepthUnorm };

RefLoad GetRefLoad(sdk::xenos::TextureFormat f) {
  using F = sdk::xenos::TextureFormat;
  switch (f) {
    case F::k_8:
    case F::k_8_A:
    case F::k_8_8:
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_AS_16_16_16_16:
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
    case F::k_DXT1:
    case F::k_DXT1_AS_16_16_16_16:
    case F::k_DXT2_3:
    case F::k_DXT2_3_AS_16_16_16_16:
    case F::k_DXT4_5:
    case F::k_DXT4_5_AS_16_16_16_16:
    case F::k_DXN:
    case F::k_DXT5A:
    case F::k_16:
    case F::k_16_16:
    case F::k_16_16_16_16:
    case F::k_16_FLOAT:
    case F::k_16_16_FLOAT:
    case F::k_16_16_16_16_FLOAT:
    case F::k_32_FLOAT:
    case F::k_32_32_FLOAT:
    case F::k_32_32_32_32_FLOAT:
      return RefLoad::kCopy;
    case F::k_24_8:
      return RefLoad::kDepthUnorm;
    default:
      return RefLoad::kUnsupported;
  }
}

// The swap the load shaders apply to each loaded 32-bit word: byte a of the swapped data is byte a ^ mask of
// memory (8in16: bytes in 16-bit units, 8in32: bytes in 32-bit units, 16in32: halves of 32-bit units).
uint32_t SwapMask(sdk::xenos::Endian e) {
  switch (e) {
    case sdk::xenos::Endian::k8in16:
      return 1;
    case sdk::xenos::Endian::k8in32:
      return 3;
    case sdk::xenos::Endian::k16in32:
      return 2;
    default:
      return 0;
  }
}

using kknr_sdkref::RefSubresource;

// The host format swizzle of the SDK's D3D12 host_formats_ table (the formats the census needs), and the SDK's
// TextureCache::GuestToHostSwizzle: the SRV swizzle today's renderer binds. 3 bits per component, 0-3 = R-A,
// 4 = 0, 5 = 1 (the same encoding as HostTexturePlan::view_swizzle). UINT32_MAX: not in the table here.
uint32_t SdkFormatSwizzle(sdk::xenos::TextureFormat f) {
  using F = sdk::xenos::TextureFormat;
  constexpr uint32_t kRRRR = 0, kRGGG = 0 | 1 << 3 | 1 << 6 | 1 << 9, kRGBA = 0 | 1 << 3 | 2 << 6 | 3 << 9;
  switch (f) {
    case F::k_8: case F::k_8_A: case F::k_24_8: case F::k_32_FLOAT: case F::k_DXT5A:
      return kRRRR;
    case F::k_8_8: case F::k_DXN:
      return kRGGG;
    case F::k_8_8_8_8: case F::k_DXT1: case F::k_DXT2_3: case F::k_DXT4_5:
      return kRGBA;
    default:
      return UINT32_MAX;
  }
}

uint32_t SdkHostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle) {
  uint32_t host = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t g = (guest_swizzle >> (3 * i)) & 7;
    const uint32_t h = g >= 4 ? (g & 5) : (host_format_swizzle >> (3 * g)) & 7;
    host |= h << (3 * i);
  }
  return host;
}

// One stored level (or packed tail) of one array slice, as the load shader writes it into the copy buffer:
// size_blocks x/y/z host blocks, tightly packed here.
struct RefLoaded {
  uint32_t width_blocks = 0, height_blocks = 0, depth = 0;
  std::vector<uint8_t> blocks;
};

bool LoadLevel(const std::vector<uint8_t>& memory, uint32_t guest_offset, const tu::TextureGuestLayout::Level& lg,
               bool tiled, bool is_3d_tiling, uint32_t bytes_per_block, uint32_t bpb_log2, uint32_t swap,
               RefLoad load, uint32_t size_x, uint32_t size_y, uint32_t size_z, RefLoaded& out) {
  out.width_blocks = size_x;
  out.height_blocks = size_y;
  out.depth = size_z;
  out.blocks.assign(size_t(size_x) * size_y * size_z * bytes_per_block, 0);
  // The shader's guest_pitch_aligned: blocks for tiled textures, bytes for linear ones.
  const uint32_t pitch = tiled ? lg.row_pitch_bytes / bytes_per_block : lg.row_pitch_bytes;
  for (uint32_t z = 0; z < size_z; ++z)
    for (uint32_t y = 0; y < size_y; ++y)
      for (uint32_t x = 0; x < size_x; ++x) {
        int64_t offset;
        if (tiled) {
          offset = is_3d_tiling ? tu::GetTiledOffset3D(int32_t(x), int32_t(y), int32_t(z), pitch,
                                                       lg.z_slice_stride_block_rows, bpb_log2)
                                : tu::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, bpb_log2) +
                                      int64_t(z) * lg.z_slice_stride_block_rows * lg.row_pitch_bytes;
        } else {
          offset = (int64_t(z) * lg.z_slice_stride_block_rows + y) * pitch + int64_t(x) * bytes_per_block;
        }
        uint8_t* dst = &out.blocks[((size_t(z) * size_y + y) * size_x + x) * bytes_per_block];
        const uint64_t at = uint64_t(guest_offset) + uint64_t(offset);
        for (uint32_t b = 0; b < bytes_per_block; ++b) {
          const uint64_t a = (at + b) ^ swap;
          dst[b] = a < memory.size() ? memory[size_t(a)] : 0;
        }
        if (load == RefLoad::kDepthUnorm) {
          // texture_load_depth_unorm: d = v >> 8; float(d + (d >> 23)) * 2^-24.
          uint32_t v;
          std::memcpy(&v, dst, 4);
          const uint32_t d = v >> 8;
          const float f = float(d + (d >> 23)) * 0x1p-24f;
          std::memcpy(dst, &f, 4);
        }
      }
  return true;
}

// The SDK's whole load, for every level and slice the texture cache would create.
bool SdkReference(const uint32_t words[6], const std::vector<uint8_t>& memory, std::vector<RefSubresource>& out,
                  std::string& why) {
  sdk::xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, words, sizeof(fetch));
  const RefLoad load = GetRefLoad(fetch.format);
  if (load == RefLoad::kUnsupported) return why = "format not emulated", false;
  uint32_t width_m1, height_m1, depth_m1, base_page, mip_page, mip_min, mip_max;
  tu::GetSubresourcesFromFetchConstant(fetch, &width_m1, &height_m1, &depth_m1, &base_page, &mip_page, &mip_min,
                                       &mip_max);
  if (!base_page && !mip_page) return why = "no data", false;
  const sdk::xenos::DataDimension dimension = fetch.dimension;
  const uint32_t width = width_m1 + 1, height = height_m1 + 1, depth_or_array = depth_m1 + 1;
  const bool is_3d = dimension == sdk::xenos::DataDimension::k3D;
  const uint32_t depth = is_3d ? depth_or_array : 1, array_size = is_3d ? 1 : depth_or_array;
  const tu::TextureGuestLayout layout =
      tu::GetGuestTextureLayout(dimension, fetch.pitch, width, height, depth_or_array, fetch.tiled, fetch.format,
                                fetch.packed_mips, base_page != 0, mip_max);
  const sdk::FormatInfo* info = sdk::FormatInfo::Get(fetch.format);
  const uint32_t bw = info->block_width, bh = info->block_height, bpb = info->bytes_per_block();
  uint32_t bpb_log2 = 0;
  while ((2u << bpb_log2) <= bpb) ++bpb_log2;
  const uint32_t swap = SwapMask(fetch.endianness);
  const bool host_bc = bw > 1 || bh > 1;  // the copy formats with blocks are BC on the host
  const uint32_t host_bw = host_bc ? bw : 1, host_bh = host_bc ? bh : 1;

  const uint32_t level_first = base_page ? 0 : 1, level_last = mip_page ? mip_max : 0;
  const uint32_t level_packed = layout.packed_level;
  // Loaded copy-buffer contents: [0] base, [1 + level] mips (level = 0 for a level-0 mip tail), per slice.
  std::vector<std::vector<RefLoaded>> loaded(2 + sdk::xenos::kTextureMaxMips, std::vector<RefLoaded>(array_size));
  uint32_t loop_first, loop_last;
  if (level_packed == 0) {
    loop_first = uint32_t(level_first != 0);
    loop_last = uint32_t(level_last != 0);
  } else {
    loop_first = std::min(level_first, level_packed);
    loop_last = std::min(level_last, level_packed);
  }
  for (uint32_t loop = loop_first; loop <= loop_last; ++loop) {
    const bool is_base = loop == 0;
    const uint32_t level = level_packed == 0 ? 0 : loop;
    uint32_t guest_offset = (is_base ? base_page : mip_page) << 12;
    if (!is_base) guest_offset += layout.mip_offsets_bytes[level];
    const tu::TextureGuestLayout::Level& lg = is_base ? layout.base : layout.mips[level];
    uint32_t lw, lh, ld;
    if (level == level_packed) {
      lw = lg.x_extent_blocks * bw;
      lh = lg.y_extent_blocks * bh;
      ld = lg.z_extent;
    } else {
      lw = std::max(width >> level, 1u);
      lh = std::max(height >> level, 1u);
      ld = std::max(depth >> level, 1u);
    }
    const uint32_t sx = (lw + bw - 1) / bw, sy = (lh + bh - 1) / bh;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      LoadLevel(memory, guest_offset + slice * lg.array_slice_stride_bytes, lg, fetch.tiled, is_3d, bpb,
                bpb_log2, swap, load, sx, sy, ld, loaded[is_base ? 0 : 1 + level][slice]);
    }
  }
  // CopyTextureRegion into each host subresource.
  for (uint32_t level = level_first; level <= level_last; ++level) {
    const uint32_t guest_level = std::min(level, level_packed);
    const uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u),
                   ld = std::max(depth >> level, 1u);
    uint32_t ox = 0, oy = 0, oz = 0;
    if (level >= level_packed) tu::GetPackedMipOffset(width, height, depth, fetch.format, level, ox, oy, oz);
    // The box is in texels, aligned to the host block; blocks here.
    const uint32_t wb = (lw + host_bw - 1) / host_bw, hb = (lh + host_bh - 1) / host_bh;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      const RefLoaded& src = loaded[level ? 1 + guest_level : 0][slice];
      RefSubresource r;
      r.level = level;
      r.layer = slice;
      r.width_blocks = wb;
      r.height_blocks = hb;
      r.depth = ld;
      r.bytes_per_block = bpb;
      r.blocks.assign(size_t(wb) * hb * ld * bpb, 0);
      for (uint32_t z = 0; z < ld; ++z)
        for (uint32_t y = 0; y < hb; ++y)
          for (uint32_t x = 0; x < wb; ++x) {
            const uint32_t gx = ox + x, gy = oy + y, gz = oz + z;
            if (gx >= src.width_blocks || gy >= src.height_blocks || gz >= src.depth) continue;
            std::memcpy(&r.blocks[((size_t(z) * hb + y) * wb + x) * bpb],
                        &src.blocks[((size_t(gz) * src.height_blocks + gy) * src.width_blocks + gx) * bpb], bpb);
          }
      out.push_back(std::move(r));
    }
  }
  return true;
}

struct Options {
  fs::path out = ".";
  std::string csv;
  bool png = false;
  bool quiet = false;
  bool gpu = false;
};

struct Totals {
  int files = 0, pass = 0, fail = 0, skipped = 0;
};

// Ours against one reference, block for block.
struct Comparison {
  size_t blocks = 0, bad_blocks = 0, bad_subs = 0, missing = 0, extra = 0;
  uint32_t first_bad_level = UINT32_MAX;
  bool Match() const { return bad_subs == 0 && extra == 0; }
  std::string Text() const {
    char b[160];
    std::snprintf(b, sizeof(b), "%zu of %zu blocks differ in %zu subresources (first level %u), %zu missing, %zu extra",
                  bad_blocks, blocks, bad_subs, first_bad_level, missing, extra);
    return b;
  }
};

Comparison Compare(const kknr::HostTextureData& ours, const std::vector<RefSubresource>& ref) {
  Comparison c;
  const kknr::HostFormatInfo& hi = kknr::GetHostFormatInfo(ours.plan.format);
  for (const RefSubresource& r : ref) {
    const kknr::HostSubresource* s = ours.Find(r.level, r.layer);
    if (!s) {
      ++c.missing;
      ++c.bad_subs;
      c.first_bad_level = std::min(c.first_bad_level, r.level);
      continue;
    }
    const uint32_t hb_w = (s->width + hi.block_size - 1) / hi.block_size,
                   hb_h = (s->height + hi.block_size - 1) / hi.block_size;
    size_t bad = 0;
    if (hb_w != r.width_blocks || hb_h != r.height_blocks || s->depth != r.depth ||
        hi.bytes_per_block != r.bytes_per_block) {
      bad = r.blocks.size() / std::max(r.bytes_per_block, 1u);
    } else {
      for (uint32_t z = 0; z < r.depth; ++z)
        for (uint32_t y = 0; y < r.height_blocks; ++y)
          for (uint32_t x = 0; x < r.width_blocks; ++x) {
            const uint8_t* a = &r.blocks[((size_t(z) * r.height_blocks + y) * r.width_blocks + x) * r.bytes_per_block];
            const uint8_t* b = ours.bytes.data() + s->offset + size_t(z) * s->depth_pitch + size_t(y) * s->row_pitch +
                               size_t(x) * r.bytes_per_block;
            bad += std::memcmp(a, b, r.bytes_per_block) != 0;
          }
    }
    c.blocks += r.blocks.size() / std::max(r.bytes_per_block, 1u);
    c.bad_blocks += bad;
    if (bad) {
      ++c.bad_subs;
      c.first_bad_level = std::min(c.first_bad_level, r.level);
    }
  }
  for (const kknr::HostSubresource& s : ours.subresources) {
    bool found = false;
    for (const RefSubresource& r : ref) found |= r.level == s.level && r.layer == s.layer;
    c.extra += !found;
  }
  return c;
}

void Process(const fs::path& path, const Options& o, FILE* csv, Totals& t) {
  ++t.files;
  const std::string stem = path.stem().string();
  kknr_tools::Kktx dump;
  std::string load_why;
  if (!kknr_tools::LoadKktx(path, dump, load_why)) {
    std::printf("%s: %s\n", stem.c_str(), load_why.c_str());
    ++t.fail;
    return;
  }
  const uint32_t base_bytes = uint32_t(dump.base.size()), mip_bytes = uint32_t(dump.mips.size());
  // The same small physical memory as kknr_texdump: base at 4 KB, mips on the next 4 KB boundary after it.
  kknr::TextureFetch fetch = kknr::TextureFetch::FromWords(dump.words);
  const uint32_t base_at = 0x1000, mip_at = (base_at + base_bytes + 0x1FFF) & ~0xFFFu;
  std::vector<uint8_t> memory(size_t(mip_at) + mip_bytes + 0x1000, 0);
  if (base_bytes) std::memcpy(memory.data() + base_at, dump.base.data(), base_bytes);
  if (mip_bytes) std::memcpy(memory.data() + mip_at, dump.mips.data(), mip_bytes);
  if (fetch.BaseAddress()) fetch.words[1] = (fetch.words[1] & 0xFFFu) | base_at;
  if (fetch.MipAddress()) fetch.words[5] = (fetch.words[5] & 0xFFFu) | mip_at;

  kknr::HostTextureData ours;
  std::string why;
  const bool ours_ok = kknr::ConvertTexture(fetch, kknr::GuestMemory{memory.data(), memory.size()}, ours, &why);
  std::vector<RefSubresource> ref, gpu_ref;
  std::string ref_why, gpu_why;
  const bool ref_ok = SdkReference(fetch.words, memory, ref, ref_why);
  const bool gpu_ok = o.gpu && ref_ok && kknr_sdkref::SdkGpuReference(fetch.words, memory, gpu_ref, gpu_why);
  const kknr::FormatInfo& info = kknr::GetFormatInfo(fetch.Format());
  std::string verdict, cpu_verdict = "-", gpu_verdict = "-", detail;
  Comparison cpu, gpu;
  if (!ref_ok) {
    verdict = "skipped";
    detail = ref_why;
    ++t.skipped;
  } else if (!ours_ok) {
    verdict = "FAIL";
    detail = "ConvertTexture: " + why;
    ++t.fail;
  } else if (ours.plan.decompressed) {
    verdict = "skipped";
    detail = "decompressed on our side";
    ++t.skipped;
  } else {
    cpu = Compare(ours, ref);
    cpu_verdict = cpu.Match() ? "match" : "MISMATCH";
    bool pass = cpu.Match();
    if (o.gpu) {
      if (gpu_ok) {
        gpu = Compare(ours, gpu_ref);
        gpu_verdict = gpu.Match() ? "match" : "MISMATCH";
        pass = pass && gpu.Match();
      } else {
        gpu_verdict = "FAIL";
        pass = false;
      }
    }
    // The view: the SRV swizzle today's renderer uses for this fetch constant against ours.
    const uint32_t format_swizzle = SdkFormatSwizzle(sdk::xenos::TextureFormat(fetch.Format()));
    if (format_swizzle != UINT32_MAX &&
        SdkHostSwizzle(fetch.Swizzle(), format_swizzle) != uint32_t(ours.plan.view_swizzle)) {
      pass = false;
      char b[96];
      std::snprintf(b, sizeof(b), "view swizzle %03o, the SDK binds %03o; ", unsigned(ours.plan.view_swizzle),
                    unsigned(SdkHostSwizzle(fetch.Swizzle(), format_swizzle)));
      detail += b;
    }
    verdict = pass ? "match" : "MISMATCH";
    pass ? ++t.pass : ++t.fail;
    if (!cpu.Match()) detail += "CPU emulation: " + cpu.Text();
    if (o.gpu && gpu_ok && !gpu.Match()) detail += (detail.empty() ? "" : "; ") + std::string("GPU: ") + gpu.Text();
    if (o.gpu && !gpu_ok) detail += (detail.empty() ? "" : "; ") + std::string("GPU run failed: ") + gpu_why;
    if (detail.empty()) {
      char b[128];
      std::snprintf(b, sizeof(b), "%zu subresources, %zu blocks%s", ref.size(), cpu.blocks,
                    o.gpu ? ", CPU emulation and GPU shaders" : "");
      detail = b;
    }
  }
  if (!o.quiet || verdict != "match")
    std::printf("%s: %s %s %s %s %ux%u levels %u-%u%s -> %s: %s (%s)\n", stem.c_str(), info.name,
                kknr::EndianName(fetch.EndianMode()), fetch.Tiled() ? "tiled" : "linear",
                kknr::DimensionName(fetch.Dim()), fetch.Width(), fetch.Height(), fetch.MipMinLevel(),
                fetch.MipMaxLevel(), fetch.PackedMips() ? " packed" : "",
                kknr::GetHostFormatInfo(ours.plan.format).name, verdict.c_str(), detail.c_str());
  if (csv)
    std::fprintf(csv, "%s,%s,%s,%s,%s,%u,%u,%u,%u,%d,%s,%zu,%zu,%zu,%s,%zu,%s,%s\n", stem.c_str(), info.name,
                 kknr::EndianName(fetch.EndianMode()), fetch.Tiled() ? "tiled" : "linear",
                 kknr::DimensionName(fetch.Dim()), fetch.Width(), fetch.Height(), fetch.MipMinLevel(),
                 fetch.MipMaxLevel(), int(fetch.PackedMips()), kknr::GetHostFormatInfo(ours.plan.format).name,
                 ref.size(), cpu.blocks, cpu.bad_blocks, cpu_verdict.c_str(), gpu.bad_blocks, gpu_verdict.c_str(),
                 detail.c_str());

  const std::vector<RefSubresource>& shown = gpu_ok ? gpu_ref : ref;
  if (o.png && ref_ok && ours_ok && !ours.plan.decompressed) {
    // The reference written into a copy of our host texture (same plan and layout), then decoded the same way.
    kknr::HostTextureData r = ours;
    for (const RefSubresource& rs : shown) {
      const kknr::HostSubresource* s = r.Find(rs.level, rs.layer);
      if (!s || rs.level != r.plan.min_level) continue;
      for (uint32_t z = 0; z < std::min(rs.depth, s->depth); ++z)
        for (uint32_t y = 0; y < rs.height_blocks; ++y)
          std::memcpy(r.bytes.data() + s->offset + size_t(z) * s->depth_pitch + size_t(y) * s->row_pitch,
                      &rs.blocks[(size_t(z) * rs.height_blocks + y) * rs.width_blocks * rs.bytes_per_block],
                      std::min<size_t>(s->row_pitch, size_t(rs.width_blocks) * rs.bytes_per_block));
    }
    std::vector<uint8_t> png;
    uint32_t width = 0, height = 0;
    for (uint32_t layer = 0; layer < r.plan.layers; ++layer) {
      const kknr::HostSubresource* s = r.Find(r.plan.min_level, layer);
      if (!s) continue;
      for (uint32_t z = 0; z < s->depth; ++z) {
        const std::vector<uint8_t> rgba = kknr_tools::HostToRgba8(r, *s, z);
        png.insert(png.end(), rgba.begin(), rgba.end());
        width = s->width;
        height += s->height;
      }
    }
    if (width) kknr_tools::WritePng((o.out / (stem + ".ref.png")).string(), width, height, png);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  std::vector<fs::path> inputs;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--out" && i + 1 < argc)
      o.out = argv[++i];
    else if (a == "--csv" && i + 1 < argc)
      o.csv = argv[++i];
    else if (a == "--png")
      o.png = true;
    else if (a == "--quiet")
      o.quiet = true;
    else if (a == "--gpu")
      o.gpu = true;
    else if (a.rfind("--", 0) == 0) {
      std::printf("unknown option %s\n", a.c_str());
      return 2;
    } else
      inputs.push_back(a);
  }
  if (inputs.empty()) {
    std::printf("usage: kknr_sdkref [--out DIR] [--csv FILE] [--png] [--quiet] [--gpu] FILE_OR_DIR...\n");
    return 2;
  }
  std::error_code ec;
  fs::create_directories(o.out, ec);
  FILE* csv = o.csv.empty() ? nullptr : std::fopen(o.csv.c_str(), "w");
  if (csv)
    std::fprintf(csv, "name,format,endian,tiling,dim,width,height,min_level,max_level,packed,host_format,"
                      "subresources,blocks,cpu_bad_blocks,cpu_verdict,gpu_bad_blocks,gpu_verdict,detail\n");
  Totals t;
  for (const fs::path& in : inputs) {
    std::vector<fs::path> files;
    if (fs::is_directory(in)) {
      for (const auto& e : fs::directory_iterator(in))
        if (e.path().extension() == ".bin") files.push_back(e.path());
      std::sort(files.begin(), files.end());
    } else {
      files.push_back(in);
    }
    for (const fs::path& f : files) Process(f, o, csv, t);
  }
  if (csv) std::fclose(csv);
  std::printf("%d textures: %d match the SDK, %d differ or failed, %d skipped%s\n", t.files, t.pass, t.fail,
              t.skipped, o.gpu ? " (CPU emulation and the SDK's shaders on the GPU)" : " (CPU emulation)");
  return t.fail ? 1 : 0;
}
