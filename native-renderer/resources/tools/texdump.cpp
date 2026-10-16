// kknr_texdump: converts texture dumps to host data with the library, as the renderer will.
//
//   kknr_texdump [--out DIR] [--csv FILE] [--no-images] [--tail-check FILE [--tail-images DIR]] FILE_OR_DIR...
//
// --tail-check FILE writes only a CSV that checks the levels native_texture_tail_mips would read (see
// TailCheck below); nothing else is converted or written.
// Input: KKTX files (KK_DEV_TEX_DUMP in kk/src/dev_d3d_trace.cpp, or kknr_bfscan --dump; see kktx.h). For each one it writes <name>.dds (the host format, every level and layer) and
// <name>.png (level 0, layers or depth slices stacked vertically, with the view swizzle applied), prints one
// summary line and adds a CSV row (with an FNV-1a hash of the host data, to compare runs).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "image_io.h"
#include "kknr/texture_convert.h"
#include "kknr/texture_pack.h"
#include "kktx.h"

namespace fs = std::filesystem;
using namespace kknr;

namespace {

std::string Hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08X", v);
  return b;
}

std::string SwizzleText(uint16_t s) {
  static const char kC[] = "XYZW01??";
  std::string t;
  for (int i = 0; i < 4; ++i) t += kC[SwizzleComponent(s, i)];
  return t;
}

std::string HostSwizzleText(uint16_t s) {
  static const char kC[] = "RGBA01??";
  std::string t;
  for (int i = 0; i < 4; ++i) t += kC[SwizzleComponent(s, i)];
  return t;
}

std::string FlagsText(uint8_t f) {
  std::string t;
  const char* names[] = {"bias", "gamma", "signs", "exp", "int", "norm32"};
  for (int i = 0; i < 6; ++i)
    if (f & (1 << i)) t += (t.empty() ? "" : "+") + std::string(names[i]);
  return t.empty() ? "-" : t;
}

struct Options {
  fs::path out = ".";
  std::string csv;
  bool images = true;
  fs::path pack;
  bool pack_png = false;
  std::string tail_check;  // --tail-check FILE: the "Mips in the base's tail" data check only
  fs::path tail_images;    // --tail-images DIR: with --tail-check, a PNG of each distinct texture's levels
};

// A dump's memory rebuilt at small physical addresses: the base region at 4 KB, the mips after it (both stay
// 4 KB aligned, so the layout and the endian swap units are unchanged).
std::vector<uint8_t> RebuildMemory(const kknr_tools::Kktx& dump, TextureFetch& fetch) {
  const uint32_t original_base = fetch.BaseAddress(), original_mip = fetch.MipAddress();
  const uint32_t base_bytes = uint32_t(dump.base.size()), mip_bytes = uint32_t(dump.mips.size());
  const uint32_t base_at = 0x1000, mip_at = (base_at + base_bytes + 0x1FFF) & ~0xFFFu;
  std::vector<uint8_t> memory(size_t(mip_at) + mip_bytes + 0x1000, 0);
  if (base_bytes) std::memcpy(memory.data() + base_at, dump.base.data(), base_bytes);
  if (mip_bytes) std::memcpy(memory.data() + mip_at, dump.mips.data(), mip_bytes);
  if (original_base) fetch.words[1] = (fetch.words[1] & 0xFFFu) | base_at;
  if (original_mip) fetch.words[5] = (fetch.words[5] & 0xFFFu) | mip_at;
  return memory;
}

// --tail-check (docs/formats.md, "Mips in the base's tail"): for every texture whose whole chain would sit
// in the base level's packed tail, reads levels 1+ from that tail as native_texture_tail_mips does and
// measures how far each level is from a 2x2 box filter of the level above (mean absolute difference, 0..255
// per channel, after the view swizzle). Real mips land near the compression error; a slot holding something
// else does not. The control is a flat picture of the box filter's mean colour: the error of a level that
// carries no picture at all. With --tail-images DIR, each distinct texture's levels are also written as one
// PNG (every level enlarged to level 0's size, top row; the box filter of the level above, bottom row).
std::vector<uint8_t> BoxDown(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
  std::vector<uint8_t> out(size_t(dw) * dh * 4);
  for (uint32_t y = 0; y < dh; ++y)
    for (uint32_t x = 0; x < dw; ++x)
      for (int c = 0; c < 4; ++c) {
        uint32_t sum = 0, n = 0;
        for (uint32_t dy = 0; dy < 2; ++dy)
          for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint32_t sx = std::min(sw - 1, x * 2 + dx), sy = std::min(sh - 1, y * 2 + dy);
            sum += src[(size_t(sy) * sw + sx) * 4 + c];
            ++n;
          }
        out[(size_t(y) * dw + x) * 4 + c] = uint8_t((sum + n / 2) / n);
      }
  return out;
}

double MeanAbs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size() || a.empty()) return -1.0;
  double sum = 0;
  for (size_t i = 0; i < a.size(); ++i) sum += std::abs(int(a[i]) - int(b[i]));
  return sum / double(a.size());
}

std::vector<uint8_t> Flat(const std::vector<uint8_t>& src) {
  std::vector<uint8_t> out(src.size());
  for (int c = 0; c < 4; ++c) {
    uint64_t sum = 0;
    for (size_t i = c; i < src.size(); i += 4) sum += src[i];
    const uint8_t mean = uint8_t((sum + src.size() / 8) / std::max<size_t>(1, src.size() / 4));
    for (size_t i = c; i < out.size(); i += 4) out[i] = mean;
  }
  return out;
}

bool TailCheck(const fs::path& path, FILE* out, const fs::path& images, std::map<uint64_t, int>& seen, int& checked) {
  kknr_tools::Kktx dump;
  std::string load_why;
  if (!kknr_tools::LoadKktx(path, dump, load_why)) return false;
  TextureFetch fetch = TextureFetch::FromWords(dump.words);
  if (!HasMipsInBaseTail(fetch)) return true;
  const std::vector<uint8_t> memory = RebuildMemory(dump, fetch);
  TextureOptions options;
  options.mips_from_base_tail = true;
  HostTextureData data;
  std::string why;
  const std::string stem = path.stem().string();
  if (!ConvertTexture(fetch, GuestMemory{memory.data(), memory.size()}, data, &why, options)) {
    std::fprintf(out, "%s,%s,,,,,,,,FAILED %s\n", stem.c_str(), GetFormatInfo(fetch.Format()).name, why.c_str());
    return false;
  }
  std::vector<std::vector<uint8_t>> levels;  // RGBA8, layer 0
  std::vector<std::pair<uint32_t, uint32_t>> sizes;
  for (uint32_t level = data.plan.min_level; level < data.plan.levels; ++level) {
    const HostSubresource* s = data.Find(level, 0);
    if (!s) break;
    levels.push_back(kknr_tools::HostToRgba8(data, *s, 0));
    sizes.push_back({s->width, s->height});
  }
  const uint64_t content = kknr_tools::Fnv1a64(data.bytes.data(), data.bytes.size());
  const bool first = seen.find(content) == seen.end();
  const int distinct = first ? int(seen.size()) : seen[content];
  if (first) seen[content] = distinct;
  std::string per_level, flat_level;
  double worst = 0, flat_best = -1;
  std::vector<std::vector<uint8_t>> expected(levels.size());
  for (size_t l = 1; l < levels.size(); ++l) {
    const auto [pw, ph] = sizes[l - 1];
    const auto [w, h] = sizes[l];
    expected[l] = BoxDown(levels[l - 1], pw, ph, w, h);
    const double e = MeanAbs(levels[l], expected[l]);
    const double f = MeanAbs(levels[l], Flat(expected[l]));
    char b[64];
    std::snprintf(b, sizeof(b), "%s%.2f", per_level.empty() ? "" : " ", e);
    per_level += b;
    std::snprintf(b, sizeof(b), "%s%.2f", flat_level.empty() ? "" : " ", f);
    flat_level += b;
    worst = std::max(worst, e);
    if (w * h >= 4) flat_best = flat_best < 0 ? f : std::min(flat_best, f);  // 1x1 levels are their own mean
  }
  std::fprintf(out, "%s,%s,%u,%u,%zu,%d,%s,%s,%.3f,%.3f\n", stem.c_str(), GetFormatInfo(fetch.Format()).name,
               data.plan.width, data.plan.height, levels.size(), distinct, per_level.c_str(), flat_level.c_str(), worst,
               flat_best);
  if (first && !images.empty() && !levels.empty()) {
    const uint32_t w0 = sizes[0].first, h0 = sizes[0].second, gap = 2;
    const uint32_t width = uint32_t(levels.size()) * (w0 + gap), height = 2 * h0 + gap;
    std::vector<uint8_t> png(size_t(width) * height * 4, 0);
    auto blit = [&](const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, uint32_t ox, uint32_t oy) {
      for (uint32_t y = 0; y < h0; ++y)
        for (uint32_t x = 0; x < w0; ++x) {
          const uint32_t sx = x * sw / w0, sy = y * sh / h0;
          std::memcpy(&png[(size_t(oy + y) * width + ox + x) * 4], &src[(size_t(sy) * sw + sx) * 4], 4);
          png[(size_t(oy + y) * width + ox + x) * 4 + 3] = 255;
        }
    };
    for (size_t l = 0; l < levels.size(); ++l) {
      blit(levels[l], sizes[l].first, sizes[l].second, uint32_t(l) * (w0 + gap), 0);
      if (l) blit(expected[l], sizes[l].first, sizes[l].second, uint32_t(l) * (w0 + gap), h0 + gap);
    }
    kknr_tools::WritePng((images / ("tail-" + std::to_string(distinct) + "-" + stem + ".png")).string(), width, height, png);
  }
  ++checked;
  return true;
}
bool Process(const fs::path& path, const Options& o, FILE* csv, TexturePackManifest& manifest) {
  kknr_tools::Kktx dump;
  std::string load_why;
  if (!kknr_tools::LoadKktx(path, dump, load_why)) {
    std::printf("%s: %s\n", path.string().c_str(), load_why.c_str());
    return false;
  }
  TextureFetch fetch = TextureFetch::FromWords(dump.words);
  const uint32_t original_base = fetch.BaseAddress(), original_mip = fetch.MipAddress();
  const uint32_t base_bytes = uint32_t(dump.base.size()), mip_bytes = uint32_t(dump.mips.size());
  const std::vector<uint8_t> memory = RebuildMemory(dump, fetch);

  HostTextureData data;
  std::string why;
  const bool ok = ConvertTexture(fetch, GuestMemory{memory.data(), memory.size()}, data, &why);
  const TextureRanges ranges = GetTextureRanges(fetch);
  const FormatInfo& info = GetFormatInfo(fetch.Format());
  const std::string stem = path.stem().string();
  const uint64_t base_hash = ok ? TextureBaseHash(data) : 0;
  const auto alias = std::find_if(manifest.entries.begin(), manifest.entries.end(), [&](const auto& e) {
    return e.hash == base_hash && e.Matches(data.plan);
  });
  // Every CSV row must point at an image that the template actually writes,
  // including BF records with identical converted content.
  const std::string pack_key = base_hash ? TexturePackKey(base_hash,
      alias == manifest.entries.end() ? stem : alias->name) : "";
  std::string signs;
  for (int i = 0; i < 4; ++i) signs += "uSbg"[int(fetch.Sign(i))];
  std::string short_regions =
      (base_bytes < ranges.base_bytes || mip_bytes < ranges.mip_bytes) ? " (dump shorter than the layout: zeros)" : "";
  if (dump.v1_page_fix) short_regions += " (version 1 dump: moved up a page, last page zero)";
  std::printf("%s: %s %s %s %s %ux%ux%u levels %u-%u%s signs %s swizzle %s -> %s %s view %s flags %s%s%s\n",
              stem.c_str(), info.name, EndianName(fetch.EndianMode()), fetch.Tiled() ? "tiled" : "linear",
              DimensionName(fetch.Dim()), data.plan.width, data.plan.height,
              data.plan.dimension == Dimension::k3D ? data.plan.depth : data.plan.layers, data.plan.min_level,
              data.plan.levels ? data.plan.levels - 1 : 0, fetch.PackedMips() ? " packed" : "", signs.c_str(),
              SwizzleText(fetch.Swizzle()).c_str(), GetHostFormatInfo(data.plan.format).name,
              ConversionName(data.plan.conversion), HostSwizzleText(data.plan.view_swizzle).c_str(),
              FlagsText(data.plan.shader_flags).c_str(), ok ? "" : (" FAILED: " + why).c_str(), short_regions.c_str());
  if (csv)
    std::fprintf(csv, "%s,%08X,%08X,%s,%s,%s,%s,%u,%u,%u,%u,%u,%d,%s,%s,%s,%s,%s,%s,%d,%d,%016llX,%s,%016llX,%s\n", stem.c_str(),
                 original_base, original_mip, info.name, EndianName(fetch.EndianMode()),
                 fetch.Tiled() ? "tiled" : "linear", DimensionName(fetch.Dim()), data.plan.width, data.plan.height,
                 data.plan.dimension == Dimension::k3D ? data.plan.depth : data.plan.layers, data.plan.min_level,
                 data.plan.levels, int(fetch.PackedMips()), signs.c_str(), SwizzleText(fetch.Swizzle()).c_str(),
                 GetHostFormatInfo(data.plan.format).name, ConversionName(data.plan.conversion),
                 HostSwizzleText(data.plan.view_swizzle).c_str(), FlagsText(data.plan.shader_flags).c_str(),
                 int(data.plan.decompressed), int(ok),
                 (unsigned long long)kknr_tools::Fnv1a64(data.bytes.data(), data.bytes.size()),
                 ok ? "" : why.c_str(), (unsigned long long)base_hash, pack_key.c_str());
  if (ok && base_hash && !o.pack.empty() && stem.size()==20 && stem.substr(0,3)=="bf_") {
    // Identical BF records cannot be distinguished at runtime; use the first
    // sorted alias and list the content key on every CSV row.
    if (alias == manifest.entries.end()) {
      TexturePackEntry e{base_hash,stem,pack_key+(o.pack_png?".png":".dds"),data.plan.format,data.plan.width,data.plan.height};
      if (o.pack_png) {
        auto raw=data; raw.plan.view_swizzle=kSwizzleXYZW;
        const auto* sub=raw.Find(0,0);
        if (!sub || !kknr_tools::WritePng((o.pack/e.file).string(),sub->width,sub->height,
            kknr_tools::HostToRgba8(raw,*sub,0))) return false;
      } else if (!kknr_tools::WriteDds((o.pack/e.file).string(),data)) return false;
      manifest.entries.push_back(std::move(e));
    }
  }
  if (!ok || !o.images) return ok;

  kknr_tools::WriteDds((o.out / (stem + ".dds")).string(), data);
  // Level 0 (or the first stored level) of every layer / slice, stacked.
  std::vector<uint8_t> png;
  uint32_t width = 0, height = 0;
  const uint32_t level = data.plan.min_level;
  for (uint32_t layer = 0; layer < data.plan.layers; ++layer) {
    const HostSubresource* s = data.Find(level, layer);
    if (!s) continue;
    for (uint32_t z = 0; z < s->depth; ++z) {
      const std::vector<uint8_t> rgba = kknr_tools::HostToRgba8(data, *s, z);
      png.insert(png.end(), rgba.begin(), rgba.end());
      width = s->width;
      height += s->height;
    }
  }
  if (width) kknr_tools::WritePng((o.out / (stem + ".png")).string(), width, height, png);
  return true;
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
    else if (a == "--no-images")
      o.images = false;
    else if (a == "--pack-template" && i + 1 < argc)
      o.pack = fs::u8path(argv[++i]);
    else if (a == "--pack-png")
      o.pack_png = true;
    else if (a == "--tail-check" && i + 1 < argc)
      o.tail_check = argv[++i];
    else if (a == "--tail-images" && i + 1 < argc)
      o.tail_images = fs::u8path(argv[++i]);
    else if (a.rfind("--", 0) == 0) {
      std::printf("unknown option %s\n", a.c_str());
      return 2;
    } else
      inputs.push_back(a);
  }
  if (inputs.empty()) {
    std::printf("usage: kknr_texdump [--out DIR] [--csv FILE] [--no-images] [--pack-template DIR] [--pack-png] "
                "[--tail-check FILE [--tail-images DIR]] FILE_OR_DIR...\n");
    return 2;
  }
  std::error_code ec;
  if (!o.tail_check.empty()) {
    FILE* out = std::fopen(o.tail_check.c_str(), "w");
    if (!out) return 1;
    if (!o.tail_images.empty()) fs::create_directories(o.tail_images, ec);
    std::fprintf(out, "name,format,width,height,levels,distinct,box_mae_per_level,flat_mae_per_level,worst_box_mae,"
                      "best_flat_mae\n");
    std::map<uint64_t, int> seen;
    int total = 0, checked = 0, failed = 0;
    for (const fs::path& in : inputs) {
      std::vector<fs::path> files;
      if (fs::is_directory(in)) {
        for (const auto& e : fs::directory_iterator(in))
          if (e.path().extension() == ".bin") files.push_back(e.path());
        std::sort(files.begin(), files.end());
      } else {
        files.push_back(in);
      }
      for (const fs::path& f : files) {
        ++total;
        failed += !TailCheck(f, out, o.tail_images, seen, checked);
      }
    }
    std::fclose(out);
    std::printf("%d textures, %d with their mips in the base's tail (%zu distinct), %d failed\n", total, checked,
                seen.size(), failed);
    return failed ? 1 : 0;
  }
  fs::create_directories(o.out, ec);
  if (!o.pack.empty()) { fs::create_directories(o.pack,ec); if(ec) return 1; }
  TexturePackManifest manifest;
  FILE* csv = o.csv.empty() ? nullptr : std::fopen(o.csv.c_str(), "w");
  if (csv)
    std::fprintf(csv, "name,base,mip,format,endian,tiling,dim,width,height,depth_or_layers,min_level,levels,packed,"
                      "signs,swizzle,host_format,conversion,view,flags,decompressed,ok,host_hash,error,base_xxh3,pack_key\n");
  int total = 0, failed = 0;
  for (const fs::path& in : inputs) {
    std::vector<fs::path> files;
    if (fs::is_directory(in)) {
      for (const auto& e : fs::directory_iterator(in))
        if (e.path().extension() == ".bin") files.push_back(e.path());
      std::sort(files.begin(), files.end());
    } else {
      files.push_back(in);
    }
    for (const fs::path& f : files) {
      ++total;
      failed += !Process(f, o, csv, manifest);
    }
  }
  if (csv) std::fclose(csv);
  if (!o.pack.empty() && !manifest.Write(o.pack)) ++failed;
  std::printf("%d textures, %d failed\n", total, failed);
  return failed ? 1 : 0;
}
