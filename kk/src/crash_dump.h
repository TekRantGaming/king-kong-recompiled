#pragma once

#include <cstdint>

#include <filesystem>

namespace kk {
// A concurrent frame can be newer than the watchdog's sampled clock.
constexpr uint64_t FrameStallMilliseconds(uint64_t now, uint64_t last) {
  return now >= last ? now - last : 0;
}

// On a crash, writes crash-<time>.dmp and redacted diagnostics into user_root/crashes.
// Call after the runtime has set up its own handlers; they still run after.
// Also starts a watchdog that writes hang-<time>.dmp and .txt there when the
// game draws no frame for 20 seconds.
void InstallCrashDumps(const std::filesystem::path& logs_dir, const std::filesystem::path& user_root);

// Called for every guest frame; feeds the hang watchdog.
void NoteGuestFrame();

// Task 229: a crash report for an end that raises no exception (abort(), std::terminate, an invalid C
// runtime argument), with `reason` in the .txt. Once per process (shares the crash handler's guard).
void WriteCrashReportNow(const char* reason);

// Task 270: the graphics API this session renders with (1 Vulkan, 2 Direct3D 12, 0 not known yet), written
// into crash and hang reports as "Graphics API: ..." so their upload names this session's API, not the API
// of the later session that sends them. Any thread.
void SetCrashReportApi(int api);

}  // namespace kk
