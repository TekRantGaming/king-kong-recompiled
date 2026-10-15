// Build kkresources.list from local .resources-<level>-<start> recordings and
// previous bundles. Nothing is written unless every selected input is valid.
// Only descriptions and content hashes are copied, never game resource bytes.
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "backend/resource_preload_bundle.h"

namespace {
namespace fs = std::filesystem;
using Key = std::pair<uint32_t, uint32_t>;

int Usage() {
  std::fprintf(stderr, "usage: nr_resources -o <kkresources.list> <recording-or-bundle-or-directory>...\n"
                       "       nr_resources merge -o <kkresources.list> <input>...\n");
  return 1;
}

bool Number(std::string_view text, uint32_t& value) {
  if (text.empty()) return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool ChapterKey(const fs::path& path, Key& key) {
  const std::string name = path.filename().string();
  constexpr std::string_view marker = ".resources-";
  const size_t offset = name.rfind(marker);
  if (offset == std::string::npos) return false;
  const std::string_view suffix(name.data() + offset + marker.size(), name.size() - offset - marker.size());
  const size_t split = suffix.find('-');
  return split != std::string_view::npos && Number(suffix.substr(0, split), key.first) &&
         Number(suffix.substr(split + 1), key.second);
}

bool Candidate(const fs::path& path) {
  Key key;
  return ChapterKey(path, key) || path.filename() == "kkresources.list";
}

bool SameFile(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (fs::equivalent(a, b, ec)) return true;  // Includes aliases, hard links and symlinks.
  ec.clear();
  const auto left = fs::weakly_canonical(a, ec);
  if (ec) return false;
  const auto right = fs::weakly_canonical(b, ec);
  return !ec && left == right;
}

int Merge(int argc, char** argv) {
  fs::path output;
  std::vector<fs::path> inputs;
  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-o") {
      if (!output.empty() || ++i == argc) return Usage();
      output = argv[i];
    } else if (arg.empty() || arg[0] == '-') return Usage();
    else inputs.emplace_back(arg);
  }
  if (output.empty() || inputs.empty()) return Usage();
  std::vector<fs::path> files;
  for (const auto& input : inputs) {
    std::error_code ec;
    if (fs::is_regular_file(input, ec)) {
      files.push_back(input);
      continue;
    }
    if (ec || !fs::is_directory(input, ec) || fs::is_symlink(fs::symlink_status(input, ec))) {
      std::fprintf(stderr, "%s: input is absent, unreadable, or not a regular file/directory\n", input.string().c_str());
      return 2;
    }
    const size_t before = files.size();
    // The default iterator does not follow directory symlinks; skip every
    // symlink entry as well, including file aliases and directory junctions.
    fs::recursive_directory_iterator it(input, fs::directory_options::none, ec), end;
    if (ec) {
      std::fprintf(stderr, "%s: directory could not be read\n", input.string().c_str());
      return 2;
    }
    for (; it != end; it.increment(ec)) {
      if (ec) break;
      const auto status = it->symlink_status(ec);
      if (ec) break;
      if (fs::is_symlink(status)) { it.disable_recursion_pending(); continue; }
      if (fs::is_regular_file(status) && Candidate(it->path())) files.push_back(it->path());
    }
    if (ec || files.size() == before) {
      std::fprintf(stderr, "%s: directory unreadable or contains no resource lists\n", input.string().c_str());
      return 2;
    }
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  // Check before any read/write. In particular, an output discovered by a
  // recursive directory scan must never be truncated in place.
  for (const auto& file : files) if (SameFile(output, file)) {
    std::fprintf(stderr, "%s: output must differ from every input\n", output.string().c_str());
    return 2;
  }

  using Records = std::set<nr::ResourcePreloadRecord, decltype(&nr::ResourcePreloadBundle::RecordLess)>;
  std::map<Key, Records> merged;
  size_t total = 0;
  for (const auto& file : files) {
    std::vector<nr::ResourcePreloadChapter> chapters;
    if (!nr::ResourcePreloadBundle::Read(file.string(), chapters)) {
      nr::ResourcePreloadChapter local;
      Key key;
      if (!ChapterKey(file, key) || !nr::ResourcePreloadFile::Read(file.string(), local.records)) {
        std::fprintf(stderr, "%s: invalid or incompatible resource list\n", file.string().c_str());
        return 2;
      }
      local.level = key.first; local.start = key.second;
      chapters.push_back(std::move(local));
    }
    for (const auto& chapter : chapters) {
      auto [entry, added] = merged.try_emplace({chapter.level, chapter.start}, &nr::ResourcePreloadBundle::RecordLess);
      (void)added;
      if (merged.size() > nr::ResourcePreloadBundle::kMaxChapters) {
        std::fprintf(stderr, "nothing written: chapter limit exceeded\n");
        return 2;
      }
      for (const auto& record : chapter.records) {
        if (entry->second.insert(record).second) ++total;
        if (entry->second.size() > nr::ResourcePreloadBundle::kMaxRecords ||
            total > nr::ResourcePreloadBundle::kMaxTotalRecords) {
          std::fprintf(stderr, "nothing written: resource count limit exceeded\n");
          return 2;
        }
      }
    }
  }
  std::vector<nr::ResourcePreloadChapter> chapters;
  for (const auto& [key, records] : merged)
    chapters.push_back({key.first, key.second, {records.begin(), records.end()}});
  if (!nr::ResourcePreloadBundle::Write(output.string(), chapters)) {
    std::fprintf(stderr, "%s: resource bundle could not be written\n", output.string().c_str());
    return 3;
  }
  std::printf("%s: %zu chapters, %zu descriptions\n", output.string().c_str(), chapters.size(), total);
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "merge") return Merge(argc - 2, argv + 2);
  return Merge(argc - 1, argv + 1);
}
