// Shader packs: shaders other players' games have already prepared, published
// on the GitHub release "shader-packs". Merging one into the shader cache lets
// the runtime prepare them all at startup, so the game never pauses for a new
// effect the first time it appears.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace kk {

// Release that holds the pack: shader-pack.txt ("version=N" plus one file name
// per line) and the files it lists.
constexpr const char* kShaderPackUrl =
    "https://github.com/TekRantGaming/king-kong-recompiled/releases/download/shader-packs/";

struct ShaderPackStatus {
  std::atomic<bool> busy{false};
  std::atomic<bool> done{false};
  std::atomic<bool> failed{false};
  std::atomic<uint64_t> bytes{0}, total{0};  // download progress
  std::string message;                       // read once done
};

// Installed pack version (0 if none), from <cache>/shaders/shader-pack.txt.
int InstalledShaderPackVersion(const std::filesystem::path& cache_dir);

// Downloads the pack and merges it into <cache>/shaders/shareable, keeping
// everything already there. Runs on the calling thread; reports in `status`.
void DownloadAndInstallShaderPack(const std::filesystem::path& cache_dir, ShaderPackStatus& status);

// Removes damaged records from the shader and pipeline storage files in
// <cache>/shaders (a write the game never finished). The runtime stops reading
// a file at the first damaged record, so one would hide all the rest. Returns
// how many were removed. Call before the runtime opens the files.
int RepairShaderStorage(const std::filesystem::path& cache_dir);

// Merges a shader (.xsh) or pipeline (.xpso) storage file into another,
// adding only records it doesn't have. Returns records added, or -1 if the
// files are from a different runtime version (or unreadable).
int MergeShaderStorageFile(const std::filesystem::path& from, const std::filesystem::path& into);

}  // namespace kk
