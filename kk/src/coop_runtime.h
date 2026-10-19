#pragma once

#include <cstdint>
#include <filesystem>
#include "steam_lobby.h"
#include "coop_enemies.h"
#include "coop_session.h"
#include "coop_flow.h"
#include "play_session.h"

namespace rex::ui { class ImGuiDrawer; }
struct PPCContext;
namespace kk::coop {
// App lifetime. Disabled never loads Steam, starts a worker/socket, or reads guest memory.
void Configure(const std::filesystem::path& user_data);
void Start(const std::filesystem::path& user_data);
// Available to the launcher before Play. Open is the explicit lazy Steam opt-in.
ISocialService& Social();
// UI-thread start handoff; all peers launch through the existing Play callback.
void SelectSessionStart(const play::StartRequest& request);
bool RequestSessionStart();
bool TakeSessionLaunch(play::StartRequest& request);
FlowView SessionFlowSnapshot();
bool SubmitFlowMessage(Message message);
std::filesystem::path SessionUserDir(const std::filesystem::path& personal);
bool SessionInProcess();
void PublishHostCamera(std::optional<CameraPose> camera);
std::optional<CameraPose> HostCameraSnapshot(double now);
CameraDiagnostics HostCameraDiagnostics();
bool CameraConnectionReady();
void ApplySessionSettings();  // transient host settings after mods, never persisted
// Game-thread control at the synchronous uni_go load boundary.
bool SessionBeforeGo(PPCContext& ctx, uint8_t* base, uint32_t g);
void SessionAfterGo(uint8_t* base, uint32_t g, bool loaded);
void SessionCheckpoint(uint32_t level, uint32_t checkpoint);
void SessionReloadFrame(uint8_t* base);
#if defined(KK_DEV_TOOLS)
bool SessionDevChapterEnd();  // same finish messages as a native chapter exit, no hero dependency
#endif
// Private death reload does not advance the shared load barrier or enemy visit.
bool SessionPrivateReload();
void SessionReviveBeforeAi(PPCContext& ctx, uint8_t* base);
int SessionCheats();  // -1 outside a C24 launch, otherwise immutable host settings
void StopSessionFlow();
bool MoveToPartnerOffered();
void MoveToPartner();
struct EnemyLink {
  bool connected = false;
  Role role = Role::Host;
  enemies::Frame remote;
  uint64_t sent = 0;
  uint64_t shots_sent = 0;
  std::optional<PlayerPose> partner;
  uint64_t epoch = 0;
};
EnemyLink EnemyLinkSnapshot();
bool EnemyConnectionReady();
void PublishEnemyFrame(const enemies::Frame& frame);
void SetEnemyCounters(enemies::Counters counters);
bool SubmitShotIntent(const ShotIntent& shot, const InventoryState& inventory);
void PublishInventoryState(const InventoryState& inventory);
// UI/game callers publish own choices and apply the worker's value-only overlay.
void RefreshSessionCheats();
std::vector<ReceivedShot> TakeShotIntents();
void SetShotCounters(ShotCounters counters);
bool SubmitCombat(Message message, double now);
std::vector<ReceivedCombat> TakeCombat();
void ClearCombat();
bool SubmitOwnership(Message message);
std::vector<Message> TakeOwnership();
void ClearOwnership();
bool SubmitStory(Message message);
std::vector<Message> TakeStory();
// Value-only snapshot, including CoopSession's interpolated pose. No transport work.
struct PuppetSnapshot {
  bool enabled = false, fresh = false;
  SessionState state = SessionState::Idle;
  std::optional<PlayerPose> local, partner;
};
PuppetSnapshot PartnerBodySnapshot();
void SetPartnerBodyDriven(bool driven);
// Game-thread copied head anchor; cleared whenever body driving stops.
void SetPartnerHead(std::optional<ShotVector> head, uint32_t level);
#if defined(KK_DEV_TOOLS)
bool SocialFakeScenario(std::string_view scenario);
bool SteamSelfTestRequested();
int RunSteamSelfTest();
#endif
void Shutdown();
// Only caller: the existing per-frame game-thread hook in hfr_frame_time.cpp.
void GameFrame(uint8_t* base);
void CreateOverlay(rex::ui::ImGuiDrawer* drawer);
}  // namespace kk::coop
