#include "hdr_display.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <shlobj.h>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#else
#include <SDL3/SDL.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

namespace kk::hdr {

namespace {

std::mutex g_mutex;
std::function<void*()> g_window;
DisplayInfo g_last;
std::chrono::steady_clock::time_point g_last_time{};
bool g_have_last = false;

#if defined(_WIN32)
constexpr const wchar_t* kCalibrationAumid = L"MicrosoftCorporationII.WindowsHDRCalibration_8wekyb3d8bbwe!App";
constexpr const wchar_t* kCalibrationStore = L"ms-windows-store://pdp/?ProductId=9N7F2SM5D1LR";

std::string Narrow(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2 (Windows 11 24H2; not in every SDK's headers).
struct AdvancedColorInfo2 {
  DISPLAYCONFIG_DEVICE_INFO_HEADER header;
  UINT32 bits;  // 0 advancedColorSupported, 1 advancedColorActive, 4 highDynamicRangeSupported, 5 hdr user enabled
  UINT32 color_encoding, bits_per_channel, active_color_mode;  // mode 2 = HDR
};
constexpr int kGetAdvancedColorInfo2 = 15;

// The DisplayConfig path for a GDI device name ("\\.\DISPLAY1"): SDR white level, advanced colour, the
// monitor's name and the default advanced colour profile.
void ReadDisplayConfig(const wchar_t* gdi_name, DisplayInfo& info) {
  UINT32 np = 0, nm = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS)
    return;
  for (UINT32 i = 0; i < np; ++i) {
    const DISPLAYCONFIG_PATH_INFO& p = paths[i];
    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof(src);
    src.header.adapterId = p.sourceInfo.adapterId;
    src.header.id = p.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, gdi_name) != 0)
      continue;
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = p.targetInfo.adapterId;
    target.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
      info.monitor_name = Narrow(target.monitorFriendlyDeviceName);
    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof(white);
    white.header.adapterId = p.targetInfo.adapterId;
    white.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) info.sdr_white_nits = SdrWhiteNits(white.SDRWhiteLevel);
    AdvancedColorInfo2 color2{};
    color2.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(kGetAdvancedColorInfo2);
    color2.header.size = sizeof(color2);
    color2.header.adapterId = p.targetInfo.adapterId;
    color2.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&color2.header) == ERROR_SUCCESS) {
      info.hdr_supported = (color2.bits >> 4) & 1;
    } else {
      DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
      color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
      color.header.size = sizeof(color);
      color.header.adapterId = p.targetInfo.adapterId;
      color.header.id = p.targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) info.hdr_supported = color.advancedColorSupported;
    }
    // The default advanced colour profile (where the Windows HDR Calibration app's profile goes), from mscms
    // (Windows 10 2004 and later; looked up so older systems still start).
    using GetDefault = HRESULT(WINAPI*)(int scope, LUID adapter, UINT32 source, int type, int subtype, LPWSTR* name);
    static const GetDefault get_default = [] {
      HMODULE m = LoadLibraryExW(L"mscms.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      return m ? reinterpret_cast<GetDefault>(reinterpret_cast<void*>(GetProcAddress(m, "ColorProfileGetDisplayDefault")))
               : nullptr;
    }();
    if (get_default) {
      constexpr int kUserScope = 1, kSystemScope = 0, kIcc = 0, kExtendedDisplayColorMode = 8;
      LPWSTR name = nullptr;
      for (int scope : {kUserScope, kSystemScope}) {
        if (SUCCEEDED(get_default(scope, p.targetInfo.adapterId, p.sourceInfo.id, kIcc, kExtendedDisplayColorMode,
                                  &name)) && name && *name)
          break;
        if (name) LocalFree(name);
        name = nullptr;
      }
      if (name) {
        info.profile_name = Narrow(name);
        wchar_t dir[MAX_PATH] = {};
        DWORD size = sizeof(dir);
        using GetDir = BOOL(WINAPI*)(LPCWSTR, LPWSTR, PDWORD);
        static const GetDir get_dir = [] {
          HMODULE m = LoadLibraryExW(L"mscms.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
          return m ? reinterpret_cast<GetDir>(reinterpret_cast<void*>(GetProcAddress(m, "GetColorDirectoryW"))) : nullptr;
        }();
        std::wstring path = name;
        if (path.find(L'\\') == std::wstring::npos && get_dir && get_dir(nullptr, dir, &size))
          path = std::wstring(dir) + L"\\" + name;
        LocalFree(name);
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.size() <= (1u << 20)) info.profile = ParseIccLuminance(bytes.data(), bytes.size());
      }
    }
    return;
  }
}
#endif

}  // namespace

void SetWindowProvider(std::function<void*()> provider) {
  std::lock_guard lock(g_mutex);
  g_window = std::move(provider);
}

DisplayInfo QueryDisplay(void* native_window) {
  DisplayInfo info;
#if defined(_WIN32)
  HMONITOR want = native_window ? MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTONEAREST)
                                : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  IDXGIFactory1* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !info.detected && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
      IDXGIOutput* output = nullptr;
      for (UINT o = 0; !info.detected && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
        IDXGIOutput6* output6 = nullptr;
        DXGI_OUTPUT_DESC1 desc{};
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6)))) {
          if (SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == want) {
            info.detected = true;
            info.hdr_on = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            info.dxgi_min_nits = desc.MinLuminance;
            info.dxgi_max_nits = desc.MaxLuminance;
            info.dxgi_full_frame_nits = desc.MaxFullFrameLuminance;
            ReadDisplayConfig(desc.DeviceName, info);
          }
          output6->Release();
        }
        output->Release();
      }
      adapter->Release();
    }
    factory->Release();
  }
#else
  (void)native_window;
  if (SDL_WasInit(SDL_INIT_VIDEO)) {
    const SDL_DisplayID id = SDL_GetPrimaryDisplay();
    const SDL_PropertiesID props = id ? SDL_GetDisplayProperties(id) : 0;
    info.detected = id != 0;
    info.hdr_on = props && SDL_GetBooleanProperty(props, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
    info.hdr_supported = info.hdr_on;
    if (const char* name = id ? SDL_GetDisplayName(id) : nullptr) info.monitor_name = name;
  }
#endif
  if (const char* v = std::getenv("KK_DEV_SCREEN_HDR"); v && *v) {  // tests
    info.detected = true;
    info.hdr_on = info.hdr_supported = *v == '1';
  }
  return info;
}

namespace {

bool SetIfDifferent(const char* name, const std::string& value, bool numeric) {
  const std::string now = rex::cvar::GetFlagByName(name);
  if (numeric ? !now.empty() && std::abs(std::atof(now.c_str()) - std::atof(value.c_str())) < 0.5 : now == value)
    return false;
  return rex::cvar::SetFlagByName(name, value);
}

std::string Describe(const DisplayInfo& d) {
  std::string s = d.detected ? (d.monitor_name.empty() ? std::string("screen") : d.monitor_name) : "no screen found";
  s += d.hdr_on ? ", Windows HDR on" : ", Windows HDR off";
  s += d.hdr_supported ? " (HDR capable)" : " (not HDR capable)";
  s += ", DXGI " + std::to_string(int(d.dxgi_min_nits * 1000) / 1000.0).substr(0, 5) + "/" +
       std::to_string(int(d.dxgi_max_nits)) + "/" + std::to_string(int(d.dxgi_full_frame_nits)) + " nits";
  s += ", SDR white " + std::to_string(int(d.sdr_white_nits)) + " nits";
  if (d.profile)
    s += ", profile '" + d.profile_name + "' " + (d.profile->calibrated ? "calibrated " : "") +
         std::to_string(int(d.profile->min_nits * 1000) / 1000.0).substr(0, 5) + "/" + std::to_string(int(d.profile->max_nits)) +
         "/" + std::to_string(int(d.profile->full_frame_nits)) + " nits";
  else
    s += d.profile_name.empty() ? ", no advanced colour profile" : ", profile '" + d.profile_name + "' without brightness";
  return s;
}

}  // namespace

void Apply(const char* why) {
  std::function<void*()> window;
  {
    std::lock_guard lock(g_mutex);
    window = g_window;
  }
  const DisplayInfo info = QueryDisplay(window ? window() : nullptr);
  const Mode mode = ParseMode(rex::cvar::GetFlagByName("kk_hdr"));
  const bool automatic = rex::cvar::GetFlagByName("kk_hdr_levels") != "manual";
  bool changed = SetIfDifferent("present_hdr", WantHdr(mode, info) ? "true" : "false", false);
  if (automatic) {
    const Levels l = AutoLevels(info);
    changed |= SetIfDifferent("present_hdr_peak", std::to_string(int(l.peak)), true);
    changed |= SetIfDifferent("present_hdr_paper_white", std::to_string(int(l.paper_white)), true);
  }
  {
    // Task 262 (SDK patch 0044): unknown cvars are ignored by older SDKs.
    const DisplayRange r = AutoRange(info);
    char black[32];
    std::snprintf(black, sizeof(black), "%.4f", r.black);
    if (rex::cvar::GetFlagInfo("present_hdr_black")) {
      if (std::abs(std::atof(rex::cvar::GetFlagByName("present_hdr_black").c_str()) - r.black) > 1e-4)
        changed |= rex::cvar::SetFlagByName("present_hdr_black", black);
      changed |= SetIfDifferent("present_hdr_full_frame", std::to_string(int(r.full_frame)), true);
    }
  }
  bool first = false, screen_changed = false;
  {
    std::lock_guard lock(g_mutex);
    first = !g_have_last;
    screen_changed = Describe(info) != Describe(g_last);
    g_last = info;
    g_have_last = true;
    g_last_time = std::chrono::steady_clock::now();
  }
  if (first || changed || screen_changed)
    REXLOG_INFO("KK: HDR ({}): {}; kk_hdr={} kk_hdr_levels={} -> present_hdr={} peak={} paper white={} black={} "
                "full frame={} highlights={} knee={} cap={}", why,
                Describe(info), rex::cvar::GetFlagByName("kk_hdr"), automatic ? "auto" : "manual",
                rex::cvar::GetFlagByName("present_hdr"), rex::cvar::GetFlagByName("present_hdr_peak"),
                rex::cvar::GetFlagByName("present_hdr_paper_white"), rex::cvar::GetFlagByName("present_hdr_black"),
                rex::cvar::GetFlagByName("present_hdr_full_frame"), rex::cvar::GetFlagByName("present_hdr_highlights"),
                rex::cvar::GetFlagByName("present_hdr_knee"), rex::cvar::GetFlagByName("present_hdr_peak_cap"));
}

void ApplyIfStale() {
  {
    std::lock_guard lock(g_mutex);
    if (g_have_last && std::chrono::steady_clock::now() - g_last_time < std::chrono::seconds(2)) return;
  }
  Apply("refresh");
}

DisplayInfo LastDisplay() {
  std::lock_guard lock(g_mutex);
  return g_last;
}

bool CalibrationAppInstalled() {
#if defined(_WIN32)
  static int installed = -1;  // checked once a run: the app list does not change while the launcher is open often
  if (installed < 0) {
    IShellItem* item = nullptr;
    const std::wstring path = std::wstring(L"shell:AppsFolder\\") + kCalibrationAumid;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    installed = SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) ? 1 : 0;
    if (item) item->Release();
    if (SUCCEEDED(init)) CoUninitialize();
  }
  return installed == 1;
#else
  return false;
#endif
}

bool OpenCalibrationApp(bool* opened_store) {
  if (opened_store) *opened_store = false;
#if defined(_WIN32)
  const bool installed = CalibrationAppInstalled();
  const std::wstring target = installed ? std::wstring(L"shell:AppsFolder\\") + kCalibrationAumid : kCalibrationStore;
  const bool ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
                                                          SW_SHOWNORMAL)) > 32;
  if (opened_store) *opened_store = !installed;
  REXLOG_INFO("KK: HDR: Windows HDR Calibration {} ({})", installed ? "app" : "Store page", ok ? "opened" : "failed");
  {
    std::lock_guard lock(g_mutex);
    g_have_last = false;  // read again at the next refresh, as the player comes back
  }
  return ok;
#else
  return false;
#endif
}

}  // namespace kk::hdr
