#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::ui { class ImGuiDrawer; class ImmediateDrawer; }

namespace kk::movies {
// The restored PS2 cutscenes (Windows: Media Foundation; Linux: the bundled LGPL FFmpeg, ps2_movie_decoder.h).
// The files stay in the player's folder.
// Whether this build can play them: the launcher shows the cutscene setup page and Settings rows only then.
bool PlaybackAvailable();
std::filesystem::path Folder();
// Kong cutscene mods (mods.cpp, at the game's start): folders whose clips play instead of the PS2 clip of the
// same name, in load order (the last one that has the clip wins). They play even with kk_ps2_cutscenes off;
// clips they do not have come from Folder() only when kk_ps2_cutscenes is on.
void SetOverrideFolders(std::vector<std::filesystem::path> folders);
std::string BrowseFolder();
void CreateMovieDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                  const std::filesystem::path& glyph_dir);
// Called on the universe thread, immediately before the original level loader. When a clip plays, this
// returns only after it has ended: the level load starts after the cutscene, never during it (task 359,
// owner 23 Oct: nothing is loaded or prepared in the background while a PS2 cutscene plays).
bool BeforeLoad(const uint8_t* base, uint32_t g, bool starting, bool leaving);
// Task 217: the level loader's waits for a cutscene (any thread): how many have ended, and whether one is
// going on now. Telemetry leaves the frame that spans one out of the frame statistics.
uint32_t LoaderWaitsEnded();
bool LoaderWaiting();
void Shutdown();
}  // namespace kk::movies
