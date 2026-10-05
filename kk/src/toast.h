// Xbox 360-style achievement notifications ("toasts") with a chime, drawn over
// the game or the launcher.
//
// The chime is an original sound generated at runtime; players can replace it
// with their own achievement.wav in the save folder.

#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/ui/overlay/achievement_notification.h>

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace kk {

class AchievementToast final : public rex::ui::AchievementNotificationDialog {
 public:
  AchievementToast(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                   std::filesystem::path game_dir, std::filesystem::path user_dir);
  ~AchievementToast() override;

  // From ReXGlue when the game unlocks an achievement.
  void Push(const rex::system::AchievementEvent& event) override;

  // Shows a toast. `icon_id` is a game achievement ID (0 = the game's title icon).
  void Show(std::string title, uint32_t gamerscore, uint32_t icon_id = 0);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct Toast {
    std::string title;
    uint32_t gamerscore = 0;
    uint32_t icon_id = 0;
    double started = -1.0;  // ImGui time when it became visible
  };
  rex::ui::ImmediateTexture* Icon(uint32_t id);

  rex::ui::ImmediateDrawer* immediate_;
  std::filesystem::path game_dir_, user_dir_;
  std::mutex mutex_;
  std::deque<Toast> queue_;
  std::map<uint32_t, std::filesystem::path> icon_paths_;
  std::map<uint32_t, std::unique_ptr<rex::ui::ImmediateTexture>> icons_;
};

// A dialog is closed partway through a frame, and that frame's drawing (which
// still uses the dialog's pictures) is sent to the GPU afterwards. Freeing the
// pictures then crashes the Vulkan renderer, so dialogs hand them here instead.
void KeepTextureAlive(std::unique_ptr<rex::ui::ImmediateTexture> texture);

// Plays the achievement sound: kk_achievement_sound_file from the sounds
// folder, or the built-in chime (respects kk_achievement_sound / _volume).
void PlayAchievementSound(const std::filesystem::path& user_dir);

// Sounds the player has added: <save folder>/sounds/*.wav (file names).
std::filesystem::path SoundsDir(const std::filesystem::path& user_dir);
std::vector<std::filesystem::path> ListSounds(const std::filesystem::path& user_dir);
std::string SoundLabel(const std::filesystem::path& file);  // "xbox_360.wav" -> "Xbox 360"

// --- Port achievements (separate from the game's own 12) ---------------------
struct PortAchievement {
  const char* id;
  const char* title;
  const char* description;
};
const std::vector<PortAchievement>& PortAchievements();
bool IsPortAchievementUnlocked(const std::filesystem::path& user_dir, const char* id);
// Marks it unlocked; returns true if it was newly unlocked.
bool UnlockPortAchievement(const std::filesystem::path& user_dir, const char* id);

}  // namespace kk
