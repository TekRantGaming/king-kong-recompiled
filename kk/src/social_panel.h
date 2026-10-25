#pragma once
#include "launcher_ui.h"
#include "social_panel_logic.h"

namespace kk::ui {
// Called after the SDK registers its shortcuts, before the launcher opens.
void ReserveSocialHotkey(bool enabled);
struct SocialChapters {
  int slot = -1, continue_slot = -1, continue_chapter = -1;
  std::function<rex::ui::ImmediateTexture*(int, float*)> picture;
};
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
  std::optional<coop::SocialRowKey> focused_row() const { return focus_; }
  bool chapters_open() const { return chapter_screen_; }
  std::string editor_text() const { return editor_text_; }
#endif
  // C81: the Play Online Co-op pages use the drawer's actions, Direct IP fields and text editor (one copy of
  // the logic). ip_path and direct_mode say which of the drawer's rows the page shows (SocialRows).
  void PageActivate(const coop::SocialRow& row, coop::ISocialService& service, bool ip_path, int direct_mode);
  void PrepareDirect(int direct_mode);  // 1 host (keeps or makes a code), 2 join (asks for the host's code)
  const std::string& direct_address() const { return direct_address_; }
  const std::string& direct_port() const { return direct_port_; }
  const std::string& direct_code() const { return direct_code_; }
  coop::SocialId invited() const { return invited_friend_; }
  std::string notice() const;  // the latest action's notice while it shows, else empty
  void DrawPageEditor(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Metrics& metrics, Input& input,
                      Glyphs& glyphs, const std::string& prompt_set, coop::ISocialService& service) {
    DrawEditor(dl, origin, size, metrics, input, glyphs, prompt_set, service);
  }
  // The drawer's Host and Join buttons open the pages (join: the Join a game page).
  void SetOpenPage(std::function<void(bool join)> open) { open_page_ = std::move(open); }
  void Update(bool enabled, coop::SocialPage page, Input& input, coop::ISocialService& service);
  void Draw(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Metrics& metrics, Input& input,
            Glyphs& glyphs, const std::string& prompt_set, rex::ui::ImmediateDrawer* drawer,
            coop::ISocialService& service, bool active, std::function<void()> add_steam = {}, SocialChapters chapters = {});
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
  bool editor_sync_ = false;
  int direct_mode_ = 0;
  bool ip_path_ = false;
  coop::SocialId invited_friend_ = 0;
  bool editor_lowercase_ = false, editor_symbols_ = false;
  bool chapter_screen_ = false;
  ListState chapter_list_;
  SocialChapters chapters_;
  void DrawChapters(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Metrics& metrics, Input& in,
                    Glyphs& glyphs, const std::string& set, coop::ISocialService& service, bool active);
  std::function<void()> add_steam_;
  std::function<void(bool)> open_page_;
  bool OpenPage(const coop::SocialRow& row);
  void DrawEditor(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Metrics& metrics, Input& in,
                  Glyphs& glyphs, const std::string& prompt_set, coop::ISocialService& service);
  void Activate(const coop::SocialRow& row, coop::ISocialService& service);
};
}  // namespace kk::ui
