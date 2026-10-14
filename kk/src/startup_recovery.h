#pragma once
#include <filesystem>
#include <string>

namespace kk::startup {
void MonitorStartup();
void Configure(const std::filesystem::path& root);
std::string ApplyChoice(const std::string& plugin);
// Arms recovery for a process that runs the game (`game`). The launcher's process (the launcher
// shows before the game) only reacts to a renderer that cannot start; Begin is called again with
// `game` when its Play starts the game in the same process. Startup ends at the first shown game frame
// (reported by the native renderer) or a time limit.
void Begin(const std::string& plugin, bool game);
void CleanExit();
std::string Message();
// A fallback the player has not been told about yet: the launcher opens to show it once.
bool NeedsNotice();
// The launcher showed the notice.
void NoticeShown();
bool TryNativeAgain();
// The player changed the renderer or graphics API by hand: the fallback is forgotten.
void ManualChoice();
}  // namespace kk::startup
