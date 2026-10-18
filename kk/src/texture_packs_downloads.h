// Task 212 (owner, 18 Oct): what the Workshop says about each downloadable texture pack mod (content::kModPacks,
// content.h): Kong UHD Textures (task 209) and Kong Comic Book. workshop_texpacks.cpp builds their cards and pages
// from this table only; the texts are English and drawn through T() / F() (texture_packs_text_table.inc has each
// in five languages, tests/texture_packs_tests.cpp checks them). Data only: the tests include it on its own.
#pragma once

#include "content.h"

namespace kk::texpacks {

// The UHD pack's note (owner's words, 18 Oct): on its page, and asked before its download starts.
inline constexpr const char* kUhdNote =
    "This pack replaces many surfaces with high-detail materials that were not made for this game, so the game will "
    "no longer look exactly as its developers intended. Every effort was made to keep the original style: where no "
    "good match was found, the original texture is used, upscaled with AI.";

struct DownloadPage {
  content::Kind kind;
  const char* author;           // shown until its mod.toml has been read
  const char* version;
  int still;                    // the game still on its card until its own picture.png has been read
  const char* description;      // F(): {0} = the download in GB, {1} = installed in GB (a text may leave them out)
  const char* note;             // a warning on its page, also asked before the download (nullptr: none)
  const char* credits;          // nullptr: its CREDITS.txt, when it has one
  const char* recommended_mod;  // a mod (kk_mods id) its description recommends: its page can switch it on
};

inline constexpr DownloadPage kDownloadPages[] = {
    {content::Kind::kUhd, "TekRantGaming", "1.0", 4,
     // Task 221: what it changes, its size and what it needs are in its facts table (workshop_content.h).
     "Every texture in the game at up to four times its resolution, mixing real photographed materials with AI "
     "upscaling.",
     kUhdNote,
     "AI upscaling: 4x-UltraSharpV2 by Kim2091 (CC BY-NC-SA 4.0). Real materials: ambientCG and Poly Haven (CC0). "
     "Free to download, not for sale. The full list is in CREDITS.txt in its folder.",
     nullptr},
    // The owner's wording (18 Oct), nothing more.
    {content::Kind::kComic, "TekRantGaming", "1.0", 2,
     "A comic book style texture pack for King Kong. We recommend also enabling the Cel shading mod.", nullptr,
     nullptr, "builtin:cel-shading"},
};

constexpr const DownloadPage* DownloadPageFor(content::Kind kind) {
  for (const DownloadPage& p : kDownloadPages)
    if (p.kind == kind) return &p;
  return nullptr;
}

}  // namespace kk::texpacks
