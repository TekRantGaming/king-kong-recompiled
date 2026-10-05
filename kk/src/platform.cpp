#include "platform.h"

#include <filesystem>
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
  return false;
#endif
}

std::pair<int, int> PrimaryScreenSize() {
#if defined(_WIN32)
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
    return {int(mode.dmPelsWidth), int(mode.dmPelsHeight)};
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
#endif
}

void OpenInExplorer(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) && !path.has_extension()) std::filesystem::create_directories(path, ec);
#if defined(_WIN32)
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
  (void)atlas;
#endif
  if (!g_fonts.semibold) g_fonts.semibold = g_fonts.regular;
  if (!g_fonts.bold) g_fonts.bold = g_fonts.regular;
}

const UiFonts& GetUiFonts() { return g_fonts; }

}  // namespace kk
