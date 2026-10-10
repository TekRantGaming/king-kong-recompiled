// The pipeline cache file: records, keys, the header check and recovery. No GPU, no game data.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "backend/pipeline_cache.h"
#include "backend/tests/check.h"

using namespace nr;
using namespace nr::test;

namespace {

std::string TempPath(const char* name) {
  return (std::filesystem::temp_directory_path() / (std::string("nr_pipeline_cache_test_") + name)).string();
}

PipelineRecord Sample(uint64_t vs = 0x1111, uint64_t ps = 0x2222) {
  PipelineRecord r;
  r.vs_hash = vs;
  r.ps_hash = ps;
  r.topology = 3;
  r.color_count = 2;
  r.color_format[0] = 11;
  r.color_format[1] = 13;
  r.color_control[0] = 0x00010001;
  r.color_control[1] = 0x00050407;
  r.color_mask[0] = 0xF;
  r.color_mask[1] = 0x7;
  r.color_index[0] = 0;
  r.color_index[1] = 2;
  r.depth_format = 40;
  r.depth_control = 0x2 | 0x4 | (1u << 4);
  r.ref_mask = 0xFFFF00;
  r.mode_control = 0x6;
  r.clip_control = 0;
  return r;
}

}  // namespace

TEST(PipelineKey_ChangesWithEveryField) {
  const PipelineRecord base = Sample();
  const uint64_t key = PipelineKey(base);
  CHECK_EQ(key, PipelineKey(Sample()));  // stable
  auto changed = [&](auto edit, const char* what) {
    PipelineRecord r = base;
    edit(r);
    if (PipelineKey(r) == key) Fail(__FILE__, __LINE__, std::string("key ignores ") + what);
  };
  changed([](PipelineRecord& r) { r.vs_hash ^= 1; }, "vs_hash");
  changed([](PipelineRecord& r) { r.ps_hash = 0; }, "ps_hash");
  changed([](PipelineRecord& r) { r.topology = 4; }, "topology");
  changed([](PipelineRecord& r) { r.color_count = 1; }, "color_count");
  changed([](PipelineRecord& r) { r.color_format[1] = 12; }, "color_format");
  changed([](PipelineRecord& r) { r.color_control[1] ^= 0x100; }, "color_control");
  changed([](PipelineRecord& r) { r.color_mask[0] = 0x3; }, "color_mask");
  changed([](PipelineRecord& r) { r.color_index[1] = 1; }, "color_index");
  changed([](PipelineRecord& r) { r.depth_format = 0; }, "depth_format");
  changed([](PipelineRecord& r) { r.depth_control ^= 0x20; }, "depth_control");
  changed([](PipelineRecord& r) { r.ref_mask ^= 0x100; }, "ref_mask");
  changed([](PipelineRecord& r) { r.mode_control ^= 0x1; }, "mode_control");
  changed([](PipelineRecord& r) { r.clip_control = 1u << 16; }, "clip_control");
  changed([](PipelineRecord& r) { r.poly_scale = 0x3F800000; }, "poly_scale");
  changed([](PipelineRecord& r) { r.poly_offset = 0x3F800000; }, "poly_offset");
  changed([](PipelineRecord& r) { r.flags = 1; }, "flags");
}

TEST(PipelineCacheFile_RoundTripAndDeduplication) {
  const std::string path = TempPath("roundtrip");
  std::remove(path.c_str());
  {
    PipelineCacheFile file;
    CHECK(file.Open(path));
    CHECK(file.loaded().empty());
    CHECK(file.Append(Sample(1)));
    CHECK(file.Append(Sample(2)));
    CHECK(!file.Append(Sample(1)));  // already known
    CHECK(file.Known(Sample(2)));
    CHECK(!file.Known(Sample(3)));
    CHECK_EQ(file.appended(), size_t(2));
  }
  {
    PipelineCacheFile file;
    CHECK(file.Open(path));
    CHECK_EQ(file.loaded().size(), size_t(2));
    if (file.loaded().size() == 2) {
      CHECK(file.loaded()[0] == Sample(1));
      CHECK(file.loaded()[1] == Sample(2));
    }
    CHECK(!file.Append(Sample(2)));  // known from the file
    CHECK(file.Append(Sample(3)));
  }
  PipelineCacheFile file;
  CHECK(file.Open(path));
  CHECK_EQ(file.loaded().size(), size_t(3));
  CHECK_EQ(std::filesystem::file_size(path), uintmax_t(16 + 3 * sizeof(PipelineRecord)));
  std::remove(path.c_str());
}

TEST(PipelineCacheFile_CreatesTheDirectory) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "nr_pipeline_cache_test_dir" / "a" / "b";
  std::filesystem::remove_all(dir.parent_path().parent_path());
  PipelineCacheFile file;
  CHECK(file.Open((dir / "x.pipelines").string()));
  CHECK(file.Append(Sample()));
  file.Close();
  CHECK(std::filesystem::exists(dir / "x.pipelines"));
  std::filesystem::remove_all(dir.parent_path().parent_path());
}

TEST(PipelineCacheFile_OtherVersionIsStartedAgain) {
  const std::string path = TempPath("version");
  {
    std::ofstream out(path, std::ios::binary);
    const uint32_t header[4] = {kPipelineCacheMagic, kPipelineCacheVersion + 1, sizeof(PipelineRecord), 0x01020304u};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    PipelineRecord r = Sample();
    out.write(reinterpret_cast<const char*>(&r), sizeof(r));
  }
  PipelineCacheFile file;
  CHECK(file.Open(path));
  CHECK(file.loaded().empty());
  CHECK(!file.note().empty());
  CHECK(file.Append(Sample()));
  file.Close();
  PipelineCacheFile again;
  CHECK(again.Open(path));
  CHECK_EQ(again.loaded().size(), size_t(1));
  CHECK(again.note().empty());
  std::remove(path.c_str());
}

TEST(PipelineCacheFile_GarbageAndEmptyFilesAreStartedAgain) {
  const std::string path = TempPath("garbage");
  {
    std::ofstream out(path, std::ios::binary);
    out << "this is not a pipeline cache";
  }
  PipelineCacheFile file;
  CHECK(file.Open(path));
  CHECK(file.loaded().empty());
  CHECK(!file.note().empty());
  file.Close();
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);  // empty
  }
  PipelineCacheFile empty;
  CHECK(empty.Open(path));
  CHECK(empty.loaded().empty());
  CHECK(empty.Append(Sample()));
  std::remove(path.c_str());
}

TEST(PipelineCacheFile_ATornLastRecordIsDropped) {
  const std::string path = TempPath("torn");
  {
    PipelineCacheFile file;
    CHECK(file.Open(path));
    CHECK(file.Append(Sample(1)));
    CHECK(file.Append(Sample(2)));
  }
  // The run was killed in the middle of writing a third record.
  std::filesystem::resize_file(path, 16 + 2 * sizeof(PipelineRecord) + 40);
  PipelineCacheFile file;
  CHECK(file.Open(path));
  CHECK_EQ(file.loaded().size(), size_t(2));
  CHECK(file.Append(Sample(3)));  // appended after the last whole record, not after the stub
  file.Close();
  CHECK_EQ(std::filesystem::file_size(path), uintmax_t(16 + 3 * sizeof(PipelineRecord)));
  PipelineCacheFile again;
  CHECK(again.Open(path));
  CHECK_EQ(again.loaded().size(), size_t(3));
  std::remove(path.c_str());
}

NR_TEST_MAIN()
