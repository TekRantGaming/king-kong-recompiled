// Small OS helpers used by the app (kept out of the app header so it doesn't
// pull in <windows.h>).

#pragma once

#include <atomic>
#include <cstdint>
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

// Whether a screen has HDR switched on (Windows: a DXGI output in the HDR10 colour space; Linux: SDL's
// HDR property of the primary display). The setup's HDR row only shows when one has (job J4).
bool ScreenHdrOn();

// The graphics card the game runs on: the dedicated one when there is one,
// else the integrated one (Windows' high-performance adapter). Unknown when it
// can't be told (and on Linux).
enum class GpuVendor { kUnknown, kNvidia, kAmd, kIntel };
struct GpuInfo {
  GpuVendor vendor = GpuVendor::kUnknown;
  std::string name;
  uint64_t dedicated_memory = 0;  // bytes of its own video memory (0: unknown, or integrated)
  std::string driver;             // Windows: the display driver's version ("32.0.15.6094"); empty when unknown
};
GpuInfo DetectGpu();

// Running on a Steam Deck (SteamOS sets SteamDeck=1; else the board's name).
bool IsSteamDeck();

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

#if !defined(_WIN32)
// Runs a program found on the PATH (argv[0]) with the arguments and collects
// what it writes, adding the byte count to *bytes as it arrives. Its errors are
// dropped. The AppImage's library path isn't passed on, so system programs
// (curl, ffmpeg) load their own libraries. True when it exits with 0.
bool RunCapture(const std::vector<std::string>& argv, std::vector<uint8_t>& out,
                std::atomic<uint64_t>* bytes = nullptr);
#endif

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
