#pragma once

#include <filesystem>

namespace kk {

// On a crash, writes crash-<time>.dmp and crash-<time>.txt into `logs_dir`.
// Call after the runtime has set up its own handlers; they still run after.
void InstallCrashDumps(const std::filesystem::path& logs_dir);

}  // namespace kk
