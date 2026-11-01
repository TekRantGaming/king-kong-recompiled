// The updater's file work (task 370), shared by the updater and its first start check (update.cpp), the unit
// tests and the packaging simulation (tests/update_files_tests.cpp, codex\simulate-update.ps1). Only
// std::filesystem: no network, no Windows API, no logging library, so it builds and is tested everywhere.
//
// kk-files.txt sits in the program folder (the packaging writes it, codex\make-manifest.ps1): every file of the
// release except itself, one per line "<sha256> <size> <path>" (path relative to the program folder, '/'
// separated, it may hold spaces), plus one "version <v>" line and '#' comment lines. kk-retired.txt has the same
// line format: files earlier releases shipped that this one does not (one path may be listed with several
// hashes, one per version). Both are plain files in the program folder's root, so every updater, old or new,
// copies them.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kk::update {

namespace fs = std::filesystem;

inline constexpr const char* kFilesList = "kk-files.txt";
inline constexpr const char* kRetiredList = "kk-retired.txt";
// Updaters up to Beta 13 copy only the program folder's own files, glyphs/ and launcher_art/; the release carries
// launcher/video and mods-builtin/ in this zip too and the first start unpacks it (update.cpp). A new updater
// installs those folders directly, so it never copies this zip, and the first start check does not ask for it.
inline constexpr const char* kExtraFilesZip = "update-extra-files.zip";
// The first start check hashes only files up to this size (cheap); larger ones are checked by their size.
inline constexpr uint64_t kCheapHashLimit = 4ull << 20;

struct FileEntry {
  std::string path;  // relative, '/' separated
  uint64_t size = 0;
  std::string sha256;  // 64 lowercase hex digits
};

struct Manifest {
  std::string version;  // "" when the list has no version line
  std::vector<FileEntry> files;
  // The entry for `path` (letter case ignored, as on Windows); nullptr when not listed.
  const FileEntry* Find(std::string_view path) const;
};

// Parses a kk-files.txt or kk-retired.txt; nullopt (and *error) when a line is broken, a hash or size is not
// valid, or a path is not safe (SafePath).
std::optional<Manifest> ParseManifest(std::string_view text, std::string* error = nullptr);
std::optional<Manifest> ReadManifest(const fs::path& file, std::string* error = nullptr);

// A relative path an update may name: not empty, no drive or leading '/', no '\\' or ':', no "." or ".." or
// empty part, no control characters.
bool SafePath(std::string_view path);

std::string Lower(std::string_view s);

// Paths an update never writes or deletes (letter case ignored): game/, logs/, the user data folders a player
// may keep beside the program (user/, userdata/, cache/, mods/, texture-packs/, content/, screenshots/,
// workshop/, save-backups/), king_kong.toml, fpe.log, and each `extra` folder (relative, '/' separated: the user
// and game data folders when they are inside the program folder).
class Protected {
 public:
  explicit Protected(std::vector<std::string> extra = {});
  bool operator()(std::string_view path) const;

 private:
  std::vector<std::string> extra_;
};

// The hash a SHA256SUMS.txt gives for `file_name` ("<hash>  <name>" or "<hash> *<name>"), lowercase; "" when
// the file has no line for it.
std::string SumFor(std::string_view sums, std::string_view file_name);

std::string Sha256Of(const void* data, size_t size);
std::string FileSha256(const fs::path& file);  // "" when it cannot be read
fs::path PathOf(const fs::path& root, std::string_view rel);  // root / rel (UTF-8)

using Log = std::function<void(const std::string&)>;

// The listed files whose copy under `root` is missing, of another size, or (files up to `hash_limit` bytes)
// of another hash. Protected paths and update-extra-files.zip are skipped.
std::vector<std::string> MissingOrWrong(const fs::path& root, const Manifest& m, const Protected& keep,
                                        uint64_t hash_limit);

enum class Outcome {
  kOk,
  kNoSource,  // a file to install is not in the source folder (nothing changed)
  kWrite,     // a file could not be written beside its target (nothing changed)
  kInUse,     // a file could not be swapped in, so every file was put back (nothing changed)
};
struct InstallResult {
  Outcome outcome = Outcome::kOk;
  std::string path;  // the file that failed
  std::string error;  // why, in Windows' own words (kWrite)
};

// Copies `paths` (relative, safe) from src_root over dst_root. 1. Every file is staged as <name>.new beside its
// target; a failure removes them all (nothing changed). 2. Each is swapped in: the old file becomes <name>.old
// (allowed for a running program or a loaded DLL), the .new takes its name; a failure puts every old file back
// and removes the new ones (nothing changed). 3. The .old files are removed where possible; RemoveOldCopies at
// the next start removes the rest (the running program's own).
InstallResult InstallFiles(const fs::path& src_root, const fs::path& dst_root, const std::vector<std::string>& paths,
                           const Log& log = {});

// Deletes the files `retired` lists that `current` does not, and only while unchanged (size and sha256 equal one
// listed for that path), then each of their folders that is left empty. Anything else is never touched.
// Returns the deleted paths.
std::vector<std::string> RetireFiles(const fs::path& root, const Manifest& retired, const Manifest& current,
                                     const Protected& keep, const Log& log = {});

// Removes <path>.old beside every listed file (what a swap left behind).
void RemoveOldCopies(const fs::path& root, const Manifest& m);

// The first start check's install (task 370 E): of `needed`, the files whose copy in src_root (an unpacked copy
// of the same release) matches `m` are installed with InstallFiles; the rest are `unavailable`.
struct SourceResult {
  std::vector<std::string> installed, unavailable;
  InstallResult install;
};
SourceResult InstallFromSource(const fs::path& src_root, const fs::path& dst_root, const Manifest& m,
                               const std::vector<std::string>& needed, const Log& log = {});

// Files under `root` that `m` does not list (the simulation's stale files); protected paths, kk-files.txt and
// *.old are left out.
std::vector<std::string> Unlisted(const fs::path& root, const Manifest& m, const Protected& keep);

}  // namespace kk::update
