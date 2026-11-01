// The player's data (saves, cache, downloads, mods, crash reports) lives in <program folder>\userdata, so the whole
// King Kong folder is portable and nothing is written to Documents. Windows only: Linux keeps the SDK's
// ~/.local/share/king_kong, which was never Documents.
//
// Earlier versions kept it in Documents\king_kong. Resolve moves that folder here once, at the first start:
//  1. moving-from-documents.txt is written first, so a move cut short carries on at the next start, and while it is
//     there the old folder is the true copy (the game runs on it when the move fails).
//  2. Each top level item is renamed when it can be (same drive, not a OneDrive file that may only be in the
//     cloud), otherwise its files are copied and checked. A failure puts renamed items back.
//  3. Settings and content\*\installed.txt hold full paths into the old folder: these are rewritten.
//  4. The marker becomes cleaning-documents.txt: only now are the copied files removed from the old folder, and only
//     those still equal to their copy. What could not be removed is tried again at the next start.
//  5. The marker becomes documents-done.txt. From then on Documents is never read, so a portable copy on another
//     PC leaves that PC's Documents alone. A new install with nothing to move writes the same marker.
//
// A program folder that cannot be written (Program Files) keeps the old folder. Only std::filesystem, so it is
// tested on real files in a temp folder (tests/user_folder_tests.cpp).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace kk::user_folder {

namespace fs = std::filesystem;

inline constexpr const char* kFolderName = "userdata";
inline constexpr const char* kMoving = "moving-from-documents.txt";
inline constexpr const char* kCleaning = "cleaning-documents.txt";
inline constexpr const char* kDone = "documents-done.txt";

struct Options {
  fs::path old_root;           // Documents\king_kong; empty: nothing to move
  fs::path new_root;           // <program folder>\userdata
  fs::path config;             // king_kong.toml, rewritten when it names the old folder
  bool allow_rename = true;    // false: every file is copied (tests; set by Resolve for OneDrive files)
  std::function<void(uint64_t done, uint64_t total)> progress;  // bytes copied; never called for renames
  std::function<void(const std::string&)> log;
  // Windows: whether a file may be only in the cloud (OneDrive). Unset: never.
  std::function<bool(const fs::path&)> cloud_file;
  // Free bytes at a folder. Unset: std::filesystem::space. A test sets it to fake a full disk.
  std::function<uint64_t(const fs::path&)> free_space;
};

// True when the first start's work is over and Resolve would do nothing but return new_root.
bool Done(const fs::path& new_root);

// Does the steps above and returns the folder this run uses: new_root, or old_root when the program folder cannot
// be written or the move failed (it is tried again at the next start).
fs::path Resolve(const Options& options);

// `text` with every spelling of old_root (a\b, a\\b as in TOML, a/b) replaced by new_root spelled the same way.
std::string ReplaceRoot(const std::string& text, const std::string& old_root, const std::string& new_root);

}  // namespace kk::user_folder
