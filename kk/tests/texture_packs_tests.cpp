// Task 209: only one texture pack on at a time (texture_packs_logic.h): the HD texture pack and the player's own
// folder (settings native_texture_pack + native_texture_pack_folder) and the texture pack mods (kk_mods), while
// mods of the other types stay many at once in their load order.
#include "texture_packs_logic.h"

#include <iostream>
#include <iterator>
#include <string>

#include <rex/cvar.h>

#include "launcher_text.h"
#include "texture_packs_downloads.h"

using namespace kk::texpacks;

REXCVAR_DEFINE_INT32(user_language, 1, "Test", "Test");

static int failures = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      ++failures;                                                             \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #x "\n"; \
    }                                                                         \
  } while (0)

// Task 212: three packs the player can switch between, the HD pack (a folder) and the two Workshop downloads
// (texture pack mods): one on at a time whichever way the player goes, and the Cel shading mod the Comic page
// recommends stays on beside any of them.
static void ThreePacks() {
  const std::string uhd = kk::content::kModPacks[0].name, comic = kk::content::kModPacks[1].name;
  const IsTexturePackMod is_tex = [&](const std::string& id) { return id == uhd || id == comic; };
  const std::string hd = "C:/user/content/hd-textures/kk-4x-usv2";
  Choice c;
  c.mods = {"Fast Kong"};
  c = EnableFolder(c, hd, is_tex);
  CHECK(CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "folder:" + hd);
  // Kong Comic Book installed: it is the only one on; the HD pack's folder is kept for later.
  c = EnableMod(c, comic, is_tex);
  CHECK(!c.folder_on && c.folder == hd && CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "mod:Kong Comic Book");
  CHECK((c.mods == std::vector<std::string>{"Fast Kong", "Kong Comic Book"}));
  // Its page switches the recommended Cel shading mod on: a settings mod, so the texture pack stays the one on.
  c.mods.push_back(kk::texpacks::DownloadPageFor(kk::content::Kind::kComic)->recommended_mod);
  CHECK(CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "mod:Kong Comic Book");
  // UHD on: Comic off; Comic on again: UHD off; the settings mods stay on in their order.
  c = EnableMod(c, uhd, is_tex);
  CHECK(CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "mod:Kong UHD Textures");
  CHECK((c.mods == std::vector<std::string>{"Fast Kong", "builtin:cel-shading", "Kong UHD Textures"}));
  c = EnableMod(c, comic, is_tex);
  CHECK(CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "mod:Kong Comic Book" && !c.folder_on);
  // Back to the HD pack: both downloads off; then the original textures: nothing on.
  c = EnableFolder(c, hd, is_tex);
  CHECK(CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "folder:" + hd);
  CHECK((c.mods == std::vector<std::string>{"Fast Kong", "builtin:cel-shading"}));
  c = DisableAll(c, is_tex);
  CHECK(CountOn(c, is_tex) == 0 && (c.mods == std::vector<std::string>{"Fast Kong", "builtin:cel-shading"}));
  // Settings from before with all three on: the texture pack mod last in the load order is kept.
  Choice all;
  all.folder_on = true;
  all.folder = hd;
  all.mods = {comic, "builtin:cel-shading", uhd};
  const Choice n = Normalize(all, is_tex);
  CHECK(CountOn(n, is_tex) == 1 && ActiveId(n, is_tex) == "mod:Kong UHD Textures" && !n.folder_on);
  CHECK((n.mods == std::vector<std::string>{"builtin:cel-shading", "Kong UHD Textures"}));
  all.mods = {uhd, comic};
  CHECK(ActiveId(Normalize(all, is_tex), is_tex) == "mod:Kong Comic Book");
}

// Task 212: the table of Workshop downloads (content.h kModPacks and what the Workshop says about each,
// texture_packs_downloads.h), and the Comic description, the UHD note and the download pop-up texts in five
// languages through the launcher's own lookup (texture_packs_text_table.inc).
static bool HasDash(const std::string& s) {
  return s.find("\xE2\x80\x94") != std::string::npos || s.find("\xE2\x80\x93") != std::string::npos;
}
static void DownloadTable() {
  using kk::content::Kind;
  using kk::texpacks::DownloadPageFor;
  using kk::texpacks::kDownloadPages;
  // Every download has its page text, in the same order (the order of the cards), with its own card key.
  CHECK(std::size(kDownloadPages) == std::size(kk::content::kModPacks));
  for (size_t i = 0; i < std::size(kk::content::kModPacks); ++i) {
    const auto& mp = kk::content::kModPacks[i];
    CHECK(kDownloadPages[i].kind == mp.kind && DownloadPageFor(mp.kind) == &kDownloadPages[i]);
    CHECK(kk::content::IsModPack(mp.kind) && kk::content::ModPackFor(mp.kind) == &mp);
    for (const char* other : {"original", "hd", "folder", "import"}) CHECK(std::string(mp.key) != other);
    CHECK(std::string(mp.key).find(':') == std::string::npos);  // never taken for a "mod:<id>" card
  }
  CHECK(!DownloadPageFor(Kind::kTexturePack) && !DownloadPageFor(Kind::kImport) && !DownloadPageFor(Kind::kArtbook));
  const auto* uhd = DownloadPageFor(Kind::kUhd);
  const auto* comic = DownloadPageFor(Kind::kComic);
  // UHD as task 209 had it: its note (the owner's words) and credits; Comic: the owner's wording only, no note,
  // and its page offers the Cel shading mod.
  CHECK(uhd && uhd->note == kk::texpacks::kUhdNote && uhd->credits && !uhd->recommended_mod);
  CHECK(std::string(uhd->note) ==
        "This pack replaces many surfaces with high-detail materials that were not made for this game, so the game "
        "will no longer look exactly as its developers intended. Every effort was made to keep the original style: "
        "where no good match was found, the original texture is used, upscaled with AI.");
  CHECK(comic && !comic->note && !comic->credits && std::string(comic->recommended_mod) == "builtin:cel-shading");
  CHECK(std::string(comic->description) ==
        "A comic book style texture pack for King Kong. We recommend also enabling the Cel shading mod.");
  CHECK(std::string(comic->author) == "TekRantGaming" && std::string(comic->version) == "1.0");

  const char* langs[] = {"1", "3", "5", "4", "6"};  // English, German, Spanish, French, Italian
  const char* comic_text[] = {
      "A comic book style texture pack for King Kong. We recommend also enabling the Cel shading mod.",
      "Ein Texturpaket im Comic-Stil f\xC3\xBCr King Kong. Wir empfehlen, zus\xC3\xA4tzlich die Mod Cel-Shading zu aktivieren.",
      "Un paquete de texturas de estilo c\xC3\xB3mic para King Kong. Recomendamos activar tambi\xC3\xA9n el mod Cel shading.",
      "Un pack de textures style bande dessin\xC3\xA9" "e pour King Kong. Nous recommandons d'activer aussi le mod Cel shading.",
      "Un pacchetto texture in stile fumetto per King Kong. Consigliamo di attivare anche la mod Cel shading."};
  const char* comic_title[] = {"Downloading Kong Comic Book", "Kong Comic Book wird heruntergeladen",
                               "Descargando Kong Comic Book", "T\xC3\xA9l\xC3\xA9" "chargement de Kong Comic Book",
                               "Download di Kong Comic Book"};
  for (int l = 0; l < 5; ++l) {
    CHECK(rex::cvar::SetFlagByName("user_language", langs[l]));
    const std::string desc = kk::text::T(comic->description);
    CHECK(desc == comic_text[l] && !HasDash(desc));
    CHECK(kk::text::T("Kong Comic Book") == std::string("Kong Comic Book"));  // a name: the same everywhere
    CHECK(kk::text::T(std::string("Downloading ") + kk::content::ModPackFor(Kind::kComic)->name) == comic_title[l]);
    for (const auto& page : kDownloadPages) {
      const std::string name = kk::content::ModPackFor(page.kind)->name;
      // Every text of every download is translated (the page texts, the pop-up titles), with no dashes.
      for (const char* t : {page.description, page.note, page.credits}) {
        if (!t) continue;
        const std::string x = kk::text::T(t);
        CHECK(!HasDash(x) && (l == 0 ? x == t : x != t));
      }
      for (const std::string& t : {"Downloading " + name, name + " installed"}) {
        const std::string x = kk::text::T(t);
        CHECK(!HasDash(x) && x.find(name) != std::string::npos && (l == 0 ? x == t : x != t));
      }
    }
    const std::string body = kk::text::F("{0} is a {1} GB download. After that it is checked and unpacked. Depending on your "
                                         "connection this takes from a few minutes to an hour or more.",
                                         {"Kong Comic Book", "1.4"});
    CHECK(body.find("Kong Comic Book") == 0 && body.find("1.4") != std::string::npos && body.find('{') == std::string::npos);
    CHECK(l == 0 || body.find("connection") == std::string::npos);  // translated
    const std::string on = kk::text::F("Switch on {0}", {"Cel shading"}), is_on = kk::text::F("{0} is on", {"Cel shading"});
    CHECK(on.find("Cel shading") != std::string::npos && is_on.find("Cel shading") != std::string::npos);
    CHECK(l == 0 ? on == "Switch on Cel shading" : on != "Switch on Cel shading");
  }
  CHECK(rex::cvar::SetFlagByName("user_language", "1"));
}

int main() {
  // Texture pack mods: Kong UHD Textures, an imported pack, a pack the player put in the mods folder.
  const IsTexturePackMod is_tex = [](const std::string& id) {
    return id == "Kong UHD Textures" || id == "my-pack" || id == "hand-made";
  };
  const std::string hd = "C:/user/content/hd-textures/kk-4x-usv2";
  Choice c;
  c.mods = {"builtin:cel-shading", "Fast Kong"};  // two settings mods, both on
  CHECK(CountOn(c, is_tex) == 0 && ActiveId(c, is_tex).empty());

  // The HD texture pack on.
  c = EnableFolder(c, hd, is_tex);
  CHECK(c.folder_on && c.folder == hd && CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "folder:" + hd);
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong"}));  // other mods untouched

  // UHD on: the HD pack goes off (its folder is kept for later), the settings mods stay.
  c = EnableMod(c, "Kong UHD Textures", is_tex);
  CHECK(!c.folder_on && c.folder == hd && CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "mod:Kong UHD Textures");
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong", "Kong UHD Textures"}));

  // Another texture pack mod (a new import): UHD goes off.
  c = EnableMod(c, "my-pack", is_tex);
  CHECK(CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "mod:my-pack");
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong", "my-pack"}));

  // Back to the HD pack: the mod goes off.
  c = EnableFolder(c, hd, is_tex);
  CHECK(CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "folder:" + hd);
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong"}));

  // The player's own folder replaces the HD pack (same two settings).
  c = EnableFolder(c, "D:/my textures", is_tex);
  CHECK(CountOn(c, is_tex) == 1 && c.folder == "D:/my textures");

  // The game's own textures: nothing on; enabling the same mod twice keeps it once.
  c = DisableAll(c, is_tex);
  CHECK(CountOn(c, is_tex) == 0 && ActiveId(c, is_tex).empty() && c.folder == "D:/my textures");
  c = EnableMod(EnableMod(c, "hand-made", is_tex), "hand-made", is_tex);
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong", "hand-made"}));

  // Old settings with several on: one kept, always the same one (the texture pack mod last in the load order,
  // whose textures won before); settings mods untouched; already-normal settings are left exactly as they are.
  Choice old;
  old.folder_on = true;
  old.folder = hd;
  old.mods = {"my-pack", "builtin:cel-shading", "Kong UHD Textures", "Fast Kong"};
  const Choice n = Normalize(old, is_tex);
  CHECK(CountOn(n, is_tex) == 1 && !n.folder_on && n.folder == hd);
  CHECK((n.mods == std::vector<std::string>{"builtin:cel-shading", "Kong UHD Textures", "Fast Kong"}));
  CHECK(Normalize(n, is_tex) == n && Normalize(old, is_tex) == n);  // stable and deterministic
  CHECK(ActiveId(old, is_tex) == "mod:Kong UHD Textures");
  Choice only_folder;
  only_folder.folder_on = true;
  only_folder.folder = hd;
  only_folder.mods = {"Fast Kong"};
  CHECK(Normalize(only_folder, is_tex) == only_folder);
  Choice two_mods;
  two_mods.mods = {"hand-made", "my-pack"};
  CHECK((Normalize(two_mods, is_tex).mods == std::vector<std::string>{"my-pack"}));
  // A folder switch that is on without a folder counts as nothing on.
  Choice empty_folder;
  empty_folder.folder_on = true;
  empty_folder.mods = {"my-pack"};
  CHECK(CountOn(empty_folder, is_tex) == 1 && Normalize(empty_folder, is_tex) == empty_folder);

  ThreePacks();
  DownloadTable();
  if (failures) std::cerr << failures << " check(s) failed\n";
  else std::cout << "texture pack rules: all checks passed\n";
  return failures ? 1 : 0;
}
