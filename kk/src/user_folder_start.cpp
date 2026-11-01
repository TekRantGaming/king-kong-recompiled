// See user_folder_start.h.
#include "user_folder_start.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

#include "user_folder.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kk::user_folder {

namespace {

std::vector<std::string> g_lines;

#if defined(_WIN32)
std::atomic<uint64_t> g_done{0}, g_total{0};
std::atomic<bool> g_stop{false};

constexpr UINT_PTR kTimer = 1;
constexpr int kShowAfterMs = 700;  // a short copy shows nothing

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  static auto opened = std::chrono::steady_clock::now();
  static HFONT font = nullptr;
  switch (message) {
    case WM_CREATE: {
      opened = std::chrono::steady_clock::now();
      NONCLIENTMETRICSW metrics{};
      metrics.cbSize = sizeof(metrics);
      if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        font = CreateFontIndirectW(&metrics.lfMessageFont);
      SetTimer(window, kTimer, 100, nullptr);
      return 0;
    }
    case WM_TIMER: {
      if (g_stop) {
        DestroyWindow(window);
        return 0;
      }
      const auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened).count();
      if (ms >= kShowAfterMs && !IsWindowVisible(window)) ShowWindow(window, SW_SHOWNOACTIVATE);
      InvalidateRect(window, nullptr, TRUE);
      return 0;
    }
    case WM_CLOSE:
      return 0;  // stopping here would only make the next start carry on
    case WM_PAINT: {
      PAINTSTRUCT paint;
      HDC dc = BeginPaint(window, &paint);
      RECT area;
      GetClientRect(window, &area);
      const int margin = MulDiv(16, GetDeviceCaps(dc, LOGPIXELSY), 96);
      const int bar_height = MulDiv(14, GetDeviceCaps(dc, LOGPIXELSY), 96);
      if (font) SelectObject(dc, font);
      SetBkMode(dc, TRANSPARENT);
      const uint64_t done = g_done, total = g_total;
      wchar_t text[256];
      swprintf_s(text, L"Moving your saves and downloads into the King Kong folder.\nPlease wait: %.1f of %.1f GB",
                 double(done) / (1 << 30), double(total) / (1 << 30));
      RECT line = {margin, margin, area.right - margin, area.bottom - margin * 2 - bar_height};
      DrawTextW(dc, text, -1, &line, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
      RECT bar = {margin, area.bottom - margin - bar_height, area.right - margin, area.bottom - margin};
      FrameRect(dc, &bar, GetSysColorBrush(COLOR_BTNSHADOW));
      RECT fill = bar;
      InflateRect(&fill, -1, -1);
      fill.right = fill.left + LONG(double(fill.right - fill.left) * (total ? double(done) / double(total) : 0));
      FillRect(dc, &fill, GetSysColorBrush(COLOR_HIGHLIGHT));
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DESTROY:
      KillTimer(window, kTimer);
      if (font) DeleteObject(font);
      font = nullptr;
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

void WindowThread() {
  WNDCLASSW type{};
  type.lpfnWndProc = WindowProc;
  type.hInstance = GetModuleHandleW(nullptr);
  type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32514));  // the wait cursor
  type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  type.lpszClassName = L"KingKongUserFolder";
  RegisterClassW(&type);
  const int dpi = int(GetDpiForSystem());
  const int width = MulDiv(460, dpi, 96), height = MulDiv(150, dpi, 96);
  HWND window = CreateWindowExW(0, type.lpszClassName, L"King Kong", WS_POPUP | WS_CAPTION,
                                (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
                                (GetSystemMetrics(SM_CYSCREEN) - height) / 2, width, height, nullptr, nullptr,
                                type.hInstance, nullptr);
  if (!window) return;
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
}
#endif

}  // namespace

std::filesystem::path Start(const std::filesystem::path& program_folder, const std::filesystem::path& documents,
                            const std::filesystem::path& config) {
  Options o;
#if defined(KK_DEV_TOOLS)
  (void)documents;
#else
  o.old_root = documents;
#endif
  if (const char* from = std::getenv("KK_MOVE_FROM"); from && *from) o.old_root = from;  // NOLINT
  o.new_root = program_folder / kFolderName;
  o.config = config;
  o.log = [](const std::string& line) { g_lines.push_back(line); };
  if (Done(o.new_root)) {
    Repoint(o);
    return o.new_root;
  }
#if defined(_WIN32)
  // A second window opened during the move waits for it.
  HANDLE lock = CreateMutexW(nullptr, FALSE, L"Local\\KingKongUserFolderMove");
  if (lock) WaitForSingleObject(lock, INFINITE);
  o.cloud_file = [](const std::filesystem::path& file) {
    const DWORD a = GetFileAttributesW(file.c_str());
    return a != INVALID_FILE_ATTRIBUTES &&
           (a & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE |
                 FILE_ATTRIBUTE_PINNED | FILE_ATTRIBUTE_UNPINNED)) != 0;
  };
  std::once_flag open;
  std::thread window;
  o.progress = [&](uint64_t done, uint64_t total) {
    g_done = done;
    g_total = total;
    std::call_once(open, [&] { window = std::thread(WindowThread); });
  };
#endif
  const std::filesystem::path root = Resolve(o);
#if defined(_WIN32)
  g_stop = true;
  if (window.joinable()) window.join();
  if (lock) {
    ReleaseMutex(lock);
    CloseHandle(lock);
  }
#endif
  return root;
}

void LogMoved() {
  for (const auto& line : g_lines) REXLOG_INFO("KK user folder: {}", line);
  g_lines.clear();
}

}  // namespace kk::user_folder
