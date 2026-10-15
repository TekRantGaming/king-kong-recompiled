#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::ui { class ImGuiDrawer; class ImmediateDrawer; }

namespace kk::movies {
// Windows-only private test feature. The files stay in the player's folder.
std::filesystem::path Folder();
std::string BrowseFolder();
void CreateMovieDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                  const std::filesystem::path& glyph_dir);
// Called on the universe thread, immediately before the original level loader.
bool BeforeLoad(const uint8_t* base, uint32_t g, bool starting, bool leaving);
void Shutdown();
}  // namespace kk::movies
