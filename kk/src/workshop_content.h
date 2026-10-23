// Task 221 (owner, 18 Oct): what the Workshop says about each texture pack and built-in mod, beside its
// description: what it changes, how it looks, what it goes well with, what it needs (and, for the downloads, its
// size from content::InfoFor). The texts are English, drawn through T() (workshop_text_table.inc has them in five
// languages; tests/texture_packs_tests.cpp checks them). Kong Comic Book keeps the owner's wording only
// (DECISIONS 18 Oct): no "how it looks" of ours, only facts.
//
// Also the pure part of the pack pictures: the UHD and Comic ZIPs carry a picture.png near their start (stored,
// about 1 MB), read with a few ranged requests before the pack is downloaded (ReadZipPicture), so the card shows
// the pack's own picture; nothing of it is in git. Data and pure functions only: the tests include it on its own.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "content_logic.h"

namespace kk::workshop_content {

struct Facts {
  const char* key;      // a texture pack card key ("original", "hd", "uhd", "comic", "folder") or a mod id
  const char* changes;  // What it changes
  const char* looks;    // How it looks (nullptr: not said)
  const char* pairs;    // Goes well with (nullptr: nothing)
  const char* needs;    // Needs
};

inline constexpr const char* kVramRule =
    "Replacement textures may use up to a quarter of your graphics card's memory; past that the game uses the "
    "original textures.";

inline constexpr Facts kFacts[] = {
    {"original", "Nothing: the textures the Xbox 360 version shipped with, at their original size.",
     "Exactly as the developers made it. Surfaces look soft up close on a large screen.", nullptr, "Nothing extra."},
    {"hd",
     "Every texture in the game, at four times its size (up to 4096 pixels per side). Nothing else in the game "
     "changes.",
     "The same colours and style as the original, with sharper detail on rock, wood, ground and skin, most visible "
     "up close.",
     "Texture filtering at 16x (Settings > Graphics) keeps the sharper ground clear into the distance.", kVramRule},
    {"uhd",
     "Rock, moss, forest ground, dirt, gravel, wood, bark and rust become photographed materials, recoloured to the "
     "game's look. Everything else is the original texture upscaled with AI.",
     "More real world surface detail than the HD texture pack. Where real materials are used, the look moves away "
     "from the original (see the note).",
     "Texture filtering at 16x (Settings > Graphics) keeps the detailed ground clear into the distance.",
     "About 1 GB of graphics memory per chapter. Replacement textures may use up to a quarter of your graphics "
     "card's memory; past that the game uses the original textures."},
    // The owner's wording only (DECISIONS 18 Oct): no "how it looks" of ours.
    {"comic", "The game's textures, replaced by a comic book style set.", nullptr,
     "The Cel shading mod (built in), as the pack's makers recommend.", kVramRule},
    // Task 255: drafted from the packs' own mod.toml and README.txt, for the owner to check (DECISIONS 19 Oct).
    {"tropical",
     "The game's textures, at up to 4096 pixels per side, in sunny holiday colours, with free CC0 photo materials "
     "where they fit. Fonts, buttons, menus and logos stay as they were.",
     "Very different from the original on purpose: tropical greens, sandy stone, turquoise water and blue skies "
     "with clouds.",
     nullptr,
     "About 1 GB of graphics memory per chapter. Replacement textures may use up to a quarter of your graphics "
     "card's memory; past that the game uses the original textures."},
    {"cartoon",
     "The game's textures, at up to 4096 pixels per side, in a cartoon style, with free CC0 photo materials where "
     "they fit. Fonts, buttons, menus and logos stay as they were.",
     "Very different from the original on purpose: smooth flat colours, cheerful skies and soft outlines.",
     "The Cel shading mod (built in), as the pack's makers recommend.",
     "About 1 GB of graphics memory per chapter. Replacement textures may use up to a quarter of your graphics "
     "card's memory; past that the game uses the original textures."},
    {"folder", "The textures your folder holds replace the game's own; the others stay as they are.", nullptr,
     nullptr, "A pack.txt in the folder, as MODDING.md describes."},
    {"builtin:cel-shading",
     "Only the picture on screen: the game's lighting, textures and models stay the same. Water, smoke, sky and "
     "the HUD are left out.",
     "Like a drawn comic book: dark edges, and shading in a few flat steps instead of smooth gradients.",
     "The Kong Comic Book texture pack (Workshop > Texture packs).",
     "Nothing extra. The Original look switches it off; fine controls (outline, darkness, bands, strength) are in "
     "the F8 panel under Cel shading."},
    // Task 232: the experimental Ultrawide mod (DECISIONS 18 Oct); its description keeps the experimental warning.
    {"builtin:ultrawide",
     "Only the shape of the picture: on a 21:9 or 32:9 screen or window the game's 3D view gets wider and shows "
     "more on the left and right. Nothing is stretched.",
     "Experimental. No black bars at the sides, but the HUD, menus and some effects may still look stretched or out "
     "of place.",
     nullptr, "An ultrawide screen or window (21:9 up to 32:9). On a 16:9 screen nothing changes."},
};

inline const Facts* FactsFor(std::string_view key) {
  for (const Facts& f : kFacts)
    if (key == f.key) return &f;
  return nullptr;
}

// The size row of a download: {0} the download, {1} once installed (GB, one decimal).
inline constexpr const char* kSizeText = "A download of {0} GB that takes {1} GB once installed.";

// ------------------------------------------------------------------ the pack's own picture ---
inline constexpr uint64_t kMaxZipPicture = 12u << 20;

inline uint32_t Crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// picture.png in the ZIP's top folder (or at its root).
inline bool IsTopPicture(const std::string& name) {
  const size_t slash = name.find('/');
  const std::string leaf = slash == std::string::npos ? name : name.substr(slash + 1);
  if (leaf.find('/') != std::string::npos || leaf.size() != 11) return false;
  std::string low = leaf;
  for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
  return low == "picture.png";
}

// Reads picture.png's bytes through read_at(offset, buffer, size) -> bool from a ZIP of `file_size` bytes: the
// directory at the end, then the entry's local header and data. Only a stored entry (as the packs have it) of at
// most kMaxZipPicture bytes; false with `error` otherwise.
template <typename ReadAt>
bool ReadZipPicture(ReadAt&& read_at, uint64_t file_size, std::vector<uint8_t>& out, std::string& error) {
  out.clear();
  std::vector<content::ZipEntry> entries;
  if (!content::ReadZipDirectory(read_at, file_size, entries, error)) return false;
  const content::ZipEntry* pic = nullptr;
  for (const auto& e : entries)
    if (!e.directory && IsTopPicture(e.name)) {
      pic = &e;
      break;
    }
  if (!pic) return error = "no picture.png", false;
  if (pic->method != 0) return error = "picture.png is compressed", false;
  if (pic->size == 0 || pic->size > kMaxZipPicture || pic->packed != pic->size) return error = "bad picture.png size", false;
  uint8_t h[30];
  if (pic->local + 30 > file_size || !read_at(pic->local, h, 30)) return error = "cannot read the entry", false;
  if (!(h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4)) return error = "bad local header", false;
  const uint64_t data = pic->local + 30 + uint64_t(h[26] | h[27] << 8) + uint64_t(h[28] | h[29] << 8);
  if (data + pic->size > file_size) return error = "bad entry offset", false;
  out.resize(size_t(pic->size));
  if (!read_at(data, out.data(), out.size())) return out.clear(), error = "cannot read picture.png", false;
  if (Crc32(out.data(), out.size()) != pic->crc) return out.clear(), error = "picture.png CRC mismatch", false;
  return true;
}

}  // namespace kk::workshop_content
