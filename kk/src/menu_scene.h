// The live main menu scene (task 330, owner 20 Oct): the game's own start menu scene (the Venture on the
// stormy sea under Skull Island) runs live behind the launcher instead of the stills.
//
// How: the launcher's process starts the game (the runtime, as Play in the same process does) while the
// launcher stays up. The logo movies are skipped; on the title (level 2) the port presses Start for the
// game (the only input the game ever gets here), which loads the start menu (level 3) and opens its save
// page over the scene. The game's menus (the render executor's HUD pass: its logo, texts, pages and loading
// text) are left out whole (ultrawide.cpp tags the pass, the renderer skips its draws: draw_tags.h
// kDrawTagHideAll), every controller read is empty, its sound is muted (the launcher plays the same menu
// music itself) and the start request of the launcher's Play is never carried out here (play_session.cpp,
// SetStartRequest). The launcher draws over the game's frame without its stills
// once the scene has been up for a moment (Ready). Play always starts the game as a new process (the
// launcher's restart_and_play), so the game's own path is unchanged.
//
// The stills stay (WantsLive / Decide, menu_scene_logic.h): Settings > Display "Main menu background",
// the setup and update screens, shaders not prepared, a shader job, the low-end tier, and a launcher whose
// scene did not come up last time in this version (the marker file).
#pragma once

#include <cstdint>
#include <filesystem>

#include "menu_scene_logic.h"

namespace kk::menu_scene {

// The launcher's decision (logged), with the failure marker read from `user_dir`.
Reason DecideAtLauncherStart(StartFacts facts, const std::filesystem::path& user_dir);
// The game is about to start in the launcher's process for the scene (UI thread, before the runtime).
void Begin(const std::filesystem::path& user_dir);
// The scene runs in this process (from Begin on).
bool Active();
// The scene is up and may be shown (the start menu's level has run for a moment).
bool Ready();
// The launcher process closes normally: the marker is cleared if the scene never got that far.
void NoteClosing();
// Hidden on purpose (the setting switched to pictures while it runs; a shader rebuild): Ready() is false.
void SetHidden(bool hidden);

// Game thread, the universe's uni_go (play_session.cpp): where its variables are (the running AI instance's
// +0x40 is the universe's only while its own script runs).
void NoteUniverse(uint32_t g);
// Game thread, each controller read (input_remap.cpp): the state the game sees. Returns the XInput result.
uint32_t GuestPad(uint32_t user, uint8_t* base, uint8_t* state);
// Minimised window or the launcher handing over to the game: the scene runs at 10 frames a second.
void SetThrottled(bool throttled, const char* why);
// The frame rate cap the game's limiter uses (frame_stats.cpp): player_cap (kk_frame_rate) outside the scene.
int FrameCap(int player_cap);
// The game's menus are left out (ultrawide.cpp tags the HUD pass).
inline bool HideGameScreen() { return Active(); }

}  // namespace kk::menu_scene
