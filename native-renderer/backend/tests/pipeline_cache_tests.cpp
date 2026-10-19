// The pipeline cache file: records, keys, the header check and recovery. No GPU, no game data.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "backend/pipeline_cache.h"
#include "backend/tests/check.h"

using namespace nr;
using namespace nr::test;

namespace {

std::string TempPath(const char* name) {
  // A file left by an earlier run that stopped half way would hold the records already.
  const std::string path =
      (std::filesystem::temp_directory_path() / (std::string("nr_pipeline_cache_test_") + name)).string();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return path;
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

// Runs the nr_pipelines tool; its exit code.
int RunTool(const std::string& args) {
  std::string command = std::string("\"") + NR_PIPELINES_TOOL + "\" " + args + " > \"" +
                        TempPath("tool_output.txt") + "\"";
#if defined(_WIN32)
  // cmd /c drops the first and the last quote of a line that starts with one: one more pair.
  command = "\"" + command + "\"";
#endif
  const int status = std::system(command.c_str());
#if defined(_WIN32)
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string Quoted(const std::string& path) { return "\"" + path + "\""; }

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
  changed([](PipelineRecord& r) { r.flags = kRecordScaled; }, "flags (scaled)");
  // The same microcode with another translation (render scale aware, other interpolators) is
  // another pipeline: version 2 keys ignored these, and a file with both kinds lost one of them.
  changed([](PipelineRecord& r) { r.vs_input = 0x77; }, "vs_input");
  changed([](PipelineRecord& r) { r.ps_input = 0x88; }, "ps_input");
}

TEST(PipelineCacheFile_TwoTranslationsOfOneShaderAreTwoRecords) {
  const std::string path = TempPath("translations");
  PipelineRecord plain = Sample();
  plain.ps_input = 0x1000;
  PipelineRecord aware = plain;
  aware.ps_input = 0x2000;
  aware.flags = kRecordScaled;
  {
    PipelineCacheFile file;
    CHECK(file.Open(path));
    CHECK(file.Append(plain));
    CHECK(file.Append(aware));
  }
  PipelineCacheFile file;
  CHECK(file.Open(path));
  CHECK_EQ(file.loaded().size(), size_t(2));
  std::remove(path.c_str());
}

TEST(PipelineCacheFile_Version2IsStartedAgain) {
  // Version 2 records could hold both render scales under one key: such a file is not trusted.
  const std::string path = TempPath("version2");
  {
    std::ofstream out(path, std::ios::binary);
    const uint32_t header[4] = {kPipelineCacheMagic, 2, sizeof(PipelineRecord), 0x01020304u};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    PipelineRecord r = Sample();
    out.write(reinterpret_cast<const char*>(&r), sizeof(r));
  }
  std::vector<PipelineRecord> records;
  std::string note;
  CHECK(!ReadPipelineList(path, records, &note));
  CHECK(records.empty());
  CHECK(!note.empty());
  PipelineCacheFile file;
  CHECK(file.Open(path));
  CHECK(file.loaded().empty());
  CHECK(!file.note().empty());
  std::remove(path.c_str());
}

TEST(PipelineList_ReadLeavesTheFileAlone) {
  const std::string path = TempPath("read_only");
  {
    PipelineCacheFile file;
    CHECK(file.Open(path));
    CHECK(file.Append(Sample(1)));
    CHECK(file.Append(Sample(2)));
  }
  const uintmax_t torn = 16 + 2 * sizeof(PipelineRecord) + 40;
  std::filesystem::resize_file(path, torn);
  std::vector<PipelineRecord> records;
  std::string note;
  size_t torn_bytes = 0;
  CHECK(ReadPipelineList(path, records, &note, &torn_bytes));
  CHECK(note.empty());
  CHECK_EQ(records.size(), size_t(2));
  CHECK_EQ(torn_bytes, size_t(40));
  CHECK_EQ(std::filesystem::file_size(path), torn);  // not repaired: a reader never writes
  // Absent: false, and no note (nothing was wrong).
  std::vector<PipelineRecord> none;
  CHECK(!ReadPipelineList(path + ".absent", none, &note));
  CHECK(note.empty());
  std::remove(path.c_str());
}

TEST(PipelineList_WriteThenRead) {
  const std::string path = TempPath("write");
  std::vector<PipelineRecord> records = {Sample(1), Sample(2), Sample(1)};  // written as given
  std::string error;
  CHECK(WritePipelineList(path, records, &error));
  CHECK(error.empty());
  CHECK(!std::filesystem::exists(path + ".tmp"));
  CHECK_EQ(std::filesystem::file_size(path), uintmax_t(16 + 3 * sizeof(PipelineRecord)));
  std::vector<PipelineRecord> back;
  CHECK(ReadPipelineList(path, back));
  CHECK(back == records);
  // The file is also a valid cache file.
  PipelineCacheFile file;
  CHECK(file.Open(path));
  CHECK_EQ(file.loaded().size(), size_t(2));
  file.Close();
  // An empty list is a header alone.
  CHECK(WritePipelineList(path, {}, &error));
  CHECK_EQ(std::filesystem::file_size(path), uintmax_t(16));
  std::remove(path.c_str());
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

TEST(PipelineListMerger_EveryPipelineOnceInTheOrderFirstSeen) {
  const std::string a = TempPath("merge_a"), b = TempPath("merge_b");
  PipelineRecord flipped = Sample(9);
  flipped.flags = kRecordFlipFrontFace;
  PipelineRecord scaled = Sample(2);
  scaled.flags = kRecordScaled;
  scaled.ps_input = 0x55;
  CHECK(WritePipelineList(a, {Sample(1), Sample(2), Sample(1)}));
  CHECK(WritePipelineList(b, {Sample(3), Sample(2), flipped, scaled}));
  PipelineListMerger merger;
  const PipelineListMerger::Input ia = merger.Add(a);
  CHECK(ia.read);
  CHECK_EQ(ia.records, size_t(3));
  CHECK_EQ(ia.added, size_t(2));
  CHECK_EQ(ia.duplicates, size_t(1));
  const PipelineListMerger::Input ib = merger.Add(b);
  CHECK(ib.read);
  CHECK_EQ(ib.added, size_t(2));  // Sample(3) and the scaled one
  CHECK_EQ(ib.duplicates, size_t(1));
  CHECK_EQ(ib.debug, size_t(1));
  const std::vector<PipelineRecord> want = {Sample(1), Sample(2), Sample(3), scaled};
  CHECK(merger.records() == want);

  PipelineListMerger keep(true);
  keep.Add(a);
  keep.Add(b);
  CHECK_EQ(keep.records().size(), size_t(5));

  const PipelineListMerger::Input absent = merger.Add(a + ".absent");
  CHECK(!absent.read);
  CHECK(absent.note == "absent");
  std::remove(a.c_str());
  std::remove(b.c_str());
}

// Task 276: the level tag lives in reserved[0], outside the key; files before it read as untagged.
TEST(PipelineRecord_LevelTagIsNotPartOfThePipeline) {
  PipelineRecord r = Sample(1);
  CHECK(!RecordLevel(r));
  const uint64_t key = PipelineKey(r);
  SetRecordLevel(r, 0);  // level 0 is a level, told apart from "not known"
  uint32_t level = 99;
  CHECK(RecordLevel(r, &level));
  CHECK_EQ(level, 0u);
  SetRecordLevel(r, 303);
  CHECK(RecordLevel(r, &level));
  CHECK_EQ(level, 303u);
  CHECK_EQ(PipelineKey(r), key);
  ClearRecordLevel(r);
  CHECK(!RecordLevel(r));
  // A motion record is drawn in its draw's level.
  SetRecordLevel(r, 7);
  CHECK(RecordLevel(MotionPipelineRecord(r, 1), &level));
  CHECK_EQ(level, 7u);
}

TEST(PipelineList_LevelTagsSurviveWriteReadAndTheCacheFile) {
  const std::string list = TempPath("tags_list"), cache = TempPath("tags_cache");
  PipelineRecord a = Sample(1), b = Sample(2);
  SetRecordLevel(b, 305);
  CHECK(WritePipelineList(list, {a, b}));
  std::vector<PipelineRecord> read;
  CHECK(ReadPipelineList(list, read));
  CHECK_EQ(read.size(), size_t(2));
  if (read.size() == 2) {
    CHECK(!RecordLevel(read[0]));
    uint32_t level = 0;
    CHECK(RecordLevel(read[1], &level));
    CHECK_EQ(level, 305u);
  }
  {
    PipelineCacheFile file;
    CHECK(file.Open(cache));
    CHECK(file.Append(b));
    PipelineRecord again = b;
    SetRecordLevel(again, 9);  // the same pipeline in another level: already known, the first level stays
    CHECK(!file.Append(again));
    CHECK(file.Known(Sample(2)));
  }
  PipelineCacheFile file;
  CHECK(file.Open(cache));
  CHECK_EQ(file.loaded().size(), size_t(1));
  if (file.loaded().size() == 1) CHECK(file.loaded()[0] == b);
  file.Close();
  std::remove(list.c_str());
  std::remove(cache.c_str());
}

TEST(PipelineListMerger_LevelVotes) {
  const auto tagged = [](uint64_t vs, uint32_t level) {
    PipelineRecord r = Sample(vs);
    SetRecordLevel(r, level);
    return r;
  };
  const auto level_of = [](const PipelineRecord& r) {
    uint32_t level = 0;
    return RecordLevel(r, &level) ? int64_t(level) : int64_t(-1);
  };
  PipelineListMerger merger;
  // Sample(1): untagged first, then 5, 6, 6 -> 6 (most votes). Sample(2): 3 then 4 -> 3 (a tie: the earliest).
  // Sample(3): never tagged. Sample(4): 8 alone.
  merger.AddRecords({Sample(1), tagged(2, 3), Sample(3)});
  merger.AddRecords({tagged(1, 5), tagged(2, 4), Sample(3), tagged(4, 8)});
  merger.AddRecords({tagged(1, 6)});
  CHECK_EQ(merger.records().size(), size_t(4));
  if (merger.records().size() == 4) {
    CHECK_EQ(level_of(merger.records()[0]), int64_t(5));  // 5 and 6 tie so far: the first voted for
    CHECK_EQ(level_of(merger.records()[1]), int64_t(3));
    CHECK_EQ(level_of(merger.records()[2]), int64_t(-1));
    CHECK_EQ(level_of(merger.records()[3]), int64_t(8));
  }
  merger.AddRecords({tagged(1, 6)});
  CHECK_EQ(level_of(merger.records()[0]), int64_t(6));  // now 6 has more votes
  // The order and the keys are those of an untagged merge.
  CHECK_EQ(PipelineKey(merger.records()[0]), PipelineKey(Sample(1)));
  CHECK_EQ(PipelineKey(merger.records()[3]), PipelineKey(Sample(4)));

  // tags_only: the first input's records, the others only vote.
  PipelineListMerger tags(false, true);
  CHECK_EQ(tags.AddRecords({Sample(1), Sample(2)}), size_t(2));
  CHECK_EQ(tags.AddRecords({tagged(2, 11), tagged(9, 12)}), size_t(0));
  CHECK_EQ(tags.records().size(), size_t(2));
  if (tags.records().size() == 2) {
    CHECK_EQ(level_of(tags.records()[0]), int64_t(-1));
    CHECK_EQ(level_of(tags.records()[1]), int64_t(11));
  }
}

TEST(PipelinesTool_TagsOnly) {
  const std::string list = TempPath("tool_tag_list"), run = TempPath("tool_tag_run"), out = TempPath("tool_tag_out");
  CHECK(WritePipelineList(list, {Sample(1), Sample(2)}));
  PipelineRecord r2 = Sample(2), r3 = Sample(3);
  SetRecordLevel(r2, 305);
  SetRecordLevel(r3, 306);
  CHECK(WritePipelineList(run, {r3, r2}));
  CHECK_EQ(RunTool("merge --tags-only -o " + Quoted(out) + " " + Quoted(list) + " " + Quoted(run)), 0);
  std::vector<PipelineRecord> merged;
  CHECK(ReadPipelineList(out, merged));
  CHECK_EQ(merged.size(), size_t(2));
  if (merged.size() == 2) {
    CHECK(!RecordLevel(merged[0]));
    CHECK(merged[1] == r2);
  }
  CHECK_EQ(RunTool("info " + Quoted(out)), 0);
  // Without --tags-only the recording's new pipeline is added too.
  CHECK_EQ(RunTool("merge -o " + Quoted(out) + " " + Quoted(list) + " " + Quoted(run)), 0);
  merged.clear();
  CHECK(ReadPipelineList(out, merged));
  CHECK_EQ(merged.size(), size_t(3));
  std::remove(list.c_str());
  std::remove(run.c_str());
  std::remove(out.c_str());
}

TEST(PipelinesTool_MergeAndInfo) {
  const std::string a = TempPath("tool_a"), b = TempPath("tool_b"), out = TempPath("tool_out.list");
  CHECK(WritePipelineList(a, {Sample(1), Sample(2)}));
  CHECK(WritePipelineList(b, {Sample(2), Sample(3)}));
  CHECK_EQ(RunTool("merge -o " + Quoted(out) + " " + Quoted(a) + " " + Quoted(b)), 0);
  std::vector<PipelineRecord> merged;
  CHECK(ReadPipelineList(out, merged));
  const std::vector<PipelineRecord> want = {Sample(1), Sample(2), Sample(3)};
  CHECK(merged == want);
  CHECK_EQ(RunTool("info " + Quoted(out)), 0);
  // The output may be one of the inputs (adding a run to a list).
  CHECK(WritePipelineList(b, {Sample(4)}));
  CHECK_EQ(RunTool("merge -o " + Quoted(out) + " " + Quoted(out) + " " + Quoted(b)), 0);
  merged.clear();
  CHECK(ReadPipelineList(out, merged));
  CHECK_EQ(merged.size(), size_t(4));
  // An input that is not there, or of another version: nothing is written.
  std::remove(out.c_str());
  CHECK_EQ(RunTool("merge -o " + Quoted(out) + " " + Quoted(a) + " " + Quoted(a + ".absent")), 2);
  CHECK(!std::filesystem::exists(out));
  {
    std::ofstream old(b, std::ios::binary | std::ios::trunc);
    const uint32_t header[4] = {kPipelineCacheMagic, 2, sizeof(PipelineRecord), 0x01020304u};
    old.write(reinterpret_cast<const char*>(header), sizeof(header));
  }
  CHECK_EQ(RunTool("merge -o " + Quoted(out) + " " + Quoted(a) + " " + Quoted(b)), 2);
  CHECK(!std::filesystem::exists(out));
  CHECK_EQ(RunTool("merge " + Quoted(a)), 1);  // no output named
  CHECK_EQ(RunTool("info " + Quoted(b)), 2);
  std::remove(a.c_str());
  std::remove(b.c_str());
}

NR_TEST_MAIN()
