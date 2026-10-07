#pragma once

#include <filesystem>

namespace kk {

// On a crash, writes crash-<time>.dmp and crash-<time>.txt into `logs_dir`.
// Call after the runtime has set up its own handlers; they still run after.
// Also starts a watchdog that writes hang-<time>.dmp and .txt there when the
// game draws no frame for 20 seconds.
void InstallCrashDumps(const std::filesystem::path& logs_dir);

// Called for every guest frame; feeds the hang watchdog.
void NoteGuestFrame();

}  // namespace kk
