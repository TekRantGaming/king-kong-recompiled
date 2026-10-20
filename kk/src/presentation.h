#pragma once

#include <cstdint>

namespace kk {

enum class PresentationMode { kUnknown, kMenu, kCutscene, kGameplay };

struct PresentationChoice {
  PresentationMode mode;
  bool letterbox_16_9;
};

// Resolve the output framing from the current scene and saved display choices.
constexpr PresentationChoice ChoosePresentation(bool movie, bool menu, bool cutscenes_16_9,
                                                 bool display_letterbox) {
  if (menu) return {PresentationMode::kMenu, display_letterbox};
  if (movie) return {PresentationMode::kCutscene, cutscenes_16_9};
  return {PresentationMode::kGameplay, display_letterbox};
}

constexpr bool KeepMenuFraming(bool menu_script_active) {
  return menu_script_active;
}

constexpr bool IsGameplayCameraView(bool menu_script_active, bool movie_player_active) {
  return !menu_script_active && !movie_player_active;
}

constexpr int GameFrameWidth(int output_width, int output_height, bool letterbox_16_9) {
  if (letterbox_16_9 || output_width <= 0 || output_height <= 0) return 1280;
  const std::int64_t frame_width =
      (std::int64_t(output_width) * 720 + output_height / 2) / output_height;
  return int(frame_width);
}

constexpr bool KeepCutsceneFraming(bool movie_player_active, bool video_activity_recent,
                                  bool gameplay_camera_active) {
  return movie_player_active || (video_activity_recent && !gameplay_camera_active);
}

}  // namespace kk
