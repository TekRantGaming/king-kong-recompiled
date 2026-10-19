#pragma once
#include "coop_session.h"
#include "coop_flow.h"
#include "steam_lobby.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace kk::coop {
struct SteamRuntimeOptions {
  bool enabled = false;
  std::filesystem::path executable_folder;
  Compatibility compatibility;
  // Optional UI-thread refresh before Create/Accept/Ready/Start. Never called by the worker.
  std::function<Compatibility()> read_compatibility;
  SocialId startup_lobby = 0;
  std::function<void()> request_ui_paint;
  CheatSet cheats;
};
struct SteamSessionView {
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
// Construction/configuration never loads Steam. First Open/Create/Accept starts a single
// joined worker which owns init, every flat call, callbacks, lobby and CoopSession until shutdown.
class SteamRuntime final : public ISocialService {
 public:
  explicit SteamRuntime(SteamRuntimeOptions options);
  ~SteamRuntime() override;
  SocialModel Snapshot() const override;
  bool Act(SocialCommand command) override;
  // failure must be a static label (ReadJackPose), never guest data or a temporary string.
  void PublishPose(std::optional<PlayerPose> pose, const char* failure = "ok");
  void PublishEnemies(const enemies::Frame& frame);
  bool SubmitShot(const PendingShot& shot);
  void PublishCheats(CheatSet cheats);
  void PublishInventory(const InventoryState& inventory);
  std::vector<ReceivedShot> TakeShots();
  SteamSessionView SessionSnapshot() const;
  bool EnemyConnectionReady(double now) const;
  bool BeginFlow(SessionStart start, std::vector<uint8_t> save);
  bool SubmitFlow(Message message);
  FlowView FlowSnapshot() const;
  void Shutdown();
#if defined(KK_DEV_TOOLS)
  // Blocks caller, but all Steam calls still belong to the worker. Never signs in or starts Steam.
  int SelfTest(std::function<void(const std::string&)> redacted_log);
#endif
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Pure startup capture. No Steam/DLL, no file write; only supported process argv is examined.
SocialId CaptureStartupLobby();
}  // namespace kk::coop
