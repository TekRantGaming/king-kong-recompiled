#include "coop_runtime.h"
#include "coop_session.h"
#include "settings.h"

#if defined(KK_DEV_TOOLS)
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>

#include <imgui.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "freecam.h"
#include "hfr.h"
#include "mods.h"
#include "photo_mode.h"
#include "play_internal.h"
#include "ui_paint.h"

REXCVAR_DEFINE_STRING(kk_coop_dev, "", "KK/Debug", "Local co-op test link: host:<port> or join:<port>")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, kk_hide_hud);

namespace kk::coop {
namespace {
using Clock = std::chrono::steady_clock;
double Now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
bool Address(uint32_t a, uint32_t size = 4) {
  return a >= 0x40000000 && a <= 0xA0000000 - size && (a & 3) == 0;
}
// Same flags, extended AI and H_TRACK_tagoff validation as dev_teleport.cpp, with bounds for each span.
std::optional<PlayerPose> ReadJack(const uint8_t* base) {
  using namespace hfr;
  if (!base || play::CoverScreen()) return {};
  const uint32_t g = Load32(base, 0x82CC97C0);
  if (!Address(g, 0x1C) || Load32(base, 0x828E3B48) != 0) return {};
  const uint32_t actor = Load32(base, 0x82CCBF7C);
  if (!Address(actor, 28) || (Load32(base, actor) & 0x00400001u)) return {};
  const uint32_t ext = Load32(base, actor + 24);
  if (!Address(ext, 8)) return {};
  const uint32_t ai = Load32(base, ext + 4);
  if (!Address(ai, 84)) return {};
  const uint32_t first = Load16(base, ai + 80);
  if (first > Load16(base, ai + 82) || first > 4096 || !Address(ai, (first + 7) * 4)) return {};
  const uint32_t block = Load32(base, ai + (first + 6) * 4);
  if (!Address(block, 24) || Load32(base, block + 20) != 0x826718B8) return {};
  const uint32_t matrix = Load32(base, actor + 12), vars = Load32(base, ai + 64);
  if (!Address(matrix, 64) || !Address(vars, 11672)) return {};
  PlayerPose p;
  p.time = Now(); p.level = Load32(base, g + 0x18);
  p.x = LoadF(base, matrix + 48); p.y = LoadF(base, matrix + 52); p.z = LoadF(base, matrix + 56);
  const float fx = -LoadF(base, matrix + 16), fy = -LoadF(base, matrix + 20);
  if (!std::isfinite(fx) || !std::isfinite(fy) || std::hypot(fx, fy) < .1f) return {};
  p.facing = std::atan2(fy, fx); p.speed = LoadF(base, vars + 11668);
  const uint32_t animation_base = Load32(base, actor + 20);
  if (Address(animation_base)) {
    const uint32_t animation = Load32(base, animation_base);
    if (Address(animation, 160)) p.action = Load16(base, animation + 156);
  }
  Message check; check.type = MessageType::PlayerPose; check.pose = p;
  std::vector<uint8_t> bytes;
  return Encode(check, bytes) ? std::optional<PlayerPose>(p) : std::nullopt;
}
struct View {
  Role role = Role::Host;
  SessionState state = SessionState::Idle;
  RejectReason rejection = RejectReason::None;
  TransportStats stats;
  std::optional<PlayerPose> local, partner;
  double updated = 0;
};
class Service {
 public:
  ~Service() { Stop(); }
  void Start(SessionConfig config, uint16_t port) {
    if (running_.exchange(true)) return;
    stop_ = false;
    worker_ = std::thread([this, config = std::move(config), port] {
      auto transport = CreateDevUdp(config.role == Role::Host, port);
      CoopSession session(config, *transport, [](SessionState s) {
        REXLOG_INFO("KK co-op: {}", StateName(s));
      });
      SessionState shown = SessionState::Idle;
      while (!stop_.load()) {
        const double now = Now();
        std::optional<PlayerPose> local;
        { std::lock_guard lock(mutex_); local = local_; }
        if (local && now - local->time > .25) local.reset();
        session.Tick(now, local);
        View view{config.role, session.State(), session.Rejection(), session.Stats(), local, session.Partner(now), now};
        { std::lock_guard lock(mutex_); view_ = std::move(view); }
        if (shown != session.State()) { shown = session.State(); RequestUiPaint(); }
        if (session.State() == SessionState::Ended) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      session.End(Now());
      // Bounded clean shutdown; deliver and acknowledge Bye without touching game memory.
      const double until = Now() + .2;
      while (Now() < until) {
        transport->Poll(Now());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      transport->Close();
    });
  }
  void Stop() {
    if (!running_.exchange(false)) return;
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_); local_.reset(); view_ = {};
  }
  bool Running() const { return running_; }
  void Publish(std::optional<PlayerPose> p) { std::lock_guard lock(mutex_); local_ = p; }
  View Snapshot() { std::lock_guard lock(mutex_); return view_; }
 private:
  std::atomic<bool> running_{false}, stop_{false};
  std::thread worker_;
  std::mutex mutex_;
  std::optional<PlayerPose> local_;
  View view_;
};
Service service;  // No thread, socket, guest reads, file access or allocation until explicitly started.

class PartnerOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PartnerOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
 protected:
  void OnDraw(ImGuiIO&) override {
    if (!REXCVAR_GET(kk_coop_enabled) || !service.Running() || photo::Active() || freecam::HideHud() ||
        freecam::HideGameHud() || REXCVAR_GET(kk_hide_hud) || play::CoverScreen()) return;
    const View v = service.Snapshot();  // UI reads copied values only, never guest memory.
    if (Now() - v.updated > .5 && v.state != SessionState::Ended) return;
    char text[256];
    std::snprintf(text, sizeof(text), "Co-op %s: %s | %.0f ms | loss %.1f%% | invalid %llu | %s",
        v.role == Role::Host ? "host" : "guest", StateName(v.state), v.stats.ping_ms, v.stats.loss * 100,
        static_cast<unsigned long long>(v.stats.malformed), RejectName(v.rejection));
    auto* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImVec2(24, 64), IM_COL32(255, 230, 170, 255), text);
    if (v.state != SessionState::Connected || !v.local || !v.partner || v.local->level != v.partner->level ||
        freecam::Active()) return;
    const float dx = v.partner->x - v.local->x, dy = v.partner->y - v.local->y, dz = v.partner->z - v.local->z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float angle = std::remainder(std::atan2(dy, dx) - v.local->facing, 6.28318530718f);
    const char* directions[] = {"ahead", "ahead-left", "left", "behind-left", "behind", "behind-right", "right", "ahead-right"};
    const int sector = (int(std::lround(angle / .7853981634f)) + 8) % 8;
    std::snprintf(text, sizeof(text), "Partner: %.0f m, %s (Jack facing)", distance, directions[sector]);
    dl->AddText(ImVec2(24, 88), IM_COL32(180, 255, 210, 255), text);
  }
};
}  // namespace
void Start(const std::filesystem::path& user_data) {
  if (!REXCVAR_GET(kk_coop_enabled) || service.Running()) return;
  std::string address = REXCVAR_GET(kk_coop_dev);
  if (const char* env = std::getenv("KK_COOP_DEV"); address.empty() && env) address = env;
  const size_t colon = address.find(':');
  if (colon == std::string::npos) return;
  const std::string role = address.substr(0, colon);
  unsigned port = 0;
  const char* begin = address.data() + colon + 1;
  const char* end = address.data() + address.size();
  const auto result = std::from_chars(begin, end, port);
  if ((role != "host" && role != "join") || result.ec != std::errc{} || result.ptr != end || !port || port > 65535) {
    REXLOG_WARN("KK co-op: invalid dev link, expected host:<port> or join:<port>"); return;
  }
  SessionConfig config;
  config.enabled = true; config.role = role == "host" ? Role::Host : Role::Guest;
  if (config.role == Role::Host) {
    std::random_device random;
    config.epoch = (uint64_t(random()) << 32) ^ random();
    if (!config.epoch) config.epoch = 1;
  }
  config.compatibility = {kProtocolVersion, KK_VERSION, KK_COOP_BUILD, mods::GameplayDigest(user_data)};
  service.Start(std::move(config), uint16_t(port));
}
void Shutdown() { service.Stop(); }
void GameFrame(uint8_t* base) {
  if (!REXCVAR_GET(kk_coop_enabled)) return;
  if (service.Running()) service.Publish(ReadJack(base));
}
void CreateOverlay(rex::ui::ImGuiDrawer* drawer) {
  if (REXCVAR_GET(kk_coop_enabled)) new PartnerOverlay(drawer);
}
}  // namespace kk::coop
#else
// Steam transport will use the same protocol/session in C05. No dev link or worker in shipping builds.
namespace kk::coop {
void Start(const std::filesystem::path&) {}
void Shutdown() {}
void GameFrame(uint8_t*) {}
void CreateOverlay(rex::ui::ImGuiDrawer*) {}
}  // namespace kk::coop
#endif
