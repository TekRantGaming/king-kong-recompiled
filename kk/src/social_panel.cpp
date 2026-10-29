#include "social_panel.h"
#include "launcher_text.h"
#include "ui_paint.h"
#include "toast.h"
#include "coop_runtime.h"
#include "coop_direct_udp.h"
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <algorithm>
#include <charconv>

namespace kk::ui {
using namespace coop;
using text::T;
using text::F;
void ReserveSocialHotkey(bool enabled) { if (enabled) rex::ui::UnregisterBind("bind_settings"); }
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
SocialPanel::SocialPanel() : avatars_(std::make_unique<Avatars>()), direct_code_(NewJoinCode()) {}
SocialPanel::~SocialPanel() = default;
#if defined(KK_DEV_TOOLS)
void SocialPanel::ResetPreview() {
  notice_.clear();
  notice_until_ = 0;
  seen_lobby_ = 0;
  seen_members_ = 0;
  editor_=0;
  ip_path_=false; direct_mode_=0; invited_friend_=0;
}
#endif
rex::ui::ImmediateTexture* SocialPanel::Avatar(const SocialFriend& f, rex::ui::ImmediateDrawer* drawer) {
  return avatars_->Get(f, drawer);
}
void SocialPanel::Tick(const SocialModel& model) {
  if (model.lobby!=seen_lobby_ || !model.lobby) invited_friend_=0;
  if (model.lobby && model.lobby == seen_lobby_ && seen_members_ == 2 && model.members.size() < 2) {
    notice_ = "Your partner left";
    notice_until_ = ImGui::GetTime() + 4.0;
  }
  seen_lobby_ = model.lobby;
  seen_members_ = model.members.size();
}
void SocialPanel::PageActivate(const SocialRow& row, ISocialService& service, bool ip_path, int direct_mode) {
  ip_path_=ip_path; direct_mode_=direct_mode;
  Activate(row,service);
}
void SocialPanel::PrepareDirect(int mode) {
  if (mode==2 && direct_mode_!=2) direct_code_.clear();
  if (mode==1 && !ValidJoinCode(direct_code_)) direct_code_=NewJoinCode();
  direct_mode_=mode; ip_path_=true;
}
std::string SocialPanel::notice() const { return notice_until_>ImGui::GetTime() ? notice_ : std::string(); }
void SocialPanel::Activate(const SocialRow& row, ISocialService& service) {
  const auto rows=SocialRows(service.Snapshot(),direct_mode_,ip_path_);
  const auto current=std::find_if(rows.begin(),rows.end(),[&](const auto& r) { return r.key==row.key; });
  if (current==rows.end() || !current->enabled) { Cue(Sound::kDeny); return; }
  const auto kind=row.key.kind;
  if (kind==SocialRowKind::DirectChooseHost || kind==SocialRowKind::DirectChooseJoin) {
    PrepareDirect(kind==SocialRowKind::DirectChooseHost ? 1 : 2);
    return;
  }
  if (kind==SocialRowKind::DirectAddress || kind==SocialRowKind::DirectPort || kind==SocialRowKind::DirectCode) {
    editor_=kind==SocialRowKind::DirectAddress ? 1 : kind==SocialRowKind::DirectPort ? 2 : 3;
    editor_text_=editor_==1 ? direct_address_ : editor_==2 ? direct_port_ : direct_code_;
    editor_key_=0; editor_just_opened_=true; editor_sync_=false; editor_lowercase_=editor_symbols_=false; return;
  }
  if (kind==SocialRowKind::DirectHost || kind==SocialRowKind::DirectJoin) {
    unsigned port=0; const auto parsed=std::from_chars(direct_port_.data(),direct_port_.data()+direct_port_.size(),port);
    bool ok=parsed.ec==std::errc{} && parsed.ptr==direct_port_.data()+direct_port_.size() && port && port<=65535 && ValidJoinCode(direct_code_);
    if (ok) ok=kind==SocialRowKind::DirectHost ? HostDirect(uint16_t(port),direct_code_) : JoinDirect(direct_address_,uint16_t(port),direct_code_);
    notice_=ok ? "" : "Enter an IP, port 1 to 65535 and a 6 character code";
    notice_until_=ImGui::GetTime()+6; return;
  }
  const auto command = SocialRowCommand(row, service.Snapshot());
  if (!command) { Cue(Sound::kDeny); return; }
  if (command->action == SocialAction::Start) notice_ = RequestSessionStart() ? "Starting co-op..." : "Choose Continue or a chapter before starting co-op";
  else if (!service.Act(*command)) notice_ = "Could not complete Social action";
  else if (command->action == SocialAction::Invite) { invited_friend_=command->id; notice_ = "Inviting..."; }
  else notice_.clear();
  notice_until_ = ImGui::GetTime() + 4.0;
}
void SocialPanel::DrawEditor(ImDrawList* dl,ImVec2 o,ImVec2 size,const Metrics& m,Input& in,Glyphs& glyphs,const std::string& set,ISocialService& service) {
  const auto f=GetFonts(); const float s=m.s;
  dl->AddRectFilled(o,ImVec2(o.x+size.x,o.y+size.y),IM_COL32(14,16,20,255));
  const char* title=editor_==1 ? "IP address" : editor_==2 ? "UDP port" : "Join code";
  dl->AddText(f.display,26*s,ImVec2(o.x+24*s,o.y+30*s),color::kText,T(title));
  const size_t limit=editor_==1 ? 64 : editor_==2 ? 5 : 6;
  // A real InputText requests OS text input through the SDK's IME callback.
  // Reading InputQueueCharacters alone never starts SDL text input.
  char text[65]{};
  std::copy_n(editor_text_.data(),std::min(editor_text_.size(),limit),text);
  ImGui::SetCursorScreenPos(ImVec2(o.x+24*s,o.y+84*s));
  ImGui::SetNextItemWidth(size.x-48*s);
  // Restore text focus after a click on our drawn on-screen keys, too.
  ImGui::SetKeyboardFocusHere();
  const auto callback=[](ImGuiInputTextCallbackData* data) -> int {
    auto& self=*static_cast<SocialPanel*>(data->UserData);
    if (data->EventFlag==ImGuiInputTextFlags_CallbackCharFilter) {
      auto& c=data->EventChar;
      if (self.editor_==3 && c>='a' && c<='z') c-='a'-'A';
      return c<33 || c>126 || (self.editor_==2 && (c<'0' || c>'9')) ||
          (self.editor_==3 && !(c>='0' && c<='9') && !(c>='A' && c<='Z'));
    }
    if (self.editor_sync_) {
      data->DeleteChars(0,data->BufTextLen); data->InsertChars(0,self.editor_text_.c_str()); self.editor_sync_=false;
    }
    return 0;
  };
  ImGui::PushFont(f.text,18*s);
  bool done=ImGui::InputText("##social-entry",text,limit+1,ImGuiInputTextFlags_CallbackAlways |
      ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll,callback,this);
  ImGui::PopFont();
  editor_text_=text;
  if (editor_just_opened_) { editor_just_opened_=false; in.ConsumeAll(); RequestUiPaint(); return; }
  auto append=[&](char c) {
    if (editor_==3 && c>='a' && c<='z') c-='a'-'A';
    if (editor_text_.size()<limit && c>=33 && c<=126 && (editor_!=2 || (c>='0' && c<='9')) &&
        (editor_!=3 || (c>='0' && c<='9') || (c>='A' && c<='Z'))) { editor_text_+=c; editor_sync_=true; }
  };
  const std::string_view keys=editor_symbols_ ? "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~" :
      editor_lowercase_ ? "1234567890qwertyuiopasdfghjkl.zxcvbnm:%-" : "1234567890QWERTYUIOPASDFGHJKL.ZXCVBNM:%-";
  constexpr int count=45, columns=10; // final focus target is the large confirmation button
  if (in.Pressed(Action::kLeft)) editor_key_=(editor_key_+count-1)%count;
  if (in.Pressed(Action::kRight)) editor_key_=(editor_key_+1)%count;
  if (in.Pressed(Action::kUp)) editor_key_=std::max(0,editor_key_-columns);
  if (in.Pressed(Action::kDown)) editor_key_=std::min(count-1,editor_key_+columns);
  bool erase=in.Pressed(Action::kDefault);
  done=done || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
  bool cancel=ImGui::IsKeyPressed(ImGuiKey_Escape) || in.Pressed(Action::kBack);
  const float cell=(size.x-48*s)/columns;
  for (int i=0;i<44;++i) {
    const ImVec2 a(o.x+24*s+(i%columns)*cell,o.y+150*s+(i/columns)*46*s), b(a.x+cell-3*s,a.y+42*s);
    const bool hover=ImGui::IsMouseHoveringRect(a,b);
    if (hover && ImGui::IsMouseClicked(0)) editor_key_=i;
    dl->AddRectFilled(a,b,editor_key_==i ? color::kFocus : IM_COL32(34,38,42,255));
    const std::string label=i<40 ? (size_t(i)<keys.size() ? std::string(1,keys[size_t(i)]) : "") :
        i==40 ? "a/A" : i==41 ? "#" : i==42 ? "<" : "OK";
    dl->AddText(f.text,18*s,ImVec2(a.x+9*s,a.y+10*s),editor_key_==i ? color::kOnFocus : color::kText,label.c_str());
    if ((in.Pressed(Action::kAccept) && editor_key_==i) || (hover && ImGui::IsMouseClicked(0))) {
      if (i<40) { if (size_t(i)<keys.size()) append(keys[size_t(i)]); }
      else if (i==40) editor_lowercase_=!editor_lowercase_;
      else if (i==41) editor_symbols_=!editor_symbols_;
      else if (i==42) erase=true;
      else done=true;
    }
  }
  dl->AddText(f.text,14*s,ImVec2(o.x+24*s,o.y+410*s),color::kTextDim,T("Type on your keyboard, or select keys with the controller."),nullptr,size.x-48*s);
  // A full-width, named confirmation button is always visible. Editing a
  // guest code confirms and joins; host code editing simply keeps the code.
  const char* confirm=editor_==3 && direct_mode_==2 ? "Join" : "Confirm";
  const ImVec2 confirm_a(o.x+24*s,o.y+480*s),confirm_b(o.x+size.x-24*s,o.y+526*s);
  dl->AddRectFilled(confirm_a,confirm_b,editor_key_==44 ? color::kFocus : IM_COL32(54,62,68,255));
  const std::string confirm_label=std::string(T(confirm))+"  [Enter]";
  dl->AddText(f.semibold,18*s,ImVec2(confirm_a.x+14*s,confirm_a.y+12*s),editor_key_==44 ? color::kOnFocus : color::kText,confirm_label.c_str());
  done=done || (editor_key_==44 && in.Pressed(Action::kAccept)) ||
      (ImGui::IsMouseHoveringRect(confirm_a,confirm_b) && ImGui::IsMouseClicked(0));
  // Start confirms from any on-screen key; A still enters the selected key.
  done=done || in.Pressed(Action::kPlay);
  const auto confirm_clicked=in.device()==Device::kKeyboard ? Action::kCount :
      DrawPrompts(dl,glyphs,set,{{Action::kPlay,confirm}},o.x+size.x-24*s,o.y+560*s,m,true);
  done=done || confirm_clicked==Action::kPlay;
  const auto clicked=DrawPrompts(dl,glyphs,set,{{Action::kDefault,"Delete"},{Action::kBack,"Cancel"}},
      o.x+size.x-24*s,o.y+size.y-31*s,m,true);
  erase=erase || clicked==Action::kDefault; cancel=cancel || clicked==Action::kBack;
  if (erase && !editor_text_.empty()) { editor_text_.pop_back(); editor_sync_=true; }
  const bool join=done && editor_==3 && direct_mode_==2;
  if (done && !cancel) {
    (editor_==1 ? direct_address_ : editor_==2 ? direct_port_ : direct_code_)=editor_text_;
  }
  if (done || cancel) { editor_=0; Cue(cancel ? Sound::kBack : Sound::kSelect); }
  if (join && !cancel) Activate({{SocialRowKind::DirectJoin,0},"Join",{},true},service);
  in.ConsumeAll(); RequestUiPaint();
}
#if defined(KK_DEV_TOOLS)
bool SocialPanel::DirectInput(std::string_view command,ISocialService& service) {
  if (command=="steam-connect" || command=="steam-host") {
    ip_path_=false;
    const auto kind=command=="steam-connect" ? SocialRowKind::ConnectSteam : SocialRowKind::Create;
    const auto rows=SocialRows(service.Snapshot(),direct_mode_,ip_path_);
    auto row=std::find_if(rows.begin(),rows.end(),[&](const auto& r) { return r.key.kind==kind && r.enabled; });
    if (row==rows.end()) return false;
    Activate(*row,service); return true;
  }
  if (command=="host" || command=="join" || command.starts_with("edit-")) { ip_path_=true; direct_mode_=command=="host" ? 1 : 2; }
  if (command.starts_with("address:")) { direct_address_=command.substr(8,64); return true; }
  if (command.starts_with("port:")) { direct_port_=command.substr(5,5); return true; }
  if (command.starts_with("code:")) { direct_code_=NormalizeJoinCode(command.substr(5,6)); return true; }
  const auto kind=command=="host" ? SocialRowKind::DirectHost : command=="join" ? SocialRowKind::DirectJoin :
      command=="ready" ? SocialRowKind::Ready : command=="edit-address" ? SocialRowKind::DirectAddress :
      command=="edit-port" ? SocialRowKind::DirectPort : command=="edit-code" ? SocialRowKind::DirectCode :
      command=="leave" ? SocialRowKind::Leave : SocialRowKind::Start;
  const auto rows=SocialRows(service.Snapshot(),direct_mode_,ip_path_);
  const auto row=std::find_if(rows.begin(),rows.end(),[&](const auto& r) { return r.key.kind==kind; });
  if (row==rows.end() || !row->enabled) return false;
  Activate(*row,service); return true;
}
#endif
}  // namespace kk::ui
