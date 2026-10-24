#pragma once
// Task 229: every way the program can end leaves a word behind (logic and file format: exit_evidence_logic.h).
// - Last words: abort() (rex::FatalError, e.g. a lost graphics device, failed asserts), std::terminate (an
//   uncaught C++ exception), an invalid C runtime argument and a pure virtual call each log one line,
//   flush the log and write a crash report before the program ends exactly as it did before.
// - logs\session-<role>.txt: alive time every 30 s, the last frame, "clean" on a clean exit. Left "running"
//   by a run that ended without a word: the next start logs it, and what Windows' Application log recorded
//   about that process (crash code and module name, or "stopped responding").
// Windows only; the other platforms get empty functions.

#include <filesystem>

namespace kk::exit_evidence {

// Once per process, after the log is open. `role`: "launcher" or "game".
void Install(const std::filesystem::path& logs_dir, const char* role);
// Task 353: the last-words handlers alone (abort, std::terminate, invalid C runtime argument, pure virtual
// call), before the window and graphics device are made; Install does it too.
void InstallHandlers();
// The launcher started the game in this process.
void SetRole(const char* role);
// A frame was drawn (launcher or game). Any thread; cheap.
void NoteFrame();
// The program ends on purpose (quit, restart for the game, update).
void NoteCleanExit(const char* how);
// The crash handler wrote its report (the marker says so; the next start says nothing more).
void NoteCrashReport();

}  // namespace kk::exit_evidence
