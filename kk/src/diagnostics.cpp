#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <deque>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <vector>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <spdlog/sinks/base_sink.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <intrin.h>
#pragma comment(lib, "dxgi.lib")
#endif

namespace kk {
namespace {
constexpr size_t kReportSize = 32768;
std::mutex g_mutex;
std::string g_hardware;
std::string g_settings;
std::deque<std::string> g_errors, g_fallbacks, g_selections;
std::array<char, kReportSize> g_snapshot{}, g_initial{};
std::vector<std::string> g_private;

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
void Remember(std::deque<std::string>& lines, std::string line) {
  line = RedactDiagnostics(std::move(line));
  if (line.size() > 768) line.resize(768);
  if (!lines.empty() && lines.back() == line) return;
  if (lines.size() == 8) lines.pop_front();
  lines.push_back(std::move(line));
}
void Snapshot() { // g_mutex held
  std::ostringstream out;
  out << "King Kong PC port diagnostics\nVersion: " KK_VERSION "\n" << g_hardware << g_settings;
  out << "Renderer selections from the log (last 8):\n";
  if (g_selections.empty()) out << "  None recorded\n";
  for (const auto& line : g_selections) out << "  " << line << '\n';
  out << "Startup/runtime fallbacks (last 8):\n";
  if (g_fallbacks.empty()) out << "  None recorded\n";
  for (const auto& line : g_fallbacks) out << "  " << line << '\n';
  out << "Last log errors (last 8):\n";
  if (g_errors.empty()) out << "  None recorded\n";
  for (const auto& line : g_errors) out << "  " << line << '\n';
  out << "Local only. Personal names and file paths are redacted from this text.\n"
         "Minidumps may contain private process memory. Review before sharing.\n";
  const auto text = RedactDiagnostics(out.str());
  const size_t n = std::min(text.size(), g_snapshot.size() - 1);
  std::memcpy(g_snapshot.data(), text.data(), n);
  g_snapshot[n] = 0;
}
void Capture(std::string line, bool error) {
  const auto lower = Lower(line);
  const bool fallback = lower.find("fallback") != std::string::npos ||
      lower.find("falling back") != std::string::npos || lower.find("not supported") != std::string::npos ||
      lower.find("unsupported") != std::string::npos || lower.find("using classic") != std::string::npos ||
      lower.find("disabled") != std::string::npos || lower.find("not available") != std::string::npos;
  const bool selection = lower.starts_with("kk: renderer ") || lower.starts_with("kk: fullscreen mode:") ||
      lower.starts_with("kk: graphics card:") || lower.find("using \"nvidia") != std::string::npos ||
      lower.find("using \"amd") != std::string::npos || lower.find("using \"intel") != std::string::npos ||
      (lower.find("rexgpu-native:") != std::string::npos &&
       (lower.find("device:") != std::string::npos || lower.find("using ") != std::string::npos ||
        lower.find("upscaler ") != std::string::npos || lower.find("ao method ") != std::string::npos ||
        lower.find("adapter:") != std::string::npos));
  if (!error && !fallback && !selection) return;
  std::lock_guard lock(g_mutex);
  if (error) Remember(g_errors, line);
  if (fallback) Remember(g_fallbacks, line);
  if (selection) Remember(g_selections, line);
  Snapshot();
}
class DiagnosticSink final : public spdlog::sinks::base_sink<std::mutex> {
  void sink_it_(const spdlog::details::log_msg& msg) override {
    try {
      Capture(std::string(msg.payload.data(), msg.payload.size()), msg.level >= spdlog::level::err);
    } catch (...) { /* Diagnostics must not turn a log message into a crash. */ }
  }
  void flush_() override {}
};

std::string Hardware() {
  std::ostringstream out;
#if defined(_WIN32)
  using VersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  auto get_version = reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (get_version && get_version(&version) == 0)
    out << "Windows: " << version.dwMajorVersion << '.' << version.dwMinorVersion << " build " << version.dwBuildNumber << '\n';
  else out << "Windows: unavailable\n";
  int regs[4]{};
  char brand[49]{};
  __cpuid(regs, int(0x80000000));
  if (unsigned(regs[0]) >= 0x80000004)
    for (int i = 0; i < 3; ++i) {
      __cpuid(regs, int(0x80000002) + i);
      std::memcpy(brand + i * 16, regs, 16);
    }
  SYSTEM_INFO system{};
  GetNativeSystemInfo(&system);
  out << "CPU: " << (brand[0] ? brand : "unavailable") << " (" << system.dwNumberOfProcessors << " logical CPUs)\n";
  MEMORYSTATUSEX memory{sizeof(memory)};
  if (GlobalMemoryStatusEx(&memory)) out << "RAM: " << memory.ullTotalPhys / (1024 * 1024) << " MiB\n";
  else out << "RAM: unavailable\n";
  IDXGIFactory1* factory = nullptr;
  out << "GPU inventory (DXGI order, not necessarily the renderer's selected adapter):\n";
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    for (UINT i = 0; ; ++i) {
      IDXGIAdapter1* adapter = nullptr;
      if (FAILED(factory->EnumAdapters1(i, &adapter))) break;
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        char name[512]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        const char* vendor = desc.VendorId == 0x10de ? "NVIDIA" : desc.VendorId == 0x1002 ? "AMD" :
                             desc.VendorId == 0x8086 ? "Intel" : "Other";
        out << "  " << name << "; vendor " << vendor << " (0x" << std::hex << desc.VendorId << std::dec
            << "); dedicated VRAM " << desc.DedicatedVideoMemory / (1024 * 1024) << " MiB; shared limit "
            << desc.SharedSystemMemory / (1024 * 1024) << " MiB; driver ";
        LARGE_INTEGER driver{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
          const auto n = static_cast<unsigned long long>(driver.QuadPart);
          out << (n >> 48) << '.' << ((n >> 32) & 65535) << '.' << ((n >> 16) & 65535) << '.' << (n & 65535);
        } else out << "unavailable";
        out << '\n';
      }
      adapter->Release();
    }
    factory->Release();
  } else out << "  unavailable\n";
#else
  out << "OS: non-Windows; hardware inventory unavailable\n";
#endif
  return out.str();
}
}

std::string RedactDiagnostics(std::string text) {
  // Drop absolute paths, including profiles that do not belong to this user.
  static const std::regex paths(R"(([A-Za-z]:[\\/]|\\\\|/home/|/Users/)[^\r\n]*?($|[\r\n]))");
  text = std::regex_replace(text, paths, "[path redacted]$2");
  for (const auto& name : g_private) {
    if (name.empty()) continue;
    size_t at = 0;
    while ((at = Lower(text).find(Lower(name), at)) != std::string::npos) {
      text.replace(at, name.size(), "[redacted]");
      at += 10;
    }
  }
  return text;
}

void RefreshDiagnostics() {
  std::ostringstream out;
#if defined(_WIN32)
  DEVMODEW desktop{};
  desktop.dmSize = sizeof(desktop);
  out << "Primary desktop mode (Windows, may differ from the selected monitor): ";
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &desktop))
    out << desktop.dmPelsWidth << 'x' << desktop.dmPelsHeight << " @ " << desktop.dmDisplayFrequency
        << " Hz, " << desktop.dmBitsPerPel << " bits\n";
  else out << "unavailable\n";
#endif
  out << "Settings (requested; actual selection/failures appear in the log):\n";
  for (const char* key : {"kk_update_channel", "fullscreen", "kk_fullscreen_mode", "window_width", "window_height",
       "monitor", "kk_render_quality", "native_render_scale", "ao_mode", "ao_strength",
       "kk_renderer", "gpu_plugin", "native_backend", "resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y",
       "native_upscaler", "native_upscaler_quality", "native_ao_method", "native_ao_apply",
       "native_ao_quality", "native_ao_samples", "present_effect", "ssao"}) {
    const auto value = rex::cvar::GetFlagByName(key);
    out << "  " << key << " = " << (value.empty() ? "default/unavailable" : value) << '\n';
  }
  std::lock_guard lock(g_mutex);
  g_settings = out.str();
  Snapshot();
}

void InitDiagnostics() {
  static std::once_flag once;
  std::call_once(once, [] {
    for (const char* key : {"USERNAME", "USER", "COMPUTERNAME"})
      if (const char* name = std::getenv(key); name && *name) g_private.emplace_back(name);
    g_hardware = Hardware();
    RefreshDiagnostics();
    std::memcpy(g_initial.data(), g_snapshot.data(), g_snapshot.size());
    // Include startup messages before the sink was installed. Bound IO to 256 KiB.
    std::ifstream log(rex::cvar::GetFlagByName("log_file"), std::ios::binary);
    if (log) {
      log.seekg(0, std::ios::end);
      const auto end = log.tellg();
      const auto start = std::max<std::streamoff>(0, std::streamoff(end) - 262144);
      log.seekg(start);
      std::string line;
      if (start) std::getline(log, line);
      while (std::getline(log, line)) {
        const auto lower = Lower(line);
        Capture(line, lower.find("[error]") != std::string::npos || lower.find("[critical]") != std::string::npos);
      }
    }
    rex::AddSink(std::make_shared<DiagnosticSink>());
  });
}

std::string DiagnosticsReport() {
  RefreshDiagnostics();
  std::lock_guard lock(g_mutex);
  return g_snapshot.data();
}
void WriteCrashDiagnostics(FILE* file) {
  if (g_mutex.try_lock()) {
    std::fwrite(g_snapshot.data(), 1, std::strlen(g_snapshot.data()), file);
    g_mutex.unlock();
  } else {
    std::fwrite(g_initial.data(), 1, std::strlen(g_initial.data()), file);
    std::fputs("Crash interrupted a diagnostics update; initial snapshot used.\n", file);
  }
}
bool HasCrashReports(const std::filesystem::path& root) {
  std::error_code ec;
  for (std::filesystem::directory_iterator it(root / "crashes", ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (name.starts_with("crash-") && (it->path().extension() == ".txt" || it->path().extension() == ".dmp")) return true;
  }
  return false;
}
}
