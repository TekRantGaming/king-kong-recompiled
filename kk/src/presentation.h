#pragma once

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

constexpr bool KeepCutsceneFraming(bool movie_player_active, bool video_activity_recent,
                                  bool gameplay_camera_active) {
  return movie_player_active || (video_activity_recent && !gameplay_camera_active);
}

}  // namespace kk
