#include "coop_runtime.h"
#include "cheats.h"
#include "coop_session.h"
#include "coop_pose.h"
#include "coop_nametag.h"
#include "settings.h"
#include "steam_runtime.h"
#include "social_fake.h"

#include <atomic>
#include <algorithm>
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
#include <rex/filesystem.h>
#include <rex/ui/imgui_dialog.h>

#include "freecam.h"
#include "hfr.h"
#include "mods.h"
#include "photo_mode.h"
#include "play_internal.h"
#include "ui_paint.h"
#include "coop_enemies_game.h"
#include "coop_partner_body.h"

#if defined(KK_DEV_TOOLS)
REXCVAR_DEFINE_STRING(kk_coop_policy_override, "", "KK/Debug", "Chapter policies: entry:together|solo|observer,... (same on both peers)")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(kk_coop_dev, "", "KK/Debug", "Local co-op test link: host:<port> or join:<port>")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(kk_steam_selftest, false, "KK/Debug", "Test Steam services and exit without starting a game")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(kk_social_fake, false, "KK/Debug", "Use an offline Social panel preview")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif
REXCVAR_DECLARE(bool, kk_hide_hud);
REXCVAR_DECLARE(bool, present_letterbox);

namespace kk::coop {
namespace {
std::unique_ptr<SteamRuntime> steam;
#if defined(KK_DEV_TOOLS)
std::unique_ptr<ISocialPreview> social_preview;
#endif
bool configured = false;
using Clock = std::chrono::steady_clock;
double Now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
// Same validated reader in runtime and sparse-memory unit tests.
PoseRead ReadJack(const uint8_t* base) {
  if (!base) return {{}, "no-base"};
  if (play::CoverScreen()) return {{}, "cover"};
  return ReadJackPose([base](uint32_t a) { return hfr::Load32(base, a); },
      [base](uint32_t a) { return hfr::Load16(base, a); },
      [base](uint32_t a) { return hfr::LoadF(base, a); }, Now());
}
struct View {
  Role role = Role::Host;
  SessionState state = SessionState::Idle;
  RejectReason rejection = RejectReason::None;
  TransportStats stats;
  std::optional<PlayerPose> local, partner;
  double updated = 0;
  const char* local_failure = "not-published";
  PoseCounters poses;
  enemies::Frame enemies;
  uint64_t enemies_sent = 0;
  uint64_t shots_sent = 0;
  std::optional<CheatSet> host_cheats;
  std::string partner_name;
};
std::mutex enemy_status_mutex;
enemies::Counters enemy_status;
ShotCounters shot_status;
std::atomic<bool> partner_body_driven{false};
std::mutex head_mutex;
struct HeadAnchor { std::optional<ShotVector> point; uint32_t level = 0; double time = 0; } head_anchor;
#if defined(KK_DEV_TOOLS)
void LogPoses(const View& v, double now, double& next) {
  if (v.state != SessionState::Connected) { next = now + 10; return; }
  if (now < next) return;
  next = now + 10;
  const float distance = v.local && v.partner && v.local->level == v.partner->level ?
      PartnerDistance(*v.local, *v.partner) : -1;
  REXLOG_INFO("KK co-op poses: local={} check={} sent={} received={} remote-level={} distance={:.1f}",
      v.local ? "yes" : "no", v.local_failure, v.poses.sent, v.poses.received,
      v.poses.remote_level ? std::to_string(*v.poses.remote_level) : "none", distance);
}
class Service {
 public:
  ~Service() { Stop(); }
  void Start(SessionConfig config, uint16_t port) {
    if (running_.exchange(true)) return;
    stop_ = false;
    { std::lock_guard lock(mutex_); local_cheats_ = config.cheats;
      view_ = {}; view_.role = config.role; view_.state = SessionState::Connecting; view_.updated = Now(); }
    worker_ = std::thread([this, config = std::move(config), port] {
      auto transport = CreateDevUdp(config.role == Role::Host, port);
      CoopSession session(config, *transport, [](SessionState s) {
        REXLOG_INFO("KK co-op: {}", StateName(s));
      });
      SessionState shown = SessionState::Idle;
      double next_pose_log = Now() + 10;
      while (!stop_.load()) {
        const double now = Now();
        std::optional<PlayerPose> local;
        enemies::Frame frame;
        CheatSet cheats;
        const char* failure;
        { std::lock_guard lock(mutex_); local = local_.pose; failure = local_.failure; frame = enemies_;
          cheats = local_cheats_.value_or(config.cheats); }
        if (local && now - local->time > .25) { local.reset(); failure = "publish-expired"; }
        session.Tick(now, local, &frame, &cheats);
        shots_.Exchange(session);
        flow_.Exchange(session, now);
        View view{config.role, session.State(), session.Rejection(), session.Stats(), local, session.Partner(now), now};
        view.local_failure = failure; view.poses = session.Poses();
        view.enemies = session.Enemies(now); view.enemies_sent = session.EnemiesSent();
        view.shots_sent = session.ShotsSent();
        view.host_cheats = session.HostCheats();
        view.partner_name = session.PartnerName();
        LogPoses(view, now, next_pose_log);
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
    std::lock_guard lock(mutex_); local_ = {}; view_ = {}; enemies_ = {}; local_cheats_.reset();
    shots_.Clear();
  }
  bool Running() const { return running_; }
  void Publish(PoseRead p) { std::lock_guard lock(mutex_); local_ = p; }
  View Snapshot() { std::lock_guard lock(mutex_); return view_; }
  bool EnemyReady(double now) {
    std::lock_guard lock(mutex_); return view_.state == SessionState::Connected && now-view_.updated <= .5;
  }
  void PublishEnemies(const enemies::Frame& frame) { std::lock_guard lock(mutex_); enemies_ = frame; }
  bool SubmitShot(const PendingShot& shot) { return shots_.Submit(shot); }
  void PublishCheats(CheatSet cheats) { std::lock_guard lock(mutex_); local_cheats_ = cheats; }
  void PublishInventory(const InventoryState& inventory) { shots_.PublishInventory(inventory); }
  std::vector<ReceivedShot> TakeShots() { return shots_.Take(); }
  bool BeginFlow(SessionStart start, std::vector<uint8_t> save) { return flow_.Begin(start, std::move(save)); }
  bool SubmitFlow(Message m) { return flow_.Submit(std::move(m)); }
  FlowView FlowSnapshot() const { return flow_.Snapshot(); }
 private:
  std::atomic<bool> running_{false}, stop_{false};
  std::thread worker_;
  std::mutex mutex_;
  PoseRead local_;
  View view_;
  enemies::Frame enemies_;
  ShotMailbox shots_;
  std::optional<CheatSet> local_cheats_;
  FlowMailbox flow_;
};
Service service;  // No thread, socket, guest reads, file access or allocation until explicitly started.
#endif

#if defined(KK_DEV_TOOLS)
bool DevRunning() {
  return service.Running();
}
bool DevSelected() {
  const char* env = std::getenv("KK_COOP_DEV");
  return !REXCVAR_GET(kk_coop_dev).empty() || (env && *env);
}
#endif
View CurrentView() {
#if defined(KK_DEV_TOOLS)
  if (service.Running()) return service.Snapshot();
#endif
  if (!steam) return {};
  const auto v = steam->SessionSnapshot();
  return {v.role, v.state, v.rejection, v.stats, v.local, v.partner, v.updated, v.local_failure, v.poses, v.enemies, v.enemies_sent, v.shots_sent, v.host_cheats, v.partner_name};
}

class PartnerOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PartnerOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
 protected:
  void OnDraw(ImGuiIO& io) override {
    if (REXCVAR_GET(kk_coop_enabled)) {
      const auto flow=SessionFlowSnapshot();
      if (flow.active && *flow.status) {
        auto* draw=ImGui::GetForegroundDrawList();
        const bool waiting=flow.phase!=FlowPhase::Playing || (flow.policy==ChapterPolicy::Observer && !flow.host);
        if (waiting) draw->AddRectFilled(ImVec2(0,0),ImGui::GetIO().DisplaySize,IM_COL32(10,12,16,255));
        char status[256];
        std::string line=flow.status;
        if (flow.phase==FlowPhase::Waiting) {
          const auto social=Social().Snapshot();
          for (const auto& member : social.members) if (member.id!=social.self) { line="Waiting for "+member.name+" to finish this chapter..."; break; }
        }
        std::snprintf(status,sizeof(status),"%s\nHost chapter %d | level %u | checkpoint %u",line.c_str(),
            FlowChapter(flow.world.level)+1,flow.world.level,flow.checkpoint);
        draw->AddText(ImVec2(36,40),IM_COL32(240,240,240,255),status);
        if (waiting || flow.policy!=ChapterPolicy::Together) return;
      }
      if (MoveToPartnerOffered()) {
        ImGui::SetNextWindowPos(ImVec2(24,190),ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(.9f);
        if (ImGui::Begin("##coop-catchup",nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration)) {
          ImGui::TextUnformatted("Your partner is far ahead.");
          ImGui::TextUnformatted("Recovery reloads both players at the host's last checkpoint.");
          if (ImGui::Button("Move to partner")) MoveToPartner();
        }
        ImGui::End();
      }
    }
    if (!REXCVAR_GET(kk_coop_enabled) || photo::Active() || freecam::HideHud() ||
        freecam::HideGameHud() || REXCVAR_GET(kk_hide_hud) || play::CoverScreen() ||
        !play::SessionActive() || play::StartLevelLoading() || play::PauseMenuOpen() || play::GamePausePageUp()) return;
    const View v = CurrentView();  // UI reads copied values only, never guest memory.
    if (v.state == SessionState::Idle) return;
    if (Now() - v.updated > .5 && v.state != SessionState::Ended) return;
#if defined(KK_DEV_TOOLS)
    char text[256];
    std::snprintf(text, sizeof(text), "Co-op %s: %s | %.0f ms | loss %.1f%% | invalid %llu | %s",
        v.role == Role::Host ? "host" : "guest", StateName(v.state), v.stats.ping_ms, v.stats.loss * 100,
        static_cast<unsigned long long>(v.stats.malformed), RejectName(v.rejection));
    auto* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImVec2(24, 64), IM_COL32(255, 230, 170, 255), text);
    const float status_distance = v.local && v.partner && v.local->level == v.partner->level ?
        PartnerDistance(*v.local, *v.partner) : -1;
    std::snprintf(text, sizeof(text), "Pose: %s (%s) | sent %llu recv %llu | level %s | distance %.1f",
        v.local ? "yes" : "no", v.local_failure,
        static_cast<unsigned long long>(v.poses.sent), static_cast<unsigned long long>(v.poses.received),
        v.poses.remote_level ? std::to_string(*v.poses.remote_level).c_str() : "none", status_distance);
    dl->AddText(ImVec2(24, 88), IM_COL32(255, 230, 170, 255), text);
    enemies::Counters counts;
    { std::lock_guard lock(enemy_status_mutex); counts = enemy_status; }
    std::snprintf(text, sizeof(text), "Enemies: sent %llu | matched %u | unmatched %u | driven %u",
        static_cast<unsigned long long>(v.enemies_sent), counts.matched, counts.unmatched, counts.driven);
    dl->AddText(ImVec2(24, 112), IM_COL32(255, 230, 170, 255), text);
    ShotCounters shots;
    { std::lock_guard lock(enemy_status_mutex); shots = shot_status; }
    std::snprintf(text, sizeof(text), "Shots: sent %llu | applied %llu | late %llu | rejected %llu (%s)",
        static_cast<unsigned long long>(v.shots_sent), static_cast<unsigned long long>(shots.applied),
        static_cast<unsigned long long>(shots.late),
        static_cast<unsigned long long>(shots.rejected), ShotRejectName(shots.last));
    dl->AddText(ImVec2(24, 136), IM_COL32(255, 230, 170, 255), text);
#else
    auto* dl = ImGui::GetForegroundDrawList();
#endif
    if (v.state != SessionState::Connected || !v.local || !v.partner || v.local->level != v.partner->level ||
        freecam::Active()) return;
#if defined(KK_DEV_TOOLS)
    const float dx = v.partner->x - v.local->x, dy = v.partner->y - v.local->y;
    const float partner_distance = PartnerDistance(*v.local, *v.partner);
    const float angle = std::remainder(std::atan2(dy, dx) - v.local->facing, 6.28318530718f);
    const char* directions[] = {"ahead", "ahead-left", "left", "behind-left", "behind", "behind-right", "right", "ahead-right"};
    const int sector = (int(std::lround(angle / .7853981634f)) + 8) % 8;
    if (PartnerHere(partner_distance)) std::snprintf(text, sizeof(text), "Partner: here");
    else std::snprintf(text, sizeof(text), "Partner: %.0f m, %s (Jack facing)", partner_distance, directions[sector]);
    if (partner_body_driven.load()) {
      const size_t used = std::char_traits<char>::length(text);
      std::snprintf(text + used, sizeof(text) - used, " (body)");
    }
    dl->AddText(ImVec2(24, 160), IM_COL32(180, 255, 210, 255), text);
    std::snprintf(text,sizeof(text),"Tag camera: %s",nametag::Status());
    dl->AddText(ImVec2(24, 184), IM_COL32(180, 255, 210, 255), text);
#endif
    const auto camera = nametag::Snapshot();
    if (!camera || Now()-camera->time > .15 || camera->level != v.local->level) return;
    const float distance = PartnerDistance(*v.local,*v.partner);
    const float alpha = nametag::Opacity(distance);
    if (alpha <= 0 || v.partner_name.empty()) return;
    auto head = nametag::AboveHead({v.partner->x,v.partner->y,v.partner->z},std::nullopt);
    {
      std::lock_guard lock(head_mutex);
      if (partner_body_driven.load() && head_anchor.point && head_anchor.level == v.partner->level &&
          Now()-head_anchor.time <= .15)
        head = {head_anchor.point->x,head_anchor.point->y,head_anchor.point->z};
    }
    const auto* viewport = ImGui::GetMainViewport();
    const auto picture = nametag::Picture(io.DisplaySize.x,io.DisplaySize.y,
        (16.f/9.f)*kk::UltrawideFactor(), REXCVAR_GET(present_letterbox));
    nametag::Rect rect{viewport->Pos.x+picture.x,viewport->Pos.y+picture.y,picture.w,picture.h};
    const float ui_scale = std::clamp(io.DisplaySize.y/720.f,.65f,2.f);
    const auto projected = nametag::Project(*camera,head,rect,32.f*ui_scale);
    if (!projected) return;
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize()*ui_scale*nametag::FontScale(distance);
    const ImU32 white = IM_COL32(245,245,240,int(alpha*255));
    const ImU32 shadow = IM_COL32(0,0,0,int(alpha*180));
    const ImVec2 measure = font->CalcTextSizeA(size,1e6f,0,v.partner_name.c_str());
    dl->PushClipRect(ImVec2(rect.x,rect.y),ImVec2(rect.x+rect.w,rect.y+rect.h),true);
    if (projected->on_screen) {
      // Clamp the full glyph bounds, so a long name at the edge stays readable.
      const auto bounds = nametag::TextBounds(*projected,measure.x,measure.y,rect,8*ui_scale);
      const ImVec2 pos(bounds.x,bounds.y);
      dl->AddText(font,size,ImVec2(pos.x+ui_scale,pos.y+ui_scale),shadow,v.partner_name.c_str());
      dl->AddText(font,size,pos,white,v.partner_name.c_str());
    } else {
      const float x = projected->x, y = projected->y, dx = projected->dx, dy = projected->dy;
      const float length = 11*ui_scale, half = 4*ui_scale;
      dl->AddTriangleFilled(ImVec2(x+dx*length*.5f,y+dy*length*.5f),
          ImVec2(x-dx*length*.5f-dy*half,y-dy*length*.5f+dx*half),
          ImVec2(x-dx*length*.5f+dy*half,y-dy*length*.5f-dx*half),white);
      char metres[32]; std::snprintf(metres,sizeof(metres),"%.0f m",distance);
      const auto extent = font->CalcTextSizeA(size,1e6f,0,metres);
      const float tx = std::clamp(x-dx*22*ui_scale-extent.x*.5f,rect.x+4*ui_scale,
          std::max(rect.x+4*ui_scale,rect.x+rect.w-extent.x-4*ui_scale));
      const float ty = std::clamp(y-dy*22*ui_scale-extent.y*.5f,rect.y+4*ui_scale,
          std::max(rect.y+4*ui_scale,rect.y+rect.h-extent.y-4*ui_scale));
      dl->AddText(font,size,ImVec2(tx+ui_scale,ty+ui_scale),shadow,metres);
      dl->AddText(font,size,ImVec2(tx,ty),white,metres);
    }
    dl->PopClipRect();
  }
};
}  // namespace
void Configure(const std::filesystem::path& user_data) {
  if (configured) return;
  configured = true;
#if defined(KK_DEV_TOOLS)
  if (const auto overrides = ParsePolicyOverrides(REXCVAR_GET(kk_coop_policy_override)))
    dev_policy_overrides = *overrides;
  else REXLOG_WARN("KK co-op: invalid policy override; using C29 defaults");
  if (REXCVAR_GET(kk_coop_enabled) && REXCVAR_GET(kk_social_fake)) {
    social_preview = MakeSocialPreview();
    return;
  }
#endif
  SteamRuntimeOptions options;
  options.cheats = kk::OwnCheats();
  options.enabled = REXCVAR_GET(kk_coop_enabled);
  options.executable_folder = rex::filesystem::GetExecutableFolder();
  options.startup_lobby = CaptureStartupLobby();
  options.request_ui_paint = [] { RequestUiPaint(); };
  options.read_compatibility = [user_data] {
    return Compatibility{kProtocolVersion, KK_VERSION, KK_COOP_BUILD, mods::GameplayDigest(user_data)};
  };
  if (options.enabled) options.compatibility = {kProtocolVersion, KK_VERSION, KK_COOP_BUILD, mods::GameplayDigest(user_data)};
  steam = std::make_unique<SteamRuntime>(std::move(options));
#if defined(KK_DEV_TOOLS)
  if (DevSelected()) Start(user_data);
#endif
}
void Start(const std::filesystem::path& user_data) {
  Configure(user_data);
  if (!REXCVAR_GET(kk_coop_enabled)) return;
#if defined(KK_DEV_TOOLS)
  if (service.Running()) return;
  if (social_preview) return;
  if (REXCVAR_GET(kk_steam_selftest)) return;
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
  config.cheats = kk::OwnCheats();
  config.display_name = config.role == Role::Host ? "Host" : "Guest";
  if (config.role == Role::Host) {
    std::random_device random;
    config.epoch = (uint64_t(random()) << 32) ^ random();
    if (!config.epoch) config.epoch = 1;
  }
  config.compatibility = {kProtocolVersion, KK_VERSION, KK_COOP_BUILD, mods::GameplayDigest(user_data)};
  REXLOG_INFO("KK co-op: dev link {} on 127.0.0.1:{}", role, port);
  service.Start(std::move(config), uint16_t(port));
#endif
}
ISocialService& Social() {
  // A pre-configuration caller can show disabled state, but cannot initialize Steam early.
  static SteamRuntime disabled(SteamRuntimeOptions{});
#if defined(KK_DEV_TOOLS)
  class DevSocial final : public ISocialService {
   public:
    SocialModel Snapshot() const override {
      const auto v=CurrentView();
      SocialModel m; m.self=v.role==Role::Host ? 1 : 2; m.owner=1; m.lobby=1;
      const bool connected=v.state==SessionState::Connected;
      m.status=connected ? SocialStatus::Lobby : SocialStatus::Starting;
      m.status_text=connected ? "Local dev partner connected" : "Waiting for local dev partner";
      m.members={{1,"Dev host",connected},{2,"Dev guest",connected}};
      // Dev peers opt into readiness when selecting the explicit local link.
      m.can_start=connected && v.role==Role::Host; return m;
    }
    bool Act(SocialCommand c) override {
      if (c.action==SocialAction::Start) return CanStartLobby(Snapshot());
      return c.action==SocialAction::Open || c.action==SocialAction::SetReady;
    }
  };
  static DevSocial dev;
  if (social_preview) return *social_preview;
  if (DevSelected()) return dev;
#endif
  return steam ? static_cast<ISocialService&>(*steam) : static_cast<ISocialService&>(disabled);
}
FlowView SessionFlowSnapshot() {
#if defined(KK_DEV_TOOLS)
  if (service.Running()) return service.FlowSnapshot();
#endif
  return steam ? steam->FlowSnapshot() : FlowView{};
}
bool BeginSessionFlow(SessionStart start, std::vector<uint8_t> save) {
#if defined(KK_DEV_TOOLS)
  if (service.Running()) return service.BeginFlow(start, std::move(save));
#endif
  return steam && steam->BeginFlow(start, std::move(save));
}
bool SubmitFlowMessage(Message m) {
#if defined(KK_DEV_TOOLS)
  if (service.Running()) return service.SubmitFlow(std::move(m));
#endif
  return steam && steam->SubmitFlow(std::move(m));
}
void Shutdown() {
  StopSessionFlow();
#if defined(KK_DEV_TOOLS)
  service.Stop();
#endif
  if (steam) steam->Shutdown();
  kk::SetSessionCheats({});
}
#if defined(KK_DEV_TOOLS)
bool SocialFakeScenario(std::string_view scenario) {
  if (!social_preview || !social_preview->Scenario(scenario)) return false;
  RequestUiPaint();
  return true;
}
#endif
void GameFrame(uint8_t* base) {
  RefreshSessionCheats();
  kk::PartnerBodyGameFrame(base);
  if (!REXCVAR_GET(kk_coop_enabled)) return;
  SessionReloadFrame(base);
  EnemyGameFrame();
#if defined(KK_DEV_TOOLS)
  if (service.Running()) service.Publish(ReadJack(base));
  else
#endif
  if (steam && steam->SessionSnapshot().state != SessionState::Idle) {
    const auto read = ReadJack(base);
    steam->PublishPose(read.pose, read.failure);
  }
}
void CreateOverlay(rex::ui::ImGuiDrawer* drawer) {
  if (REXCVAR_GET(kk_coop_enabled)) new PartnerOverlay(drawer);
}
EnemyLink EnemyLinkSnapshot() {
  if (!REXCVAR_GET(kk_coop_enabled)) return {};
  const auto v = CurrentView();
  const auto flow=SessionFlowSnapshot();
  if (flow.active && (flow.phase!=FlowPhase::Playing || flow.policy!=ChapterPolicy::Together)) return {};
  return {v.state == SessionState::Connected && Now()-v.updated <= .5, v.role, v.enemies, v.enemies_sent, v.shots_sent, v.partner};
}
bool EnemyConnectionReady() {
  if (!REXCVAR_GET(kk_coop_enabled)) return false;
  const auto flow=SessionFlowSnapshot();
  if (flow.active && (flow.phase!=FlowPhase::Playing || flow.policy!=ChapterPolicy::Together)) return false;
#if defined(KK_DEV_TOOLS)
  if (service.Running()) return service.EnemyReady(Now());
#endif
  return steam && steam->EnemyConnectionReady(Now());
}
void PublishEnemyFrame(const enemies::Frame& frame) {
#if defined(KK_DEV_TOOLS)
  if (service.Running()) { service.PublishEnemies(frame); return; }
#endif
  if (steam) steam->PublishEnemies(frame);
}
void SetEnemyCounters(enemies::Counters counts) {
  std::lock_guard lock(enemy_status_mutex); enemy_status = counts;
}
void RefreshSessionCheats() {
  if (!REXCVAR_GET(kk_coop_enabled)) { kk::SetSessionCheats({}); return; }
  const auto own = kk::OwnCheats();
#if defined(KK_DEV_TOOLS)
  if (service.Running()) service.PublishCheats(own);
  else
#endif
  if (steam) steam->PublishCheats(own);
  const auto view = CurrentView();
  const bool guest_session = view.role == Role::Guest && (view.state == SessionState::Connecting ||
      view.state == SessionState::Connected || view.state == SessionState::Lost);
  kk::SetSessionCheats(guest_session ? std::optional<CheatSet>(view.host_cheats.value_or(CheatSet{})) : std::nullopt);
}
bool SubmitShotIntent(const ShotIntent& shot, const InventoryState& inventory) {
  if (!EnemyConnectionReady()) return false;
#if defined(KK_DEV_TOOLS)
  if (service.Running()) return service.SubmitShot({shot, inventory});
#endif
  return steam && steam->SubmitShot({shot, inventory});
}
void PublishInventoryState(const InventoryState& inventory) {
  if (!EnemyConnectionReady()) return;
#if defined(KK_DEV_TOOLS)
  if (service.Running()) { service.PublishInventory(inventory); return; }
#endif
  if (steam) steam->PublishInventory(inventory);
}
std::vector<ReceivedShot> TakeShotIntents() {
#if defined(KK_DEV_TOOLS)
  if (service.Running()) return service.TakeShots();
#endif
  return steam ? steam->TakeShots() : std::vector<ReceivedShot>{};
}
void SetShotCounters(ShotCounters counts) { std::lock_guard lock(enemy_status_mutex); shot_status = counts; }
PuppetSnapshot PartnerBodySnapshot() {
  if (!REXCVAR_GET(kk_coop_enabled)) return {};
  const auto v = CurrentView();
  const auto flow=SessionFlowSnapshot();
  if (flow.active && (flow.phase!=FlowPhase::Playing || flow.policy!=ChapterPolicy::Together)) return {};
  return {true, Now() - v.updated <= .5, v.state, v.local, v.partner};
}
void SetPartnerBodyDriven(bool driven) {
  partner_body_driven.store(driven);
  if (!driven) { std::lock_guard lock(head_mutex); head_anchor = {}; }
}
void SetPartnerHead(std::optional<ShotVector> head, uint32_t level) {
  std::lock_guard lock(head_mutex); head_anchor = {head,level,Now()};
}
#if defined(KK_DEV_TOOLS)
bool SteamSelfTestRequested() { return REXCVAR_GET(kk_steam_selftest); }
int RunSteamSelfTest() {
  if (!steam || DevRunning()) { REXLOG_INFO("Steam self-test: unavailable while dev link selected"); return 1; }
  return steam->SelfTest([](const std::string& line) { REXLOG_INFO("{}", line); });
}
#endif
}  // namespace kk::coop
