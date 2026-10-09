// Small OS helpers used by the app (kept out of the app header so it doesn't
// pull in <windows.h>).

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ImFont;
struct ImFontAtlas;

namespace kk {

bool IsShiftHeld();

// Primary monitor resolution in physical pixels.
std::pair<int, int> PrimaryScreenSize();

struct MonitorInfo {
  std::string name;
  int width = 0, height = 0;
  bool primary = false;
};
std::vector<MonitorInfo> ListMonitors();

// Starts a new instance of this executable with the current command line plus
// `extra_args`.
void RelaunchSelf(std::wstring_view extra_args);

// Asks the OS for game-friendly scheduling: 1 ms timer resolution (the game's
// short sleeps otherwise last ~15.6 ms on Windows 10 2004+ and 11) and no
// power throttling (Windows 11 may otherwise run it on efficiency cores and drop
// the timer request while the window is covered). Logs the measured length of
// a 1 ms sleep before and after. Call once, early.
void TuneProcessScheduling();

// Runs a command line (no window) and waits; true when it exits with 0.
bool RunAndWait(const std::wstring& command_line);

// Opens a web page in the default browser.
void OpenUrl(const std::string& url);

// Opens a folder (created if missing) or file in the system file browser.
void OpenInExplorer(const std::filesystem::path& path);

// Adds system UI fonts to the ImGui atlas; the regular face becomes the default.
void LoadUiFont(ImFontAtlas* atlas);

struct UiFonts {
  ImFont* regular = nullptr;
  ImFont* semibold = nullptr;  // falls back to regular
  ImFont* bold = nullptr;      // falls back to regular
  ImFont* display = nullptr;   // the launcher's headings (Bahnschrift); falls back to bold
};
const UiFonts& GetUiFonts();

}  // namespace kk
