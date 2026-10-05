#include "platform.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#include <timeapi.h>
#else
#include <SDL3/SDL.h>
#include <cstdlib>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <imgui.h>

#include <rex/logging.h>

namespace kk {
namespace {
UiFonts g_fonts;
}

bool IsShiftHeld() {
#if defined(_WIN32)
  return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
  SDL_PumpEvents();
  return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
#endif
}

std::pair<int, int> PrimaryScreenSize() {
#if defined(_WIN32)
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
    return {int(mode.dmPelsWidth), int(mode.dmPelsHeight)};
#else
  if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay()))
    return {int(mode->w * mode->pixel_density), int(mode->h * mode->pixel_density)};
#endif
  return {1 << 16, 1 << 16};
}

std::vector<MonitorInfo> ListMonitors() {
  std::vector<MonitorInfo> out;
#if defined(_WIN32)
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR mon, HDC, LPRECT, LPARAM user) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(mon, &info)) return TRUE;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        MonitorInfo m;
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
          m.width = int(mode.dmPelsWidth);
          m.height = int(mode.dmPelsHeight);
        }
        m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        DISPLAY_DEVICEW dev{};
        dev.cb = sizeof(dev);
        std::wstring wname = EnumDisplayDevicesW(info.szDevice, 0, &dev, 0) ? dev.DeviceString : info.szDevice;
        m.name.assign(wname.begin(), wname.end());  // device names are ASCII in practice
        reinterpret_cast<std::vector<MonitorInfo>*>(user)->push_back(std::move(m));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
#else
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
  for (int i = 0; ids && i < count; ++i) {
    MonitorInfo m;
    if (const char* name = SDL_GetDisplayName(ids[i])) m.name = name;
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(ids[i])) {
      m.width = int(mode->w * mode->pixel_density);
      m.height = int(mode->h * mode->pixel_density);
    }
    m.primary = ids[i] == primary;
    out.push_back(std::move(m));
  }
  SDL_free(ids);
#endif
  return out;
}

void RelaunchSelf(std::wstring_view extra_args) {
#if defined(_WIN32)
  std::wstring cmd = GetCommandLineW();
  cmd += L" ";
  cmd += extra_args;
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const auto dir = std::filesystem::path(exe).parent_path();
  if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else {
    REXLOG_ERROR("KK: relaunch failed ({})", GetLastError());
  }
#else
  // Same arguments as this run (from /proc/self/cmdline) plus the extras.
  std::vector<std::string> args;
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  for (std::string arg; std::getline(cmdline, arg, '\0');) args.push_back(arg);
  std::string extra;
  for (wchar_t c : extra_args) extra += static_cast<char>(c);  // ASCII flags
  args.push_back(extra);
  // Inside an AppImage, relaunch the AppImage itself rather than the mounted binary.
  const char* appimage = std::getenv("APPIMAGE");
  const std::string exe = appimage ? appimage : std::filesystem::read_symlink("/proc/self/exe").string();
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (size_t i = 1; i < args.size(); ++i) argv.push_back(args[i].data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
    REXLOG_ERROR("KK: relaunch failed");
#endif
}

namespace {
// Average real length of Sleep(1), in milliseconds.
double MeasureSleep1() {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  for (int i = 0; i < 10; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count() / 10;
}
}  // namespace

void TuneProcessScheduling() {
#if defined(_WIN32)
  const double before = MeasureSleep1();
  const bool timer = timeBeginPeriod(1) == TIMERR_NOERROR;  // held until exit
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;  // opt out of both
  const bool power = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
  REXLOG_INFO("KK: 1 ms sleep took {:.1f} ms, now {:.1f} ms (timer {}, power throttling off {})", before,
              MeasureSleep1(), timer ? "1 ms" : "unchanged", power ? "yes" : "no");
#endif
}

void OpenInExplorer(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) && !path.has_extension()) std::filesystem::create_directories(path, ec);
#if defined(_WIN32)
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  const std::string target = path.string();
  const char* argv[] = {"xdg-open", target.c_str(), nullptr};
  pid_t pid = 0;
  if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0)
    REXLOG_WARN("KK: could not run xdg-open for {}", target);
#endif
}

void LoadUiFont(ImFontAtlas* atlas) {
#if defined(_WIN32)
  wchar_t windir[MAX_PATH];
  if (!GetWindowsDirectoryW(windir, MAX_PATH)) return;
  const auto fonts = std::filesystem::path(windir) / "Fonts";
  // Latin-1 plus General Punctuation: the game's achievement text uses curly
  // quotes (as in "Kong's power").
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2010, 0x205E, 0};
  auto load = [&](const char* file) -> ImFont* {
    const auto path = fonts / file;
    return std::filesystem::exists(path) ? atlas->AddFontFromFileTTF(path.string().c_str(), 18.0f, nullptr, kRanges)
                                         : nullptr;
  };
  g_fonts.regular = load("segoeui.ttf");
  if (!g_fonts.regular) return;
  g_fonts.semibold = load("seguisb.ttf");
  g_fonts.bold = load("segoeuib.ttf");
  ImGui::GetIO().FontDefault = g_fonts.regular;
#else
  // Common Linux UI fonts; the first family found wins.
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2010, 0x205E, 0};
  struct Family { const char* regular; const char* semibold; const char* bold; };
  static const Family kFamilies[] = {
      {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
       "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"},
      {"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
       "/usr/share/fonts/noto/NotoSans-Bold.ttf"},
      {"/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/google-noto/NotoSans-SemiBold.ttf",
       "/usr/share/fonts/google-noto/NotoSans-Bold.ttf"},
      {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", nullptr, "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"},
      {"/usr/share/fonts/TTF/DejaVuSans.ttf", nullptr, "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"},
      {"/usr/share/fonts/dejavu/DejaVuSans.ttf", nullptr, "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"},
      {"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", nullptr,
       "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"},
  };
  auto load = [&](const char* file) -> ImFont* {
    std::error_code ec;
    return file && std::filesystem::exists(file, ec) ? atlas->AddFontFromFileTTF(file, 18.0f, nullptr, kRanges)
                                                     : nullptr;
  };
  for (const auto& f : kFamilies) {
    if (!(g_fonts.regular = load(f.regular))) continue;
    g_fonts.semibold = load(f.semibold ? f.semibold : f.bold);
    g_fonts.bold = load(f.bold);
    ImGui::GetIO().FontDefault = g_fonts.regular;
    break;
  }
#endif
  if (!g_fonts.semibold) g_fonts.semibold = g_fonts.regular;
  if (!g_fonts.bold) g_fonts.bold = g_fonts.regular;
}

const UiFonts& GetUiFonts() { return g_fonts; }

}  // namespace kk
