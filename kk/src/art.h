// Launcher art and metadata taken from the player's own game files and from
// the game itself on first play (nothing is shipped with the port):
//  - title.bmp: a capture of the game's title screen (header background)
//  - achievements.toml: achievement names/descriptions read by the runtime
//  - achievement names and icons: game/achievements (extracted from the
//    player's default.xex by the builder), title icon: Images/sg.png

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace rex::system {
struct AchievementInfo;
}
namespace rex::ui {
struct RawImage;
}

namespace kk::art {

struct Image {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
  explicit operator bool() const { return width > 0 && height > 0; }
};

std::filesystem::path CacheDir(const std::filesystem::path& user_data_root);
std::filesystem::path TitleCapturePath(const std::filesystem::path& user_data_root);
std::filesystem::path AchievementCachePath(const std::filesystem::path& user_data_root);
// Where ReXGlue keeps unlocked achievements for this title.
std::filesystem::path AchievementUnlockPath(const std::filesystem::path& user_data_root);

bool SaveTitleCapture(const rex::ui::RawImage& image, const std::filesystem::path& path);
Image LoadImage(const std::filesystem::path& path);  // .bmp (ours) or .png

void WriteAchievementCache(const std::vector<rex::system::AchievementInfo>& achievements,
                           const std::filesystem::path& path);

// game/achievements: achievements.toml + icons/, from the player's own default.xex.
std::filesystem::path AchievementDir(const std::filesystem::path& game_dir);
// Achievement ID -> icon file.
std::map<uint32_t, std::filesystem::path> AchievementIcons(const std::filesystem::path& game_dir);
// The game's own title icon (Kong's face).
std::filesystem::path TitleIconPath(const std::filesystem::path& game_dir);

}  // namespace kk::art
