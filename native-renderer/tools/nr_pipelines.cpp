// nr_pipelines: the native renderer's pipeline files (docs/backend.md, "Pipeline lists").
//
//   nr_pipelines merge [--keep-debug] -o <out> <file>...
//       Merges the pipeline files of several runs (each <cache root>/native-pipelines/555307D3.pipelines)
//       into one list, every pipeline once, in the order first seen. That list, shipped beside the
//       game as kkpipelines.list, lets a fresh install make every pipeline at start
//       (--native_pipeline_list). Records of runs with --native_flip_front_face are left out unless
//       --keep-debug. Nothing is written when an input is absent or of another version.
//   nr_pipelines info <file>...
//       What a pipeline file holds.
//
// Exit code: 0 done, 1 usage, 2 an input could not be read, 3 the output could not be written.

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "backend/pipeline_cache.h"

namespace {

int Usage() {
  std::fprintf(stderr,
               "usage: nr_pipelines merge [--keep-debug] -o <out> <file>...\n"
               "       nr_pipelines info <file>...\n");
  return 1;
}

int Merge(int argc, char** argv) {
  std::string out;
  bool keep_debug = false;
  std::vector<std::string> inputs;
  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) {
      out = argv[++i];
    } else if (a == "--keep-debug") {
      keep_debug = true;
    } else if (!a.empty() && a[0] == '-') {
      return Usage();
    } else {
      inputs.push_back(a);
    }
  }
  if (out.empty() || inputs.empty()) return Usage();
  nr::PipelineListMerger merger(keep_debug);
  bool all_read = true;
  for (const std::string& path : inputs) {
    const nr::PipelineListMerger::Input in = merger.Add(path);
    if (!in.read) {
      std::fprintf(stderr, "%s: not read: %s\n", path.c_str(), in.note.c_str());
      all_read = false;
      continue;
    }
    std::printf("%s: %zu records, %zu new, %zu already listed", path.c_str(), in.records, in.added, in.duplicates);
    if (in.debug) std::printf(", %zu with flipped faces left out", in.debug);
    if (in.torn_bytes) std::printf(", an incomplete last record (%zu bytes) left out", in.torn_bytes);
    std::printf("\n");
  }
  if (!all_read) {
    std::fprintf(stderr, "nothing written: every input must be a pipeline file of version %u\n",
                 nr::kPipelineCacheVersion);
    return 2;
  }
  std::string error;
  if (!nr::WritePipelineList(out, merger.records(), &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 3;
  }
  std::printf("%s: %zu pipelines\n", out.c_str(), merger.records().size());
  return 0;
}

int Info(int argc, char** argv) {
  if (argc == 0) return Usage();
  int result = 0;
  for (int i = 0; i < argc; ++i) {
    std::vector<nr::PipelineRecord> records;
    std::string note;
    size_t torn = 0;
    if (!nr::ReadPipelineList(argv[i], records, &note, &torn)) {
      std::fprintf(stderr, "%s: not read: %s\n", argv[i], note.empty() ? "absent" : note.c_str());
      result = 2;
      continue;
    }
    nr::PipelineListMerger unique(true);
    unique.AddRecords(records);
    size_t scaled = 0, flipped = 0, depth_only = 0;
    std::set<std::pair<uint64_t, uint64_t>> vs, ps;
    for (const nr::PipelineRecord& r : unique.records()) {
      if (r.flags & nr::kRecordScaled) ++scaled;
      if (r.flags & nr::kRecordFlipFrontFace) ++flipped;
      if (!r.ps_hash) ++depth_only;
      vs.insert({r.vs_hash, r.vs_input});
      if (r.ps_hash) ps.insert({r.ps_hash, r.ps_input});
    }
    std::printf("%s: version %u, %zu records, %zu pipelines (%zu at 1:1, %zu at a render scale, %zu with flipped "
                "faces, %zu without a pixel shader), %zu vertex and %zu pixel shader translations",
                argv[i], nr::kPipelineCacheVersion, records.size(), unique.records().size(),
                unique.records().size() - scaled, scaled, flipped, depth_only, vs.size(), ps.size());
    if (torn) std::printf(", an incomplete last record (%zu bytes)", torn);
    std::printf("\n");
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return Usage();
  if (std::strcmp(argv[1], "merge") == 0) return Merge(argc - 2, argv + 2);
  if (std::strcmp(argv[1], "info") == 0) return Info(argc - 2, argv + 2);
  return Usage();
}
