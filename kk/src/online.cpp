#include "online.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "http.h"
#include "settings.h"

namespace kk::online {
namespace {

constexpr const char* kTelemetryUrl = "https://kk-launcher-telemetry.tekrantgaming.workers.dev";
constexpr auto kEvery = std::chrono::minutes(5);

std::mutex g_mutex;
std::condition_variable g_wake;
bool g_woken = false;
bool g_playing = false;
std::string g_available;
Counts g_counts;

std::string Url() {
#if defined(KK_DEV_TOOLS)
  // Developer aid: KK_DEV_ONLINE_URL=http://localhost:8787 points at a local server.
  if (const char* v = std::getenv("KK_DEV_ONLINE_URL"); v && *v) return v;
#endif
  return kTelemetryUrl;
}

// "win32" on Windows; "linux" on Linux, Proton (Wine) included.
const char* Platform() {
#if defined(_WIN32)
  static const bool wine = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }();
  return wine ? "linux" : "win32";
#else
  return "linux";
#endif
}

// The number after "key": in a small JSON reply, or -1.
int JsonInt(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = json.find(needle);
  return at == std::string::npos ? -1 : std::atoi(json.c_str() + at + needle.size());
}

std::string Quoted(const std::string& s) {
  std::string out = "\"";
  for (const char c : s)
    if (c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20) out += c;
  return out + "\"";
}

void CheckIn() {
  if (REXCVAR_GET(kk_coop_enabled)) return;
  const std::string id = InstallId();
  if (id.empty()) return;
  bool playing;
  std::string available;
  {
    std::lock_guard lock(g_mutex);
    playing = g_playing;
    available = g_available.empty() ? KK_VERSION : g_available;
  }
  const std::string body = "{\"installId\":" + Quoted(id) + ",\"launcherVersion\":" + Quoted(KK_VERSION) +
                           ",\"gameVersion\":null,\"availableVersion\":" + Quoted(available) +
                           ",\"platform\":" + Quoted(Platform()) + ",\"arch\":\"x64\",\"channel\":" +
                           Quoted(REXCVAR_GET(kk_update_channel)) + ",\"playing\":" + (playing ? "true" : "false") +
                           "}";
  std::vector<uint8_t> reply;
  if (!HttpPost(Url() + "/v1/heartbeat", body, reply)) {
    REXLOG_INFO("KK: online count: the server didn't answer");
    return;
  }
  const std::string json(reply.begin(), reply.end());
  const int online = JsonInt(json, "online"), in_game = JsonInt(json, "playing");
  if (online < 0) return;
  std::lock_guard lock(g_mutex);
  if (!REXCVAR_GET(kk_online_count)) return;  // switched off meanwhile
  g_counts = {true, online, std::max(0, in_game)};
}

void Run() {
  for (;;) {
    if (REXCVAR_GET(kk_online_count)) CheckIn();
    std::unique_lock lock(g_mutex);
    g_wake.wait_for(lock, kEvery, [] { return g_woken; });
    g_woken = false;
    if (!REXCVAR_GET(kk_online_count)) g_counts = {};
  }
}

}  // namespace

std::string NewInstallId() {
  std::random_device rd;
  std::mt19937_64 rng((uint64_t(rd()) << 32) ^ rd());
  uint8_t b[16];
  for (int i = 0; i < 16; i += 8) {
    const uint64_t v = rng();
    std::memcpy(b + i, &v, 8);
  }
  b[6] = uint8_t((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = uint8_t((b[8] & 0x3F) | 0x80);  // variant
  char s[37];
  std::snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return s;
}

std::string InstallId() {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  if (!REXCVAR_GET(kk_online_count)) return {};
  std::string id = REXCVAR_GET(kk_install_id);
  if (id.empty()) {  // switched on without one (the game started without the launcher)
    id = NewInstallId();
    rex::cvar::SetFlagByName("kk_install_id", id);
  }
  return id;
}

std::string ServerUrl() { return Url(); }

void Start() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(Run).detach(); });
}

void Wake() {
  {
    std::lock_guard lock(g_mutex);
    g_woken = true;
    if (!REXCVAR_GET(kk_online_count)) g_counts = {};
  }
  g_wake.notify_all();
}

void SetPlaying(bool playing) {
  {
    std::lock_guard lock(g_mutex);
    if (g_playing == playing) return;
    g_playing = playing;
  }
  Wake();
}

void SetAvailableVersion(const std::string& version) {
  std::lock_guard lock(g_mutex);
  g_available = version;
}

Counts Get() {
  std::lock_guard lock(g_mutex);
  return g_counts;
}

}  // namespace kk::online
