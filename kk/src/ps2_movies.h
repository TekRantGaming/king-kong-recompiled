#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::ui { class ImGuiDrawer; class ImmediateDrawer; }

namespace kk::movies {
// The restored PS2 cutscenes (Windows: Media Foundation; Linux: the bundled LGPL FFmpeg, ps2_movie_decoder.h).
// The files stay in the player's folder.
// Whether this build can play them: the launcher shows the cutscene setup page and Settings rows only then.
bool PlaybackAvailable();
std::filesystem::path Folder();
std::string BrowseFolder();
void CreateMovieDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                  const std::filesystem::path& glyph_dir);
// Called on the universe thread, immediately before the original level loader.
bool BeforeLoad(const uint8_t* base, uint32_t g, bool starting, bool leaving);
void Shutdown();
}  // namespace kk::movies
