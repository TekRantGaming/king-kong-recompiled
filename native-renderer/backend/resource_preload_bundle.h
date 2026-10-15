// Shipped chapter-start hints: resource descriptions and hashes only, never
// guest payloads. A matching full guest-content hash is still required to replay.
#pragma once

#include <set>
#include <utility>
#include "backend/resource_preload.h"

namespace nr {

struct ResourcePreloadChapter {
  uint32_t level = 0, start = 0;
  std::vector<ResourcePreloadRecord> records;
};

class ResourcePreloadBundle {
 public:
  static constexpr uint32_t kMagic = 0x42524B4B;  // bytes: KKRB
  static constexpr uint32_t kVersion = 1;
  static constexpr uint32_t kResourceKeyVersion = 1;
  static constexpr uint32_t kTitleId = 0x555307D3;
  static constexpr size_t kMaxChapters = 128;
  static constexpr size_t kMaxRecords = ResourcePreloadFile::kMaxRecords;
  static constexpr size_t kMaxTotalRecords = 65536;
  static constexpr size_t kHeaderBytes = 32, kChapterBytes = 12, kRecordBytes = 36;

  static bool SameRecord(const ResourcePreloadRecord& a, const ResourcePreloadRecord& b) {
    return a.SameResource(b) && a.hash == b.hash;
  }
  static bool RecordLess(const ResourcePreloadRecord& a, const ResourcePreloadRecord& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.words != b.words) return a.words < b.words;
    return a.hash < b.hash;
  }

  // Wire v1, all integers little endian, without native structs/padding:
  // header: magic, format version, key version, title id, chapter count,
  //         total record count, FNV-1a-64(payload) low/high words.
  // payload: for each chapter, level/start/count then count records, each
  //          kind, six description words, content hash low/high words.
  // Counts/versions/title/size/checksum and every record are checked before
  // publishing anything. An incompatible or torn bundle has no usable prefix.
  static bool Read(const std::string& path, std::vector<ResourcePreloadChapter>& chapters) {
    chapters.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto bytes = file.tellg();
    constexpr size_t max_bytes = kHeaderBytes + kMaxChapters * kChapterBytes + kMaxTotalRecords * kRecordBytes;
    if (bytes < std::streamoff(kHeaderBytes) || bytes > std::streamoff(max_bytes)) return false;
    file.seekg(0);
    std::vector<uint8_t> wire(static_cast<size_t>(bytes));
    file.read(reinterpret_cast<char*>(wire.data()), static_cast<std::streamsize>(wire.size()));
    if (!file || Word(wire, 0) != kMagic || Word(wire, 4) != kVersion ||
        Word(wire, 8) != kResourceKeyVersion || Word(wire, 12) != kTitleId) return false;
    const uint32_t chapter_count = Word(wire, 16), record_count = Word(wire, 20);
    if (chapter_count > kMaxChapters || record_count > kMaxTotalRecords ||
        wire.size() != kHeaderBytes + size_t(chapter_count) * kChapterBytes + size_t(record_count) * kRecordBytes ||
        Checksum(wire.data() + kHeaderBytes, wire.size() - kHeaderBytes) !=
            (uint64_t(Word(wire, 24)) | uint64_t(Word(wire, 28)) << 32)) return false;
    std::vector<ResourcePreloadChapter> parsed;
    std::set<std::pair<uint32_t, uint32_t>> keys;
    size_t offset = kHeaderBytes, seen = 0;
    for (uint32_t c = 0; c < chapter_count; ++c) {
      if (wire.size() - offset < kChapterBytes) return false;
      ResourcePreloadChapter chapter;
      chapter.level = Word(wire, offset); chapter.start = Word(wire, offset + 4);
      const uint32_t count = Word(wire, offset + 8);
      offset += kChapterBytes;
      if (count > kMaxRecords || count > record_count - seen ||
          size_t(count) * kRecordBytes > wire.size() - offset ||
          !keys.insert({chapter.level, chapter.start}).second) return false;
      chapter.records.reserve(count);
      for (uint32_t r = 0; r < count; ++r, offset += kRecordBytes) {
        ResourcePreloadRecord record;
        record.kind = Word(wire, offset);
        if (record.kind > ResourcePreloadRecord::Index32) return false;
        for (size_t w = 0; w < record.words.size(); ++w) record.words[w] = Word(wire, offset + 4 + 4 * w);
        record.hash = uint64_t(Word(wire, offset + 28)) | uint64_t(Word(wire, offset + 32)) << 32;
        chapter.records.push_back(record);
      }
      seen += count;
      UniqueRecords(chapter.records);
      parsed.push_back(std::move(chapter));
    }
    if (seen != record_count || offset != wire.size()) return false;
    chapters.swap(parsed);
    return true;
  }

  // Canonical chapter/record ordering makes merged bundles reproducible.
  // Exact duplicate records collapse; hashes for one description remain distinct.
  // Invalid input is rejected before opening/truncating the destination.
  static bool Write(const std::string& path, const std::vector<ResourcePreloadChapter>& chapters) {
    if (path.empty() || chapters.size() > kMaxChapters) return false;
    auto canonical = chapters;
    std::sort(canonical.begin(), canonical.end(), [](const auto& a, const auto& b) {
      return std::pair(a.level, a.start) < std::pair(b.level, b.start);
    });
    size_t total = 0;
    for (size_t c = 0; c < canonical.size(); ++c) {
      auto& chapter = canonical[c];
      if (c && canonical[c - 1].level == chapter.level && canonical[c - 1].start == chapter.start) return false;
      for (const auto& r : chapter.records) if (r.kind > ResourcePreloadRecord::Index32) return false;
      UniqueRecords(chapter.records);
      if (chapter.records.size() > kMaxRecords || chapter.records.size() > kMaxTotalRecords - total) return false;
      total += chapter.records.size();
    }
    std::vector<uint8_t> wire;
    wire.reserve(kHeaderBytes + canonical.size() * kChapterBytes + total * kRecordBytes);
    for (uint32_t w : {kMagic, kVersion, kResourceKeyVersion, kTitleId,
                       uint32_t(canonical.size()), uint32_t(total), 0u, 0u}) Put(wire, w);
    for (const auto& chapter : canonical) {
      Put(wire, chapter.level); Put(wire, chapter.start); Put(wire, uint32_t(chapter.records.size()));
      for (const auto& r : chapter.records) {
        Put(wire, r.kind);
        for (uint32_t word : r.words) Put(wire, word);
        Put(wire, uint32_t(r.hash)); Put(wire, uint32_t(r.hash >> 32));
      }
    }
    const uint64_t checksum = Checksum(wire.data() + kHeaderBytes, wire.size() - kHeaderBytes);
    for (size_t i = 0; i < 8; ++i) wire[24 + i] = uint8_t(checksum >> (8 * i));
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(wire.data()), static_cast<std::streamsize>(wire.size()));
    file.close();
    return bool(file);
  }

 private:
  static uint32_t Word(const std::vector<uint8_t>& wire, size_t offset) {
    return uint32_t(wire[offset]) | uint32_t(wire[offset + 1]) << 8 |
           uint32_t(wire[offset + 2]) << 16 | uint32_t(wire[offset + 3]) << 24;
  }
  static void Put(std::vector<uint8_t>& wire, uint32_t value) {
    for (size_t b = 0; b < 4; ++b) wire.push_back(uint8_t(value >> (8 * b)));
  }
  static uint64_t Checksum(const uint8_t* wire, size_t bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ wire[i]) * 1099511628211ull;
    return hash;
  }
  static void UniqueRecords(std::vector<ResourcePreloadRecord>& records) {
    std::sort(records.begin(), records.end(), RecordLess);
    records.erase(std::unique(records.begin(), records.end(), SameRecord), records.end());
  }
};

}  // namespace nr
