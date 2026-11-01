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
// The API that failed and the native API used instead (Vulkan or DirectX 12).
void NoticeApis(std::string& failed, std::string& selected);
// A fallback the player has not been told about yet: the launcher opens to show it once.
bool NeedsNotice();
// The launcher showed the notice.
void NoticeShown();
bool TryNativeAgain();
// The player changed the renderer or graphics API by hand: the fallback is forgotten.
void ManualChoice();
// Task 228 (startup_recovery_state.h LauncherGuard). The launcher has shown its first frames; while
// `preparing_shaders` the marker says so, otherwise it is cleared. Cheap: writes only on a change.
void LauncherShown(bool preparing_shaders);
// The last launcher start ended unexpectedly: "window" (before it showed) or "shaders" (while it
// prepared shaders); empty otherwise or once CrashNoticeShown was called.
std::string CrashNotice();
void CrashNoticeShown();
// How many launcher starts in a row ended unexpectedly while preparing shaders.
int ShaderCrashes();
// kk_amd_shader_cache_offer is on (on by default, owner 18 Oct) and this PC has AMD
// driver shader cache folders.
bool AmdCacheOffer();
// The player agreed: the AMD driver shader caches are emptied at the next launcher start.
bool RequestAmdCacheClear();
// "<files removed> <files kept>" once after this start emptied the AMD caches, else empty.
std::string AmdCacheReport();
}  // namespace kk::startup
