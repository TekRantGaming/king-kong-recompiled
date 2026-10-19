#pragma once
#include "launcher_ui.h"
#include "social_panel_logic.h"

namespace kk::ui {
class SocialPanel {
 public:
  SocialPanel();
  ~SocialPanel();
  bool blocking() const { return state_.blocking(); }
  bool animating() const { return state_.blocking() && (!state_.open() || state_.slide() < 1); }
  bool editing() const { return editor_ != 0; }
#if defined(KK_DEV_TOOLS)
  void ResetPreview();  // deterministic focus/scroll and notices after a developer scene change
  bool DirectInput(std::string_view command, coop::ISocialService& service);
#endif
  void Update(bool enabled, coop::SocialPage page, Input& input, coop::ISocialService& service);
  void Draw(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Metrics& metrics, Input& input,
            Glyphs& glyphs, const std::string& prompt_set, rex::ui::ImmediateDrawer* drawer,
            coop::ISocialService& service, bool active, std::function<void()> add_steam = {});
 private:
  struct Avatars;
  std::unique_ptr<Avatars> avatars_;
  coop::SocialPanelState state_;
  ListState list_;
  std::optional<coop::SocialRowKey> focus_;
  std::string notice_;
  double notice_until_ = 0;
  bool just_opened_ = false;
  coop::SocialId seen_lobby_ = 0;
  size_t seen_members_ = 0;
  std::string direct_address_ = "127.0.0.1", direct_port_ = "27080", direct_code_;
  std::string editor_text_;
  int editor_ = 0, editor_key_ = 0;
  bool editor_just_opened_ = false;
  bool editor_lowercase_ = false, editor_symbols_ = false;
  std::function<void()> add_steam_;
  void DrawEditor(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Metrics& metrics, Input& in,
                  Glyphs& glyphs, const std::string& prompt_set);
  void Activate(const coop::SocialRow& row, coop::ISocialService& service);
};
}  // namespace kk::ui
