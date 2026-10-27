// The resumed process after the co-op Play restart (task C80, coop_handover.h): no launcher; a small
// "Connecting to <name>" screen while both games reconnect, then the existing SessionStart / save transfer /
// load barrier. On failure: "Couldn't reach <name>" with Try again and Back to menu.
#pragma once

#include <filesystem>
#include <functional>

#include "play_session.h"
#include "coop_protocol.h"

namespace rex::ui { class ImGuiDrawer; }

namespace kk::coop {
struct ResumeCallbacks {
  // Starts the game in this process with the request (the app's Play path; UI thread).
  std::function<void(const play::StartRequest&)> play;
  // Restarts into the launcher (the scene's menu) and ends this process.
  std::function<void()> back_to_menu;
};
// --kk_coop_resume was given (read once; the cvar is never saved).
bool ResumeRequested(const std::filesystem::path& user_dir = {});
void UpdateRejoinCheckpoint(const SessionStart& checkpoint);
bool RejoinCheckpoint(SessionStart& checkpoint);
void ForgetRejoin();
namespace handover { struct File; }
bool ReadRejoinRecord(const std::filesystem::path& root, handover::File& record);
bool RejoinAvailable(const std::filesystem::path& root);
// Reads and consumes the hand over file in user_dir. A missing, malformed or stale file goes straight
// back to the menu. Otherwise shows the connecting screen and starts the link.
void BeginResume(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& user_dir, ResumeCallbacks callbacks);
}  // namespace kk::coop
