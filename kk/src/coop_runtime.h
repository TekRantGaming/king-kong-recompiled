#pragma once

#include <cstdint>
#include <filesystem>
#include "steam_lobby.h"
#if defined(KK_DEV_TOOLS)
#include "coop_session.h"
#endif

namespace rex::ui { class ImGuiDrawer; }
namespace kk::coop {
// App lifetime. Disabled never loads Steam, starts a worker/socket, or reads guest memory.
void Configure(const std::filesystem::path& user_data);
void Start(const std::filesystem::path& user_data);
// Available to the launcher before Play. Open is the explicit lazy Steam opt-in.
ISocialService& Social();
#if defined(KK_DEV_TOOLS)
bool SocialFakeScenario(std::string_view scenario);
// Value-only snapshot, including CoopSession's interpolated pose. No transport work.
struct PuppetSnapshot {
  bool enabled = false, fresh = false;
  SessionState state = SessionState::Idle;
  std::optional<PlayerPose> local, partner;
};
PuppetSnapshot PartnerBodySnapshot();
void SetPartnerBodyDriven(bool driven);
bool SteamSelfTestRequested();
int RunSteamSelfTest();
#endif
void Shutdown();
// Only caller: the existing per-frame game-thread hook in hfr_frame_time.cpp.
void GameFrame(uint8_t* base);
void CreateOverlay(rex::ui::ImGuiDrawer* drawer);
}  // namespace kk::coop
