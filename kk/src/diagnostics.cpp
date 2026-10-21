#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <spdlog/sinks/base_sink.h>

#include "log_tail_logic.h"
#include "background_log_sink.h"

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
// The last 200 log lines for crash and hang reports, redacted as for uploads (task 197). Fixed memory.
LogTail<> g_tail;
std::shared_ptr<BackgroundLogSink> g_background_output;
#if defined(KK_DEV_TOOLS)
std::string DevLogSetting(const char* name) {
#if defined(_WIN32)
  char value[32]{};
  const DWORD size = GetEnvironmentVariableA(name, value, DWORD(std::size(value)));
  return size && size < std::size(value) ? value : "";
#else
  const char* value = std::getenv(name);
  return value ? value : "";
#endif
}
// Diagnostic injection only: model the original 18.6 s Guest FPS file write.
// The mutex also stalls concurrent output, as the original file sink does.
class SlowOutputForTest final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  SlowOutputForTest(std::vector<spdlog::sink_ptr> outputs, int delay_ms)
      : outputs_(std::move(outputs)), delay_ms_(delay_ms) {}
 private:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    if (std::string_view(msg.payload.data(), msg.payload.size()).starts_with("Guest FPS:") && ++fps_lines_ == 13)
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
    for (const auto& output : outputs_)
      if (output->should_log(msg.level)) output->log(msg);
  }
  void flush_() override { for (const auto& output : outputs_) output->flush(); }
  std::vector<spdlog::sink_ptr> outputs_;
  int delay_ms_, fps_lines_ = 0;
};
#endif
telemetry::Private g_personal;  // set once in InitDiagnostics, before any line is added
void RememberLogLine(std::string_view line) {
  g_tail.Add(RedactLogLine(line, g_personal, decltype(g_tail)::kMaxLineBytes - 1));
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
constexpr size_t kLineSize = 768;
// Longest text the redaction matches: a path start ("/Users/") or a private name.
size_t RedactionReach() {
  size_t reach = 7;
  for (const auto& name : g_private) reach = std::max(reach, name.size());
  return reach;
}
// Truncates before redacting (log lines can be long), as strongly as redacting the whole line: the
// window keeps a margin of the longest match, and when the line was cut the margin's output is
// dropped, so a name or path start cut in half at the window's end never reaches the report.
std::string RedactLine(std::string line) {
  const size_t reach = RedactionReach();
  const bool cut = line.size() > kLineSize + reach;
  if (cut) line.resize(kLineSize + reach);
  line = RedactDiagnostics(std::move(line));
  if (cut) line.resize(line.size() - std::min(reach, line.size()));
  if (line.size() > kLineSize) line.resize(kLineSize);
  return line;
}
// Returns whether the list changed.
bool Remember(std::deque<std::string>& lines, const std::string& redacted) {
  if (!lines.empty() && lines.back() == redacted) return false;
  if (lines.size() == 8) lines.pop_front();
  lines.push_back(redacted);
  return true;
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
      lower.find("unsupported") != std::string::npos || lower.find("both native graphics APIs failed") != std::string::npos ||
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
  const std::string redacted = RedactLine(std::move(line));
  bool changed = false;
  if (error) changed |= Remember(g_errors, redacted);
  if (fallback) changed |= Remember(g_fallbacks, redacted);
  if (selection) changed |= Remember(g_selections, redacted);
  // A repeated line leaves the report as it was.
  if (changed) Snapshot();
}
class DiagnosticSink final : public spdlog::sinks::base_sink<std::mutex> {
  void sink_it_(const spdlog::details::log_msg& msg) override {
    try {
      // The whole line as the log file has it (time, level, text) for the crash report's log tail.
      spdlog::memory_buf_t formatted;
      formatter_->format(msg, formatted);
      RememberLogLine(std::string_view(formatted.data(), formatted.size()));
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

namespace {
// Where an absolute path starts: "X:\", "X:/", "\\", "/home/" or "/Users/".
bool PathStartsAt(const std::string& text, size_t i) {
  const size_t left = text.size() - i;
  const unsigned char c = static_cast<unsigned char>(text[i]);
  if (left >= 3 && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) && text[i + 1] == ':' &&
      (text[i + 2] == '\\' || text[i + 2] == '/'))
    return true;
  if (left >= 2 && c == '\\' && text[i + 1] == '\\') return true;
  return text.compare(i, 6, "/home/") == 0 || text.compare(i, 7, "/Users/") == 0;
}
}

std::string RedactDiagnostics(std::string text) {
  // Drop absolute paths, including profiles that do not belong to this user: from where a path
  // starts to the end of its line.
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (PathStartsAt(text, i)) {
      out += "[path redacted]";
      while (i < text.size() && text[i] != '\r' && text[i] != '\n') ++i;
      continue;
    }
    out += text[i++];
  }
  text = std::move(out);
  for (const auto& name : g_private) {
    if (name.empty()) continue;
    const std::string lower_name = Lower(name);
    const std::string lower = Lower(text);
    std::string replaced;
    replaced.reserve(text.size());
    size_t from = 0;
    for (size_t at; (at = lower.find(lower_name, from)) != std::string::npos; from = at + name.size()) {
      replaced.append(text, from, at - from);
      replaced += "[redacted]";
    }
    replaced.append(text, from, std::string::npos);
    text = std::move(replaced);
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
       "native_upscaler", "native_upscaler_quality", "native_upscaler_output", "native_frame_size", "native_ao_method", "native_ao_apply",
       "native_ao_quality", "native_ao_samples", "native_ao_normal_smoothing", "native_cel_shading", "native_cel_outline",
       "native_cel_darkness", "native_cel_bands", "native_cel_strength", "present_effect", "ssao"}) {
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
    // As the uploads (telemetry.cpp PersonalNames).
    for (const char* key : {"USERNAME", "USER", "LOGNAME", "COMPUTERNAME"})
      if (const char* v = std::getenv(key); v && *v) g_personal.names.emplace_back(v);
    for (const char* key : {"USERPROFILE", "HOME"})
      if (const char* v = std::getenv(key); v && *v) g_personal.homes.emplace_back(v);
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
        RememberLogLine(line);
        const auto lower = Lower(line);
        Capture(line, lower.find("[error]") != std::string::npos || lower.find("[critical]") != std::string::npos);
      }
    }
    // The SDK's normal categories share their output sinks and flush level.
    // Keep custom category routing unchanged if an embedding supplies it.
    // Built-ins are lazy: register them before changing automatic flushes,
    // including categories whose first message is emitted during gameplay.
    for (const auto category : {rex::log::core(), rex::log::cpu(), rex::log::apu(), rex::log::gpu(),
                                rex::log::krnl(), rex::log::sys(), rex::log::fs()}) (void)category;
    const auto logger = rex::GetLogger();
    auto outputs = logger->sinks();
    const auto flush_level = logger->flush_level();
    bool shared = !outputs.empty();
    for (const auto& category : rex::GetAllCategories())
      shared &= category.logger->sinks() == outputs && category.logger->flush_level() == flush_level;
    bool background = true;
#if defined(KK_DEV_TOOLS)
    if (shared) {
      const long delay = std::clamp(std::strtol(DevLogSetting("KK_DEV_LOG_IO_STALL_MS").c_str(), nullptr, 10), 0L, 20000L);
      if (delay > 0) {
        auto slow = std::make_shared<SlowOutputForTest>(outputs, int(delay));
        for (const auto& output : outputs) rex::RemoveSink(output);
        rex::AddSink(slow);
        outputs = {slow};
      }
    }
    background = DevLogSetting("KK_DEV_SYNC_LOG_OUTPUT") != "1";
#endif
    if (shared && background) {
      // Logging keeps working synchronously if a worker cannot be created.
      try { g_background_output = std::make_shared<BackgroundLogSink>(outputs, flush_level); }
      catch (const std::exception&) {}
    }
    if (g_background_output) {
      for (const auto& output : outputs) rex::RemoveSink(output);
      rex::AddSink(g_background_output);
      // Preserve automatic flushes on the worker. Explicit/periodic flushes
      // still wait for preceding output; the per-frame log call only enqueues.
      for (const auto& category : rex::GetAllCategories()) category.logger->flush_on(spdlog::level::off);
    }
    rex::AddSink(std::make_shared<DiagnosticSink>());
    REXLOG_INFO("KK: log output {}", g_background_output ? "background" : "direct");
  });
}

void StopBackgroundLogOutput() {
  if (g_background_output) g_background_output->Stop();
}

std::string DiagnosticsHardware() {
  std::lock_guard lock(g_mutex);
  return RedactDiagnostics(g_hardware);
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
void WriteLogTail(FILE* file) {
  std::fputs("Last log lines (oldest first; user names and personal folders removed):\n", file);
  const size_t n = g_tail.ForEach([file](const char* text, size_t size) {
    std::fputs("  ", file);
    std::fwrite(text, 1, size, file);
    std::fputs("\n", file);
  });
  if (!n) std::fputs("  None recorded\n", file);
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
