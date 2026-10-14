#pragma once
#include <filesystem>
#include <string>

namespace kk::startup {
void MonitorStartup();
void Configure(const std::filesystem::path& root);
std::string ApplyChoice(const std::string& plugin);
void Begin(const std::string& plugin);
void ChapterReached();
void CleanExit();
std::string Message();
bool NeedsNotice();
bool TryNativeAgain();
}  // namespace kk::startup
