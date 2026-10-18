// Beta 7 uploads (owner, 17 Oct 2026; codex/state/telemetry-b7.md), only while the Online player count
// switch (kk_online_count) is on, to the same server as the player count:
//   on quit:            the pipeline record file (gzip) and one performance summary for the session
//                       (per chapter: time, average FPS, 1% low, worst frame, frames over 50 ms, GPU,
//                       game-thread and pipeline-wait times, and what the 3 slowest frames over 250 ms were
//                       doing; the graphics card, driver, CPU, RAM, display and main graphics settings; no
//                       names, no paths);
//   on the next start:  a crash or hang report from this version (its text, gzip, user names and personal
//                       folders removed), then remembered as sent.
// Network work runs on its own threads. Quitting waits for it at most about 2 s; what is not done by then
// is kept and tried next time. Developer builds: KK_DEV_TELEMETRY_URL=http://127.0.0.1:<port> sends to a
// local server instead (KK_DEV_ONLINE_URL too, when that is set), and KK_DEV_TELEMETRY_NOW=<seconds> runs
// the quit uploads that long after the first frame (and sends the pipeline file even if unchanged).

#pragma once

#include <filesystem>

namespace kk::telemetry {

// Where things are: user data (crashes/, telemetry/), the logs folder (hang reports) and the cache root.
void Configure(const std::filesystem::path& user_root, const std::filesystem::path& logs_dir,
               const std::filesystem::path& cache_root);
// Once per process, a few seconds later on a worker: last session's unsent summary and crash/hang reports.
void StartupUploads();
// This process runs the game: its quit sends the pipeline file and the summary.
void SetGameRunning();
// Game thread, every guest frame (the time since the previous one), with the game's swap call and the frame
// rate cap's wait inside it (ms, negative when not known). Frames of 250 ms and more get the worst-frame
// detail (task 211, telemetry_logic.h WorstFrame); other frames only refresh a few running totals.
void NoteFrame(double frame_ms, double swap_ms = -1, double cap_wait_ms = -1);
// Game thread, every frame of the level script: the chapter-select entry being played (-1: menus), and
// whether a level change was carried out (the next frames hold the load and are left out).
void NoteChapter(int entry, bool loading);
// The game is quitting: starts the uploads (returns at once). Any thread; once.
void BeginQuitUploads();
// Waits for them, at most about 2 s after BeginQuitUploads. Writes no log.
void WaitQuitUploads();

}  // namespace kk::telemetry
