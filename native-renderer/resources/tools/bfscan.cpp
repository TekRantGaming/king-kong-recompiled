// kknr_bfscan: lists the textures stored in the game's KKTextures.bf and, with --dump, writes each one as a
// KKTX file for kknr_texdump. Reads game data: run it on the Windows PC, never commit what it writes.
//
//   kknr_bfscan <KKTextures.bf> [--dump DIR] [--limit N] [--pack KEY] [--assume-tiled]
//
// --assume-tiled: treat every record as tiled whatever its format word says (the port's launcher reads the
// front-end logo as tiled; whether the record's word carries the tiling bit is not confirmed).
//
// The file is a Jade engine BIG file: a table of (offset, key) entries at 0x44, one per pack; a pack is a run
// of 12-byte-header chunks of LZO1X data (kk/src/launcher_art.cpp reads the front end's pack the same way).
// A texture record has the magic 0xCAD01234 (little-endian) 12 bytes before "D2KK", then big-endian width,
// height and a 360 D3DFORMAT word, four more words (printed, meaning not known yet) and the texels from +0x20.
// The fetch constant written with --dump is synthesized from that: base level only, the D3DFORMAT's format,
// endian, tiling, signs and swizzle. Mip chains are not described by these records as far as is known, so
// only level 0 converts; the listing's extra words are there to find out more.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "kknr/tiling.h"
#include "kknr/xenos.h"

namespace fs = std::filesystem;
using namespace kknr;

namespace {

uint32_t LE32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// LZO1X decompression with bounds checks (as lzo1x_decompress_safe); the port's launcher_art.cpp version.
bool Lzo1xDecompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
  size_t ip = 0, op = 0;
  auto have = [&](size_t n) { return in_len - ip >= n; };
  auto literals = [&](size_t n) {
    if (!have(n) || out_len - op < n) return false;
    std::memcpy(out + op, in + ip, n);
    ip += n;
    op += n;
    return true;
  };
  auto match = [&](size_t dist, size_t n) {
    if (dist == 0 || dist > op || out_len - op < n) return false;
    for (size_t i = 0; i < n; ++i, ++op) out[op] = out[op - dist];
    return true;
  };
  auto extend = [&](size_t& n, size_t base) {
    while (have(1) && in[ip] == 0) {
      n += 255;
      ++ip;
    }
    if (!have(1)) return false;
    n += base + in[ip++];
    return true;
  };
  int state = 0;
  if (!have(1)) return false;
  if (in[0] > 17) {
    const size_t n = size_t(in[ip++]) - 17;
    if (!literals(n)) return false;
    state = n < 4 ? int(n) : 4;
  }
  for (;;) {
    if (!have(1)) return false;
    size_t t = in[ip++];
    if (t < 16) {
      if (state == 0) {
        if (t == 0 && !extend(t, 15)) return false;
        if (!literals(t + 3)) return false;
        state = 4;
        continue;
      }
      if (!have(1)) return false;
      const size_t b = in[ip++];
      const bool after_run = state == 4;
      if (!match((after_run ? 1 + 0x0800 : 1) + (t >> 2) + (b << 2), after_run ? 3 : 2)) return false;
    } else if (t >= 64) {
      if (!have(1)) return false;
      const size_t b = in[ip++];
      if (!match(1 + ((t >> 2) & 7) + (b << 3), (t >> 5) + 1)) return false;
    } else if (t >= 32) {
      size_t n = t & 31;
      if (n == 0 && !extend(n, 31)) return false;
      if (!have(2)) return false;
      const size_t d = (size_t(in[ip]) | (size_t(in[ip + 1]) << 8)) >> 2;
      ip += 2;
      if (!match(1 + d, n + 2)) return false;
    } else {
      size_t n = t & 7;
      if (n == 0 && !extend(n, 7)) return false;
      if (!have(2)) return false;
      const size_t d = ((t & 8) << 11) + ((size_t(in[ip]) | (size_t(in[ip + 1]) << 8)) >> 2);
      ip += 2;
      if (d == 0) return op == out_len;
      if (!match(d + 0x4000, n + 2)) return false;
    }
    state = in[ip - 2] & 3;
    if (state && !literals(size_t(state))) return false;
  }
}

bool ReadPack(std::ifstream& f, uint64_t file_size, uint64_t offset, std::vector<uint8_t>& out) {
  out.clear();
  uint8_t h[12];
  f.clear();
  f.seekg(std::streamoff(offset));
  if (!f.read(reinterpret_cast<char*>(h), 4)) return false;
  const uint64_t end = std::min<uint64_t>(offset + 4 + LE32(h), file_size);
  std::vector<uint8_t> stored;
  for (uint64_t pos = offset; pos + 12 <= end;) {
    f.seekg(std::streamoff(pos));
    if (!f.read(reinterpret_cast<char*>(h), 12)) break;
    const uint32_t size = LE32(h + 4), stored_size = LE32(h + 8);
    if (size == 0 || size > (1u << 24) || stored_size > size || pos + 12 + stored_size > end) break;
    stored.resize(stored_size);
    if (!f.read(reinterpret_cast<char*>(stored.data()), stored_size)) break;
    const size_t at = out.size();
    out.resize(at + size);
    if (stored_size == size)
      std::memcpy(&out[at], stored.data(), size);
    else if (!Lzo1xDecompress(stored.data(), stored_size, &out[at], size))
      return false;
    pos += 12 + stored_size;
  }
  return !out.empty();
}

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: kknr_bfscan <KKTextures.bf> [--dump DIR] [--limit N] [--pack KEY] [--assume-tiled]\n");
    return 2;
  }
  fs::path dump;
  long limit = -1;
  long long only_pack = -1;
  bool assume_tiled = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dump" && i + 1 < argc)
      dump = argv[++i];
    else if (a == "--limit" && i + 1 < argc)
      limit = std::atol(argv[++i]);
    else if (a == "--pack" && i + 1 < argc)
      only_pack = std::stoll(argv[++i], nullptr, 16);
    else if (a == "--assume-tiled")
      assume_tiled = true;
  }
  std::ifstream f(argv[1], std::ios::binary);
  if (!f) {
    std::printf("can't open %s\n", argv[1]);
    return 1;
  }
  f.seekg(0, std::ios::end);
  const uint64_t file_size = uint64_t(f.tellg());
  uint8_t header[0x44];
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "BIG", 4) != 0) {
    std::printf("not a Jade BIG file\n");
    return 1;
  }
  const uint32_t count = LE32(header + 8);
  std::vector<uint8_t> table(size_t(count) * 8);
  if (count == 0 || count > 65536 || !f.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size()))) {
    std::printf("unexpected BIG table\n");
    return 1;
  }
  if (!dump.empty()) fs::create_directories(dump);

  std::map<std::string, int> combos;
  long found = 0;
  std::vector<uint8_t> pack;
  for (uint32_t e = 0; e < count && (limit < 0 || found < limit); ++e) {
    const uint32_t offset = LE32(&table[e * 8]), key = LE32(&table[e * 8 + 4]);
    if (only_pack >= 0 && key != uint32_t(only_pack)) continue;
    if (!offset || offset + 12 > file_size || !ReadPack(f, file_size, offset, pack)) continue;
    static const uint8_t kMagic[4] = {0x34, 0x12, 0xD0, 0xCA};
    for (size_t i = 12; i + 0x20 <= pack.size() && (limit < 0 || found < limit); ++i) {
      if (std::memcmp(&pack[i], "D2KK", 4) != 0 || std::memcmp(&pack[i - 12], kMagic, 4) != 0) continue;
      const uint32_t w = BE32(&pack[i + 4]), h = BE32(&pack[i + 8]);
      const D3DFormat fmt{BE32(&pack[i + 12])};
      if (!w || !h || w > 8192 || h > 8192) continue;
      ++found;
      const FormatInfo& info = GetFormatInfo(fmt.Format());
      std::printf("pack %08X +%08zX %4ux%-4u fmt %08X %-22s %-6s %-6s extra %08X %08X %08X %08X\n", key, i, w, h,
                  fmt.value, info.name, EndianName(fmt.EndianMode()), fmt.Tiled() ? "tiled" : "linear",
                  BE32(&pack[i + 16]), BE32(&pack[i + 20]), BE32(&pack[i + 24]), BE32(&pack[i + 28]));
      ++combos[std::string(info.name) + " " + EndianName(fmt.EndianMode()) + (fmt.Tiled() ? " tiled" : " linear")];
      if (dump.empty()) continue;
      TextureFetchDesc d;
      d.format = fmt.Format();
      d.endian = fmt.EndianMode();
      d.tiled = fmt.Tiled() || assume_tiled;
      d.width = w;
      d.height = h;
      d.base_address = 0x1000;
      d.swizzle = fmt.Swizzle();
      d.integer = fmt.NumFormatInteger();
      for (int c = 0; c < 4; ++c) d.signs[c] = fmt.Sign(c);
      const TextureFetch fetch = MakeTextureFetch(d);
      const GuestLayout layout = ComputeGuestLayout(fetch);
      const size_t data = i + 0x20;
      const uint32_t bytes = uint32_t(std::min<size_t>(layout.base_extent_bytes, pack.size() - data));
      char name[64];
      std::snprintf(name, sizeof(name), "bf_%08X_%08zX.bin", key, i);
      std::ofstream o(dump / name, std::ios::binary);
      const uint32_t hdr[10] = {0x58544B4Bu, 1, fetch.words[0], fetch.words[1], fetch.words[2],
                                fetch.words[3], fetch.words[4], fetch.words[5], bytes, 0};
      for (uint32_t v : hdr)
        for (int s = 0; s < 32; s += 8) o.put(char(v >> s));
      o.write(reinterpret_cast<const char*>(&pack[data]), bytes);
    }
  }
  std::printf("\n%ld textures; by format / endian / tiling:\n", found);
  for (const auto& [k, n] : combos) std::printf("%6d  %s\n", n, k.c_str());
  return 0;
}
