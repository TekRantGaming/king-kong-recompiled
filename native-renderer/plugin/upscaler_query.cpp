// The launcher's question "which temporal upscalers run here" (native_upscaler_query.h).

#include "plugin/upscaler_query.h"
#include "plugin/gpu_preference.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#if NR_GAME_RENDERER && NR_HAS_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <nvrhi/d3d12.h>

#include "backend/startup_steps.h"
#include "backend/upscaler.h"
#endif
#if NR_HAS_VULKAN
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#endif

REXCVAR_DECLARE(std::string, native_backend);

#if NR_GAME_RENDERER && NR_HAS_D3D12
// Task 335: two players' launchers (Vulkan, NVIDIA) crashed in NVIDIA's Direct3D driver while this question's
// Direct3D 12 device was being made beside the launcher's own presentation. The answer is kept, with the graphics
// card, driver and renderer build it is for, and asked again only when one of them changes.
REXCVAR_DEFINE_STRING(native_upscaler_query_saved, "", "Native renderer",
                      "Internal: the last answer to the launcher's upscaler question and the graphics card, driver "
                      "and renderer build it is for; asked again when one of them changes. Empty: asked again.");
#endif

namespace nr {

namespace {

std::mutex g_mutex;
bool g_started = false;
uint32_t g_state = NR_UPSCALER_QUERY_OFF;
NrUpscalerSupport g_result = {};
// Never destroyed: the process may end while it runs (the launcher's Quit); the game's start and
// the launcher's restart wait for it (WaitUpscalerQuery).
std::thread* g_thread = nullptr;

#if NR_GAME_RENDERER && NR_HAS_D3D12
std::string g_key;          // the card, driver and build of this question (SavedKey)
bool g_save_pending = false;  // the thread's answer is still to go into native_upscaler_query_saved

// What the answer depends on: the adapter the question uses (name, vendor and device id), its driver version
// and this renderer's build (its own file's link time stamp: a new build asks again). Empty when unknown.
std::string SavedKey() {
  std::string name;
  IDXGIAdapter1* adapter = ChosenD3D12Adapter(&name);
  if (!adapter) return {};
  DXGI_ADAPTER_DESC1 desc{};
  LARGE_INTEGER driver{};
  const bool have_desc = SUCCEEDED(adapter->GetDesc1(&desc));
  const bool have_driver = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver));
  adapter->Release();
  if (!have_desc || !have_driver) return {};
  uint32_t stamp = 0;
  HMODULE self = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&SavedKey), &self) && self) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
    stamp = nt->FileHeader.TimeDateStamp;
  }
  for (char& c : name)
    if (c == '|') c = ' ';
  const auto n = static_cast<unsigned long long>(driver.QuadPart);
  char key[512];
  std::snprintf(key, sizeof(key), "%s/%04x:%04x/%llu.%llu.%llu.%llu/%08x", name.c_str(), desc.VendorId, desc.DeviceId,
                n >> 48, (n >> 32) & 65535, (n >> 16) & 65535, n & 65535, stamp);
  return key;
}

// native_upscaler_query_saved: "1|key|vendor|fsr3 fsr4 dlss xess (four 0/1 digits)|notes".
std::string SavedText(const std::string& key, const NrUpscalerSupport& r) {
  char head[64];
  std::snprintf(head, sizeof(head), "|%x|%d%d%d%d|", r.vendor, r.fsr3 ? 1 : 0, r.fsr4 ? 1 : 0, r.dlss ? 1 : 0,
                r.xess ? 1 : 0);
  return "1|" + key + head + r.notes;
}

bool ReadSaved(const std::string& text, const std::string& key, NrUpscalerSupport* out) {
  if (key.empty() || text.rfind("1|" + key + "|", 0) != 0) return false;
  const size_t vendor_at = 3 + key.size();
  const size_t flags_at = text.find('|', vendor_at);
  if (flags_at == std::string::npos || text.size() < flags_at + 6 || text[flags_at + 5] != '|') return false;
  const std::string flags = text.substr(flags_at + 1, 4);
  if (flags.find_first_not_of("01") != std::string::npos) return false;
  *out = {};
  out->size = sizeof(*out);
  out->vendor = uint32_t(std::strtoul(text.substr(vendor_at, flags_at - vendor_at).c_str(), nullptr, 16));
  out->fsr3 = flags[0] == '1', out->fsr4 = flags[1] == '1', out->dlss = flags[2] == '1', out->xess = flags[3] == '1';
  std::snprintf(out->notes, sizeof(out->notes), "%s", text.c_str() + flags_at + 6);
  return true;
}
#endif

#if NR_GAME_RENDERER && NR_HAS_D3D12

void Finish(uint32_t state, const NrUpscalerSupport& result) {
  std::lock_guard lock(g_mutex);
  g_result = result;
  g_result.state = state;
  g_state = state;
}

// Task 353: how long the check's process may take before it is stopped (a hang in an SDK or the driver).
constexpr DWORD kCheckTimeoutMs = 15000;
constexpr UINT kCheckStoppedExit = 0x4B4B0353;

struct CheckSdk {
  const char* step;   // the report's name
  const char* title;  // the log's
  int32_t NrUpscalerSupport::*flag;
};
constexpr CheckSdk kCheckSdks[] = {{"fsr3", "FSR 3.1", &NrUpscalerSupport::fsr3},
                                   {"fsr4", "FSR 4", &NrUpscalerSupport::fsr4},
                                   {"dlss", "DLSS", &NrUpscalerSupport::dlss},
                                   {"xess", "XeSS", &NrUpscalerSupport::xess}};

std::string ExitText(DWORD code, bool stopped) {
  if (stopped) return "no answer in " + std::to_string(kCheckTimeoutMs / 1000) + " s, stopped";
  char text[96];
  const char* what = code == 0xC0000005   ? " access violation"
                     : code == 0xC0000409 ? " fail-fast or stack buffer overrun"
                     : code == 0xC00000FD ? " stack overflow"
                     : code == 0xC0000374 ? " heap corruption"
                     : code == 0xC000001D ? " illegal instruction"
                                          : "";
  std::snprintf(text, sizeof(text), "exit code 0x%08lX%s", static_cast<unsigned long>(code), what);
  return text;
}

std::string ReadReport(const std::wstring& path) {
  std::string text;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return text;
  char buf[4096];
  DWORD got = 0;
  while (text.size() < (1u << 20) && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
  CloseHandle(f);
  return text;
}

// The question (task 353): king_kong.exe again, as a short-lived process of its own with no window
// (upscaler_check_child.cpp), on the adapter the game's D3D12 provider uses (plugin/gpu_preference.h). It
// reports each SDK before and after asking it; if it crashes, or hangs past kCheckTimeoutMs, the SDK it was
// asking and those not reached are unavailable, and that answer is kept like any other
// (native_upscaler_query_saved: asked again when the card, driver or build changes).
void Run() {
  NrUpscalerSupport out = {};
  out.size = sizeof(out);
  out.vulkan = 0;
  {
    wchar_t skip[8] = L"";
    if (GetEnvironmentVariableW(L"KK_SKIP_UPSCALER_CHECK", skip, 8) && skip[0] == L'1') {
      // "Start without upscalers": no check, no SDK in this process, nothing kept.
      std::snprintf(out.notes, sizeof(out.notes), "%s", "not checked (started without upscalers)");
      REXLOG_WARN("rexgpu-native: upscalers: not checked and none offered (KK_SKIP_UPSCALER_CHECK=1, the "
                  "\"Start without upscalers\" start)");
      steps::Note("upscaler check: skipped (KK_SKIP_UPSCALER_CHECK)");
      Finish(NR_UPSCALER_QUERY_DONE, out);
      return;
    }
  }
  std::string adapter_name;
  LUID luid{};
  bool have_adapter = false;
  if (IDXGIAdapter1* adapter = ChosenD3D12Adapter(&adapter_name)) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      luid = desc.AdapterLuid;
      out.vendor = desc.VendorId;
      have_adapter = true;
    }
    adapter->Release();
  }
  if (!have_adapter) {
    REXLOG_WARN("rexgpu-native: upscaler query: no Direct3D 12 graphics card");
    Finish(NR_UPSCALER_QUERY_FAILED, out);
    return;
  }
  wchar_t exe[MAX_PATH * 2] = L"";
  wchar_t temp[MAX_PATH] = L"";
  const DWORD exe_len = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
  const DWORD temp_len = GetTempPathW(DWORD(std::size(temp)), temp);
  if (!exe_len || exe_len >= std::size(exe) || !temp_len || temp_len >= std::size(temp)) {
    REXLOG_WARN("rexgpu-native: upscaler query: no program or temporary folder path");
    Finish(NR_UPSCALER_QUERY_FAILED, out);
    return;
  }
  const std::wstring report =
      std::wstring(temp) + L"king_kong-upscaler-check-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
  DeleteFileW(report.c_str());  // one left by an earlier process with the same id
  wchar_t luid_text[40];
  std::swprintf(luid_text, std::size(luid_text), L"%lx:%lx", static_cast<unsigned long>(luid.HighPart),
                static_cast<unsigned long>(luid.LowPart));
  std::wstring command = L"\"" + std::wstring(exe) + L"\" --kk_upscaler_check=\"" + report +
                         L"\" --kk_upscaler_check_adapter=" + luid_text;
  // The check's process ends with this one (a job that kills it when its last handle closes).
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  }
  const auto started = std::chrono::steady_clock::now();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  steps::Note("upscaler check: starting its process on %s", adapter_name.c_str());
  if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                      nullptr, &si, &pi)) {
    const DWORD error = GetLastError();
    if (job) CloseHandle(job);
    REXLOG_WARN("rexgpu-native: upscaler query: the check's process could not start (error {}): no upscalers",
                error);
    steps::Note("upscaler check: its process could not start (error %lu)", error);
    std::snprintf(out.notes, sizeof(out.notes), "the check's process could not start (error %lu)", error);
    Finish(NR_UPSCALER_QUERY_FAILED, out);
    return;
  }
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  steps::Note("upscaler check: process %lu started", pi.dwProcessId);
  const bool stopped = WaitForSingleObject(pi.hProcess, kCheckTimeoutMs) == WAIT_TIMEOUT;
  if (stopped) {
    TerminateProcess(pi.hProcess, kCheckStoppedExit);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  if (job) CloseHandle(job);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  steps::Note("upscaler check: process %lu ended after %.0f ms, %s", pi.dwProcessId, ms,
              ExitText(code, stopped).c_str());

  // The report, line by line, into this process's log too.
  const std::string text = ReadReport(report);
  DeleteFileW(report.c_str());
  bool done = false, device_ok = false, any_line = false;
  std::string open_step, crash;
  bool answered[std::size(kCheckSdks)] = {};
  std::string why[std::size(kCheckSdks)];
  for (size_t at = 0; at < text.size();) {
    size_t eol = text.find('\n', at);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(at, eol - at);
    at = eol + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    any_line = true;
    REXLOG_INFO("rexgpu-native: upscaler check: {}", line);
    if (line.rfind("begin ", 0) == 0) {
      open_step = line.substr(6);
    } else if (line.rfind("end ", 0) == 0) {
      // "end <step> <1|0> <why>"
      const size_t a = line.find(' ', 4);
      const std::string step = line.substr(4, a == std::string::npos ? std::string::npos : a - 4);
      const bool ok = a != std::string::npos && line.size() > a + 1 && line[a + 1] == '1';
      const std::string reason = a != std::string::npos && line.size() > a + 3 ? line.substr(a + 3) : std::string();
      if (step == open_step) open_step.clear();
      if (step == "device") device_ok = ok;
      for (size_t i = 0; i < std::size(kCheckSdks); ++i) {
        if (step != kCheckSdks[i].step) continue;
        answered[i] = true;
        out.*(kCheckSdks[i].flag) = ok ? 1 : 0;
        why[i] = reason;
      }
    } else if (line.rfind("vendor ", 0) == 0) {
      out.vendor = uint32_t(std::strtoul(line.c_str() + 7, nullptr, 16));
    } else if (line.rfind("crash ", 0) == 0 && crash.empty()) {
      crash = line.substr(6);
    } else if (line == "done") {
      done = true;
    }
  }
  const bool clean = done && code == 0 && !stopped;
  std::string ended = ExitText(code, stopped);
  if (!crash.empty()) ended += "; " + crash;
  std::string notes;
  auto note = [&](const std::string& n) { notes += (notes.empty() ? "" : "; ") + n; };
  if (!device_ok && !clean)
    note(std::string("the check's process ended ") +
         (open_step == "device" ? "making its Direct3D 12 device" : "before its device") + " (" + ended + ")");
  for (size_t i = 0; i < std::size(kCheckSdks); ++i) {
    const CheckSdk& sdk = kCheckSdks[i];
    if (answered[i]) {
      if (!(out.*(sdk.flag)) && !why[i].empty()) note(std::string(sdk.title) + ": " + why[i]);
    } else {
      out.*(sdk.flag) = 0;
      if (open_step == sdk.step)
        note(std::string(sdk.title) + ": its check ended the check's process (" + ended + ")");
      else if (device_ok)
        note(std::string(sdk.title) + ": not checked (the check's process ended before it)");
    }
  }
  if (!clean && device_ok && open_step != "finish")
    REXLOG_WARN("rexgpu-native: upscaler check: the check's process ended during '{}' ({}): that upscaler and those "
                "after it are not offered", open_step.empty() ? "?" : open_step, ended);
  std::snprintf(out.notes, sizeof(out.notes), "%s", notes.c_str());
  REXLOG_INFO("rexgpu-native: upscalers this PC offers ({}, {}, checked in a separate process in {:.0f} ms): FSR 3.1 "
              "{}, FSR 4 {}, DLSS {}, XeSS {}{}{}",
              adapter_name, "Direct3D 12", ms, out.fsr3 ? "yes" : "no", out.fsr4 ? "yes" : "no",
              out.dlss ? "yes" : "no", out.xess ? "yes" : "no", notes.empty() ? "" : "; ", notes);
  {
    std::lock_guard lock(g_mutex);
    // Kept unless the process said nothing at all (it could not even write its report: asked again next time).
    g_save_pending = !g_key.empty() && (any_line || stopped);
  }
  Finish(NR_UPSCALER_QUERY_DONE, out);
}

// Task 353: the gate the backend asks before it loads an SDK in this process (backend/upscaler.h). The
// question is started if nobody asked it yet (a start without the launcher and no kept answer) and waited for.
bool StartLocked();
bool ConfirmedByCheck(UpscalerKind kind, std::string* why) {
  bool wait = false;
  {
    std::lock_guard lock(g_mutex);
    StartLocked();
    wait = g_state == NR_UPSCALER_QUERY_RUNNING;
  }
  if (wait) WaitUpscalerQuery();
  std::lock_guard lock(g_mutex);
  if (g_state != NR_UPSCALER_QUERY_DONE) {
    if (why) *why = "the upscaler check could not run on this PC";
    return false;
  }
  bool on = true;
  switch (kind) {
    case UpscalerKind::kFsr3: on = g_result.fsr3 != 0; break;
    case UpscalerKind::kFsr4: on = g_result.fsr4 != 0; break;
    case UpscalerKind::kDlss: on = g_result.dlss != 0; break;
    case UpscalerKind::kXess: on = g_result.xess != 0; break;
    default: break;
  }
  if (!on && why) *why = "the upscaler check did not confirm it on this PC";
  return on;
}

const bool g_gate_set = [] {
  SetUpscalerGate(&ConfirmedByCheck);
  return true;
}();

#endif

}  // namespace

uint32_t UpscalerQuery(NrUpscalerSupport* out) { return UpscalerQueryAnswer(out, true); }

uint32_t UpscalerQueryPeek(NrUpscalerSupport* out) { return UpscalerQueryAnswer(out, false); }

#if NR_GAME_RENDERER && NR_HAS_D3D12
namespace {
// g_mutex held. Starts the question once a process: the kept answer when it is for this card, driver and
// build, else the check's process on a thread. True when it started now.
bool StartLocked() {
  if (g_started) return false;
  g_started = true;
  {
    g_key = SavedKey();
    NrUpscalerSupport saved;
    if (ReadSaved(REXCVAR_GET(native_upscaler_query_saved), g_key, &saved)) {
      // Task 335: the same card, driver and build as the saved answer: no Direct3D 12 device in this process.
      g_result = saved;
      g_result.state = g_state = NR_UPSCALER_QUERY_DONE;
      REXLOG_INFO("rexgpu-native: upscalers this PC offers ({}, Direct3D 12, saved answer for this graphics card, "
                  "driver and build): FSR 3.1 {}, FSR 4 {}, DLSS {}, XeSS {}{}{}",
                  g_key.substr(0, g_key.find('/')), saved.fsr3 ? "yes" : "no", saved.fsr4 ? "yes" : "no",
                  saved.dlss ? "yes" : "no", saved.xess ? "yes" : "no", saved.notes[0] ? "; " : "", saved.notes);
    } else {
      g_state = NR_UPSCALER_QUERY_RUNNING;
      g_thread = new std::thread([] { Run(); });
    }
  }
  return true;
}
}  // namespace
#endif

uint32_t UpscalerQueryAnswer(NrUpscalerSupport* out, bool start) {
  std::lock_guard lock(g_mutex);
  if (!g_started && start) {
#if NR_GAME_RENDERER && NR_HAS_D3D12
    StartLocked();
#else
    g_started = true;
    REXLOG_INFO("rexgpu-native: upscaler query: this build has no temporal upscalers to offer");
    g_state = NR_UPSCALER_QUERY_OFF;
#endif
  }
#if NR_GAME_RENDERER && NR_HAS_D3D12
  if (g_save_pending && g_state == NR_UPSCALER_QUERY_DONE) {
    g_save_pending = false;
    rex::cvar::SetFlagByName("native_upscaler_query_saved", SavedText(g_key, g_result));
  }
#endif
  if (out && out->size >= sizeof(NrUpscalerSupport)) {
    if (g_state == NR_UPSCALER_QUERY_DONE) {
      *out = g_result;
      // Keep the SDK answer for both APIs: the player can change the API in the launcher.
#if NR_HAS_VULKAN
      out->vulkan = REXCVAR_GET(native_backend) != "d3d12";
      if (out->vulkan) {
        out->fsr4 = 0;  // AMD FSR 4 has no Vulkan backend.
        std::string notes = out->notes;
        if (g_result.fsr4)
          notes += std::string(notes.empty() ? "" : "; ") + "FSR 4: needs Direct3D 12 (Vulkan selected)";
#if !(defined(REX_UI_VULKAN_HAS_INSTANCE_REQUESTS) && defined(REX_UI_VULKAN_HAS_DEVICE_REQUESTS))
        out->dlss = out->xess = 0;
        if (g_result.dlss || g_result.xess)
          notes += std::string(notes.empty() ? "" : "; ") + "DLSS, XeSS: this build cannot enable their Vulkan extensions";
#endif
        std::snprintf(out->notes, sizeof(out->notes), "%s", notes.c_str());
      }
#endif
    } else {
      *out = {};
      out->size = sizeof(NrUpscalerSupport);
    }
    out->state = g_state;
  }
  return g_state;
}

void WaitUpscalerQuery() {
  std::thread* thread = nullptr;
  {
    std::lock_guard lock(g_mutex);
    thread = g_thread;
  }
  static std::mutex join_mutex;  // two callers: one joins, the other waits for it
  std::lock_guard join_lock(join_mutex);
  if (thread && thread->joinable()) thread->join();
}

}  // namespace nr
