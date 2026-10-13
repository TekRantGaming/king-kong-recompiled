// Game side of the launcher's play session (native-renderer/docs/menus.md): what play_session.cpp and
// pause_hooks.cpp share with the rest of the port (king_kong_app.h, overlay.cpp, input_remap.cpp,
// cheats.cpp). The UI side only uses play_session.h.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "play_session.h"

namespace kk::play {

// ---- set up by the app (king_kong_app.h) ----

// The user data root (saves live under it); set before the game starts.
void SetUserDir(const std::filesystem::path& user_dir);
const std::filesystem::path& UserDir();
// How the app quits (deferred to its UI thread); used when the game goes back to the launcher.
void SetQuitHandler(std::function<void()> quit);

// The start request as a command-line argument ("--kk_start=..."), empty for none: the launcher's
// restart_and_play passes it on to the new process.
std::wstring StartRequestArg();
// Reads --kk_start (and, in developer builds, KK_DEV_START) once the settings are loaded.
void LoadStartRequestFromSettings();

// Starts this program again with the port's session arguments replaced by `extra` (each "--name=value"):
// --kk_skip_launcher, --kk_start and --kk_launcher_open are never passed on twice.
bool RelaunchWith(const std::wstring& extra);

// Restarts into the launcher (--kk_launcher_open=<page>: play, the Play menu, or chapters, Chapter
// select) and quits this process.
void ReturnToLauncher(const char* why, const char* page);

// ---- what the game side tells the rest of the port ----

// True while the screen should be black: the start request is being carried out (the logos, the title
// and the start menu must never show) or the game is on its way back to the launcher.
bool CoverScreen();

// The pause takeover's backdrop: the game draws a black screen while paused, so the last frame of play is
// captured as the pause starts (`grab` fills RGBA pixels from the presenter's guest output; set by the
// app) and drawn under the port's pause menu (CreatePauseBackdrop, a dialog of the app's UI).
void SetFrameGrabber(std::function<bool(int& width, int& height, std::vector<uint8_t>& rgba)> grab);

// Called at the end of the port's XInput hook (input_remap.cpp) with each pad state the game reads:
// while the port's pause menu is open the game gets an empty pad (and buttons still held when it
// closes stay hidden until they are let go).
void FilterPad(uint32_t user, uint8_t* state);

// cheats.cpp: switches on the launcher's cheats now (after a start request, which never shows the
// start menu where they are normally applied). False when the game is not ready yet.
bool ApplyCheatsNow(uint8_t* base);

// The session the launcher started (a start request was carried out).
bool SessionActive();

// One of the game's menu pages (the chapter select, the start menu) is about to run. In a session the
// launcher started, that means play is over (a chapter-select run ends on the start menu's level, after
// its Score pages and save question): back to the launcher instead. True when the page must not run.
bool OnGameMenuPage(const char* page);

// pause_hooks.cpp, for play_session.cpp: forget the pause state (a new level).
void ResetPause();
// True when the last thing done in the pause menu was Quit game (the start menu request that follows
// goes back to the Play menu rather than Chapter select).
bool LeftByQuit();

}  // namespace kk::play

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace kk::play {

void CreatePauseBackdrop(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate);

#if defined(KK_DEV_TOOLS)
// Developer builds: the raw buttons of the last pad read (before FilterPad emptied it), for the
// ImGui stand-in of the pause menu (KK_DEV_PAUSE_STANDIN=1).
uint16_t DevRawButtons();
#endif

}  // namespace kk::play
