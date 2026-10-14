// Read-only task-047 frame sampler for unmodified release executables without dev logging.
// Polling has roughly 1 ms timing uncertainty; validate against dev_frame_log before comparing.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
  if (argc != 4) return 2;  // owned game PID, seconds, CSV
  const DWORD pid = DWORD(std::strtoul(argv[1], nullptr, 10));
  HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!process) return 3;
  wchar_t image[32768];
  DWORD length = DWORD(std::size(image));
  if (!QueryFullProcessImageNameW(process, 0, image, &length)) { CloseHandle(process); return 3; }
  const std::filesystem::path executable(std::wstring(image, length));
  const std::wstring prefix = L"F:\\KK-native-renderer\\runs\\task047-";
  const auto path = executable.wstring();
  const std::filesystem::path output = std::filesystem::absolute(argv[3]).lexically_normal();
  if (path.size() < prefix.size() || _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) ||
      _wcsicmp(executable.filename().c_str(), L"king_kong.exe") ||
      _wcsicmp(output.parent_path().c_str(), executable.parent_path().c_str())) {
    std::puts("Refused: only a task047 private game and a CSV beside that executable are allowed");
    CloseHandle(process);
    return 4;
  }
  FILE* csv = nullptr;
  if (_wfopen_s(&csv, output.c_str(), L"w") || !csv) { CloseHandle(process); return 5; }
  std::fputs("sample,t,host_ms,game_ms\n", csv);
  // ReXGlue's fixed virtual arena; the same guest field recorded by kk/src/dev_frame_log.cpp.
  constexpr uintptr_t address = 0x100000000ull + 0x82CCAE1Cull;
  using Clock = std::chrono::steady_clock;
  const auto begin = Clock::now();
  auto last = begin;
  uint32_t previous = 0;
  bool have_previous = false;
  unsigned samples = 0, reads_failed = 0;
  const double seconds = std::max(0.1, std::strtod(argv[2], nullptr));
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - begin).count();
    if (elapsed >= seconds) break;
    uint32_t big_endian = 0;
    SIZE_T got = 0;
    if (ReadProcessMemory(process, reinterpret_cast<const void*>(address), &big_endian, sizeof(big_endian), &got)
        && got == sizeof(big_endian)) {
      const uint32_t current = std::byteswap(big_endian);
      if (!have_previous) { previous = current; last = now; have_previous = true; }
      else if (current != previous) {
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        std::fprintf(csv, "%u,%.6f,%.6f,%u\n", samples++, elapsed, ms, current);
        last = now;
        previous = current;
      }
    } else ++reads_failed;
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -10000;  // one millisecond, relative; no global timer-resolution change
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 10);
    } else Sleep(1);
  }
  std::fclose(csv);
  if (timer) CloseHandle(timer);
  CloseHandle(process);
  std::printf("%u observed frame-clock changes, %u failed reads\n", samples, reads_failed);
  return samples > 0 && reads_failed == 0 ? 0 : 6;
}
