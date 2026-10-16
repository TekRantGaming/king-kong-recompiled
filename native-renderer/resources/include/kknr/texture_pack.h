// Version 1 player-owned texture packs. No guest addresses or game data in code.
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "kknr/texture_convert.h"

namespace kknr {
// Same XXH3-64 identity as the material census: tightly packed converted level
// zero, excluding other mips, layers and allocation padding. Zero means ineligible.
uint64_t TextureBaseHash(const HostTextureData& data);
std::string TexturePackKey(uint64_t hash, const std::string& bf_name);
bool TexturePackEligible(const HostTexturePlan& plan);
struct TexturePackEntry {
  uint64_t hash = 0;
  std::string name, file;
  HostFormat format = HostFormat::UNKNOWN;
  uint32_t width = 0, height = 0;
  bool Matches(const HostTexturePlan& plan) const;
};
struct TexturePackManifest {
  static constexpr size_t kMaxEntries = 32768;
  std::vector<TexturePackEntry> entries;
  bool Read(const std::filesystem::path& folder, std::string& why);
  bool Write(const std::filesystem::path& folder) const;
};
// Bounded input and decoded allocation, strict 2D DDS/PNG parsing. Keeps raw
// host channels (no display swizzle, sRGB view or normal-Z reconstruction).
bool LoadTextureReplacement(const std::filesystem::path& folder, const TexturePackEntry& entry,
                            HostTextureData& data, std::string& why);

// All manifest/file I/O, decompression and mip generation are asynchronous.
// The dispatcher uses the same bounded TextureConversionWorkers as originals.
// Requests/results are generation-tagged; Configure and Take never wait for I/O.
class TexturePackStream {
 public:
  explicit TexturePackStream(unsigned workers);
  ~TexturePackStream();
  void Configure(const std::string& folder);
  void Request(uint64_t hash, const HostTexturePlan& original);
  // A ready image larger than max_bytes is dropped (its decoded bytes freed and
  // its identity requestable again) and reported through too_large, so a full
  // pack limit never leaves the ready set re-reading files. Without too_large
  // it stays ready.
  std::shared_ptr<const HostTextureData> Take(uint64_t hash, const HostTexturePlan& original, uint64_t max_bytes = UINT64_MAX,
                                              uint64_t* too_large = nullptr);
  std::string Status() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace kknr
