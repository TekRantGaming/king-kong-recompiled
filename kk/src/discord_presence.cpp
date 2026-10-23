#include "discord_presence.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "settings.h"

namespace kk::discord {
namespace {

// The port's Discord application (discord.com/developers/applications): profiles show "Playing"
// and its name, with its icon. kk_discord_app_id (testing) overrides it.
constexpr const char* kApplicationId = "1559247649289412788";  // "King Kong Recompiled"
constexpr const char* kWebsite = "https://kong.tekrantgaming.com";
constexpr const char* kDiscordInvite = "https://discord.gg/8eakYSvBZS";  // as the launcher's
constexpr auto kPoll = std::chrono::seconds(5);   // pings answered, the status kept current
constexpr auto kRetry = std::chrono::seconds(20);  // Discord not running

// The IPC's frames: a little-endian op and length, then that many bytes of JSON.
enum Op : uint32_t { kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4 };

std::mutex g_mutex;
std::condition_variable g_wake;
bool g_woken = false;
bool g_playing = false;
int64_t g_since = 0;  // when the launcher opened or the game started (Unix time)

std::string ApplicationId() {
  const std::string id = REXCVAR_GET(kk_discord_app_id);
  return id.empty() ? kApplicationId : id;
}

std::string Quoted(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out + "\"";
}

// One connection to the Discord app.
class Connection {
 public:
  Connection() = default;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() { Close(); }

#if defined(_WIN32)
  bool Open() {
    for (int i = 0; i < 10 && !IsOpen(); ++i) {
      const std::wstring name = L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(i);
      HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE) pipe_ = h;
    }
    return IsOpen();
  }
  bool IsOpen() const { return pipe_ != INVALID_HANDLE_VALUE; }
  void Close() {
    if (IsOpen()) CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }

 private:
  bool Send(const void* data, size_t size) {
    DWORD done = 0;
    return WriteFile(pipe_, data, DWORD(size), &done, nullptr) && done == size;
  }
  size_t Available() {
    DWORD avail = 0;
    if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) return SIZE_MAX;  // gone
    return avail;
  }
  bool Receive(void* data, size_t size) {
    auto* p = static_cast<uint8_t*>(data);
    while (size) {
      DWORD done = 0;
      if (!ReadFile(pipe_, p, DWORD(size), &done, nullptr) || !done) return false;
      p += done;
      size -= done;
    }
    return true;
  }
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
#else
  bool Open() {
    std::string dir;
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
      if (const char* v = std::getenv(var); v && *v && dir.empty()) dir = v;
    if (dir.empty()) dir = "/tmp";
    // Discord itself, then its Flatpak and Snap packages.
    for (const char* sub : {"", "app/com.discordapp.Discord/", "snap.discord/"}) {
      for (int i = 0; i < 10 && !IsOpen(); ++i) {
        const std::string path = dir + "/" + sub + "discord-ipc-" + std::to_string(i);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) continue;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) fd_ = fd;
        else close(fd);
      }
    }
    return IsOpen();
  }
  bool IsOpen() const { return fd_ >= 0; }
  void Close() {
    if (IsOpen()) close(fd_);
    fd_ = -1;
  }

 private:
  bool Send(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size) {
      const ssize_t n = send(fd_, p, size, MSG_NOSIGNAL);
      if (n <= 0) return false;
      p += n;
      size -= size_t(n);
    }
    return true;
  }
  size_t Available() {
    pollfd p{fd_, POLLIN, 0};
    if (poll(&p, 1, 0) < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return SIZE_MAX;
    if (!(p.revents & POLLIN)) return 0;
    int avail = 0;
    return ioctl(fd_, FIONREAD, &avail) < 0 ? SIZE_MAX : size_t(avail);
  }
  bool Receive(void* data, size_t size) {
    auto* p = static_cast<uint8_t*>(data);
    while (size) {
      const ssize_t n = recv(fd_, p, size, 0);
      if (n <= 0) return false;
      p += n;
      size -= size_t(n);
    }
    return true;
  }
  int fd_ = -1;
#endif

 public:
  bool Write(uint32_t op, const std::string& json) {
    std::string frame(8, '\0');
    const uint32_t size = uint32_t(json.size());
    std::memcpy(frame.data(), &op, 4);
    std::memcpy(frame.data() + 4, &size, 4);
    return IsOpen() && Send((frame + json).data(), frame.size() + json.size());
  }

  enum class Got { kNothing, kFrame, kError };
  // One whole frame when its header has arrived: within `wait` (0: only one that is already here).
  Got Read(uint32_t& op, std::string& json, std::chrono::milliseconds wait) {
    const auto until = std::chrono::steady_clock::now() + wait;
    for (;;) {
      const size_t avail = Available();
      if (avail == SIZE_MAX) return Got::kError;
      if (avail >= 8) break;
      if (std::chrono::steady_clock::now() >= until) return Got::kNothing;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    uint32_t header[2];
    if (!Receive(header, sizeof(header)) || header[1] > (1u << 20)) return Got::kError;
    op = header[0];
    json.assign(header[1], '\0');
    const auto payload_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (header[1] > 0) {
      const size_t payload_available = Available();
      if (payload_available == SIZE_MAX) return Got::kError;
      if (payload_available >= header[1]) break;
      if (std::chrono::steady_clock::now() >= payload_until) return Got::kError;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return header[1] == 0 || Receive(json.data(), json.size()) ? Got::kFrame : Got::kError;
  }
};

// What the profile shows now (the activity's JSON).
std::string Activity() {
  bool playing;
  int64_t since;
  {
    std::lock_guard lock(g_mutex);
    playing = g_playing;
    since = g_since;
  }
  // "logo": the application's Rich Presence art asset (the website's mark, discord-app\logo.png).
  return std::string("{\"details\":") + Quoted(playing ? "In the game" : "In the launcher") +
         ",\"state\":" + Quoted(std::string("PC port v") + KK_VERSION) +
         ",\"assets\":{\"large_image\":\"logo\",\"large_text\":\"King Kong Recompiled\"}" +
         ",\"timestamps\":{\"start\":" + std::to_string(since) + "},\"buttons\":[{\"label\":\"Get the PC port\",\"url\":" +
         Quoted(kWebsite) + "},{\"label\":\"Join the Discord\",\"url\":" + Quoted(kDiscordInvite) + "}]}";
}

bool SetActivity(Connection& c, const std::string& activity) {
  static uint64_t nonce = 0;
#if defined(_WIN32)
  const unsigned long pid = GetCurrentProcessId();
#else
  const unsigned long pid = (unsigned long)getpid();
#endif
  return c.Write(kFrame, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) +
                             (activity.empty() ? "" : ",\"activity\":" + activity) + "},\"nonce\":\"" +
                             std::to_string(++nonce) + "\"}");
}

// Connects and says hello. False when Discord isn't running or refused (logged once per reason).
bool Connect(Connection& c, std::string& last_problem) {
  auto problem = [&](const std::string& why) {
    if (why != last_problem) REXLOG_INFO("KK: Discord status: {}", why);
    last_problem = why;
    c.Close();
    return false;
  };
  if (!c.Open()) return problem("Discord isn't running (trying again every 20 seconds)");
  if (!c.Write(kHandshake, "{\"v\":1,\"client_id\":" + Quoted(ApplicationId()) + "}"))
    return problem("Discord didn't take the connection");
  uint32_t op = 0;
  std::string json;
  if (c.Read(op, json, std::chrono::seconds(5)) != Connection::Got::kFrame) return problem("Discord didn't answer");
  if (op == kClose || json.find("\"READY\"") == std::string::npos)
    return problem("Discord refused the connection: " + json.substr(0, 200));
  REXLOG_INFO("KK: Discord status: connected");
  last_problem.clear();
  return true;
}

// Answers pings and reads replies. False when Discord closed the connection.
bool Service(Connection& c) {
  for (;;) {
    uint32_t op = 0;
    std::string json;
    switch (c.Read(op, json, std::chrono::milliseconds(0))) {
      case Connection::Got::kNothing:
        return true;
      case Connection::Got::kError:
        return false;
      case Connection::Got::kFrame:
        if (op == kClose) return false;
        if (op == kPing && !c.Write(kPong, json)) return false;
        if (op == kFrame && json.find("\"evt\":\"ERROR\"") != std::string::npos)
          REXLOG_WARN("KK: Discord status: {}", json.substr(0, 300));
        break;
    }
  }
}

void Run() {
  Connection connection;
  std::string shown;         // the activity Discord has (empty: none sent on this connection)
  std::string last_problem;  // logged once until it changes
  auto next_try = std::chrono::steady_clock::now();
  for (;;) {
    const bool on = REXCVAR_GET(kk_discord_status) && !ApplicationId().empty();
    if (!on && connection.IsOpen()) {
      SetActivity(connection, "");  // cleared at once (closing alone can leave it for a while)
      connection.Close();
      shown.clear();
      REXLOG_INFO("KK: Discord status: off");
    }
    if (on && !connection.IsOpen() && std::chrono::steady_clock::now() >= next_try) {
      if (Connect(connection, last_problem)) shown.clear();
      else next_try = std::chrono::steady_clock::now() + kRetry;
    }
    if (on && connection.IsOpen()) {
      bool ok = Service(connection);
      if (ok) {
        const std::string activity = Activity();
        if (activity != shown) {
          ok = SetActivity(connection, activity);
          if (ok) shown = activity;
        }
      }
      if (!ok) {
        REXLOG_INFO("KK: Discord status: Discord closed the connection");
        connection.Close();
        shown.clear();
        next_try = std::chrono::steady_clock::now() + kRetry;
      }
    }
    std::unique_lock lock(g_mutex);
    g_wake.wait_for(lock, kPoll, [] { return g_woken; });
    g_woken = false;
  }
}

}  // namespace

void Start() {
  static std::once_flag once;
  std::call_once(once, [] {
    {
      std::lock_guard lock(g_mutex);
      g_since = int64_t(std::time(nullptr));
    }
    std::thread(Run).detach();
  });
}

void SetPlaying(bool playing) {
  {
    std::lock_guard lock(g_mutex);
    if (g_playing == playing) return;
    g_playing = playing;
    g_since = int64_t(std::time(nullptr));
  }
  Wake();
}

void Wake() {
  {
    std::lock_guard lock(g_mutex);
    g_woken = true;
  }
  g_wake.notify_all();
}

bool Available() { return !ApplicationId().empty(); }

}  // namespace kk::discord
