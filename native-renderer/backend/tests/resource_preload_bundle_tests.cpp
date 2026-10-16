// Synthetic descriptions/hashes only. No GPU, runtime, game or game data.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "backend/resource_preload_bundle.h"
#include "backend/tests/check.h"

using namespace nr;
using namespace nr::test;
namespace fs = std::filesystem;

namespace {
fs::path Root() {
  static const auto root = fs::temp_directory_path() /
      ("nr resource bundle tests " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  return root;
}
std::string Path(const std::string& name) { return (Root() / name).string(); }
using Bytes = std::vector<uint8_t>;
Bytes Load(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Save(const std::string& path, const Bytes& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  CHECK(bool(file));
}
void SetWord(Bytes& bytes, size_t offset, uint32_t value) {
  for (size_t b = 0; b < 4; ++b) bytes[offset + b] = uint8_t(value >> (8 * b));
}
void Put(Bytes& bytes, uint32_t value) {
  const size_t offset = bytes.size(); bytes.resize(offset + 4); SetWord(bytes, offset, value);
}
void Rechecksum(Bytes& wire) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = ResourcePreloadBundle::kHeaderBytes; i < wire.size(); ++i) {
    hash ^= wire[i]; hash *= 1099511628211ull;
  }
  SetWord(wire, 24, uint32_t(hash)); SetWord(wire, 28, uint32_t(hash >> 32));
}
// Independent writer allows structurally corrupt but checksum-valid fixtures.
Bytes Wire(const std::vector<ResourcePreloadChapter>& chapters) {
  Bytes bytes;
  uint32_t count = 0;
  for (const auto& chapter : chapters) count += uint32_t(chapter.records.size());
  for (uint32_t v : {0x42524B4Bu, 1u, 1u, 0x555307D3u, uint32_t(chapters.size()), count, 0u, 0u}) Put(bytes, v);
  for (const auto& chapter : chapters) {
    Put(bytes, chapter.level); Put(bytes, chapter.start); Put(bytes, uint32_t(chapter.records.size()));
    for (const auto& r : chapter.records) {
      Put(bytes, r.kind);
      for (uint32_t w : r.words) Put(bytes, w);
      Put(bytes, uint32_t(r.hash)); Put(bytes, uint32_t(r.hash >> 32));
    }
  }
  Rechecksum(bytes);
  return bytes;
}
ResourcePreloadRecord Record(uint32_t id = 1, uint32_t kind = ResourcePreloadRecord::Texture) {
  ResourcePreloadRecord record;
  record.kind = kind;
  for (size_t i = 0; i < record.words.size(); ++i) record.words[i] = 0x01020300u + id + uint32_t(i * 256);
  record.hash = 0x0123456789ABCDEFuLL + id;
  return record;
}
ResourcePreloadChapter Chapter(uint32_t level = 42, uint32_t start = 7, size_t count = 1) {
  ResourcePreloadChapter chapter{level, start, {}};
  for (size_t i = 0; i < count; ++i) chapter.records.push_back(Record(uint32_t(i + 1), uint32_t(i % 4)));
  return chapter;
}
void Reject(const Bytes& wire) {
  const auto path = Path("invalid.list");
  Save(path, wire);
  std::vector<ResourcePreloadChapter> read{Chapter()};
  CHECK(!ResourcePreloadBundle::Read(path, read));
  CHECK(read.empty());
}
std::string Quoted(const std::string& arg) { return "\"" + arg + "\""; }
int Tool(const std::string& args) {
  std::string command = Quoted(NR_RESOURCES_TOOL) + " " + args + " > " + Quoted(Path("tool.txt")) + " 2>&1";
#if defined(_WIN32)
  command = "\"" + command + "\"";
#endif
  const int status = std::system(command.c_str());
#if defined(_WIN32)
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
}  // namespace

TEST(ResourceBundle_RoundTripCanonicalAndExactVariants) {
  auto a = Chapter(99, 4, 4), b = Chapter(1, 9, 2);
  a.records.push_back(a.records[0]);
  auto variant = a.records[0]; variant.hash ^= 1uLL << 48;
  a.records.push_back(variant);
  const auto file = Path("roundtrip.list");
  CHECK(ResourcePreloadBundle::Write(file, {a, b}));
  std::vector<ResourcePreloadChapter> read;
  CHECK(ResourcePreloadBundle::Read(file, read));
  CHECK_EQ(read.size(), 2u);
  if (read.size() != 2) return;
  CHECK_EQ(read[0].level, 1u); CHECK_EQ(read[1].level, 99u);
  CHECK_EQ(read[1].records.size(), 5u);
  CHECK(std::any_of(read[1].records.begin(), read[1].records.end(), [&](const auto& r) {
    return ResourcePreloadBundle::SameRecord(r, variant);
  }));
  CHECK(ResourcePreloadBundle::Write(Path("canonical.list"), read));
  CHECK(Load(file) == Load(Path("canonical.list")));
  CHECK(Load(file) == Wire(read));  // Byte-level LE contract, independent serializer.
  const auto bytes = Load(file);
  CHECK_EQ(bytes[0], uint8_t('K')); CHECK_EQ(bytes[3], uint8_t('B'));
  CHECK_EQ(bytes[12], 0xD3u); CHECK_EQ(bytes[15], 0x55u);
  CHECK_EQ(bytes.size(), 32u + 2u * 12u + 7u * 36u);
}

TEST(ResourceBundle_EmptyAndAbsent) {
  std::vector<ResourcePreloadChapter> read{Chapter()};
  CHECK(!ResourcePreloadBundle::Read(Path("absent.list"), read)); CHECK(read.empty());
  CHECK(ResourcePreloadBundle::Write(Path("empty.list"), {}));
  CHECK(ResourcePreloadBundle::Read(Path("empty.list"), read)); CHECK(read.empty());
  CHECK(!ResourcePreloadBundle::Write("", {}));
  CHECK(!ResourcePreloadBundle::Write(Path("missing-directory/list"), {}));
}

TEST(ResourceBundle_EveryByteMutationAndTornPrefixRejectWholeFile) {
  const auto valid = Wire({Chapter(1, 1, 4), Chapter(2, 2, 1)});
  for (size_t i = 0; i < valid.size(); ++i) {
    auto changed = valid; changed[i] ^= 1; Reject(changed);
  }
  for (size_t size = 0; size < valid.size(); ++size) Reject(Bytes(valid.begin(), valid.begin() + size));
  auto trailing = valid; trailing.push_back(0); Reject(trailing);
}

TEST(ResourceBundle_ChecksumValidStructuralAndCompatibilityFailures) {
  const auto valid = Wire({Chapter(1, 1), Chapter(2, 2)});
  for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12)}) {
    auto wrong = valid; SetWord(wrong, offset, 0); Rechecksum(wrong); Reject(wrong);
  }
  auto wrong = valid;
  SetWord(wrong, 32 + 12 + 36, 1); SetWord(wrong, 32 + 12 + 36 + 4, 1);
  Rechecksum(wrong); Reject(wrong);  // Duplicate chapter key, intact checksum.
  wrong = valid; SetWord(wrong, 32 + 12, 4); Rechecksum(wrong); Reject(wrong);  // Unknown kind.
  wrong = valid; SetWord(wrong, 32 + 8, 1025); Rechecksum(wrong); Reject(wrong);
  wrong = valid; SetWord(wrong, 32 + 8, 0); Rechecksum(wrong); Reject(wrong);
  wrong = valid; SetWord(wrong, 16, 0); Reject(wrong);
  wrong = valid; SetWord(wrong, 20, 1); Reject(wrong);
  // Duplicate variants on wire collapse only exact records.
  auto chapter = Chapter(); chapter.records.push_back(chapter.records[0]);
  Save(Path("wire-duplicates.list"), Wire({chapter}));
  std::vector<ResourcePreloadChapter> read;
  CHECK(ResourcePreloadBundle::Read(Path("wire-duplicates.list"), read));
  if (!read.empty()) CHECK_EQ(read[0].records.size(), 1u);
}

TEST(ResourceBundle_BoundsReadWriteAndNoTruncateOnInvalidInput) {
  std::vector<ResourcePreloadChapter> chapters;
  for (size_t i = 0; i < ResourcePreloadBundle::kMaxChapters + 1; ++i) chapters.push_back(Chapter(uint32_t(i), 1));
  const auto file = Path("preserved.list");
  const Bytes marker{1, 2, 3, 4}; Save(file, marker);
  CHECK(!ResourcePreloadBundle::Write(file, chapters)); CHECK(Load(file) == marker); Reject(Wire(chapters));
  chapters = {Chapter(1, 1, ResourcePreloadBundle::kMaxRecords + 1)};
  CHECK(!ResourcePreloadBundle::Write(file, chapters)); Reject(Wire(chapters));
  chapters.clear();
  for (uint32_t i = 0; i < 65; ++i) chapters.push_back(Chapter(i, 1, 1024));
  CHECK(!ResourcePreloadBundle::Write(file, chapters)); Reject(Wire(chapters));
  chapters.pop_back();
  CHECK(ResourcePreloadBundle::Write(Path("total-bound.list"), chapters));
  std::vector<ResourcePreloadChapter> read;
  CHECK(ResourcePreloadBundle::Read(Path("total-bound.list"), read));
  CHECK_EQ(read.size(), 64u);
  chapters = {Chapter(), Chapter()}; CHECK(!ResourcePreloadBundle::Write(file, chapters));
  chapters = {Chapter()}; chapters[0].records[0].kind = 99;
  CHECK(!ResourcePreloadBundle::Write(file, chapters)); CHECK(Load(file) == marker);
  // File-size rejection happens before allocating or reading this sparse body.
  std::ofstream large(Path("oversized.list"), std::ios::binary);
  large.seekp(32 + 128 * 12 + 65536 * 36); large.put(0); large.close();
  CHECK(!ResourcePreloadBundle::Read(Path("oversized.list"), read)); CHECK(read.empty());
}

TEST(ResourceBundle_ToolMergesLocalDirectoriesBundlesAndContentVariants) {
  fs::create_directories(Path("recordings/deep"));
  const auto first = Path("recordings/cache.resources-10-2");
  const auto second = Path("recordings/deep/cache.resources-10-2");
  const auto third = Path("recordings/deep/cache.resources-4294967295-3");
  auto a = Record(1), variant = a; variant.hash ^= 0x8000000000000000uLL;
  CHECK(ResourcePreloadFile::Write(first, {a, a}));
  CHECK(ResourcePreloadFile::Write(second, {a, variant}));
  CHECK(ResourcePreloadFile::Write(third, {Record(2, ResourcePreloadRecord::Index32)}));
  Save(Path("recordings/irrelevant.txt"), {9, 8, 7});
  const auto previous = Path("previous.list"), output = Path("merged.list"), reversed = Path("reversed.list");
  CHECK(ResourcePreloadBundle::Write(previous, {Chapter(10, 2), Chapter(77, 4)}));
  CHECK_EQ(Tool("-o " + Quoted(output) + " " + Quoted(Path("recordings")) + " " + Quoted(previous)), 0);
  CHECK_EQ(Tool("merge -o " + Quoted(reversed) + " " + Quoted(previous) + " " + Quoted(third) + " " +
               Quoted(second) + " " + Quoted(first)), 0);
  CHECK(Load(output) == Load(reversed));
  std::vector<ResourcePreloadChapter> read;
  CHECK(ResourcePreloadBundle::Read(output, read)); CHECK_EQ(read.size(), 3u);
  if (read.size() == 3) {
    CHECK_EQ(read[0].level, 10u); CHECK_EQ(read[0].records.size(), 2u);
    CHECK_EQ(read[1].level, 77u); CHECK_EQ(read[2].level, 0xFFFFFFFFu);
  }
}

TEST(ResourceBundle_ToolRejectsAbsentInvalidSameFileAndAliasWithoutWriting) {
  const auto output = Path("never-overwrite.list"), local = Path("same.resources-7-8");
  const Bytes marker{2, 4, 6, 8}; Save(output, marker);
  CHECK(ResourcePreloadFile::Write(local, {Record()}));
  CHECK(Tool("-o " + Quoted(output) + " " + Quoted(local) + " " + Quoted(Path("missing"))) != 0);
  CHECK(Load(output) == marker);
  Save(Path("bad.resources-7-8"), {0, 0, 0});
  CHECK(Tool("-o " + Quoted(output) + " " + Quoted(Path("bad.resources-7-8"))) != 0);
  CHECK(ResourcePreloadFile::Write(Path("bad.resources-4294967296-8"), {Record()}));
  CHECK(Tool("-o " + Quoted(output) + " " + Quoted(Path("bad.resources-4294967296-8"))) != 0);
  fs::create_directories(Path("empty-directory"));
  CHECK(Tool("-o " + Quoted(output) + " " + Quoted(Path("empty-directory"))) != 0);
  const auto original = Load(local);
  CHECK(Tool("-o " + Quoted(local) + " " + Quoted(local)) != 0); CHECK(Load(local) == original);
  const auto alias = Path("alias.resources-7-8");
  std::error_code ec; fs::create_hard_link(local, alias, ec);
  CHECK(!ec);
  if (!ec) {
    CHECK(Tool("-o " + Quoted(alias) + " " + Quoted(local)) != 0); CHECK(Load(local) == original);
  }
  fs::create_directories(Path("self-directory"));
  const auto self = Path("self-directory/kkresources.list");
  CHECK(ResourcePreloadBundle::Write(self, {Chapter()})); const auto before = Load(self);
  CHECK(Tool("-o " + Quoted(self) + " " + Quoted(Path("self-directory"))) != 0); CHECK(Load(self) == before);
  CHECK(Load(output) == marker);
}

TEST(ResourceBundle_ToolMergedCountLimitsAndDirectorySymlink) {
  auto first = Chapter(1, 1, 600), second = first;
  for (auto& record : second.records) record.hash ^= 1uLL << 63;
  const auto one = Path("limit-one.resources-1-1"), two = Path("limit-two.resources-1-1");
  CHECK(ResourcePreloadFile::Write(one, first.records)); CHECK(ResourcePreloadFile::Write(two, second.records));
  const auto output = Path("limit-output.list");
  CHECK(Tool("-o " + Quoted(output) + " " + Quoted(one) + " " + Quoted(two)) != 0);
  CHECK(!fs::exists(output));
  fs::create_directories(Path("symlink-input")); fs::create_directories(Path("symlink-target"));
  CHECK(ResourcePreloadFile::Write(Path("symlink-input/cache.resources-1-1"), {Record()}));
  Save(Path("symlink-target/bad.resources-1-2"), {1});
  std::error_code ec;
  fs::create_directory_symlink(Path("symlink-target"), Path("symlink-input/link"), ec);
  if (!ec) {
    CHECK_EQ(Tool("-o " + Quoted(Path("symlink-output.list")) + " " + Quoted(Path("symlink-input"))), 0);
  } else {
    std::printf("directory symlink fixture unavailable: OS denied symlink creation\n");
  }
}

NR_TEST_MAIN()

// Task 177: the list is now encoded on the render thread and written by the background file writer
// (temp file + rename). The bytes must be exactly what the old synchronous writer produced.
namespace {
bool OldWrite(const std::string& path, const std::vector<ResourcePreloadRecord>& records) {
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
}  // namespace

TEST(ResourcePreloadFile_BackgroundWriteIsByteIdenticalToTheOldWriter) {
  for (size_t count : {size_t(0), size_t(1), size_t(155), ResourcePreloadFile::kMaxRecords}) {
    std::vector<ResourcePreloadRecord> records;
    for (size_t i = 0; i < count; ++i) {
      auto r = Record(uint32_t(i + 1), i % 4 == 3 ? ResourcePreloadRecord::Index32 : ResourcePreloadRecord::Vertex);
      r.hash = 0x9E3779B97F4A7C15uLL * (i + 7);
      records.push_back(r);
    }
    const auto old_path = Path("old-" + std::to_string(count)), new_path = Path("new-" + std::to_string(count));
    CHECK(OldWrite(old_path, records));
    Bytes encoded;
    CHECK(ResourcePreloadFile::Encode(records, encoded));
    {
      BackgroundFileWriter writer;
      writer.set_delay_ms(20);
      writer.Replace(new_path, encoded);
    }  // the destructor flushes
    CHECK(Load(old_path) == Load(new_path));
    CHECK(Load(old_path) == encoded);
    std::vector<ResourcePreloadRecord> decoded, read;
    CHECK(ResourcePreloadFile::Decode(encoded, decoded));
    CHECK(ResourcePreloadFile::Read(new_path, read));
    CHECK_EQ(decoded.size(), count); CHECK_EQ(read.size(), count);
    for (size_t i = 0; i < std::min(count, read.size()); ++i)
      CHECK(read[i].SameResource(records[i]) && read[i].hash == records[i].hash);
    CHECK(!fs::exists(new_path + ".tmp"));
  }
  std::vector<ResourcePreloadRecord> too_many(ResourcePreloadFile::kMaxRecords + 1);
  Bytes encoded;
  CHECK(!ResourcePreloadFile::Encode(too_many, encoded));
}
