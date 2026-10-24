#include "coop_runtime.h"
#include "cheats.h"
#include "coop_session.h"
#include "coop_direct_udp.h"
#include "coop_pose.h"
#include "coop_nametag.h"
#include "coop_observer.h"
#include "coop_story_game.h"
#include "coop_desync_game.h"
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
#include "ps2_movies.h"
#include "ui_paint.h"
#include "coop_enemies_game.h"
#include "coop_partner_body.h"
#include "launcher_text.h"

#if defined(KK_DEV_TOOLS)
REXCVAR_DEFINE_STRING(kk_coop_policy_override, "", "KK/Debug", "Chapter policies: entry:together|solo|observer,... (same on both peers)")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(kk_coop_dev, "", "KK/Debug", "Local co-op test link: host:<port> or join:<port>")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(kk_coop_debug_overlay, false, "KK/Debug", "Show detailed co-op diagnostics during gameplay")
    .debug_only();
REXCVAR_DEFINE_BOOL(kk_steam_selftest, false, "KK/Debug", "Test Steam services and exit without starting a game")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(kk_steam_loopback_test, false, "KK/Debug", "Probe Steam self-send for every co-op message and exit")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(kk_social_fake, false, "KK/Debug", "Use an offline Social panel preview")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(kk_direct_loopback, false, "KK/Debug", "Direct IP UI tests: loopback host and no Steam initialization")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif
REXCVAR_DEFINE_STRING(kk_coop_resume, "", "KK",
                      "Internal: the co-op hand over file (a name in the user folder) after the Play restart; never saved")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
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
uint32_t animation_fire = 0;
uint8_t animation_weapon = 0;
// Same validated reader in runtime and sparse-memory unit tests.
PoseRead ReadJack(const uint8_t* base) {
  if (!base) return {{}, "no-base"};
  if (play::CoverScreen()) return {{}, "cover"};
  auto read = ReadJackPose([base](uint32_t a) { return hfr::Load32(base, a); },
      [base](uint32_t a) { return hfr::Load16(base, a); },
      [base](uint32_t a) { return hfr::LoadF(base, a); }, Now());
  static PoseVelocity velocity;
  if (!read.pose) { velocity.Clear(); return read; }
  auto& p = *read.pose;
  velocity.Apply(p);
  if (const auto pitch = nametag::ViewPitch(base,p.level)) p.pitch = *pitch;
  p.fire = animation_fire; p.fire_weapon = animation_weapon;
  return read;
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
  uint64_t epoch = 0;
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
#endif
class Service {
 public:
  ~Service() { Stop(); }
  void Start(SessionConfig config, DirectUdpOptions options, bool direct = false) {
    if (running_.exchange(true)) return;
#if defined(KK_DEV_TOOLS)
    ConfigureDevUdpSimulation(options);
#endif
    stop_ = false;
    direct_ = direct; local_ready_ = remote_ready_ = starting_ = code_rejected_ = open_failed_ = false;
    choice_ = {};
    options_ = options;
    addresses_ = direct && options.host && options.address=="::" ? DirectLocalAddresses() : options.address;
    compatibility_ = config.compatibility;
    { std::lock_guard lock(mutex_); local_cheats_ = config.cheats;
      view_ = {}; view_.role = config.role; view_.state = SessionState::Connecting; view_.updated = Now();
      state_events_.clear();
#if defined(KK_DEV_TOOLS)
      wait_events_.clear(); wait_count_ = 0; next_pose_log_ = Now()+10;
#endif
    }
    worker_ = std::thread([this, config = std::move(config), options = std::move(options)] {
      auto transport = CreateDirectUdp(options);
      { std::lock_guard lock(mutex_); open_failed_ = transport->State()!=TransportState::Ready; }
      CoopSession session(config, *transport, [this](SessionState s) {
        // A synchronous log sink can stall on disk or another producer. Never
        // call it from the heartbeat worker. UI drains bounded copied events.
        std::lock_guard lock(mutex_);
        if (state_events_.size()==32) state_events_.erase(state_events_.begin());
        state_events_.push_back(s);
      });
      SessionState shown = SessionState::Idle;
#if defined(KK_DEV_TOOLS)
      double previous_poll = Now();
#endif
      while (!stop_.load()) {
        const double copy_started = Now();
        std::optional<PlayerPose> local;
        enemies::Frame frame;
        CheatSet cheats;
        const char* failure;
        { std::lock_guard lock(mutex_); local = local_.pose; failure = local_.failure; frame = enemies_;
          cheats = local_cheats_.value_or(config.cheats); }
        // Copying a frame may wait for the game thread or allocator. Stamp
        // received packets with the time we actually poll, not before that wait.
        const double now = Now();
#if defined(KK_DEV_TOOLS)
        const double poll_gap = now - previous_poll;
        previous_poll = now;
#endif
        if (local && now - local->time > .25) { local.reset(); failure = "publish-expired"; }
        bool ready; LobbyChoice choice;
        { std::lock_guard lock(mutex_); ready = local_ready_; choice = choice_; }
        if (direct_ && session.State() == SessionState::Connected) {
          if (config.role==Role::Host && choice.valid() && choice!=transport->Choice()) transport->SetChoice(choice);
          transport->SetReady(ready,choice.revision);
        }
        session.Tick(now, local, &frame, &cheats);
        shots_.Exchange(session);
        combat_.Exchange(session, now);
        ownership_.Exchange(session);
        story_.Exchange(session);
        camera_.Exchange(session, now);
        flow_.Exchange(session, now);
        const bool flow_active=flow_.Snapshot().active;
        View view{config.role, session.State(), session.Rejection(), session.Stats(), local, session.Partner(now), now};
        view.local_failure = failure; view.poses = session.Poses();
        view.enemies = session.Enemies(now); view.enemies_sent = session.EnemiesSent();
        view.shots_sent = session.ShotsSent();
        view.host_cheats = session.HostCheats();
        view.partner_name = session.PartnerName();
        view.epoch = session.Epoch();
#if defined(KK_DEV_TOOLS)
        if ((now-copy_started>.5 || poll_gap>.5) && wait_count_<8) {
          std::lock_guard lock(mutex_);
          wait_events_.push_back({now-copy_started,poll_gap}); ++wait_count_;
        }
#endif
        bool lobby_changed;
        { std::lock_guard lock(mutex_);
          lobby_changed=remote_ready_!=transport->PeerReady() || (!starting_ && flow_active);
          if (config.role==Role::Guest && choice_!=transport->Choice()) {
            choice_=transport->Choice(); local_ready_=false; lobby_changed=true;
          }
          view_=std::move(view); remote_ready_=transport->Choice()==choice_ && transport->PeerReady();
          code_rejected_=transport->CodeRejected(); starting_=starting_ || flow_active;
        }
        if (shown != session.State() || lobby_changed) { shown = session.State(); RequestUiPaint(); }
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
    combat_.Clear();
    ownership_.Clear();
    story_.Clear();
    camera_.Clear();
    flow_.Disconnect();
  }
  bool Running() const { return running_; }
  // UI-thread only, including while the launcher or a cover screen is up.
  // Release every shared lock before formatting or writing any log message.
  void LogDiagnostics() {
    std::vector<SessionState> states;
#if defined(KK_DEV_TOOLS)
    View view;
    std::vector<std::pair<double,double>> waits;
    const double now=Now();
    const bool pose_due=now>=next_pose_log_;
#endif
    {
      std::lock_guard lock(mutex_); states.swap(state_events_);
#if defined(KK_DEV_TOOLS)
      waits.swap(wait_events_); if (pose_due) view=view_;
#endif
    }
    for (const auto s : states) REXLOG_INFO("KK co-op: {}",StateName(s));
#if defined(KK_DEV_TOOLS)
    if (pose_due) LogPoses(view,now,next_pose_log_);
    for (const auto& [snapshot,gap] : waits)
      REXLOG_INFO("KK co-op worker: shared snapshot waited {:.3f}s; poll gap {:.3f}s; packet clock sampled after wait",snapshot,gap);
#endif
  }
  bool Direct() const { return running_ && direct_; }
  SocialModel SocialSnapshot() {
    std::lock_guard lock(mutex_);
    SocialModel m; m.direct=true; m.self=view_.role==Role::Host ? 1 : 2; m.owner=1;
    m.choice=choice_;
    m.direct_port=options_.port; m.direct_code=options_.join_code; m.direct_addresses=addresses_;
    const bool connected=view_.state==SessionState::Connected;
    const bool ended=view_.state==SessionState::Ended;
    m.lobby=ended ? 0 : (view_.epoch ? view_.epoch : 1);
    m.status=ended ? SocialStatus::Ended : connected ? (starting_ ? SocialStatus::Starting : SocialStatus::Lobby) : SocialStatus::Joining;
    m.start_requested=starting_;
    m.status_text=ended ? "Direct connection ended" : connected ? "Direct partner connected" : "Waiting for Direct IP partner";
    if (code_rejected_) m.last_error="Wrong join code";
    else if (open_failed_) m.last_error="Could not open Direct connection. Check the IP and UDP port.";
    else if (view_.rejection!=RejectReason::None) m.last_error="Lobby is not compatible";
    m.members.push_back({m.self,m.self==1 ? "Host" : "Guest",local_ready_});
    if (connected) m.members.push_back({m.self==1 ? 2ull : 1ull,view_.partner_name,remote_ready_});
    m.can_start=CanStartLobby(m); return m;
  }
  bool SocialAct(SocialCommand command) {
    if (command.action==SocialAction::Open) return true;
    if (command.action==SocialAction::Leave) { Stop(); return true; }
    if (command.action==SocialAction::SetReady || command.action==SocialAction::Start) {
      const Compatibility current{kProtocolVersion,KK_VERSION,KK_COOP_BUILD,mods::GameplayDigest(play::UserDir())};
      if (CheckCompatibility(compatibility_,current)!=RejectReason::None) { Stop(); return false; }
    }
    const auto m=SocialSnapshot();
    std::lock_guard lock(mutex_);
    if (command.action==SocialAction::SelectChapter && m.owner==m.self && !starting_ &&
        command.choice.chapter>=0 && command.choice.chapter<42) {
      if (choice_.valid() && choice_.chapter==command.choice.chapter && choice_.continue_game==command.choice.continue_game) return true;
      choice_={choice_.revision+1,command.choice.chapter,command.choice.continue_game};
      local_ready_=remote_ready_=false; RequestUiPaint(); return true;
    }
    if (command.action==SocialAction::SetReady && m.status==SocialStatus::Lobby &&
        choice_.valid() && command.choice==choice_) { local_ready_=command.ready; return true; }
    if (command.action==SocialAction::Start && command.choice==choice_ && choice_.valid() && m.can_start && view_.state==SessionState::Connected && local_ready_ && remote_ready_) return true;
    return false;
  }
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
  bool SubmitReceipt(const ShotReceipt& receipt) { return shots_.SubmitReceipt(receipt); }
  std::vector<ShotReceipt> TakeReceipts() { return shots_.TakeReceipts(); }
  bool SubmitCombat(Message m, double now) { return combat_.Submit(std::move(m), now); }
  std::vector<ReceivedCombat> TakeCombat() { return combat_.Take(); }
  void ClearCombat() { combat_.Clear(); }
  bool SubmitOwnership(Message m) { return ownership_.Submit(std::move(m)); }
  std::vector<Message> TakeOwnership() { return ownership_.Take(); }
  void ClearOwnership() { ownership_.Clear(); }
  bool SubmitStory(Message m) { return story_.Submit(std::move(m)); }
  std::vector<Message> TakeStory() { return story_.Take(); }
  void PublishCamera(std::optional<CameraPose> camera) { camera_.Publish(std::move(camera)); }
  std::optional<CameraPose> HostCamera(double now) const { return camera_.Sample(now); }
  CameraDiagnostics CameraInfo() const { return camera_.Info(); }
  bool BeginFlow(SessionStart start, std::vector<uint8_t> save) {
    if (!flow_.Begin(start,std::move(save))) return false;
    std::lock_guard lock(mutex_); starting_=true; return true;
  }
  bool SubmitFlow(Message m) { return flow_.Submit(std::move(m)); }
  FlowView FlowSnapshot() const { return flow_.Snapshot(); }
  bool BeginPlan(const StartPlan& plan) {
    if (!flow_.BeginPlan(plan)) return false;
    std::lock_guard lock(mutex_); starting_=true; return true;
  }
  handover::PlanPhase PlanState(StartPlan* plan, const char** failure) const { return flow_.PlanState(plan,failure); }
  DirectUdpOptions Options() { std::lock_guard lock(mutex_); return options_; }
 private:
  std::atomic<bool> running_{false}, stop_{false};
  std::thread worker_;
  std::mutex mutex_;
  std::vector<SessionState> state_events_; // 32 diagnostic transitions, never gameplay messages.
#if defined(KK_DEV_TOOLS)
  std::vector<std::pair<double,double>> wait_events_; // At most 8 per session.
  unsigned wait_count_ = 0;
  double next_pose_log_ = 0; // UI-thread log cadence.
#endif
  bool direct_ = false, local_ready_ = false, remote_ready_ = false, starting_ = false, code_rejected_ = false, open_failed_ = false;
  Compatibility compatibility_;
  DirectUdpOptions options_;
  LobbyChoice choice_;
  std::string addresses_;
  PoseRead local_;
  View view_;
  enemies::Frame enemies_;
  ShotMailbox shots_;
  CombatMailbox combat_;
  OwnershipMailbox ownership_;
  StoryMailbox story_;
  CameraMailbox camera_;
  std::optional<CheatSet> local_cheats_;
  FlowMailbox flow_;
};
Service service;  // No thread or socket until explicitly started.

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
  if (service.Running()) return service.Snapshot();
  if (!steam) return {};
  const auto v = steam->SessionSnapshot();
  return {v.role, v.state, v.rejection, v.stats, v.local, v.partner, v.updated, v.local_failure, v.poses, v.enemies, v.enemies_sent, v.shots_sent, v.host_cheats, v.partner_name, v.epoch};
}

class PartnerOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PartnerOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
 protected:
  void OnDraw(ImGuiIO& io) override {
    service.LogDiagnostics();
    if (REXCVAR_GET(kk_coop_enabled)) {
      // Movies can run inside the synchronous loader, before Loaded/LoadRelease
      // or another GameFrame. Never put session UI over their presenter.
      if (movies::LoaderWaiting()) return;
      const auto flow=SessionFlowSnapshot();
      if (flow.active && (*flow.status || observer::Active())) {
        auto* draw=ImGui::GetForegroundDrawList();
        const bool waiting=flow.phase!=FlowPhase::Playing ||
            (!flow.host && (flow.policy==ChapterPolicy::Observer || observer::Active()) && observer::StatusScreen());
        // Flow phase describes peer synchronization, not what is on screen.
        // Native intro movies also play inside uni_go while phase is Loading.
        // Keep every wait/fallback non-occluding, even without a camera tick.
        std::string line=flow.phase==FlowPhase::Playing && !flow.host &&
            (flow.policy==ChapterPolicy::Observer || observer::Active()) ? observer::Status() : flow.status;
        if (flow.phase==FlowPhase::Waiting) {
          const auto social=Social().Snapshot();
          for (const auto& member : social.members) if (member.id!=social.self) { line="Waiting for "+member.name+" to finish this chapter..."; break; }
        }
        const auto* vp=ImGui::GetMainViewport();
        const float size=std::clamp(io.DisplaySize.y/720.f,.65f,1.5f)*16.f;
        const ImVec2 pos(vp->Pos.x+12,vp->Pos.y+12);
        draw->PushClipRect(pos,ImVec2(vp->Pos.x+io.DisplaySize.x-12,pos.y+size+2),true);
        draw->AddText(ImGui::GetFont(),size,pos,IM_COL32(240,240,240,110),line.c_str());
        draw->PopClipRect();
        if (waiting || observer::Active() || flow.policy!=ChapterPolicy::Together) return;
      }
      if (observer::CutsceneActive()) return;
      if (MoveToPartnerOffered()) {
        ImGui::SetNextWindowPos(ImVec2(24,190),ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(.9f);
        if (ImGui::Begin("##coop-catchup",nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration)) {
          const bool checkpoint_recovery=PrivateCatchupOffered();
          ImGui::TextUnformatted(checkpoint_recovery || StoryNeedsMove() ? "Your world needs to catch up with the host." : "Your partner is far ahead.");
          ImGui::TextUnformatted(checkpoint_recovery ? "Catch up at the host's checkpoint and rejoin your partner." :
              "Move beside the host and let your story catch up.");
          if (ImGui::Button(checkpoint_recovery ? "Catch up with partner" : "Move to partner")) MoveToPartner(checkpoint_recovery);
        }
        ImGui::End();
      }
      if (CatchupNotice() && flow.active && !flow.host && flow.phase==FlowPhase::Playing) {
        const auto view=CurrentView();
        const std::string notice="Catching up with "+(view.partner_name.empty() ? std::string("Partner") : view.partner_name);
        ImGui::GetForegroundDrawList()->AddText(ImVec2(36,72),IM_COL32(240,240,240,255),notice.c_str());
      }
    }
    if (!REXCVAR_GET(kk_coop_enabled) || photo::Active() || freecam::HideHud() ||
        freecam::HideGameHud() || REXCVAR_GET(kk_hide_hud) || play::CoverScreen() ||
        !play::SessionActive() || play::StartLevelLoading() || play::PauseMenuOpen() || play::GamePausePageUp()) return;
    const View v = CurrentView();  // UI reads copied values only, never guest memory.
    const auto life_flow=SessionFlowSnapshot();
    if (notice_epoch_!=v.epoch || notice_generation_!=life_flow.world.generation) {
      notice_epoch_=v.epoch; notice_generation_=life_flow.world.generation;
      notice_number_=0; notice_until_=0;
    }
    const auto& life=life_flow.partner_life;
    if (life.notice!=notice_number_) {
      notice_number_=life.notice; notice_until_=Now()+4;
      notice_=text::F(life.downed ? "{0} is down" : "{0} is back",
          {v.partner_name.empty() ? "Partner" : v.partner_name});
    }
    if (Now()<notice_until_) ImGui::GetForegroundDrawList()->AddText(ImVec2(36,40),IM_COL32(240,240,240,255),notice_.c_str());
    if (v.state == SessionState::Idle) return;
    if (Now() - v.updated > .5 && v.state != SessionState::Ended) return;
#if defined(KK_DEV_TOOLS)
    char text[256];
    auto* dl = ImGui::GetForegroundDrawList();
    // Some native openings animate Jack through his normal camera, so they
    // cannot be identified from scripted camera packets. Keep diagnostics opt-in.
    if (REXCVAR_GET(kk_coop_debug_overlay)) {
      std::snprintf(text, sizeof(text), "Co-op %s: %s | %.0f ms | loss %.1f%% | invalid %llu | %s",
          v.role == Role::Host ? "host" : "guest", StateName(v.state), v.stats.ping_ms, v.stats.loss * 100,
          static_cast<unsigned long long>(v.stats.malformed), RejectName(v.rejection));
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
      std::snprintf(text, sizeof(text), "Enemies: owned-by-me %u | owned-by-partner %u | matched %u | unmatched %u | driven %u",
          counts.owned_me, counts.owned_partner, counts.matched, counts.unmatched, counts.driven);
      dl->AddText(ImVec2(24, 112), IM_COL32(255, 230, 170, 255), text);
      ShotCounters shots;
      { std::lock_guard lock(enemy_status_mutex); shots = shot_status; }
      std::snprintf(text, sizeof(text), "Shots: sent %llu | applied %llu | late %llu | rejected %llu (%s)",
          static_cast<unsigned long long>(v.shots_sent), static_cast<unsigned long long>(shots.applied),
          static_cast<unsigned long long>(shots.late),
          static_cast<unsigned long long>(shots.rejected), ShotRejectName(shots.last));
      dl->AddText(ImVec2(24, 136), IM_COL32(255, 230, 170, 255), text);
    }
#else
    auto* dl = ImGui::GetForegroundDrawList();
#endif
    if (v.state != SessionState::Connected || !v.local || !v.partner || v.local->level != v.partner->level ||
        freecam::Active()) return;
#if defined(KK_DEV_TOOLS)
    if (REXCVAR_GET(kk_coop_debug_overlay)) {
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
    }
#endif
    const auto camera = nametag::Snapshot();
    if (!camera || Now()-camera->time > .15 || camera->level != v.local->level) return;
    const float distance = PartnerDistance(*v.local,*v.partner);
    // A nearby stand-in must keep its username even when players overlap.
    const float alpha = partner_body_driven.load() && distance < 2.f ?
        std::max(.35f, nametag::Opacity(distance)) : nametag::Opacity(distance);
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
 private:
  uint64_t notice_epoch_=0, notice_number_=0;
  uint32_t notice_generation_=0;
  double notice_until_=0;
  std::string notice_;
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
  REXLOG_INFO("KK co-op: enabled={} (Social {})", options.enabled, options.enabled ? "available" : "disabled");
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
  if (SteamSelfTestRequested()) return;
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
  config.join_code = "LDEV23";
  service.Start(std::move(config), {role == "host", "127.0.0.1", uint16_t(port), "LDEV23"});
#endif
}
ISocialService& Social() {
  // A pre-configuration caller can show disabled state, but cannot initialize Steam early.
  static SteamRuntime disabled(SteamRuntimeOptions{});
  class DirectSocial final : public ISocialService {
   public:
    SocialModel Snapshot() const override { return service.SocialSnapshot(); }
    bool Act(SocialCommand c) override { return service.SocialAct(c); }
  };
  static DirectSocial direct;
  if (service.Direct()) return direct;
#if defined(KK_DEV_TOOLS)
  if (REXCVAR_GET(kk_direct_loopback)) return disabled;
  class DevSocial final : public ISocialService {
    LobbyChoice choice_;
   public:
    SocialModel Snapshot() const override {
      const auto v=CurrentView();
      SocialModel m; m.self=v.role==Role::Host ? 1 : 2; m.owner=1; m.lobby=1;
      m.choice=choice_;
      const bool connected=v.state==SessionState::Connected;
      m.status=connected ? SocialStatus::Lobby : SocialStatus::Starting;
      m.status_text=connected ? "Local dev partner connected" : "Waiting for local dev partner";
      m.members={{1,"Dev host",connected},{2,"Dev guest",connected}};
      // Dev peers opt into readiness when selecting the explicit local link.
      m.can_start=connected && v.role==Role::Host; return m;
    }
    bool Act(SocialCommand c) override {
      if (c.action==SocialAction::SelectChapter) { choice_={choice_.revision+1,c.choice.chapter,c.choice.continue_game}; return true; }
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
namespace {
bool OpenDirect(DirectUdpOptions options, uint64_t resume_token = 0) {
  if (!REXCVAR_GET(kk_coop_enabled) || SessionInProcess() || !ValidJoinCode(options.join_code)) return false;
  if (steam && steam->Snapshot().lobby) return false;
#if defined(KK_DEV_TOOLS)
  if (DevSelected() || social_preview) return false;
  if (REXCVAR_GET(kk_direct_loopback)) {
    if (options.host) options.address="127.0.0.1";
    else if (options.address!="127.0.0.1" && options.address!="::1") return false;
  }
#endif
  // Cancel queued Steam creation/join work before switching. Leave cannot initialize Steam.
  if (steam) steam->Act({SocialAction::Leave});
  service.Stop();
  SessionConfig config;
  config.enabled=true; config.role=options.host ? Role::Host : Role::Guest;
  config.display_name=options.host ? "Host" : "Guest";
  config.cheats=kk::OwnCheats(); config.join_code=options.join_code;
  config.wait_for_peer=options.host;
  config.compatibility={kProtocolVersion,KK_VERSION,KK_COOP_BUILD,mods::GameplayDigest(play::UserDir())};
  if (options.host) {
    std::random_device random; config.epoch=(uint64_t(random())<<32)^random();
    if (!config.epoch) config.epoch=1;
  }
  // C80: a resumed session continues the old one: its token is the host's epoch, checked in both Hellos.
  config.resume_token=resume_token;
  if (resume_token && options.host) config.epoch=resume_token;
  REXLOG_INFO("KK co-op: Direct IP {} selected{}",options.host ? "host" : "join",resume_token ? " (resumed session)" : "");
  service.Start(std::move(config),std::move(options),true);
  return true;
}
}
bool HostDirect(uint16_t port,std::string code) { return OpenDirect({true,"::",port,std::move(code)}); }
bool JoinDirect(std::string address,uint16_t port,std::string code) {
  return OpenDirect({false,std::move(address),port,std::move(code)});
}
LinkIdentity CurrentLink() {
  LinkIdentity l;
  const auto v=CurrentView();
  l.role=v.role; l.state=v.state; l.rejection=v.rejection; l.partner_name=v.partner_name;
  if (service.Running()) {
    const auto o=service.Options();
    l.direct=service.Direct(); l.port=o.port; l.address=o.address; l.code=o.join_code;
    return l;
  }
  if (steam) {
    const auto m=steam->Snapshot();
    l.self=m.self;
    for (const auto& member : m.members) if (member.id!=m.self) { l.partner=member.id; if (l.partner_name.empty()) l.partner_name=member.name; }
    l.rejection=steam->SessionSnapshot().rejection;
  }
  return l;
}
bool BeginStartPlan(const StartPlan& plan) {
  if (service.Running()) return service.Direct() && service.BeginPlan(plan);
  return steam && steam->BeginPlan(plan);
}
handover::PlanPhase StartPlanState(StartPlan* plan, const char** failure) {
  if (service.Running()) return service.PlanState(plan,failure);
  return steam ? steam->PlanState(plan,failure) : handover::PlanPhase::Idle;
}
void ReleaseLinkForHandover() {
  // The new process binds the same Direct port and is the same Steam user: this one lets go first.
  REXLOG_INFO("KK co-op hand over: this process releases its link before the restart");
  service.Stop();
  if (steam) steam->Shutdown();
}
bool ResumeLink(const handover::File& f) {
  if (f.plan.transport==StartTransport::Direct) {
    DirectUdpOptions o{f.host,f.host ? std::string("::") : f.address,f.plan.port,f.code};
    return OpenDirect(std::move(o),f.plan.token);
  }
  if (!steam) return false;
  const uint64_t partner=f.host ? f.plan.guest_id : f.plan.host_id;
  return steam->Resume(f.host,partner,f.plan.token);
}
void StopResumeLink() {
  service.Stop();
  if (steam) steam->Act({SocialAction::Leave});
}
FlowView SessionFlowSnapshot() {
  if (service.Running()) return service.FlowSnapshot();
  return steam ? steam->FlowSnapshot() : FlowView{};
}
bool BeginSessionFlow(SessionStart start, std::vector<uint8_t> save) {
  if (service.Running()) return service.BeginFlow(start, std::move(save));
  return steam && steam->BeginFlow(start, std::move(save));
}
bool SubmitFlowMessage(Message m) {
  if (service.Running()) return service.SubmitFlow(std::move(m));
  return steam && steam->SubmitFlow(std::move(m));
}
void Shutdown() {
  StopSessionFlow();
  service.Stop();
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
  StoryGameFrame(base);
  WorldGuardFrame(base);
  if (!REXCVAR_GET(kk_coop_enabled)) { observer::GameFrame(base); return; }
  SessionReloadFrame(base);
  observer::GameFrame(base);
  EnemyGameFrame();
  if (service.Running()) service.Publish(ReadJack(base));
  else
  if (steam && steam->SessionSnapshot().state != SessionState::Idle) {
    const auto read = ReadJack(base);
    steam->PublishPose(read.pose, read.failure);
  }
}
void PublishHostCamera(std::optional<CameraPose> camera) {
  if (service.Running()) { service.PublishCamera(std::move(camera)); return; }
  if (steam) steam->PublishCamera(std::move(camera));
}
std::optional<CameraPose> HostCameraSnapshot(double now) {
  if (!REXCVAR_GET(kk_coop_enabled)) return {};
  if (service.Running()) return service.HostCamera(now);
  return steam ? steam->HostCamera(now) : std::nullopt;
}
bool CameraConnectionReady() {
  if (!REXCVAR_GET(kk_coop_enabled)) return false;
  const auto v=CurrentView();
  return v.state==SessionState::Connected && Now()-v.updated<=.5;
}
CameraDiagnostics HostCameraDiagnostics() {
  if (!REXCVAR_GET(kk_coop_enabled)) return {};
  if (service.Running()) return service.CameraInfo();
  return steam ? steam->CameraInfo() : CameraDiagnostics{};
}
void CreateOverlay(rex::ui::ImGuiDrawer* drawer) {
  if (REXCVAR_GET(kk_coop_enabled)) new PartnerOverlay(drawer);
}
EnemyLink EnemyLinkSnapshot() {
  if (!REXCVAR_GET(kk_coop_enabled)) return {};
  const auto v = CurrentView();
  const auto flow=SessionFlowSnapshot();
  if (flow.active && (flow.phase!=FlowPhase::Playing || flow.policy!=ChapterPolicy::Together)) return {};
  return {v.state == SessionState::Connected && Now()-v.updated <= .5, v.role, v.enemies, v.enemies_sent, v.shots_sent, v.partner, v.epoch, v.state};
}
bool EnemyConnectionReady() {
  if (!REXCVAR_GET(kk_coop_enabled)) return false;
  const auto flow=SessionFlowSnapshot();
  if (flow.active && (flow.phase!=FlowPhase::Playing || flow.policy!=ChapterPolicy::Together)) return false;
  if (service.Running()) return service.EnemyReady(Now());
  return steam && steam->EnemyConnectionReady(Now());
}
void PublishEnemyFrame(const enemies::Frame& frame) {
  if (service.Running()) { service.PublishEnemies(frame); return; }
  if (steam) steam->PublishEnemies(frame);
}
void SetEnemyCounters(enemies::Counters counts) {
  std::lock_guard lock(enemy_status_mutex); enemy_status = counts;
}
void RefreshSessionCheats() {
  if (!REXCVAR_GET(kk_coop_enabled)) { kk::SetSessionCheats({}); return; }
  const auto own = kk::OwnCheats();
  if (service.Running()) service.PublishCheats(own);
  else
  if (steam) steam->PublishCheats(own);
  const auto view = CurrentView();
  const bool guest_session = view.role == Role::Guest && (view.state == SessionState::Connecting ||
      view.state == SessionState::Connected || view.state == SessionState::Lost);
  kk::SetSessionCheats(guest_session ? std::optional<CheatSet>(view.host_cheats.value_or(CheatSet{})) : std::nullopt);
}
bool SubmitShotIntent(const ShotIntent& shot, const InventoryState& inventory) {
  if (!EnemyConnectionReady()) return false;
  if (service.Running()) return service.SubmitShot({shot, inventory});
  return steam && steam->SubmitShot({shot, inventory});
}
void PublishInventoryState(const InventoryState& inventory) {
  if (!EnemyConnectionReady()) return;
  if (service.Running()) { service.PublishInventory(inventory); return; }
  if (steam) steam->PublishInventory(inventory);
}
std::vector<ReceivedShot> TakeShotIntents() {
  if (service.Running()) return service.TakeShots();
  return steam ? steam->TakeShots() : std::vector<ReceivedShot>{};
}
void SetShotCounters(ShotCounters counts) { std::lock_guard lock(enemy_status_mutex); shot_status = counts; }
bool SubmitShotReceipt(const ShotReceipt& receipt) {
  if (service.Running()) return service.SubmitReceipt(receipt);
  return steam && steam->SubmitShotReceipt(receipt);
}
std::vector<ShotReceipt> TakeShotReceipts() {
  if (service.Running()) return service.TakeReceipts();
  return steam ? steam->TakeShotReceipts() : std::vector<ShotReceipt>{};
}
bool SubmitCombat(Message message, double now) {
  if (!EnemyConnectionReady()) return false;
  if (service.Running()) return service.SubmitCombat(std::move(message), now);
  return steam && steam->SubmitCombat(std::move(message), now);
}
std::vector<ReceivedCombat> TakeCombat() {
  if (service.Running()) return service.TakeCombat();
  return steam ? steam->TakeCombat() : std::vector<ReceivedCombat>{};
}
void ClearCombat() {
  if (service.Running()) { service.ClearCombat(); return; }
  if (steam) steam->ClearCombat();
}
bool SubmitOwnership(Message message) {
  if (!EnemyConnectionReady()) return false;
  if (service.Running()) return service.SubmitOwnership(std::move(message));
  return steam && steam->SubmitOwnership(std::move(message));
}
std::vector<Message> TakeOwnership() {
  if (service.Running()) return service.TakeOwnership();
  return steam ? steam->TakeOwnership() : std::vector<Message>{};
}
void ClearOwnership() {
  if (service.Running()) { service.ClearOwnership(); return; }
  if (steam) steam->ClearOwnership();
}
bool SubmitStory(Message message) {
  if (!EnemyConnectionReady()) return false;
  if (service.Running()) return service.SubmitStory(std::move(message));
  return steam && steam->SubmitStory(std::move(message));
}
uint64_t StorySessionEpoch() { return CurrentView().epoch; }
std::vector<Message> TakeStory() {
  if (service.Running()) return service.TakeStory();
  return steam ? steam->TakeStory() : std::vector<Message>{};
}
PuppetSnapshot PartnerBodySnapshot() {
  if (!REXCVAR_GET(kk_coop_enabled)) return {};
  const auto v = CurrentView();
  const auto flow=SessionFlowSnapshot();
  if (flow.active && (flow.phase!=FlowPhase::Playing || flow.policy!=ChapterPolicy::Together)) return {};
  return {true, Now() - v.updated <= .5, v.state, v.local, v.partner};
}
void PartnerAnimationShot(uint8_t weapon) {
  if (weapon < 1 || weapon > 4) return;
  ++animation_fire; animation_weapon = weapon;
}
void SetPartnerBodyDriven(bool driven) {
  partner_body_driven.store(driven);
  if (!driven) { std::lock_guard lock(head_mutex); head_anchor = {}; }
}
void SetPartnerHead(std::optional<ShotVector> head, uint32_t level) {
  std::lock_guard lock(head_mutex); head_anchor = {head,level,Now()};
}
#if defined(KK_DEV_TOOLS)
bool SteamSelfTestRequested() { return REXCVAR_GET(kk_steam_selftest) || REXCVAR_GET(kk_steam_loopback_test); }
int RunSteamSelfTest() {
  if (!steam || DevSelected() || social_preview) { REXLOG_INFO("Steam self-test: unavailable while dev link or preview selected"); return 1; }
  return steam->SelfTest([](const std::string& line) { REXLOG_INFO("{}", line); }, REXCVAR_GET(kk_steam_loopback_test));
}
#endif
}  // namespace kk::coop
