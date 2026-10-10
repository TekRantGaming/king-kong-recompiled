// kknr_texdump: converts texture dumps to host data with the library, as the renderer will.
//
//   kknr_texdump [--out DIR] [--csv FILE] [--no-images] FILE_OR_DIR...
//
// Input: KKTX files (KK_DEV_TEX_DUMP in kk/src/dev_d3d_trace.cpp, or kknr_bfscan --dump; see kktx.h). For each one it writes <name>.dds (the host format, every level and layer) and
// <name>.png (level 0, layers or depth slices stacked vertically, with the view swizzle applied), prints one
// summary line and adds a CSV row (with an FNV-1a hash of the host data, to compare runs).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "image_io.h"
#include "kknr/texture_convert.h"
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
};

bool Process(const fs::path& path, const Options& o, FILE* csv) {
  kknr_tools::Kktx dump;
  std::string load_why;
  if (!kknr_tools::LoadKktx(path, dump, load_why)) {
    std::printf("%s: %s\n", path.string().c_str(), load_why.c_str());
    return false;
  }
  TextureFetch fetch = TextureFetch::FromWords(dump.words);
  const uint32_t original_base = fetch.BaseAddress(), original_mip = fetch.MipAddress();
  const uint32_t base_bytes = uint32_t(dump.base.size()), mip_bytes = uint32_t(dump.mips.size());

  // Rebuild a small physical memory: the base region at 4 KB, the mips after it (both stay 4 KB aligned, so
  // the layout and the endian swap units are unchanged).
  const uint32_t base_at = 0x1000, mip_at = (base_at + base_bytes + 0x1FFF) & ~0xFFFu;
  std::vector<uint8_t> memory(size_t(mip_at) + mip_bytes + 0x1000, 0);
  if (base_bytes) std::memcpy(memory.data() + base_at, dump.base.data(), base_bytes);
  if (mip_bytes) std::memcpy(memory.data() + mip_at, dump.mips.data(), mip_bytes);
  if (original_base) fetch.words[1] = (fetch.words[1] & 0xFFFu) | base_at;
  if (original_mip) fetch.words[5] = (fetch.words[5] & 0xFFFu) | mip_at;

  HostTextureData data;
  std::string why;
  const bool ok = ConvertTexture(fetch, GuestMemory{memory.data(), memory.size()}, data, &why);
  const TextureRanges ranges = GetTextureRanges(fetch);
  const FormatInfo& info = GetFormatInfo(fetch.Format());
  const std::string stem = path.stem().string();
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
    std::fprintf(csv, "%s,%08X,%08X,%s,%s,%s,%s,%u,%u,%u,%u,%u,%d,%s,%s,%s,%s,%s,%s,%d,%d,%016llX,%s\n", stem.c_str(),
                 original_base, original_mip, info.name, EndianName(fetch.EndianMode()),
                 fetch.Tiled() ? "tiled" : "linear", DimensionName(fetch.Dim()), data.plan.width, data.plan.height,
                 data.plan.dimension == Dimension::k3D ? data.plan.depth : data.plan.layers, data.plan.min_level,
                 data.plan.levels, int(fetch.PackedMips()), signs.c_str(), SwizzleText(fetch.Swizzle()).c_str(),
                 GetHostFormatInfo(data.plan.format).name, ConversionName(data.plan.conversion),
                 HostSwizzleText(data.plan.view_swizzle).c_str(), FlagsText(data.plan.shader_flags).c_str(),
                 int(data.plan.decompressed), int(ok),
                 (unsigned long long)kknr_tools::Fnv1a64(data.bytes.data(), data.bytes.size()),
                 ok ? "" : why.c_str());
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
    else if (a.rfind("--", 0) == 0) {
      std::printf("unknown option %s\n", a.c_str());
      return 2;
    } else
      inputs.push_back(a);
  }
  if (inputs.empty()) {
    std::printf("usage: kknr_texdump [--out DIR] [--csv FILE] [--no-images] FILE_OR_DIR...\n");
    return 2;
  }
  std::error_code ec;
  fs::create_directories(o.out, ec);
  FILE* csv = o.csv.empty() ? nullptr : std::fopen(o.csv.c_str(), "w");
  if (csv)
    std::fprintf(csv, "name,base,mip,format,endian,tiling,dim,width,height,depth_or_layers,min_level,levels,packed,"
                      "signs,swizzle,host_format,conversion,view,flags,decompressed,ok,host_hash,error\n");
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
      failed += !Process(f, o, csv);
    }
  }
  if (csv) std::fclose(csv);
  std::printf("%d textures, %d failed\n", total, failed);
  return failed ? 1 : 0;
}
