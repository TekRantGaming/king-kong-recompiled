// This file is UTF-8.
#include "workshop_texpacks.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "content.h"
#include "launcher_text.h"
#include "mods.h"
#include "texture_packs_downloads.h"
#include "texture_packs_logic.h"
#include "workshop.h"
#include "workshop_mods.h"

REXCVAR_DECLARE(std::string, kk_texture_pack_own_folder);

namespace kk {

namespace fs = std::filesystem;
namespace color = ui::color;
using text::F;
using text::T;
using ui::Action;

#define KK_TP_DOT "\xC2\xB7"

namespace {

std::string Gb(uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", double(bytes) / 1e9);
  return buf;
}

std::string Flag(const char* name) { return rex::cvar::GetFlagByName(name); }

std::string Percent(const content::Status& st) {
  return st.total ? std::to_string(int(double(st.done) * 100 / double(st.total))) + "%" : std::string();
}

// What a download that is not finished shows on its card and its page.
std::string BusyText(const content::Status& st) {
  using content::Stage;
  switch (st.stage) {
    case Stage::kQueued: return T("Waiting");
    case Stage::kStarting: return T("Starting");
    case Stage::kDownloading: return F("Downloading {0}", {Percent(st)});
    case Stage::kVerifying: return T("Checking");
    case Stage::kUnpacking: return F("Installing {0}", {Percent(st)});
    case Stage::kRemoving: return T("Removing");
    case Stage::kPaused: return F("Paused at {0}", {Percent(st)});
    default: return {};
  }
}

// A small text file next to a pack (its CREDITS.txt): the first lines only.
std::string ReadHead(const fs::path& file, size_t max) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return {};
  std::string text(max, '\0');
  f.read(text.data(), std::streamsize(max));
  text.resize(size_t(f.gcount()));
  if (text.size() == max) {
    const size_t nl = text.rfind('\n');
    if (nl != std::string::npos) text.resize(nl);
    text += "\n...";
  }
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

}  // namespace

struct TexturePackShelf::Impl {
  // kDownload: a Workshop download that installs as a texture pack mod (content::kModPacks: UHD, Comic).
  enum class Kind { kOriginal, kHd, kDownload, kMod, kFolder };
  struct Pack {
    Kind kind = Kind::kOriginal;
    content::Kind content_kind = content::Kind::kTexturePack;  // kHd / kDownload: its download
    const texpacks::DownloadPage* page = nullptr;              // kDownload: what its card and page say
    std::string key, name, author, version, description;
    std::string mod_id, folder;  // mods: the kk_mods id; folder packs: the folder setting
    fs::path dir;                // mods: the mod folder
    rex::ui::ImmediateTexture* picture = nullptr;
    float aspect = 16.0f / 9.0f;
    bool installed = false, usable = true, enabled = false, busy = false, builtin = false;
    content::Status st;  // kHd / kDownload: its download
    std::vector<std::string> problems;
  };

  fs::path user_dir;
  KongMods* mods;
  const WorkshopHost* host = nullptr;
  std::string detail;  // the open page's key ("" = none)
  int focus = 0;       // the page's buttons
  std::string credits_for, credits;
  float detail_scroll = 0;
  // Startup: settings from older builds keep one texture pack (once the first mods scan is in).
  bool normalized = false;
  // A texture pack mod just installed (import or a Workshop download): switched on once the scan knows it.
  std::string pending_mod;
  std::chrono::steady_clock::time_point pending_since;
  bool pending_folder = false;  // the HD texture pack was turned on before the mods scan was in

  Impl(fs::path u, KongMods* m) : user_dir(std::move(u)), mods(m) {}

  texpacks::IsTexturePackMod IsTex() {
    return [this](const std::string& id) { return id == pending_mod || mods->IsTexturePack(id); };
  }
  static texpacks::Choice Current() {
    return {Flag("native_texture_pack") == "true", Flag("native_texture_pack_folder"),
            mods::ParseEnabled(Flag("kk_mods"))};
  }
  void Apply(const texpacks::Choice& c, const char* why) {
    const texpacks::Choice before = Current();
    if (c == before) return;
    rex::cvar::SetFlagByName("kk_mods", mods::JoinEnabled(c.mods));
    rex::cvar::SetFlagByName("native_texture_pack", c.folder_on ? "true" : "false");
    if (c.folder != before.folder) rex::cvar::SetFlagByName("native_texture_pack_folder", c.folder);
    if (host && host->save) host->save();
    mods->ReloadEnabled();
    REXLOG_INFO("KK texture packs: {}: now on: {} (kk_mods '{}', texture pack folder {} '{}')", why,
                texpacks::ActiveId(c, IsTex()).empty() ? std::string("the original textures") : texpacks::ActiveId(c, IsTex()),
                Flag("kk_mods"), c.folder_on ? "on" : "off", c.folder);
  }

  std::string HdFolder() {
    const auto st = content::Service::Get().Ready() ? content::Service::Get().Get(content::Kind::kTexturePack) : content::Status{};
    return st.installed ? mods::U8(st.folder) : std::string();
  }
  // The player's own folder: remembered on its own, so switching to the HD pack and back does not lose it.
  std::string OwnFolder() { return Flag("kk_texture_pack_own_folder"); }

  rex::ui::ImmediateTexture* Still(int i, float* aspect) {
    return host && host->still ? host->still(i, aspect) : nullptr;
  }

  std::vector<Pack> Build() {
    std::vector<Pack> out;
    auto& service = content::Service::Get();
    const bool ready = service.Ready();
    const texpacks::Choice choice = Current();
    const std::string active = texpacks::ActiveId(choice, IsTex());
    // The original textures: no pack on.
    {
      Pack p;
      p.kind = Kind::kOriginal;
      p.key = "original";
      p.name = T("Original textures");
      p.description = T("The game's own textures, as the Xbox 360 version shows them. Switch this on to play "
                        "without a texture pack.");
      p.installed = true;
      p.enabled = active.empty();
      p.picture = Still(1, &p.aspect);
      out.push_back(std::move(p));
    }
    // The HD texture pack (the setup's download).
    {
      Pack p;
      p.kind = Kind::kHd;
      p.content_kind = content::Kind::kTexturePack;
      p.key = "hd";
      p.name = T("HD texture pack");
      p.author = "TekRantGaming";
      p.st = ready ? service.Get(content::Kind::kTexturePack) : content::Status{};
      const content::Info& info = content::InfoFor(content::Kind::kTexturePack);
      p.description = F("Sharper versions of the game's textures, made at four times their original size with AI "
                        "upscaling, keeping the game's own look. A download of {0} GB that takes {1} GB once installed.",
                        {Gb(info.zip_size), Gb(info.unpacked)});
      p.installed = p.st.installed;
      p.folder = mods::U8(p.st.folder);
      p.busy = p.st.Busy() || p.st.stage == content::Stage::kPaused;
      p.enabled = p.installed && active == "folder:" + p.folder;
      p.picture = Still(3, &p.aspect);
      out.push_back(std::move(p));
    }
    // The Workshop's downloads that install as texture pack mods (content::kModPacks: Kong UHD Textures, Kong Comic
    // Book; what they say: texture_packs_downloads.h), then the other texture pack mods.
    const std::vector<KongMods::Card> cards = mods->Cards();
    std::vector<const KongMods::Card*> taken;
    for (const content::ModPack& mp : content::kModPacks) {
      const texpacks::DownloadPage* page = texpacks::DownloadPageFor(mp.kind);
      if (!page) continue;
      const content::Status st = ready ? service.Get(mp.kind) : content::Status{};
      const KongMods::Card* card = nullptr;
      for (const auto& c : cards)
        if (c.texture_pack && !c.builtin && std::find(taken.begin(), taken.end(), &c) == taken.end() &&
            ((!st.mod_id.empty() && c.id == st.mod_id) || c.id == mp.name || c.name == mp.name)) {
          card = &c;
          break;
        }
      Pack p;
      p.kind = Kind::kDownload;
      p.content_kind = mp.kind;
      p.page = page;
      p.key = mp.key;
      p.name = mp.title;
      p.author = page->author;
      p.version = page->version;
      p.st = st;
      const content::Info& info = content::InfoFor(mp.kind);
      p.description = F(page->description, {Gb(info.zip_size), Gb(info.unpacked)});
      p.busy = st.Busy() || st.stage == content::Stage::kPaused;
      if (card) {
        taken.push_back(card);
        p.installed = true;
        p.mod_id = card->id;
        p.dir = card->dir;
        p.usable = card->usable;
        p.problems = card->problems;
        if (!card->author.empty()) p.author = card->author;
        if (!card->version.empty()) p.version = card->version;
        p.enabled = active == "mod:" + card->id;
        p.picture = card->picture;
        p.aspect = card->aspect;
      }
      if (!p.picture) p.picture = Still(page->still, &p.aspect);
      out.push_back(std::move(p));
    }
    for (const auto& c : cards) {
      if (!c.texture_pack || std::find(taken.begin(), taken.end(), &c) != taken.end()) continue;
      Pack p;
      p.kind = Kind::kMod;
      p.key = "mod:" + c.id;
      p.name = c.name;
      p.author = c.author;
      p.version = c.version;
      p.description = c.description;
      p.mod_id = c.id;
      p.dir = c.dir;
      p.installed = true;
      p.usable = c.usable;
      p.problems = c.problems;
      p.builtin = c.builtin;
      p.enabled = active == "mod:" + c.id;
      p.picture = c.picture;
      p.aspect = c.aspect;
      out.push_back(std::move(p));
    }
    // The player's own folder.
    if (const std::string own = OwnFolder(); !own.empty()) {
      Pack p;
      p.kind = Kind::kFolder;
      p.key = "folder";
      p.name = T("Your texture pack folder");
      p.folder = own;
      p.description = F("The texture pack in a folder of your own: {0}", {own});
      std::error_code ec;
      p.installed = fs::is_regular_file(mods::FromU8(own) / "pack.txt", ec);
      if (!p.installed) p.problems.push_back(T("There is no pack.txt in this folder."));
      p.usable = p.installed;
      p.enabled = active == "folder:" + own;
      p.picture = Still(5, &p.aspect);
      out.push_back(std::move(p));
    }
    return out;
  }

  // Its state in words and colours (the card's badge, the page's state line).
  static void State(const Pack& p, std::string& text, ImU32& bg, ImU32& fg) {
    if (p.busy && !p.installed) {
      text = BusyText(p.st);
      bg = ui::WithAlpha(color::kAccent, 0.92f);
      fg = color::kOnFocus;
    } else if (!p.installed) {
      text = p.kind == Kind::kFolder ? T("Not found") : T("Not installed");
      bg = ui::WithAlpha(color::kBase, 0.82f);
      fg = color::kTextFaint;
    } else if (!p.usable) {
      text = T("Problem");
      bg = ui::WithAlpha(color::kBad, 0.92f);
      fg = color::kOnFocus;
    } else if (p.enabled) {
      text = T("Enabled");
      bg = ui::WithAlpha(color::kGood, 0.95f);
      fg = color::kOnFocus;
    } else {
      text = T("Disabled");
      bg = ui::WithAlpha(color::kBase, 0.82f);
      fg = color::kTextDim;
    }
  }

  static std::string SubLine(const Pack& p) {
    std::string sub = p.author;
    if (!p.version.empty()) sub += (sub.empty() ? "" : "   " KK_TP_DOT "   ") + F("Version {0}", {p.version});
    if (p.kind == Kind::kOriginal) sub = T("The game's own");
    if (p.kind == Kind::kFolder) sub = p.folder;
    return sub;
  }

  std::vector<Card> Cards() {
    std::vector<Card> out;
    for (const Pack& p : Build()) {
      Card c;
      c.key = p.key;
      c.name = p.name;
      c.sub = SubLine(p);
      c.picture = p.picture;
      c.aspect = p.aspect;
      State(p, c.state, c.state_bg, c.state_fg);
      c.hint = c.state + "   " KK_TP_DOT "   " +
               (p.enabled ? std::string(T("The textures the game uses now."))
                : p.installed && p.usable ? std::string(T("Select to see it and switch it on."))
                : p.installed ? std::string(T("Select to see what is wrong."))
                              : std::string(T("Select to see it and download it.")));
      if (p.busy && p.st.total && p.st.stage != content::Stage::kQueued && p.st.stage != content::Stage::kStarting)
        c.progress = float(std::clamp(double(p.st.done) / double(p.st.total), 0.0, 1.0));
      out.push_back(std::move(c));
    }
    Card imp;
    imp.key = "import";
    imp.action = true;
    imp.name = T("Import texture pack");
    imp.sub = T("From a ZIP file");
    imp.hint = T("A ZIP with pack.txt and its images in one folder, or a texture pack mod (mod.toml and textures/pack.txt), "
                 "as MODDING.md describes. It is checked before anything is installed.");
    out.push_back(std::move(imp));
    return out;
  }

  // -------------------------------------------------------------- rules ---
  void Enable(const std::string& key) {
    const texpacks::Choice c = Current();
    const auto is_tex = IsTex();
    if (key == "original") Apply(texpacks::DisableAll(c, is_tex), "the original textures");
    else if (key == "hd" && !HdFolder().empty()) Apply(texpacks::EnableFolder(c, HdFolder(), is_tex), "the HD texture pack");
    else if (key == "folder" && !OwnFolder().empty()) Apply(texpacks::EnableFolder(c, OwnFolder(), is_tex), "your folder");
    else if (key.rfind("mod:", 0) == 0) Apply(texpacks::EnableMod(c, key.substr(4), is_tex), "a texture pack mod");
    else {  // a Workshop download ("uhd", "comic")
      for (const Pack& p : Build())
        if (p.kind == Kind::kDownload && p.key == key && p.installed)
          Apply(texpacks::EnableMod(c, p.mod_id, is_tex), p.name.c_str());
    }
  }

  // The mod a download's description recommends (Kong Comic Book: Cel shading), switched on from its page. Mods of
  // the other types stay many at once in their load order (it goes last); texture packs are not touched.
  void EnableRecommended(const std::string& id) {
    std::vector<std::string> list = mods::ParseEnabled(Flag("kk_mods"));
    mods::SetEnabled(list, id, true);
    rex::cvar::SetFlagByName("kk_mods", mods::JoinEnabled(list));
    if (host && host->save) host->save();
    mods->ReloadEnabled();
    REXLOG_INFO("KK texture packs: the recommended mod {} switched on (kk_mods '{}')", id, Flag("kk_mods"));
  }

  void Tick() {
    if (!mods->Scanned()) return;
    if (!normalized) {
      normalized = true;
      // A folder of the player's own from before (when one folder setting held every pack): remembered as theirs.
      const std::string folder = Flag("native_texture_pack_folder");
      if (OwnFolder().empty() && !folder.empty() && folder != HdFolder() && folder.find('|') == std::string::npos &&
          folder.find("hd-textures") == std::string::npos) {
        rex::cvar::SetFlagByName("kk_texture_pack_own_folder", folder);
        if (host && host->save) host->save();
        REXLOG_INFO("KK texture packs: your own folder {} is remembered", folder);
      }
      const texpacks::Choice c = Current();
      const int on = texpacks::CountOn(c, IsTex());
      if (on > 1) {
        REXLOG_INFO("KK texture packs: {} texture packs were on; one is kept", on);
        Apply(texpacks::Normalize(c, IsTex()), "only one texture pack on (settings from before)");
      }
    }
    if (pending_folder) {
      pending_folder = false;
      const texpacks::Choice c = Current();
      if (c.folder_on) Apply(texpacks::EnableFolder(c, c.folder, IsTex()), "the HD texture pack was installed");
    }
    if (!pending_mod.empty()) {
      const bool known = mods->IsTexturePack(pending_mod);
      if (known || std::chrono::steady_clock::now() - pending_since > std::chrono::seconds(10)) {
        const std::string id = pending_mod;
        Apply(texpacks::EnableMod(Current(), id, IsTex()), "a texture pack was installed");
        pending_mod.clear();
      }
    }
  }

  // ------------------------------------------------------------- the page ---
  const Pack* Find(const std::vector<Pack>& packs, const std::string& key) {
    for (const Pack& p : packs)
      if (p.key == key) return &p;
    return nullptr;
  }

  void Open(const std::string& key) {
    detail = key;
    focus = 0;
    detail_scroll = 0;
    REXLOG_INFO("KK texture packs: page {}", key);
  }

  void Ask(const std::string& title, const std::string& text, std::vector<std::string> buttons, int focus_button,
           std::function<void(int)> then) {
    if (host && host->ask) host->ask(title, text, std::move(buttons), focus_button, std::move(then));
  }

  void Remove(const Pack& p) {
    if (p.kind == Kind::kHd) {
      if (host && host->remove) host->remove(content::Kind::kTexturePack);
      return;
    }
    const fs::path dir = p.dir;
    const std::string mod_id = p.mod_id;
    // A Workshop download's own record (content\<id>\installed.txt) goes with its folder.
    const content::Kind kind = p.content_kind;
    const bool record = p.kind == Kind::kDownload && content::Service::Get().Get(kind).installed &&
                        content::Service::Get().Get(kind).mod_id == mod_id;
    Ask("Remove this texture pack?",
        F("This deletes {0} from your mods folder. If it is on, the game goes back to its original textures. You can "
          "import or download it again at any time.",
          {p.name}),
        {"Remove", "Keep it"}, 1, [dir, record, kind](int i) {
          if (i != 0) return;
          REXLOG_INFO("KK texture packs: removing {}", mods::U8(dir));
          if (record) content::Service::Get().Remove(kind);
          else content::Service::Get().RemoveMod(dir);
        });
  }

  // A download: its note first when it has one (Kong UHD Textures: the owner's words, 18 Oct, said plainly before
  // the download starts; the player confirms), then the launcher's own "This can take a while" question.
  void Download(content::Kind kind) {
    const texpacks::DownloadPage* page = texpacks::DownloadPageFor(kind);
    if (!page || !page->note) {
      if (host && host->download) host->download(kind);
      return;
    }
    Ask("Before you download", page->note, {"Download", "Not now"}, 1, [this, kind](int i) {
      if (i != 0) return;
      REXLOG_INFO("KK texture packs: {}: the player confirmed the note; downloading", content::ModPackFor(kind)->name);
      if (host && host->download) host->download(kind);
    });
  }

  struct Button {
    std::string label;
    bool enabled = true;
    std::function<void()> run;
  };
  std::vector<Button> Buttons(const Pack& p) {
    std::vector<Button> b;
    const std::string key = p.key;
    auto enable = [this, key] {
      Enable(key);
      ui::Cue(ui::Sound::kOn);
    };
    auto disable = [this] {
      Enable("original");
      ui::Cue(ui::Sound::kOff);
    };
    const content::Kind kind = p.content_kind;
    switch (p.kind) {
      case Kind::kOriginal:
        b.push_back({p.enabled ? T("In use") : T("Use the original textures"), !p.enabled, enable});
        break;
      case Kind::kHd:
      case Kind::kDownload:
        if (!p.installed && p.busy) {
          b.push_back({T("Show progress"), true, [this, kind] { if (host && host->progress) host->progress(kind); }});
        } else if (!p.installed) {
          b.push_back({F("Download ({0} GB)", {Gb(content::InfoFor(kind).zip_size)}), true, [this, kind] { Download(kind); }});
        } else {
          b.push_back({p.enabled ? T("Disable") : T("Enable"), p.enabled || p.usable, p.enabled ? std::function<void()>(disable) : enable});
          b.push_back({T("Remove"), true, [this, p] { Remove(p); }});
        }
        // The mod its description recommends (Kong Comic Book: Cel shading): switched on from here.
        if (p.page && p.page->recommended_mod) {
          for (const auto& c : mods->Cards()) {
            if (c.id != p.page->recommended_mod) continue;
            const std::string id = c.id;
            if (c.on) b.push_back({F("{0} is on", {T(c.name)}), false, nullptr});
            else b.push_back({F("Switch on {0}", {T(c.name)}), c.usable, [this, id] { EnableRecommended(id); }});
            break;
          }
        }
        break;
      case Kind::kMod:
        b.push_back({p.enabled ? T("Disable") : T("Enable"), p.enabled || p.usable, p.enabled ? std::function<void()>(disable) : enable});
        if (!p.builtin) b.push_back({T("Remove"), true, [this, p] { Remove(p); }});
        break;
      case Kind::kFolder:
        b.push_back({p.enabled ? T("Disable") : T("Enable"), p.enabled || p.usable, p.enabled ? std::function<void()>(disable) : enable});
        b.push_back({T("Choose another folder"), true, [this] { if (host && host->choose_folder) host->choose_folder(); }});
        b.push_back({T("Forget this folder"), true, [this, p] {
                       if (p.enabled) Enable("original");
                       rex::cvar::SetFlagByName("kk_texture_pack_own_folder", "");
                       if (host && host->save) host->save();
                       detail.clear();
                       REXLOG_INFO("KK texture packs: your folder forgotten");
                     }});
        break;
    }
    return b;
  }

  std::string Credits(const Pack& p) {
    if (p.page && p.page->credits) return T(p.page->credits);
    if (p.kind == Kind::kHd) return T("AI upscaling: 4x-UltraSharpV2 by Kim2091 (CC BY-NC-SA 4.0). Free to download, not for sale.");
    if ((p.kind != Kind::kMod && p.kind != Kind::kDownload) || p.dir.empty()) return {};
    if (credits_for != p.key) {  // read once when the page opens (a few KB)
      credits_for = p.key;
      credits = ReadHead(p.dir / "CREDITS.txt", 2048);
    }
    return credits;
  }

  void DrawPicture(ImDrawList* dl, const Pack& p, ImVec2 a, ImVec2 b, float s) {
    if (p.picture) {
      const float view = (b.x - a.x) / std::max(1.0f, b.y - a.y);
      float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
      if (p.aspect > view) u0 = (1 - view / p.aspect) * 0.5f, u1 = 1 - u0;
      else v0 = (1 - p.aspect / view) * 0.5f, v1 = 1 - v0;
      dl->AddImage(ui::Tex(p.picture), a, b, ImVec2(u0, v0), ImVec2(u1, v1));
    } else {
      dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.9f));
      dl->AddRectFilledMultiColor(a, b, ui::WithAlpha(color::kAccent, 0.16f), ui::WithAlpha(color::kAccent, 0.04f),
                                  ui::WithAlpha(color::kBase, 0.0f), ui::WithAlpha(color::kAccent, 0.08f));
    }
    (void)s;
  }

  void DrawDetail(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active) {
    const std::vector<Pack> packs = Build();
    const Pack* found = Find(packs, detail);
    if (!found) {  // removed meanwhile
      detail.clear();
      return;
    }
    // LB / RB: the next pack's page.
    if (active && (in.Pressed(Action::kPrevTab) || in.Pressed(Action::kNextTab))) {
      size_t k = size_t(found - packs.data());
      k = (k + (in.Pressed(Action::kNextTab) ? 1 : packs.size() - 1)) % packs.size();
      in.Consume(Action::kPrevTab);
      in.Consume(Action::kNextTab);
      ui::Cue(ui::Sound::kTab);
      Open(packs[k].key);
      found = &packs[k];
    }
    const Pack& p = *found;
    const float s = m.s;
    const ui::Fonts f = ui::GetFonts();
    const float avail = p1.x - p0.x, col_w = std::round(avail * 0.58f), wrap = col_w - 24 * s;
    // Left: name, facts, state, the description, its note (UHD), credits, problems.
    float y = p0.y + 8 * s;
    dl->PushClipRect(p0, ImVec2(p0.x + col_w, p1.y), true);
    dl->AddText(f.display, 30 * s, ImVec2(p0.x, y), color::kText, p.name.c_str(), nullptr, wrap);
    y += f.display->CalcTextSizeA(30 * s, FLT_MAX, wrap, p.name.c_str()).y + 8 * s;
    std::vector<std::string> facts;
    facts.push_back(T("Texture pack"));
    if (!p.author.empty()) facts.push_back(F("By {0}", {p.author}));
    if (!p.version.empty()) facts.push_back(F("Version {0}", {p.version}));
    if (p.kind == Kind::kHd || p.kind == Kind::kDownload)
      facts.push_back(F("{0} GB", {Gb(content::InfoFor(p.content_kind).unpacked)}));
    std::string fl;
    for (const auto& x : facts) fl += (fl.empty() ? "" : "   " KK_TP_DOT "   ") + x;
    dl->AddText(f.text, 14.5f * s, ImVec2(p0.x, y), color::kTextDim, ui::Ellipsize(f.text, 14.5f * s, fl, wrap).c_str());
    y += 26 * s;
    std::string state;
    ImU32 bg = 0, fg = 0;
    State(p, state, bg, fg);
    {
      const std::string up = text::Upper(state);
      const float fs = 12 * s, w = ui::TrackedWidth(f.display, fs, up, 1.5f * s) + 14 * s;
      dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + w, y + fs + 10 * s), bg, 2 * s);
      ui::DrawTracked(dl, f.display, fs, ImVec2(p0.x + 7 * s, y + 5 * s), fg, up, 1.5f * s);
      const char* note = p.enabled ? T("The game uses this texture pack. Only one texture pack is on at a time.")
                                   : T("Only one texture pack is on at a time: switching this one on switches the other off.");
      dl->AddText(f.text, 13.5f * s, ImVec2(p0.x + w + 12 * s, y + 3 * s), color::kTextFaint,
                  ui::Ellipsize(f.text, 13.5f * s, note, wrap - w - 12 * s).c_str());
      y += fs + 26 * s;
    }
    auto para = [&](const std::string& t, float size, ImU32 col) {
      if (t.empty()) return;
      dl->AddText(f.text, size * s, ImVec2(p0.x, y), col, t.c_str(), nullptr, wrap);
      y += f.text->CalcTextSizeA(size * s, FLT_MAX, wrap, t.c_str()).y + 12 * s;
    };
    auto caption = [&](const char* label) {
      ui::DrawTracked(dl, f.display, 13 * s, ImVec2(p0.x, y), color::kAccent, text::Upper(label), 2.2f * s);
      y += 24 * s;
    };
    para(p.description, 16, color::kText);
    if (p.page && p.page->note) {
      // Its note (UHD: the owner's note about the look, 18 Oct), on the page as well as before the download.
      const std::string note = T(p.page->note);
      const float h = f.text->CalcTextSizeA(14.5f * s, FLT_MAX, wrap - 28 * s, note.c_str()).y;
      dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + wrap, y + h + 24 * s), ui::WithAlpha(color::kWarn, 0.10f), 4 * s);
      dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + 3 * s, y + h + 24 * s), color::kWarn);
      dl->AddText(f.text, 14.5f * s, ImVec2(p0.x + 16 * s, y + 12 * s), color::kText, note.c_str(), nullptr, wrap - 28 * s);
      y += h + 24 * s + 16 * s;
    }
    if (p.busy && !p.installed) para(BusyText(p.st), 14.5f, color::kAccent);
    if (!p.problems.empty()) {
      para(T("This texture pack cannot be switched on:"), 14.5f, color::kBad);
      for (const auto& t : p.problems) para(t, 14.5f, color::kBad);
    }
    if (const std::string c = Credits(p); !c.empty()) {
      caption(T("Credits"));
      para(c, 13.5f, color::kTextDim);
    }
    dl->PopClipRect();

    // Right: picture, buttons, notes.
    const float x0 = p0.x + col_w + 48 * s, x1 = p1.x, pw = x1 - x0;
    float ry = p0.y + 8 * s;
    const float pic_h = std::round(std::min(pw * 9.0f / 16.0f, (p1.y - p0.y) * 0.36f)), pic_w = std::round(pic_h * 16.0f / 9.0f);
    const ImVec2 pa(x0, ry), pb(x0 + pic_w, ry + pic_h);
    DrawPicture(dl, p, pa, pb, s);
    dl->AddRect(pa, pb, ui::WithAlpha(color::kText, 0.10f));
    dl->AddRectFilled(pa, ImVec2(pa.x + 3 * s, pb.y), color::kAccent);
    ry = pb.y + 20 * s;
    std::vector<Button> buttons = Buttons(p);
    const int n = int(buttons.size());
    focus = std::clamp(focus, 0, std::max(0, n - 1));
    if (active && n > 1) {
      if (in.Pressed(Action::kUp)) focus = (focus + n - 1) % n, ui::Cue(ui::Sound::kMove);
      if (in.Pressed(Action::kDown)) focus = (focus + 1) % n, ui::Cue(ui::Sound::kMove);
    }
    const float bh = 44 * s;
    int clicked = -1;
    for (int k = 0; k < n; ++k) {
      const ImVec2 a(x0, ry), c(x1, ry + bh);
      const bool hot = active && ImGui::IsMouseHoveringRect(a, c, false);
      if (hot && in.mouse_moved()) focus = k;
      if (hot && ImGui::IsMouseClicked(0)) focus = k, clicked = k;
      const bool lit = focus == k, on = buttons[size_t(k)].enabled;
      if (lit) dl->AddRectFilled(a, c, on ? color::kFocus : ui::WithAlpha(color::kFocus, 0.35f));
      else {
        dl->AddRectFilled(a, c, ui::WithAlpha(color::kPanel, 0.85f));
        dl->AddRect(a, c, ui::WithAlpha(color::kText, 0.14f));
      }
      const std::string up = text::Upper(buttons[size_t(k)].label);
      const float lfs = 15 * s, tw = ui::TrackedWidth(f.display, lfs, up, 2 * s);
      ImU32 col = lit ? color::kOnFocus : color::kText;
      if (!on) col = lit ? color::kOnFocusDim : color::kTextFaint;
      ui::DrawTracked(dl, f.display, lfs, ImVec2(a.x + std::max(10 * s, (pw - tw) * 0.5f), a.y + (bh - lfs) * 0.5f), col, up, 2 * s);
      ry += bh + 10 * s;
    }
    if (active && n > 0 && in.Pressed(Action::kAccept)) {
      in.Consume(Action::kAccept);
      clicked = focus;
    }
    if (clicked >= 0) {
      Button& b = buttons[size_t(clicked)];
      if (!b.enabled || !b.run) ui::Cue(ui::Sound::kDeny);
      else {
        if (b.label != T("Enable") && b.label != T("Disable")) ui::Cue(ui::Sound::kSelect);
        b.run();
      }
    }
    ry += 6 * s;
    const char* note = T("Texture packs switch at once, also while you play (pause menu, Graphics). Packs from the mods "
                         "folder change at the next start of the game.");
    dl->AddText(f.text, 13.5f * s, ImVec2(x0, ry), color::kTextFaint, note, nullptr, pw);
  }

  void Dev(const std::string& c) {
    if (c == "state") {
      for (const Pack& p : Build()) {
        std::string state;
        ImU32 bg = 0, fg = 0;
        State(p, state, bg, fg);
        REXLOG_INFO("KK texture packs: card {} '{}' {} installed={} enabled={} usable={} busy={}", p.key, p.name, state,
                    p.installed, p.enabled, p.usable, p.busy);
      }
      REXLOG_INFO("KK texture packs: state on={} kk_mods='{}' folder {} '{}' own '{}' page '{}'",
                  texpacks::ActiveId(Current(), IsTex()), Flag("kk_mods"), Flag("native_texture_pack"),
                  Flag("native_texture_pack_folder"), OwnFolder(), detail);
    } else if (c.rfind("enable:", 0) == 0) {
      Enable(c.substr(7));
    } else if (c.rfind("open:", 0) == 0) {
      Open(c.substr(5));
    } else if (c.rfind("button:", 0) == 0) {  // the open page's button n
      const std::vector<Pack> packs = Build();
      if (const Pack* p = Find(packs, detail)) {
        auto b = Buttons(*p);
        const int k = std::atoi(c.c_str() + 7);
        if (k >= 0 && k < int(b.size()) && b[size_t(k)].enabled && b[size_t(k)].run) b[size_t(k)].run();
      }
    }
  }
};

TexturePackShelf::TexturePackShelf(fs::path user_dir, KongMods* mods)
    : impl_(std::make_unique<Impl>(std::move(user_dir), mods)) {}
TexturePackShelf::~TexturePackShelf() = default;
void TexturePackShelf::SetHost(const WorkshopHost* host) { impl_->host = host; }
std::vector<TexturePackShelf::Card> TexturePackShelf::Cards() { return impl_->Cards(); }
void TexturePackShelf::Activate(const std::string& key) {
  if (key == "import") {
    if (impl_->host && impl_->host->import_zip) impl_->host->import_zip(content::ImportTarget::kTexturePack);
    return;
  }
  impl_->Open(key);
}
bool TexturePackShelf::InDetail() const { return !impl_->detail.empty(); }
bool TexturePackShelf::Back() {
  if (impl_->detail.empty()) return false;
  impl_->detail.clear();
  return true;
}
std::string TexturePackShelf::DetailTitle() const {
  return impl_->detail.empty() ? std::string() : std::string(T("Texture packs"));
}
void TexturePackShelf::DrawDetail(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active) {
  impl_->DrawDetail(dl, p0, p1, m, in, active);
}
std::vector<std::pair<std::string, std::string>> TexturePackShelf::Choices() {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& p : impl_->Build())
    if (p.installed && p.usable) out.emplace_back(p.key, p.name);
  return out;
}
std::string TexturePackShelf::Active() {
  for (const auto& p : impl_->Build())
    if (p.enabled) return p.key;
  return "original";
}
void TexturePackShelf::Enable(const std::string& key) { impl_->Enable(key); }
void TexturePackShelf::OnModInstalled(const std::string& id) {
  impl_->pending_mod = id;
  impl_->pending_since = std::chrono::steady_clock::now();
  impl_->mods->Rescan();
}
void TexturePackShelf::OnModRemoved(const std::string& id) {
  texpacks::Choice c = Impl::Current();
  const size_t before = c.mods.size();
  c.mods.erase(std::remove(c.mods.begin(), c.mods.end(), id), c.mods.end());
  if (c.mods.size() != before) impl_->Apply(c, "a removed mod is no longer in the load order");
  impl_->mods->Rescan();
}
void TexturePackShelf::OnFolderOn() { impl_->pending_folder = true; }
void TexturePackShelf::Tick() { impl_->Tick(); }
void TexturePackShelf::Dev(const std::string& command) { impl_->Dev(command); }

}  // namespace kk
