#include "social_panel.h"
#include "launcher_text.h"
#include "ui_paint.h"
#include "toast.h"
#include "coop_runtime.h"
#include <rex/ui/immediate_drawer.h>
#include <algorithm>

namespace kk::ui {
using namespace coop;
using text::T;
using text::F;
struct SocialPanel::Avatars {
  struct Entry {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
  };
  std::map<SocialId, Entry> entries;
  ~Avatars() { for (auto& [id, e] : entries) KeepTextureAlive(std::move(e.texture)); }
  rex::ui::ImmediateTexture* Get(const SocialFriend& f, rex::ui::ImmediateDrawer* drawer) {
    if (!drawer || !f.avatar_width || !f.avatar_height || f.avatar_width > 64 || f.avatar_height > 64 ||
        f.avatar_rgba.size() != size_t(f.avatar_width) * f.avatar_height * 4) return nullptr;
    auto& e = entries[f.id];
    if (e.width != f.avatar_width || e.height != f.avatar_height || e.pixels != f.avatar_rgba) {
      KeepTextureAlive(std::move(e.texture));
      e.width = f.avatar_width; e.height = f.avatar_height; e.pixels = f.avatar_rgba;
      e.texture = drawer->CreateTexture(e.width, e.height, rex::ui::ImmediateTextureFilter::kLinear, false, e.pixels.data());
    }
    return e.texture.get();
  }
};
SocialPanel::SocialPanel() : avatars_(std::make_unique<Avatars>()) {}
SocialPanel::~SocialPanel() = default;
#if defined(KK_DEV_TOOLS)
void SocialPanel::ResetPreview() {
  list_ = {};
  focus_.reset();
  notice_.clear();
  notice_until_ = 0;
  seen_lobby_ = 0;
  seen_members_ = 0;
}
#endif
void SocialPanel::Update(bool enabled, SocialPage page, Input& in, ISocialService& service) {
  const bool was = state_.open();
  just_opened_ = state_.Update(enabled, page, in.Pressed(Action::kSocial), in.Pressed(Action::kBack), ImGui::GetIO().DeltaTime);
  if (just_opened_) { service.Act({SocialAction::Open}); Cue(Sound::kSelect); }
  if (was && !state_.open()) Cue(Sound::kBack);
  // The closing press cannot reach the page, including a zero-duration closing animation.
  if (was || state_.blocking()) {
    in.Consume(Action::kSocial);
    in.Consume(Action::kBack);
  }
  if (was && !state_.blocking()) in.ConsumeAll();
}
void SocialPanel::Activate(const SocialRow& row, ISocialService& service) {
  const auto command = SocialRowCommand(row, service.Snapshot());
  if (!command) { Cue(Sound::kDeny); return; }
  if (command->action == SocialAction::Start) notice_ = RequestSessionStart() ? "Starting co-op..." : "Choose Continue or a chapter before starting co-op";
  else if (!service.Act(*command)) notice_ = "Could not complete Social action";
  else if (command->action == SocialAction::Invite) notice_ = "Inviting...";
  else notice_.clear();
  notice_until_ = ImGui::GetTime() + 4.0;
}
void SocialPanel::Draw(ImDrawList* dl, ImVec2 o, ImVec2 size, const Metrics& m, Input& in,
                       Glyphs& glyphs, const std::string& set, rex::ui::ImmediateDrawer* drawer,
                       ISocialService& service, bool active) {
  if (!blocking()) return;
  const float s = m.s, slide = state_.slide();
  const float width = std::min(size.x, 490 * s), right = o.x + size.x;
  const float x = right - width * slide;
  const ImVec2 end(right, o.y + size.y);
  dl->AddRectFilled(o, end, IM_COL32(0, 0, 0, int(220 * slide)));
  dl->PushClipRect(ImVec2(std::max(o.x, x), o.y), end, true);
  dl->AddRectFilled(ImVec2(x, o.y), ImVec2(x + width, end.y), color::kPanel);
  dl->AddLine(ImVec2(x, o.y), ImVec2(x, end.y), color::kAccent, s);
  const auto fonts = GetFonts();
  const float left = x + 24 * s, available = width - 48 * s;
  dl->AddText(fonts.display, 30 * s, ImVec2(left, o.y + 22 * s), color::kText,
              Ellipsize(fonts.display, 30 * s, T("Social"), available).c_str());
  const SocialModel model = service.Snapshot();
  if (model.lobby && model.lobby == seen_lobby_ && seen_members_ == 2 && model.members.size() < 2) {
    notice_ = "Your partner left";
    notice_until_ = ImGui::GetTime() + 4.0;
  }
  seen_lobby_ = model.lobby;
  seen_members_ = model.members.size();
  std::string status;
  if (notice_until_ > ImGui::GetTime() && !notice_.empty() && model.last_error.empty() &&
      model.status != SocialStatus::Unavailable) status = T(notice_);
  else if (std::string_view(SocialStatusKey(model)) == "Versions don't match: {0}")
    status = F("Versions don't match: {0}", {T(model.last_error)});
  else status = T(SocialStatusKey(model));
  dl->AddText(fonts.text, 14 * s, ImVec2(left, o.y + 60 * s),
              model.status == SocialStatus::Idle && model.last_error.empty() && notice_until_ <= ImGui::GetTime()
                  ? color::kTextDim : color::kWarn,
              Ellipsize(fonts.text, 14 * s, status, available).c_str());
  auto rows = SocialRows(model);
  list_.focus = SocialFocus(rows, focus_, list_.focus);
  list_.focus_label.clear();  // retain opaque row identity, never a friend's display name
  std::vector<Item> items;
  items.reserve(rows.size());
  for (const auto& row : rows) {
    Item item;
    const auto kind = row.key.kind;
    item.type = kind == SocialRowKind::Section ? ItemType::kSection :
                kind == SocialRowKind::Text ? ItemType::kText :
                kind == SocialRowKind::Member ? ItemType::kInfo : ItemType::kAction;
    const bool person = kind == SocialRowKind::Member || kind == SocialRowKind::Friend;
    item.label = row.label;
    item.literal_label = person;
    item.disabled = !row.enabled && item.type == ItemType::kAction;
    item.focus_subtitle_color = color::kOnFocus;
    item.value_max_width = std::max(1.0f, available * 0.28f);
    if (kind == SocialRowKind::Friend) {
      item.tall = true;
      item.subtitle = row.detail;
      item.value_text = "Invite";
      item.value_action = Action::kDefault;
      item.value_color = color::kText;
      auto f = std::find_if(model.friends.begin(), model.friends.end(), [&](const auto& f) { return f.id == row.key.id; });
      if (f != model.friends.end()) item.icon = avatars_->Get(*f, drawer);
    } else if (kind == SocialRowKind::Member) {
      item.tall = true;
      item.value_text = row.detail;
      if (row.key.id == model.owner) item.subtitle = T("Host");
      if (row.key.id == model.self) {
        if (!item.subtitle.empty()) item.subtitle += " / ";
        item.subtitle += T("You");
      }
      item.literal_subtitle = true;
      item.value_color = row.detail == "Ready" ? color::kGood : color::kTextDim;
    } else if (kind == SocialRowKind::Accept || kind == SocialRowKind::Decline) {
      item.tall = true;
      item.subtitle = row.detail.empty() ? "Friend" : row.detail;
      item.literal_subtitle = !row.detail.empty();
    }
    item.on_activate = [this, row, &service] { Activate(row, service); };
    items.push_back(std::move(item));
  }
  const bool interactive = active && state_.open() && !just_opened_;
  // Tab stays in the drawer; arrows/stick/d-pad use the launcher's own list model.
  if (interactive && (in.Pressed(Action::kNextTab) || in.Pressed(Action::kPrevTab))) {
    in.Inject(in.Pressed(Action::kPrevTab) ? Action::kUp : Action::kDown, in.device());
  }
  const float top = o.y + 100 * s;
  DrawList(dl, items, list_, ImVec2(left, top), ImVec2(x + width - 24 * s, end.y - 106 * s), m, in,
           interactive, &glyphs, set);
  if (list_.focus >= 0 && list_.focus < int(rows.size())) {
    focus_ = rows[size_t(list_.focus)].key;
    if (interactive && in.Pressed(Action::kDefault) && focus_->kind == SocialRowKind::Friend) {
      Activate(rows[size_t(list_.focus)], service);
    }
  }
  // Two compact lines keep five-language prompts inside the narrow drawer.
  std::vector<Prompt> prompts = {{Action::kAccept, "Select"}, {Action::kBack, "Close"}};
  const Action clicked = DrawPrompts(dl, glyphs, set, prompts, x + width - 24 * s, end.y - 31 * s, m, interactive);
  if (clicked == Action::kAccept && list_.focus >= 0) Activate(rows[size_t(list_.focus)], service);
  if (clicked == Action::kBack) state_.Update(true, SocialPage::Page, false, true, 0);
  if (focus_ && focus_->kind == SocialRowKind::Friend && rows[size_t(list_.focus)].enabled) {
    if (DrawPrompts(dl, glyphs, set, {{Action::kDefault, "Invite"}}, x + width - 24 * s,
                    end.y - 72 * s, m, interactive) == Action::kDefault && list_.focus >= 0)
      Activate(rows[size_t(list_.focus)], service);
  }
  dl->PopClipRect();
  if (animating()) RequestUiPaint();
}
}  // namespace kk::ui
