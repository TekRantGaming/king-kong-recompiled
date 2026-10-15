// A local, learned chapter-start hint. Descriptions never make guest bytes
// authoritative: replay requires the complete captured content hash to match.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include "kknr/texture_cache.h"

namespace nr {

struct ResourcePreloadRecord {
  enum : uint32_t { Texture = 0, Vertex = 1, Index16 = 2, Index32 = 3 };
  uint32_t kind = Texture;
  std::array<uint32_t, 6> words{};
  uint64_t hash = 0;
  bool SameResource(const ResourcePreloadRecord& other) const {
    return kind == other.kind && words == other.words;
  }
  static ResourcePreloadRecord Tex(const kknr::TextureFetch& fetch) {
    ResourcePreloadRecord r;
    const auto key = kknr::MakeTextureKey(fetch);
    std::copy(std::begin(key.words), std::end(key.words), r.words.begin());
    return r;
  }
  static ResourcePreloadRecord Buffer(uint32_t physical, uint32_t bytes, bool index, bool index32) {
    ResourcePreloadRecord r;
    r.kind = index ? (index32 ? Index32 : Index16) : Vertex;
    r.words[0] = physical;
    r.words[1] = bytes;
    return r;
  }
};

class ResourcePreloadFile {
 public:
  static constexpr size_t kMaxRecords = 1024;
  // Explicit words avoid structure padding. A checksum rejects torn writes;
  // fixed limits bound allocations even for an untrusted or obsolete cache.
  static bool Read(const std::string& path, std::vector<ResourcePreloadRecord>& records) {
    records.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto bytes = file.tellg();
    if (bytes < 24 || bytes > 24 + std::streamoff(kMaxRecords * 36)) return false;
    file.seekg(0);
    uint32_t header[6]{};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!file || header[0] != 0x52504B4B || header[1] != 1 || header[2] > kMaxRecords ||
        bytes != 24 + std::streamoff(header[2] * 36) || header[3]) return false;
    std::vector<std::array<uint32_t, 9>> wire(header[2]);
    file.read(reinterpret_cast<char*>(wire.data()), wire.size() * sizeof(wire[0]));
    if (!file || kknr::HashBytes(wire.data(), wire.size() * sizeof(wire[0])) !=
        (uint64_t(header[4]) | uint64_t(header[5]) << 32)) return false;
    for (const auto& w : wire) {
      if (w[0] > ResourcePreloadRecord::Index32) { records.clear(); return false; }
      ResourcePreloadRecord r;
      r.kind = w[0];
      std::copy(w.begin() + 1, w.begin() + 7, r.words.begin());
      r.hash = uint64_t(w[7]) | uint64_t(w[8]) << 32;
      records.push_back(r);
    }
    return true;
  }
  static bool Write(const std::string& path, const std::vector<ResourcePreloadRecord>& records) {
    if (path.empty() || records.size() > kMaxRecords) return false;
    std::vector<std::array<uint32_t, 9>> wire;
    for (const auto& r : records) {
      std::array<uint32_t, 9> w{};
      w[0] = r.kind;
      std::copy(r.words.begin(), r.words.end(), w.begin() + 1);
      w[7] = uint32_t(r.hash); w[8] = uint32_t(r.hash >> 32);
      wire.push_back(w);
    }
    const uint64_t hash = kknr::HashBytes(wire.data(), wire.size() * sizeof(wire[0]));
    const uint32_t header[] = {0x52504B4B, 1, uint32_t(wire.size()), 0, uint32_t(hash), uint32_t(hash >> 32)};
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(wire.data()), wire.size() * sizeof(wire[0]));
    file.close();
    return bool(file);
  }
};

}  // namespace nr
