#pragma once
#include <filesystem>
#include <string>
#include <cstdio>

namespace kk {
// Local, redacted text only. Dumps can contain process memory and stay local.
void InitDiagnostics();
void RefreshDiagnostics();
std::string DiagnosticsReport();
std::string RedactDiagnostics(std::string text);
// The Windows, CPU, RAM and GPU lines of the report (redacted): the graphics panel's Copy settings.
std::string DiagnosticsHardware();
// Uses only a prepared snapshot and a nonblocking lock in the crash handler.
void WriteCrashDiagnostics(FILE* file);
// The last 200 log lines (user names and personal folders removed as for uploads). Crash-handler safe: reads
// fixed memory, no allocation, does not block on a line being added.
void WriteLogTail(FILE* file);
bool HasCrashReports(const std::filesystem::path& user_root);
}
