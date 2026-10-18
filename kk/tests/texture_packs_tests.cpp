// Task 209: only one texture pack on at a time (texture_packs_logic.h): the HD texture pack and the player's own
// folder (settings native_texture_pack + native_texture_pack_folder) and the texture pack mods (kk_mods), while
// mods of the other types stay many at once in their load order.
#include "texture_packs_logic.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <iterator>
#include <string>

#include <rex/cvar.h>

#include "launcher_text.h"
#include "texture_packs_downloads.h"
#include "workshop_art.h"
#include "workshop_content.h"

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
  const char* comic_title[] = {"Downloading Comic Book Texture Pack", "Comic Book Texture Pack wird heruntergeladen",
                               "Descargando Comic Book Texture Pack", "T\xC3\xA9l\xC3\xA9" "chargement de Comic Book Texture Pack",
                               "Download di Comic Book Texture Pack"};
  for (int l = 0; l < 5; ++l) {
    CHECK(rex::cvar::SetFlagByName("user_language", langs[l]));
    const std::string desc = kk::text::T(comic->description);
    CHECK(desc == comic_text[l] && !HasDash(desc));
    CHECK(kk::text::T("Comic Book Texture Pack") == std::string("Comic Book Texture Pack"));  // a name: the same everywhere
    CHECK(kk::text::T(std::string("Downloading ") + kk::content::ModPackFor(Kind::kComic)->title) == comic_title[l]);
    for (const auto& page : kDownloadPages) {
      const std::string name = kk::content::ModPackFor(page.kind)->title;
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

// Task 221: the Workshop's facts tables (workshop_content.h) in five languages, the pack picture read from a ZIP
// with ranged reads (ReadZipPicture), and the card pictures made from a still (workshop_art.h).
static void ContentFacts() {
  using kk::workshop_content::FactsFor;
  for (const char* key : {"original", "hd", "uhd", "comic", "folder", "builtin:cel-shading", "builtin:ultrawide"}) {
    const auto* f = FactsFor(key);
    CHECK(f && f->changes && f->needs);
  }
  CHECK(!FactsFor("mod:someone-else") && !FactsFor("import"));
  // Kong Comic Book: the owner's wording only (DECISIONS 18 Oct), so no "how it looks" of ours.
  CHECK(!FactsFor("comic")->looks && std::string(FactsFor("comic")->pairs).find("Cel shading") != std::string::npos);
  CHECK(std::string(FactsFor("builtin:cel-shading")->pairs).find("Kong Comic Book") != std::string::npos);
  // Task 232: the Ultrawide mod needs an ultrawide screen and keeps saying it is experimental.
  CHECK(std::string(FactsFor("builtin:ultrawide")->needs).find("ultrawide screen") != std::string::npos);
  CHECK(std::string(FactsFor("builtin:ultrawide")->looks).find("Experimental") != std::string::npos);
  // The UHD and HD descriptions no longer repeat the size (it is the facts table's Size row).
  for (const auto& page : kDownloadPages) CHECK(std::string(page.description).find("GB") == std::string::npos);
  const char* langs[] = {"1", "3", "5", "4", "6"};
  const char* cel = "A comic book look: ink outlines and flat bands of light and shade. Water, smoke, sky and the HUD "
                    "stay as they are. Fine controls (outline, darkness, bands, strength) are in the F8 panel under Cel "
                    "shading. The Original look switches it off.";
  for (int l = 0; l < 5; ++l) {
    CHECK(rex::cvar::SetFlagByName("user_language", langs[l]));
    auto translated = [&](const char* t) {
      if (!t) return;
      const std::string x = kk::text::T(t);
      CHECK(!HasDash(x) && (l == 0 ? x == t : x != t));
      if (!(l == 0 ? x == t : x != t)) std::cerr << "  not translated (" << langs[l] << "): " << t << "\n";
    };
    for (const auto& f : kk::workshop_content::kFacts) {
      translated(f.changes);
      translated(f.looks);
      translated(f.pairs);
      translated(f.needs);
    }
    for (const char* label : {"What it changes", "How it looks", "Goes well with", "Size", "Needs"}) translated(label);
    // Task 232: a German label that fits a texture pack as well as a mod ("Was sie ändert" did not).
    if (l == 1) CHECK(kk::text::T("What it changes") == std::string("Was sich \xC3\xA4ndert"));
    translated(cel);
    translated("Sharper versions of the game's textures, made at four times their original size with AI upscaling, "
               "keeping the game's own look.");
    const std::string size = kk::text::F(kk::workshop_content::kSizeText, {"1.4", "5.3"});
    CHECK(size.find("1.4") != std::string::npos && size.find("5.3") != std::string::npos && size.find('{') == std::string::npos);
  }
  CHECK(rex::cvar::SetFlagByName("user_language", "1"));
}

// A stored ZIP in memory: (name, bytes, method) entries, as the packs are laid out.
struct FakeEntry {
  std::string name, data;
  uint16_t method = 0;
};
static std::vector<uint8_t> MakeZip(const std::vector<FakeEntry>& entries, bool bad_crc = false) {
  std::vector<uint8_t> z, dir;
  auto u16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
  auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) { u16(v, x & 0xFFFF); u16(v, x >> 16); };
  for (const auto& e : entries) {
    const uint32_t off = uint32_t(z.size());
    uint32_t crc = kk::workshop_content::Crc32(reinterpret_cast<const uint8_t*>(e.data.data()), e.data.size());
    if (bad_crc) crc ^= 1;
    u32(z, 0x04034b50); u16(z, 20); u16(z, 0); u16(z, e.method); u16(z, 0); u16(z, 0);
    u32(z, crc); u32(z, uint32_t(e.data.size())); u32(z, uint32_t(e.data.size()));
    u16(z, uint32_t(e.name.size())); u16(z, 3);
    z.insert(z.end(), e.name.begin(), e.name.end());
    z.insert(z.end(), {0, 0, 0});  // a little extra field, as some tools write
    z.insert(z.end(), e.data.begin(), e.data.end());
    u32(dir, 0x02014b50); u16(dir, 20); u16(dir, 20); u16(dir, 0); u16(dir, e.method); u16(dir, 0); u16(dir, 0);
    u32(dir, crc); u32(dir, uint32_t(e.data.size())); u32(dir, uint32_t(e.data.size()));
    u16(dir, uint32_t(e.name.size())); u16(dir, 0); u16(dir, 0); u16(dir, 0); u16(dir, 0); u32(dir, 0); u32(dir, off);
    dir.insert(dir.end(), e.name.begin(), e.name.end());
  }
  const uint32_t dir_off = uint32_t(z.size());
  z.insert(z.end(), dir.begin(), dir.end());
  u32(z, 0x06054b50); u16(z, 0); u16(z, 0); u16(z, uint32_t(entries.size())); u16(z, uint32_t(entries.size()));
  u32(z, uint32_t(dir.size())); u32(z, dir_off); u16(z, 0);
  return z;
}
static void ZipPicture() {
  const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(5000, 'p');
  int reads = 0;
  auto reader = [&](const std::vector<uint8_t>& zip) {
    return [&reads, &zip](uint64_t at, uint8_t* out, size_t n) {
      ++reads;
      if (at + n > zip.size()) return false;
      std::copy_n(zip.begin() + std::ptrdiff_t(at), n, out);
      return true;
    };
  };
  std::vector<uint8_t> got;
  std::string error;
  const auto pack = MakeZip({{"Kong Comic Book/", ""}, {"Kong Comic Book/mod.toml", "name = \"x\"", 8},
                             {"Kong Comic Book/picture.png", png}, {"Kong Comic Book/textures/picture.png", "no"}});
  CHECK(kk::workshop_content::ReadZipPicture(reader(pack), pack.size(), got, error));
  CHECK(std::string(got.begin(), got.end()) == png && reads <= 4);
  const auto root = MakeZip({{"PICTURE.PNG", png}});
  CHECK(kk::workshop_content::ReadZipPicture(reader(root), root.size(), got, error) && got.size() == png.size());
  const auto deep = MakeZip({{"a/b/picture.png", png}});
  CHECK(!kk::workshop_content::ReadZipPicture(reader(deep), deep.size(), got, error) && got.empty());
  const auto packed = MakeZip({{"p/picture.png", png, 8}});
  CHECK(!kk::workshop_content::ReadZipPicture(reader(packed), packed.size(), got, error) && error == "picture.png is compressed");
  const auto crc = MakeZip({{"p/picture.png", png}}, true);
  CHECK(!kk::workshop_content::ReadZipPicture(reader(crc), crc.size(), got, error) && got.empty());
  const std::vector<uint8_t> junk(300, 7);
  CHECK(!kk::workshop_content::ReadZipPicture(reader(junk), junk.size(), got, error));
  // A server that answers nothing: no picture, no crash.
  CHECK(!kk::workshop_content::ReadZipPicture([](uint64_t, uint8_t*, size_t) { return false; }, 1404861922ull, got, error));
}

static void CardArt() {
  // A still: left half dark green, right half light sky, a smooth gradient down.
  kk::art::Image still;
  still.width = 64;
  still.height = 36;
  still.rgba.resize(size_t(64 * 36 * 4));
  for (int y = 0; y < 36; ++y)
    for (int x = 0; x < 64; ++x) {
      uint8_t* p = &still.rgba[size_t((y * 64 + x) * 4)];
      const int g = x < 32 ? 40 + y : 180 + y;
      p[0] = uint8_t(g / 2), p[1] = uint8_t(g), p[2] = uint8_t(g * 3 / 4), p[3] = 255;
    }
  const kk::art::Image cel = kk::workshop_art::CelShade(still);
  CHECK(cel.width == 64 && cel.height == 36 && cel.rgba.size() == still.rgba.size());
  auto green = [](const kk::art::Image& im, int x, int y) { return int(im.rgba[size_t((y * im.width + x) * 4 + 1)]); };
  // Ink on the edge between the halves, none in the flat parts.
  CHECK(green(cel, 32, 18) < 40 && green(cel, 31, 18) < 40);
  CHECK(green(cel, 10, 18) > 25 && green(cel, 50, 18) > 120);
  // The smooth gradient becomes a few flat bands: few distinct values down a flat column.
  std::set<int> levels;
  for (int y = 2; y < 34; ++y) levels.insert(green(cel, 50, y));
  CHECK(levels.size() <= 3);
  const kk::art::Image hd = kk::workshop_art::HdSplit(still);
  CHECK(hd.width == 64 && hd.height == 36);
  CHECK(green(hd, 37, 0) == 235);  // the light divider at the top (58% across)
  CHECK(!kk::workshop_art::CelShade(kk::art::Image{}) && !kk::workshop_art::HdSplit(kk::art::Image{}));
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
  ContentFacts();
  ZipPicture();
  CardArt();
  if (failures) std::cerr << failures << " check(s) failed\n";
  else std::cout << "texture pack rules: all checks passed\n";
  return failures ? 1 : 0;
}
