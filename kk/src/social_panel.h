#pragma once
#include "launcher_ui.h"
#include "social_panel_logic.h"

namespace kk::ui {
// Called after the SDK registers its shortcuts, before the launcher opens.
void ReserveSocialHotkey(bool enabled);
// UI1 (29 Oct): the old RT Social drawer is gone (Craig); the Play Online Co-op pages replaced it. What is left
// is the logic those pages use: the row actions (validated against the latest Social snapshot), the Direct IP
// address, port and code with their text editor (keyboard, controller keys, mouse), the action notices and the
// Steam friends' avatars.
class SocialPanel {
 public:
  SocialPanel();
  ~SocialPanel();
  bool editing() const { return editor_ != 0; }
#if defined(KK_DEV_TOOLS)
  void ResetPreview();  // deterministic notices and fields after a developer scene change
  bool DirectInput(std::string_view command, coop::ISocialService& service);
  std::string editor_text() const { return editor_text_; }
#endif
  // The co-op pages' rows: ip_path and direct_mode say which Social rows (SocialRows) the action is checked
  // against (direct_mode 1 host, 2 join).
  void PageActivate(const coop::SocialRow& row, coop::ISocialService& service, bool ip_path, int direct_mode);
  void PrepareDirect(int direct_mode);  // 1 host (keeps or makes a code), 2 join (asks for the host's code)
  const std::string& direct_address() const { return direct_address_; }
  const std::string& direct_port() const { return direct_port_; }
  const std::string& direct_code() const { return direct_code_; }
  coop::SocialId invited() const { return invited_friend_; }
  std::string notice() const;  // the latest action's notice while it shows, else empty
  // Once a frame while the pages show: notices that follow the lobby (the partner left).
  void Tick(const coop::SocialModel& model);
  // A Steam friend's avatar (nullptr when Steam gave none).
  rex::ui::ImmediateTexture* Avatar(const coop::SocialFriend& f, rex::ui::ImmediateDrawer* drawer);
  void DrawPageEditor(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Metrics& metrics, Input& input,
                      Glyphs& glyphs, const std::string& prompt_set, coop::ISocialService& service) {
    DrawEditor(dl, origin, size, metrics, input, glyphs, prompt_set, service);
  }
 private:
  struct Avatars;
  std::unique_ptr<Avatars> avatars_;
  std::string notice_;
  double notice_until_ = 0;
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
  void DrawEditor(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Metrics& metrics, Input& in,
                  Glyphs& glyphs, const std::string& prompt_set, coop::ISocialService& service);
  void Activate(const coop::SocialRow& row, coop::ISocialService& service);
};
}  // namespace kk::ui
