// The launcher's official artwork, taken from the player's own copy of the
// game when it's installed (nothing is shipped with the port):
//  - the game's logo, from the front end's textures in KKTextures.bf
//  - backdrops: stills from the movie trailer and the intro on the disc
//    (Video/Trailer.wmv, Video/Intro.wmv), decoded with Windows' own
//    Media Foundation (Windows only; elsewhere the launcher keeps using its
//    capture of the title screen)

#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "art.h"

namespace kk::art {

struct LauncherArt {
  Image logo;                   // RGBA with transparency
  std::vector<Image> backdrops;  // 16:9 stills
};

std::filesystem::path LauncherArtDir(const std::filesystem::path& user_data_root);
// Whether the artwork was extracted (by this version of the extraction).
bool HasLauncherArt(const std::filesystem::path& dir);

struct ArtProgress {
  std::atomic<bool> busy{false}, done{false}, failed{false};
  std::atomic<float> fraction{0};  // 0..1
  std::string message;              // why it failed (read once busy is false)
};

// Writes the artwork to `dir`. Safe to run on a worker thread.
bool ExtractLauncherArt(const std::filesystem::path& game_dir, const std::filesystem::path& dir,
                        ArtProgress& progress);

LauncherArt LoadLauncherArt(const std::filesystem::path& dir);

// A 32-bit BMP with its alpha channel (art::LoadImage reads it back).
bool SaveImage(const Image& image, const std::filesystem::path& path);

}  // namespace kk::art
