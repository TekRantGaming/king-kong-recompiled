// KKTX texture dumps (KK_DEV_TEX_DUMP in the trace build, kknr_bfscan --dump): "KKTX", a version, the 6
// fetch-constant words as the texture object holds them, base bytes, mip bytes (little-endian header), then
// the base and mip regions exactly as stored in guest memory.
//
// Version 1 trace dumps read each region from the low 29 bits of the object's CPU address, which for the
// 0xE0000000 view (where the game's textures live) is one 4 KB page before the data: the GPU address is
// 4 KB further (SetTexture's conversion). Load() corrects that: the region moves up one page and its last
// page reads as zero (it was not dumped). Version 2 (fixed trace dumps, kknr_bfscan) is read as is.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace kknr_tools {

struct Kktx {
  uint32_t version = 0;
  uint32_t words[6] = {};
  std::vector<uint8_t> base, mips;
  bool v1_page_fix = false;  // regions moved by a page (version 1 dump of 0xE0000000-view addresses)
};

inline bool LoadKktx(const std::filesystem::path& path, Kktx& out, std::string& why) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto le32 = [&](size_t at) {
    return uint32_t(file[at]) | uint32_t(file[at + 1]) << 8 | uint32_t(file[at + 2]) << 16 |
           uint32_t(file[at + 3]) << 24;
  };
  if (file.size() < 40 || std::memcmp(file.data(), "KKTX", 4) != 0) return why = "not a KKTX file", false;
  out.version = le32(4);
  if (out.version != 1 && out.version != 2) return why = "unknown KKTX version", false;
  for (int i = 0; i < 6; ++i) out.words[i] = le32(8 + 4 * i);
  const uint32_t base_bytes = le32(32), mip_bytes = le32(36);
  if (40ull + base_bytes + mip_bytes > file.size()) return why = "truncated", false;
  out.base.assign(file.begin() + 40, file.begin() + 40 + base_bytes);
  out.mips.assign(file.begin() + 40 + base_bytes, file.begin() + 40 + base_bytes + mip_bytes);
  if (out.version == 1) {
    auto fix = [&](std::vector<uint8_t>& region, uint32_t word) {
      if (region.empty() || (word & 0xFFFFF000u) < 0xE0000000u) return;
      const size_t page = std::min<size_t>(4096, region.size());
      std::memmove(region.data(), region.data() + page, region.size() - page);
      std::memset(region.data() + region.size() - page, 0, page);
      out.v1_page_fix = true;
    };
    fix(out.base, out.words[1]);
    fix(out.mips, out.words[5]);
  }
  return true;
}

}  // namespace kknr_tools
