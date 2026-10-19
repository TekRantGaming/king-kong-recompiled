// This file is UTF-8.
#include "workshop_mods.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>

#include "launcher_policy.h"
#include "launcher_text.h"
#include "mods.h"
#include "settings.h"
#include "texture_packs_logic.h"
#include "workshop_net.h"
#include "workshop_texpacks.h"

namespace kk {

namespace fs = std::filesystem;
namespace ws = kk::workshop;
namespace color = ui::color;
using mods::Conflict;
using mods::Issue;
using mods::Mod;
using mods::Problem;
using mods::Type;
using text::F;
using text::T;
using ui::Action;

#define KK_MODS_DOT "\xC2\xB7"

namespace {

std::string IssueText(const Issue& i) {
  switch (i.problem) {
    case Problem::kNoToml: return T("mod.toml is missing.");
    case Problem::kBadToml: return F("mod.toml could not be read: {0}", {i.detail});
    case Problem::kNoName: return T("mod.toml has no name.");
    case Problem::kBadType: return F("Unknown mod type: {0}. Use texture-pack, settings or cutscenes.", {i.detail});
    case Problem::kNoTextures: return T("No textures folder with a pack.txt.");
    case Problem::kBadPack: return F("pack.txt could not be read: {0}", {i.detail});
    case Problem::kNoMovies: return T("No movies folder with clips (.mp4 files).");
    case Problem::kNoSettings: return T("No settings in the [settings] table.");
    case Problem::kGameTooOld: return F("Needs game version {0} or newer.", {i.detail});
    case Problem::kGameTooNew: return F("Made for game version {0} or older.", {i.detail});
    case Problem::kBadFolderName: return T("The folder name cannot contain | or :.");
    case Problem::kNoPicture: return F("Picture not shown: {0}", {i.detail});
    case Problem::kUnknownSetting: return F("Unknown setting skipped: {0}", {i.detail});
    case Problem::kBlockedSetting: return F("Setting not allowed in mods, skipped: {0}", {i.detail});
    case Problem::kBadValue: return F("Value not accepted, skipped: {0}", {i.detail});
  }
  return {};
}

// A built-in mod's description in the player's language (mods_text_table.inc); a player's mod's as written.
std::string Description(const Mod& m) { return m.builtin ? T(m.description) : m.description; }

const char* TypeName(Type t) {
  switch (t) {
    case Type::kTexturePack: return T("Texture pack");
    case Type::kSettings: return T("Settings");
    case Type::kCutscenes: return T("Cutscenes");
    default: return "";
  }
}

std::string Join(const std::vector<std::string>& items, size_t total) {
  std::string out;
  for (const auto& s : items) out += (out.empty() ? "" : ", ") + s;
  if (total > items.size()) out += ", ...";
  return out;
}

struct ScanResult {
  std::vector<Mod> mods;
  std::vector<ws::Picture> pictures;  // per mod (empty: none)
};

}  // namespace

struct KongMods::Impl {
  rex::ui::ImmediateDrawer* drawer;
  // Task 221: a built-in mod's picture made from a still of the player's game (WorkshopHost::art).
  std::function<rex::ui::ImmediateTexture*(const std::string&, float*)> art;
  rex::ui::ImmediateTexture* Tex(int i, float* aspect) {
    rex::ui::ImmediateTexture* tex = textures.size() > size_t(i) ? textures[size_t(i)].get() : nullptr;
    *aspect = aspects.size() > size_t(i) ? aspects[size_t(i)] : 16.0f / 9.0f;
    if (!tex && art && size_t(i) < mods.size() && mods[size_t(i)].builtin) tex = art(mods[size_t(i)].id, aspect);
    return tex;
  }
  fs::path user_dir, config_path;

  // Worker: one scan at a time.
  std::mutex mutex;
  std::shared_ptr<ScanResult> result;  // set by the worker
  std::thread worker;
  std::atomic<bool> scanning{false};
  bool scanned = false;

  std::vector<Mod> mods;
  std::vector<std::unique_ptr<rex::ui::ImmediateTexture>> textures;
  std::vector<float> aspects;
  std::vector<std::string> enabled;  // kk_mods, load order
  std::vector<Conflict> conflicts;
  std::vector<int> order;  // tiles: indices into mods (enabled in load order, then the others)
  int tile = 0;
  float scroll_x = 0, scroll_x_target = 0;
  std::vector<float> lift;
  int detail = -1;  // index into mods
  int focus = 0;    // 0 Enable/Disable, 1 Load earlier, 2 Load later
  KongMods::Filter filter = KongMods::Filter::kAll;
  float detail_scroll = 0;

  Impl(rex::ui::ImmediateDrawer* d, fs::path u, fs::path c) : drawer(d), user_dir(std::move(u)), config_path(std::move(c)) {
    enabled = mods::ParseEnabled(rex::cvar::GetFlagByName("kk_mods"));
    Rescan();
  }
  ~Impl() {
    if (worker.joinable()) worker.join();
  }

  void Rescan() {
    if (scanning.exchange(true)) return;
    if (worker.joinable()) worker.join();
    const fs::path dir = user_dir;
    worker = std::thread([this, dir] {
      auto s = std::make_shared<ScanResult>();
      try {
        std::error_code ec;
        fs::create_directories(mods::UserDir(dir), ec);  // so the player finds the folder
        s->mods = mods::Scan(dir);
        for (const Mod& m : s->mods) {
          ws::Picture pic;
          if (!m.picture.empty()) {
            std::ifstream f(m.picture, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
            pic = ws::DecodePicture(bytes, 640);
          }
          s->pictures.push_back(std::move(pic));
        }
      } catch (const std::exception& e) {
        REXLOG_ERROR("KK mods: scan failed: {}", e.what());
      }
      std::lock_guard lock(mutex);
      result = std::move(s);
      scanning = false;
    });
  }

  void TakeScan() {
    std::shared_ptr<ScanResult> s;
    {
      std::lock_guard lock(mutex);
      s = std::move(result);
    }
    if (!s) return;
    const std::string open_id = detail >= 0 && detail < int(mods.size()) ? mods[size_t(detail)].id : std::string();
    mods = std::move(s->mods);
    textures.clear();
    aspects.clear();
    for (const auto& pic : s->pictures) {
      textures.push_back(pic && drawer ? drawer->CreateTexture(uint32_t(pic.width), uint32_t(pic.height),
                                                               rex::ui::ImmediateTextureFilter::kLinear, false,
                                                               pic.rgba.data())
                                       : nullptr);
      aspects.push_back(pic ? float(pic.width) / float(pic.height) : 16.0f / 9.0f);
    }
    lift.assign(mods.size(), 0.0f);
    scanned = true;
    detail = -1;
    for (size_t i = 0; i < mods.size(); ++i)
      if (mods[i].id == open_id) detail = int(i);
    Reorder();
    int usable = 0, problems = 0;
    for (const Mod& m : mods) (m.Usable() ? usable : problems)++;
    REXLOG_INFO("KK mods: found {} mods ({} usable, {} with problems), {} switched on", mods.size(), usable, problems,
                mods::EnabledInOrder(mods, enabled).size());
    for (const Mod& m : mods) {
      std::string issues;
      for (const Issue& i : m.issues) issues += (issues.empty() ? "" : "; ") + IssueText(i);
      REXLOG_INFO("KK mods: {} '{}' {} {}{}{}", m.id, m.name, mods::TypeId(m.type), m.Usable() ? "usable" : "broken",
                  issues.empty() ? "" : ": ", issues);
    }
  }

  bool IsOn(const Mod& m) const { return std::find(enabled.begin(), enabled.end(), m.id) != enabled.end(); }
  int Position(const Mod& m) const {
    const auto it = std::find(enabled.begin(), enabled.end(), m.id);
    return it == enabled.end() ? -1 : int(it - enabled.begin());
  }

  // Whether the row shows this mod (Filter).
  static bool IsTexturePack(const Mod& m) { return m.type == Type::kTexturePack; }
  bool Shown(const Mod& m) const {
    return filter == KongMods::Filter::kAll || (filter == KongMods::Filter::kTexturePacks) == IsTexturePack(m);
  }
  bool Shown(const std::string& id) const {
    const Mod* m = ById(id);
    return m && Shown(*m);
  }

  // Every mod in tile order, whatever the filter shows (switched-on in load order, built-ins, the others).
  std::vector<int> FullOrder() const {
    std::vector<int> out;
    for (const auto& id : enabled)
      for (size_t i = 0; i < mods.size(); ++i)
        if (mods[i].id == id) out.push_back(int(i));
    for (size_t i = 0; i < mods.size(); ++i)
      if (!IsOn(mods[i]) && mods[i].builtin) out.push_back(int(i));
    for (size_t i = 0; i < mods.size(); ++i)
      if (!IsOn(mods[i]) && !mods[i].builtin) out.push_back(int(i));
    return out;
  }

  void Reorder() {
    const int current = tile >= 0 && tile < int(order.size()) ? order[size_t(tile)] : -1;
    order.clear();
    for (const auto& id : enabled)
      for (size_t i = 0; i < mods.size(); ++i)
        if (mods[i].id == id && Shown(mods[i])) order.push_back(int(i));
    for (size_t i = 0; i < mods.size(); ++i)
      if (!IsOn(mods[i]) && mods[i].builtin && Shown(mods[i])) order.push_back(int(i));
    for (size_t i = 0; i < mods.size(); ++i)
      if (!IsOn(mods[i]) && !mods[i].builtin && Shown(mods[i])) order.push_back(int(i));
    conflicts = mods::FindConflicts(mods::EnabledInOrder(mods, enabled));
    if (current >= 0)
      for (size_t k = 0; k < order.size(); ++k)
        if (order[k] == current) tile = int(k);
    tile = std::clamp(tile, 0, std::max(0, int(order.size()) - 1));
  }

  std::vector<const Conflict*> ConflictsOf(const Mod& m) const {
    std::vector<const Conflict*> out;
    if (!IsOn(m)) return out;
    for (const Conflict& c : conflicts)
      if (c.earlier == m.id || c.later == m.id) out.push_back(&c);
    return out;
  }

  const Mod* ById(const std::string& id) const {
    for (const Mod& m : mods)
      if (m.id == id) return &m;
    return nullptr;
  }

  void Save(const char* what, const Mod& m) {
    rex::cvar::SetFlagByName("kk_mods", mods::JoinEnabled(enabled));
    if (!config_path.empty()) SaveSettings(config_path);
    Reorder();
    REXLOG_INFO("KK mods: {} {}; load order now: {}", what, m.id, mods::JoinEnabled(enabled));
  }

  void Toggle(Mod& m) {
    const bool on = !IsOn(m);
    if (on && !m.Usable()) {
      ui::Cue(ui::Sound::kDeny);
      return;
    }
    if (on && IsTexturePack(m)) {
      // Task 209: one texture pack at a time. This one on turns the others off, the HD texture pack and the
      // player's folder too (texture_packs_logic.h; the Workshop's texture pack row normally does this).
      texpacks::Choice c{rex::cvar::GetFlagByName("native_texture_pack") == "true",
                         rex::cvar::GetFlagByName("native_texture_pack_folder"), enabled};
      c = texpacks::EnableMod(c, m.id, [this](const std::string& id) { const Mod* x = ById(id); return x && IsTexturePack(*x); });
      enabled = c.mods;
      rex::cvar::SetFlagByName("native_texture_pack", c.folder_on ? "true" : "false");
      ui::Cue(ui::Sound::kOn);
      Save("switched on (the only texture pack on)", m);
      return;
    }
    mods::SetEnabled(enabled, m.id, on);
    ui::Cue(on ? ui::Sound::kOn : ui::Sound::kOff);
    Save(on ? "switched on" : "switched off", m);
  }

  // The place in the load order of the next switched-on mod this row shows, before (-1) or after (+1) `m`
  // (-1: none).
  int NeighbourPosition(const Mod& m, int delta) const {
    std::vector<bool> shown;
    for (const auto& id : enabled) shown.push_back(Shown(id));
    return WorkshopMoveTarget(shown, Position(m), delta);
  }

  void MoveMod(Mod& m, int delta) {
    const int to = NeighbourPosition(m, delta);
    if (to < 0 || !mods::Move(enabled, m.id, to - Position(m))) {
      ui::Cue(ui::Sound::kDeny);
      return;
    }
    ui::Cue(ui::Sound::kChange);
    Save(delta < 0 ? "moved earlier" : "moved later", m);
  }

  // ------------------------------------------------------------------ drawing ---
  static void PlainPanel(ImDrawList* dl, const ui::Fonts& f, ImVec2 a, ImVec2 b, float s, const std::string& name) {
    dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.9f));
    dl->AddRectFilledMultiColor(a, b, ui::WithAlpha(color::kAccent, 0.16f), ui::WithAlpha(color::kAccent, 0.04f),
                                ui::WithAlpha(color::kBase, 0.0f), ui::WithAlpha(color::kAccent, 0.08f));
    if (name.empty()) return;
    const float fs = std::min(20 * s, (b.y - a.y) * 0.18f), wrap = (b.x - a.x) - 28 * s;
    const ImVec2 size = f.display->CalcTextSizeA(fs, FLT_MAX, wrap, name.c_str());
    dl->PushClipRect(a, b, true);
    dl->AddText(f.display, fs, ImVec2(a.x + 14 * s, (a.y + b.y - size.y) * 0.5f), ui::WithAlpha(color::kText, 0.85f),
                name.c_str(), nullptr, wrap);
    dl->PopClipRect();
  }

  void Picture(ImDrawList* dl, const ui::Fonts& f, int i, ImVec2 a, ImVec2 b, float s, bool bright) {
    float aspect = 16.0f / 9.0f;
    rex::ui::ImmediateTexture* tex = Tex(i, &aspect);
    if (!tex) {
      PlainPanel(dl, f, a, b, s, mods[size_t(i)].name);
      return;
    }
    // Cropped to the frame's shape.
    const float view = (b.x - a.x) / std::max(1.0f, b.y - a.y);
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (aspect > view) u0 = (1 - view / aspect) * 0.5f, u1 = 1 - u0;
    else v0 = (1 - aspect / view) * 0.5f, v1 = 1 - v0;
    dl->AddImage(ui::Tex(tex), a, b, ImVec2(u0, v0), ImVec2(u1, v1), bright ? IM_COL32_WHITE : IM_COL32(205, 208, 214, 255));
  }

  static float Badge(ImDrawList* dl, const ui::Fonts& f, float x, float y, float s, const std::string& label, ImU32 bg,
                     ImU32 fg) {
    const float fs = 11.5f * s;
    const std::string up = text::Upper(label);
    const float w = ui::TrackedWidth(f.display, fs, up, 1.5f * s) + 12 * s;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + fs + 8 * s), bg, 2 * s);
    ui::DrawTracked(dl, f.display, fs, ImVec2(x + 6 * s, y + 4 * s), fg, up, 1.5f * s);
    return w;
  }

  bool DrawRow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active, bool focused) {
    TakeScan();
    const float s = m.s;
    const ui::Fonts f = ui::GetFonts();
    const float avail = p1.x - p0.x, gap = 16 * s;
    const float band_h = 50 * s, foot_h = 44 * s;
    const float room = std::max(60 * s, p1.y - p0.y - foot_h);
    const float tile_w = std::round(std::clamp((room - band_h) * 16.0f / 9.0f, 150 * s, 250 * s));
    const float pic_h = std::round(tile_w * 9.0f / 16.0f), tile_h = pic_h + band_h;
    bool took = false;
    const std::string folder = mods::U8(mods::UserDir(user_dir));
    if (!scanned || order.empty()) {
      const ImVec2 a(p0.x, p0.y + 6 * s), b(p1.x, p0.y + 6 * s + std::min(tile_h, 92 * s));
      if (active && ImGui::IsMouseHoveringRect(a, b, false) && in.mouse_moved()) took = true;
      dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.82f));
      dl->AddRectFilled(a, ImVec2(a.x + 3 * s, b.y), ui::WithAlpha(color::kAccent, 0.6f));
      const char* head = scanned ? T("No mods found.") : T("Looking for mods");
      ui::DrawTracked(dl, f.display, 18 * s, ImVec2(a.x + 24 * s, a.y + 18 * s), color::kText, text::Upper(head), 2.5f * s);
      dl->AddText(f.text, 14.5f * s, ImVec2(a.x + 24 * s, a.y + 50 * s), color::kTextDim,
                  F("Put your mods in {0}. MODDING.md explains how to make one.", {folder}).c_str(), nullptr,
                  avail - 48 * s);
      if (focused) dl->AddRect(ImVec2(a.x - 1 * s, a.y - 1 * s), ImVec2(b.x + 1 * s, b.y + 1 * s), color::kFocus, 0, 0, 2 * s);
      if (focused && active && in.Pressed(Action::kAccept)) {
        in.Consume(Action::kAccept);
        ui::Cue(ui::Sound::kDeny);
      }
      return took;
    }
    const int n = int(order.size()), before = tile;
    const float content_w = n * tile_w + (n - 1) * gap, max_x = std::max(0.0f, content_w - avail);
    const ImVec2 r0(p0.x - 14 * s, p0.y), r1(p1.x + 14 * s, p0.y + tile_h + 12 * s);
    if (active && focused) {
      if (in.Pressed(Action::kLeft) || in.Pressed(Action::kPrevTab)) tile = std::max(0, tile - 1);
      if (in.Pressed(Action::kRight) || in.Pressed(Action::kNextTab)) tile = std::min(n - 1, tile + 1);
      in.Consume(Action::kPrevTab);
      in.Consume(Action::kNextTab);
    }
    if (active) {
      const float wheel = ImGui::GetIO().MouseWheel + ImGui::GetIO().MouseWheelH;
      if (wheel != 0 && ImGui::IsMouseHoveringRect(r0, r1, false))
        scroll_x_target = std::clamp(scroll_x_target - wheel * (tile_w + gap) * 0.5f, 0.0f, max_x);
    }
    if (tile != before) {
      ui::Cue(ui::Sound::kMove);
      const float tx = tile * (tile_w + gap);
      if (tx < scroll_x_target) scroll_x_target = tx;
      if (tx + tile_w > scroll_x_target + avail) scroll_x_target = tx + tile_w - avail;
    }
    scroll_x_target = std::clamp(scroll_x_target, 0.0f, max_x);
    scroll_x = ui::Approach(scroll_x, scroll_x_target, 14.0f);
    int open = -1;
    dl->PushClipRect(r0, r1, true);
    for (int k = 0; k < n; ++k) {
      const int i = order[size_t(k)];
      const Mod& md = mods[size_t(i)];
      const float x = std::round(p0.x + k * (tile_w + gap) - scroll_x);
      if (x > r1.x || x + tile_w < r0.x) continue;
      const bool on_tile = focused && tile == k;
      lift[size_t(i)] = ui::Approach(lift[size_t(i)], on_tile ? 100.0f : 0.0f, 12.0f);
      const float y = std::round(p0.y + 8 * s - 5 * s * lift[size_t(i)] / 100.0f);
      const ImVec2 a(x, y), b(x + tile_w, y + tile_h), pb(x + tile_w, y + pic_h);
      if (active && ImGui::IsMouseHoveringRect(ImVec2(std::max(a.x, p0.x - 14 * s), a.y), b, false)) {
        if (in.mouse_moved()) took = true, tile = k;
        if (ImGui::IsMouseClicked(0)) took = true, tile = k, open = k;
      }
      dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.88f));
      Picture(dl, f, i, a, pb, s, on_tile);
      const bool on = IsOn(md);
      // Badges: Built-in on the left, On / Off and a problem or conflict on the right.
      if (md.builtin) Badge(dl, f, a.x + 8 * s, a.y + 8 * s, s, T("Built-in"), ui::WithAlpha(color::kAccent, 0.92f), color::kOnFocus);
      const std::string state = on ? std::to_string(Position(md) + 1) + "  " + T("On") : std::string(T("Off"));
      const float sw = ui::TrackedWidth(f.display, 11.5f * s, text::Upper(state), 1.5f * s) + 12 * s;
      Badge(dl, f, b.x - 8 * s - sw, a.y + 8 * s, s, state, on ? ui::WithAlpha(color::kGood, 0.92f) : ui::WithAlpha(color::kBase, 0.8f),
            on ? color::kOnFocus : color::kTextDim);
      if (!md.Usable()) Badge(dl, f, a.x + 8 * s, pb.y - 28 * s, s, T("Problem"), ui::WithAlpha(color::kBad, 0.92f), color::kOnFocus);
      else if (!ConflictsOf(md).empty())
        Badge(dl, f, a.x + 8 * s, pb.y - 28 * s, s, T("Conflict"), ui::WithAlpha(color::kWarn, 0.92f), color::kOnFocus);
      const float nfs = 15.5f * s;
      dl->AddText(f.semibold, nfs, ImVec2(x + 12 * s, pb.y + 8 * s), on_tile ? color::kText : color::kTextDim,
                  ui::Ellipsize(f.semibold, nfs, md.name, tile_w - 24 * s).c_str());
      std::string sub = md.author;
      if (!md.version.empty()) sub += (sub.empty() ? "" : "   " KK_MODS_DOT "   ") + F("Version {0}", {md.version});
      dl->AddText(f.text, 12.5f * s, ImVec2(x + 12 * s, pb.y + 29 * s), color::kTextFaint,
                  ui::Ellipsize(f.text, 12.5f * s, sub, tile_w - 24 * s).c_str());
      if (on_tile) {
        dl->AddRect(ImVec2(a.x - 1 * s, a.y - 1 * s), ImVec2(b.x + 1 * s, b.y + 1 * s), color::kFocus, 0, 0, 2 * s);
        dl->AddRectFilled(ImVec2(a.x, pb.y), ImVec2(a.x + 3 * s, b.y), color::kAccent);
      } else {
        dl->AddRect(a, b, ui::WithAlpha(color::kText, 0.08f));
      }
    }
    dl->PopClipRect();
    if (scroll_x < max_x - 1)
      dl->AddRectFilledMultiColor(ImVec2(p1.x - 40 * s, r0.y), ImVec2(p1.x + 14 * s, r1.y), ui::WithAlpha(color::kBase, 0.0f),
                                  ui::WithAlpha(color::kBase, 0.75f), ui::WithAlpha(color::kBase, 0.75f),
                                  ui::WithAlpha(color::kBase, 0.0f));
    // Below: the focused mod's description (one line), then where the mods go.
    float y = p0.y + tile_h + 18 * s;
    if (focused) {
      const Mod& md = mods[size_t(order[size_t(tile)])];
      std::string line = TypeName(md.type);
      if (!md.description.empty()) {
        const std::string desc = Description(md);
        std::string d = desc.substr(0, desc.find('\n'));
        line += (line.empty() ? "" : "   " KK_MODS_DOT "   ") + d;
      }
      dl->AddText(f.text, 14.5f * s, ImVec2(p0.x, y), color::kTextDim, ui::Ellipsize(f.text, 14.5f * s, line, avail).c_str());
    }
    dl->AddText(f.text, 13 * s, ImVec2(p0.x, y + 20 * s), color::kTextFaint,
                ui::Ellipsize(f.text, 13 * s, F("Put your mods in {0}. MODDING.md explains how to make one.", {folder}), avail * 0.62f)
                    .c_str());
    if (active && focused && in.Pressed(Action::kAccept)) {
      in.Consume(Action::kAccept);
      open = tile;
    }
    if (open >= 0) {
      ui::Cue(ui::Sound::kSelect);
      Open(order[size_t(open)]);
    }
    return took;
  }

  void Open(int i) {
    detail = i;
    focus = 0;
    detail_scroll = 0;
    REXLOG_INFO("KK mods: open {}", mods[size_t(i)].id);
  }

  void DrawDetail(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active) {
    TakeScan();
    if (detail < 0 || detail >= int(mods.size())) {
      detail = -1;
      return;
    }
    const float s = m.s;
    const ui::Fonts f = ui::GetFonts();
    if (active && (in.Pressed(Action::kPrevTab) || in.Pressed(Action::kNextTab)) && !order.empty()) {
      int k = int(std::find(order.begin(), order.end(), detail) - order.begin());
      k = (k + (in.Pressed(Action::kNextTab) ? 1 : int(order.size()) - 1)) % int(order.size());
      in.Consume(Action::kPrevTab);
      in.Consume(Action::kNextTab);
      ui::Cue(ui::Sound::kTab);
      tile = k;
      Open(order[size_t(k)]);
    }
    Mod& md = mods[size_t(detail)];
    const bool on = IsOn(md);
    const float avail = p1.x - p0.x, col_w = std::round(avail * 0.58f), wrap = col_w - 24 * s;
    // Left: name, facts, description, what it changes, problems and conflicts.
    float y = p0.y + 8 * s;
    dl->PushClipRect(ImVec2(p0.x, p0.y), ImVec2(p0.x + col_w, p1.y), true);
    dl->AddText(f.display, 30 * s, ImVec2(p0.x, y), color::kText, md.name.c_str(), nullptr, wrap);
    y += f.display->CalcTextSizeA(30 * s, FLT_MAX, wrap, md.name.c_str()).y + 8 * s;
    float bx = p0.x;
    if (md.builtin) bx += Badge(dl, f, bx, y, s, T("Built-in"), ui::WithAlpha(color::kAccent, 0.92f), color::kOnFocus) + 10 * s;
    std::vector<std::string> facts;
    if (const char* t = TypeName(md.type); *t) facts.push_back(t);
    if (!md.author.empty()) facts.push_back(F("By {0}", {md.author}));
    if (!md.version.empty()) facts.push_back(F("Version {0}", {md.version}));
    std::string fl;
    for (const auto& x : facts) fl += (fl.empty() ? "" : "   " KK_MODS_DOT "   ") + x;
    dl->AddText(f.text, 14.5f * s, ImVec2(bx, y + 1 * s), color::kTextDim, ui::Ellipsize(f.text, 14.5f * s, fl, p0.x + wrap - bx).c_str());
    y += 34 * s;
    if (!md.description.empty()) {
      const std::string desc = Description(md);
      dl->AddText(f.text, 16 * s, ImVec2(p0.x, y), color::kText, desc.c_str(), nullptr, wrap);
      y += f.text->CalcTextSizeA(16 * s, FLT_MAX, wrap, desc.c_str()).y + 16 * s;
    }
    // Task 221: what it changes, how it looks, what it goes well with, what it needs (built-in mods).
    y = texpacks::DrawContentFacts(dl, md.id, {}, p0.x, y, wrap, s);
    auto caption = [&](const char* label) {
      const std::string up = text::Upper(label);
      ui::DrawTracked(dl, f.display, 13 * s, ImVec2(p0.x, y), color::kAccent, up, 2.2f * s);
      y += 24 * s;
    };
    auto para = [&](const std::string& t, ImU32 col) {
      dl->AddText(f.text, 14.5f * s, ImVec2(p0.x, y), col, t.c_str(), nullptr, wrap);
      y += f.text->CalcTextSizeA(14.5f * s, FLT_MAX, wrap, t.c_str()).y + 6 * s;
    };
    // A built-in mod's settings are ours: its facts table says what it changes in words (task 221).
    if (md.type == Type::kSettings && !md.settings.empty() && !md.builtin) {
      caption(T("What it changes"));
      size_t shown = 0;
      for (const auto& set : md.settings) {
        if (++shown > 12) {
          para("...", color::kTextDim);
          break;
        }
        para(set.name + " = " + set.value, color::kTextDim);
      }
      y += 8 * s;
    }
    std::vector<std::string> bad, warn;
    for (const Issue& i : md.issues) (mods::IsWarning(i.problem) ? warn : bad).push_back(IssueText(i));
    for (const Conflict* c : ConflictsOf(md)) {
      const Mod* other = ById(c->earlier == md.id ? c->later : c->earlier);
      const std::string name = other ? other->name : (c->earlier == md.id ? c->later : c->earlier);
      if (c->type == Type::kSettings)
        warn.push_back(F("Changes the same settings as {0}: {1}. The one later in the load order wins.", {name, Join(c->items, c->count)}));
      else if (c->type == Type::kCutscenes)
        warn.push_back(F("Replaces the same cutscenes as {0}: {1}. The one later in the load order wins.", {name, Join(c->items, c->count)}));
      else
        warn.push_back(F("Changes the same textures as {0} ({1} textures). The one later in the load order wins.",
                         {name, std::to_string(c->count)}));
    }
    if (!bad.empty()) {
      para(T("This mod cannot be switched on:"), color::kBad);
      for (const auto& t : bad) para(t, color::kBad);
      y += 6 * s;
    }
    for (const auto& t : warn) para(t, color::kWarn);
    dl->PopClipRect();

    // Right: picture, state, buttons, notes.
    const float x0 = p0.x + col_w + 48 * s, x1 = p1.x, pw = x1 - x0;
    float ry = p0.y + 8 * s;
    const float pic_h = std::round(std::min(pw * 9.0f / 16.0f, (p1.y - p0.y) * 0.34f)), pic_w = std::round(pic_h * 16.0f / 9.0f);
    const ImVec2 pa(x0, ry), pb(x0 + pic_w, ry + pic_h);
    Picture(dl, f, detail, pa, pb, s, true);
    dl->AddRect(pa, pb, ui::WithAlpha(color::kText, 0.10f));
    dl->AddRectFilled(pa, ImVec2(pa.x + 3 * s, pb.y), color::kAccent);
    ry = pb.y + 16 * s;
    const std::string state = on ? F("On: number {0} in the load order", {std::to_string(Position(md) + 1)}) : std::string(T("Off"));
    dl->AddText(f.semibold, 16 * s, ImVec2(x0, ry), on ? color::kGood : color::kTextDim, state.c_str(), nullptr, pw);
    ry += 30 * s;
    const bool can_toggle = on || md.Usable();
    const bool can_up = on && NeighbourPosition(md, -1) >= 0, can_down = on && NeighbourPosition(md, +1) >= 0;
    const bool enabled_button[3] = {can_toggle, can_up, can_down};
    if (active) {
      if (in.Pressed(Action::kUp)) focus = (focus + 2) % 3, ui::Cue(ui::Sound::kMove);
      if (in.Pressed(Action::kDown)) focus = (focus + 1) % 3, ui::Cue(ui::Sound::kMove);
    }
    const float bh = 44 * s;
    int clicked = -1;
    for (int b = 0; b < 3; ++b) {
      const ImVec2 a(x0, ry), c(x1, ry + bh);
      const bool hot = active && ImGui::IsMouseHoveringRect(a, c, false);
      if (hot && in.mouse_moved()) focus = b;
      if (hot && ImGui::IsMouseClicked(0)) focus = b, clicked = b;
      const bool lit = focus == b;
      const char* label = b == 0 ? (on ? T("Disable") : T("Enable")) : b == 1 ? T("Load earlier") : T("Load later");
      if (lit) dl->AddRectFilled(a, c, enabled_button[b] ? color::kFocus : ui::WithAlpha(color::kFocus, 0.35f));
      else {
        dl->AddRectFilled(a, c, ui::WithAlpha(color::kPanel, 0.85f));
        dl->AddRect(a, c, ui::WithAlpha(color::kText, 0.14f));
      }
      const std::string up = text::Upper(label);
      const float lfs = 15 * s, tw = ui::TrackedWidth(f.display, lfs, up, 2 * s);
      ImU32 col = lit ? color::kOnFocus : color::kText;
      if (!enabled_button[b]) col = lit ? color::kText : color::kTextFaint;  // on the faint bar (task 261)
      ui::DrawTracked(dl, f.display, lfs, ImVec2(a.x + (pw - tw) * 0.5f, a.y + (bh - lfs) * 0.5f), col, up, 2 * s);
      ry += bh + 10 * s;
    }
    if (active && in.Pressed(Action::kAccept)) {
      in.Consume(Action::kAccept);
      clicked = focus;
    }
    if (clicked == 0) Toggle(md);
    else if (clicked == 1) MoveMod(md, -1);
    else if (clicked == 2) MoveMod(md, 1);
    ry += 4 * s;
    for (const char* note : {T("Changes apply the next time the game starts."),
                             T("When two mods change the same thing, the one later in the load order wins.")}) {
      dl->AddText(f.text, 13.5f * s, ImVec2(x0, ry), color::kTextFaint, note, nullptr, pw);
      ry += f.text->CalcTextSizeA(13.5f * s, FLT_MAX, pw, note).y + 8 * s;
    }
  }

  void Dev(const std::string& c) {
    TakeScan();
    if (c == "state") {
      for (size_t k = 0; k < order.size(); ++k) {
        const Mod& md = mods[size_t(order[k])];
        REXLOG_INFO("KK mods: tile {} {} on={} pos={} usable={} conflicts={}", k, md.id, IsOn(md), Position(md), md.Usable(),
                    ConflictsOf(md).size());
      }
      REXLOG_INFO("KK mods: state scanned={} tiles={} detail={} kk_mods='{}'", scanned, order.size(),
                  detail >= 0 ? mods[size_t(detail)].id : "-", rex::cvar::GetFlagByName("kk_mods"));
    } else if (c.rfind("open:", 0) == 0) {
      const int k = std::atoi(c.c_str() + 5);
      if (k >= 0 && k < int(order.size())) tile = k, Open(order[size_t(k)]);
    } else if (c.rfind("openid:", 0) == 0) {
      for (size_t k = 0; k < order.size(); ++k)
        if (mods[size_t(order[k])].id == c.substr(7)) tile = int(k), Open(order[k]);
    } else if (c.rfind("tile:", 0) == 0) {
      detail = -1;
      tile = std::clamp(std::atoi(c.c_str() + 5), 0, std::max(0, int(order.size()) - 1));
    } else if (c == "toggle" && detail >= 0) {
      Toggle(mods[size_t(detail)]);
    } else if (c == "earlier" && detail >= 0) {
      MoveMod(mods[size_t(detail)], -1);
    } else if (c == "later" && detail >= 0) {
      MoveMod(mods[size_t(detail)], 1);
    } else if (c == "rescan") {
      Rescan();
    }
  }
};

std::vector<KongMods::Card> KongMods::Cards() {
  impl_->TakeScan();
  std::vector<Card> out;
  for (const int i : impl_->FullOrder()) {
    const Mod& m = impl_->mods[size_t(i)];
    Card c;
    c.id = m.id;
    c.name = m.name;
    c.author = m.author;
    c.version = m.version;
    c.description = Description(m);
    c.picture = impl_->Tex(i, &c.aspect);
    c.builtin = m.builtin;
    c.on = impl_->IsOn(m);
    c.position = impl_->Position(m);
    c.usable = m.Usable();
    c.conflict = !impl_->ConflictsOf(m).empty();
    c.texture_pack = Impl::IsTexturePack(m);
    c.dir = m.dir;
    for (const Issue& is : m.issues)
      if (!mods::IsWarning(is.problem)) c.problems.push_back(IssueText(is));
    out.push_back(std::move(c));
  }
  return out;
}
bool KongMods::Scanned() {
  impl_->TakeScan();
  return impl_->scanned;
}
void KongMods::OpenMod(const std::string& id) {
  impl_->TakeScan();
  for (size_t k = 0; k < impl_->order.size(); ++k)
    if (impl_->mods[size_t(impl_->order[k])].id == id) {
      impl_->tile = int(k);
      impl_->Open(impl_->order[k]);
      return;
    }
  for (size_t i = 0; i < impl_->mods.size(); ++i)
    if (impl_->mods[i].id == id) impl_->Open(int(i));
}
void KongMods::ReloadEnabled() {
  impl_->enabled = mods::ParseEnabled(rex::cvar::GetFlagByName("kk_mods"));
  impl_->Reorder();
}
void KongMods::Rescan() { impl_->Rescan(); }
bool KongMods::IsTexturePack(const std::string& id) {
  impl_->TakeScan();
  const Mod* m = impl_->ById(id);
  return m && Impl::IsTexturePack(*m);
}

KongMods::KongMods(rex::ui::ImmediateDrawer* drawer, fs::path user_dir, fs::path config_path)
    : impl_(std::make_unique<Impl>(drawer, std::move(user_dir), std::move(config_path))) {}
KongMods::~KongMods() = default;
bool KongMods::DrawRow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active, bool focused) {
  return impl_->DrawRow(dl, p0, p1, m, in, active, focused);
}
void KongMods::SetArt(std::function<rex::ui::ImmediateTexture*(const std::string&, float*)> art) {
  impl_->art = std::move(art);
}

void KongMods::SetFilter(Filter filter) {
  if (impl_->filter == filter) return;
  impl_->filter = filter;
  impl_->detail = -1;
  impl_->tile = 0;
  impl_->scroll_x = impl_->scroll_x_target = 0;
  impl_->Reorder();
}
KongMods::Counts KongMods::GetCounts() {
  impl_->TakeScan();
  Counts c;
  c.scanned = impl_->scanned;
  for (const Mod& m : impl_->mods) {
    const bool on = impl_->IsOn(m), tex = Impl::IsTexturePack(m);
    (tex ? c.texture_packs : c.mods)++;
    if (on) (tex ? c.texture_packs_on : c.mods_on)++;
  }
  return c;
}
bool KongMods::InDetail() const { return impl_->detail >= 0; }
void KongMods::DrawDetail(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active) {
  impl_->DrawDetail(dl, p0, p1, m, in, active);
}
bool KongMods::Back() {
  if (impl_->detail < 0) return false;
  impl_->detail = -1;
  return true;
}
std::vector<ui::Prompt> KongMods::Prompts(bool focused) const {
  if (impl_->detail >= 0) return {{Action::kAccept, "Select"}, {Action::kBack, "Back"}};
  if (focused && !impl_->order.empty()) return {{Action::kAccept, "Select"}, {Action::kBack, "Back"}};
  return {{Action::kBack, "Back"}};
}
void KongMods::Dev(const std::string& command) { impl_->Dev(command); }

}  // namespace kk
