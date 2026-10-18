#pragma once
// The AMD driver's own shader caches (task 228). Players on AMD saw a black window and a crash at every
// start until they emptied %LOCALAPPDATA%\AMD\VkCache, DxCache and DxcCache by hand. Emptying them is
// the owner's decision (it is not the game's data): the launcher offers it only when
// kk_amd_shader_cache_offer is on, only after a start that ended unexpectedly, and only with the
// player's consent. The folders are emptied at the next start, before this process opens the driver,
// and a file another program still has open is left alone (Windows refuses to remove it).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kk::amd_cache {

constexpr const char* kFolders[] = {"VkCache", "DxCache", "DxcCache"};

// <local app data>\AMD (empty when the folder is not there).
inline std::filesystem::path Root(const std::filesystem::path& local_app_data) {
  std::error_code ec;
  const auto root = local_app_data / "AMD";
  return std::filesystem::is_directory(root, ec) ? root : std::filesystem::path();
}

// The PC has an AMD driver shader cache (any of the three folders).
inline bool Present(const std::filesystem::path& local_app_data) {
  const auto root = Root(local_app_data);
  if (root.empty()) return false;
  std::error_code ec;
  for (const char* f : kFolders)
    if (std::filesystem::is_directory(root / f, ec)) return true;
  return false;
}

struct Result {
  uint64_t removed = 0, kept = 0;  // files; kept = in use by another program, or not removable
  uint64_t bytes = 0;              // removed
};

// Empties the three folders (their files and subfolders; the folders themselves stay). Nothing outside
// them is touched, and links are removed as links, never followed.
inline Result Clear(const std::filesystem::path& local_app_data) {
  Result r;
  const auto root = Root(local_app_data);
  if (root.empty()) return r;
  for (const char* f : kFolders) {
    const auto folder = root / f;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec) || std::filesystem::is_symlink(folder, ec)) continue;
    // List first, then remove: files, then the subfolders deepest first (each only when empty).
    std::vector<std::filesystem::path> files, dirs;
    for (auto it = std::filesystem::recursive_directory_iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      std::error_code e;
      (std::filesystem::is_directory(it->symlink_status(e)) ? dirs : files).push_back(it->path());
    }
    for (const auto& file : files) {
      std::error_code e;
      const uint64_t size = std::filesystem::is_regular_file(std::filesystem::symlink_status(file, e))
                                ? std::filesystem::file_size(file, e) : 0;
      if (std::filesystem::remove(file, e) && !e) {
        ++r.removed;
        r.bytes += size;
      } else {
        ++r.kept;
      }
    }
    for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) {
      std::error_code e;
      std::filesystem::remove(*d, e);  // only when empty
    }
  }
  return r;
}

}  // namespace kk::amd_cache
