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
#if defined(KK_DEV_TOOLS)
  void ResetPreview();  // deterministic focus/scroll and notices after a developer scene change
#endif
  void Update(bool enabled, coop::SocialPage page, Input& input, coop::ISocialService& service);
  void Draw(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Metrics& metrics, Input& input,
            Glyphs& glyphs, const std::string& prompt_set, rex::ui::ImmediateDrawer* drawer,
            coop::ISocialService& service, bool active);
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
  void Activate(const coop::SocialRow& row, coop::ISocialService& service);
};
}  // namespace kk::ui
