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
  list_ = {};
  focus_.reset();
  notice_.clear();
  notice_until_ = 0;
  seen_lobby_ = 0;
  seen_members_ = 0;
  chapter_screen_=false; editor_=0;
  ip_path_=false; direct_mode_=0; invited_friend_=0;
}
#endif
void SocialPanel::Update(bool enabled, SocialPage page, Input& in, ISocialService& service) {
  const bool was = state_.open();
  if (chapter_screen_ && in.Pressed(Action::kBack)) { chapter_screen_=false; in.Consume(Action::kBack); Cue(Sound::kBack); }
  just_opened_ = state_.Update(enabled, page, !editing() && in.Pressed(Action::kSocial), !editing() && in.Pressed(Action::kBack), ImGui::GetIO().DeltaTime);
  // Opening the drawer must have the same input behavior as any launcher page.
  // Steam init installs process-wide overlay/input hooks that survive shutdown;
  // only the explicit Steam row (or accepting a Steam invite) may request it.
  if (just_opened_) Cue(Sound::kSelect);
  if (was && !state_.open()) Cue(Sound::kBack);
  // The closing press cannot reach the page, including a zero-duration closing animation.
  if (was || state_.blocking()) {
    in.Consume(Action::kSocial);
    if (!editing()) in.Consume(Action::kBack);
  }
  if (was && !state_.blocking()) in.ConsumeAll();
}
void SocialPanel::Activate(const SocialRow& row, ISocialService& service) {
  const auto rows=SocialRows(service.Snapshot(),direct_mode_,ip_path_);
  const auto current=std::find_if(rows.begin(),rows.end(),[&](const auto& r) { return r.key==row.key; });
  if (current==rows.end() || !current->enabled) { Cue(Sound::kDeny); return; }
  const auto kind=row.key.kind;
  if (kind==SocialRowKind::PlaySteam || kind==SocialRowKind::PlayIP) {
    ip_path_=kind==SocialRowKind::PlayIP;
    focus_=SocialRowKey{ip_path_ ? SocialRowKind::DirectChooseHost :
        service.Snapshot().status==SocialStatus::Dormant ? SocialRowKind::ConnectSteam : SocialRowKind::Create,0};
    notice_.clear(); notice_until_=0;
    return;
  }
  if (kind==SocialRowKind::DirectChooseHost || kind==SocialRowKind::DirectChooseJoin) {
    if (kind==SocialRowKind::DirectChooseJoin && direct_mode_!=2) direct_code_.clear();
    if (kind==SocialRowKind::DirectChooseHost && !ValidJoinCode(direct_code_)) direct_code_=NewJoinCode();
    direct_mode_=kind==SocialRowKind::DirectChooseHost ? 1 : 2;
    focus_=SocialRowKey{direct_mode_==1 ? SocialRowKind::DirectPort : SocialRowKind::DirectAddress,0};
    return;
  }
  if (kind==SocialRowKind::ChapterSelect) {
    chapter_screen_=true; chapter_list_={};
    const auto choice=service.Snapshot().choice;
    chapter_list_.focus=choice.valid() ? (choice.continue_game ? 0 : choice.chapter+1) : 0;
    return;
  }
  if (kind==SocialRowKind::AddSteam) { if (add_steam_) { state_.Update(true,SocialPage::Page,false,true,0); add_steam_(); } return; }
  if (kind==SocialRowKind::DirectAddress || kind==SocialRowKind::DirectPort || kind==SocialRowKind::DirectCode) {
    editor_=kind==SocialRowKind::DirectAddress ? 1 : kind==SocialRowKind::DirectPort ? 2 : 3;
    editor_text_=editor_==1 ? direct_address_ : editor_==2 ? direct_port_ : direct_code_;
    editor_key_=0; editor_just_opened_=true; editor_sync_=false; editor_lowercase_=editor_symbols_=false; return;
  }
  if (kind==SocialRowKind::DirectHost || kind==SocialRowKind::DirectJoin) {
    unsigned port=0; const auto parsed=std::from_chars(direct_port_.data(),direct_port_.data()+direct_port_.size(),port);
    bool ok=parsed.ec==std::errc{} && parsed.ptr==direct_port_.data()+direct_port_.size() && port && port<=65535 && ValidJoinCode(direct_code_);
    if (ok) ok=kind==SocialRowKind::DirectHost ? HostDirect(uint16_t(port),direct_code_) : JoinDirect(direct_address_,uint16_t(port),direct_code_);
    notice_=ok ? "" : "Enter an IP, port 1 to 65535 and a 6-character code";
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
void SocialPanel::Draw(ImDrawList* dl, ImVec2 o, ImVec2 size, const Metrics& m, Input& in,
                       Glyphs& glyphs, const std::string& set, rex::ui::ImmediateDrawer* drawer,
                       ISocialService& service, bool active, std::function<void()> add_steam, SocialChapters chapters) {
  if (!blocking()) return;
  add_steam_=std::move(add_steam);
  chapters_=std::move(chapters);
  const float s = m.s, slide = state_.slide();
  const float width = std::min(size.x, 490 * s), right = o.x + size.x;
  const float x = right - width * slide;
  const ImVec2 end(right, o.y + size.y);
  dl->AddRectFilled(o, end, IM_COL32(0, 0, 0, int(220 * slide)));
  dl->PushClipRect(ImVec2(std::max(o.x, x), o.y), end, true);
  dl->AddRectFilled(ImVec2(x, o.y), ImVec2(x + width, end.y), color::kPanel);
  dl->AddLine(ImVec2(x, o.y), ImVec2(x, end.y), color::kAccent, s);
  const auto fonts = GetFonts();
  const auto snapshot=service.Snapshot();
  if (chapter_screen_ && (!snapshot.lobby || snapshot.self!=snapshot.owner || snapshot.start_requested)) chapter_screen_=false;
  if (chapter_screen_) {
    DrawChapters(dl,ImVec2(x,o.y),ImVec2(width,size.y),m,in,glyphs,set,service,active && state_.open());
    dl->PopClipRect(); if (animating()) RequestUiPaint(); return;
  }
  const float left = x + 24 * s, available = width - 48 * s;
  dl->AddText(fonts.display, 30 * s, ImVec2(left, o.y + 22 * s), color::kText,
              Ellipsize(fonts.display, 30 * s, T("Social"), available).c_str());
  const SocialModel model = service.Snapshot();
  const bool ip_path=model.lobby ? model.direct : ip_path_;
  const std::string recommendation=T(ip_path ? "Host: forward the UDP port on your router. Share your public IP and join code with your partner." :
      "Host a Steam game to invite a friend. Guests accept an invite here, or use Steam's Join game while King Kong is running.");
  const float note_height=fonts.text->CalcTextSizeA(14*s,1e6f,available,recommendation.c_str()).y;
  dl->AddText(fonts.text,14*s,ImVec2(left,o.y+88*s),color::kTextDim,recommendation.c_str(),nullptr,available);
  if (model.lobby!=seen_lobby_ || !model.lobby) invited_friend_=0;
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
  if (ip_path && !model.lobby && model.last_error.empty() &&
      (model.status==SocialStatus::Dormant || model.status==SocialStatus::Idle || model.status==SocialStatus::Unavailable))
    status=T("Choose Host or Join by IP.");
  if (model.lobby && model.last_error.empty() && model.status==SocialStatus::Lobby) {
    if (model.members.size()<2) {
      auto invited=std::find_if(model.friends.begin(),model.friends.end(),[&](const auto& f) { return f.id==invited_friend_; });
      status=model.direct ? T("Waiting for your partner to join") : invited!=model.friends.end() ?
          F("Waiting for {0} to join",{invited->name}) : T("Lobby open. Invite a friend.");
    }
    else if (model.self!=model.owner) status=T("Connected. Waiting for the host to start");
    else status=T(model.choice.valid() ? (model.can_start ? "Both ready. Select Start" : "Connected. Both players must choose Ready") : "Connected. Choose a chapter");
  }
  dl->AddText(fonts.text, 14 * s, ImVec2(left, o.y + 60 * s),
              model.status == SocialStatus::Idle && model.last_error.empty() && notice_until_ <= ImGui::GetTime()
                  ? color::kTextDim : color::kWarn,
              Ellipsize(fonts.text, 14 * s, status, available).c_str());
  auto rows = SocialRows(model,direct_mode_,ip_path_);
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
    if (kind==SocialRowKind::PlaySteam || kind==SocialRowKind::PlayIP) {
      item.tall=true;
      item.subtitle=kind==SocialRowKind::PlaySteam ? "Steam invites and friends" : "Direct IP address and join code";
    }
    if (kind==SocialRowKind::Create) {
      item.tall=true; item.subtitle="Creates a friends-only Steam lobby";
    }
    if (kind==SocialRowKind::Text && !row.detail.empty()) {
      item.type=ItemType::kInfo; item.tall=true;
      item.subtitle=row.key.id==13 ? std::string(T(row.detail)) : row.detail; item.literal_subtitle=true;
    }
    if (kind==SocialRowKind::DirectAddress || kind==SocialRowKind::DirectPort || kind==SocialRowKind::DirectCode) {
      item.tall=true; item.subtitle=kind==SocialRowKind::DirectAddress ? direct_address_ : kind==SocialRowKind::DirectPort ? direct_port_ : direct_code_;
      item.literal_subtitle=true;
    }
    if (kind==SocialRowKind::ChapterSelect) {
      item.tall=true; item.literal_subtitle=true;
      item.subtitle=T("Choose Continue or a chapter");
      if (model.choice.valid()) {
        const auto& choice=model.choice;
        const auto& chapter=play::Chapters()[size_t(choice.chapter)];
        const auto chapter_name=chapter.name.empty() ? F("Chapter {0}",{std::to_string(choice.chapter+1)}) : std::string(T(chapter.name));
        item.subtitle=(choice.continue_game ? std::string(T("Continue"))+": " : "") + chapter_name;
      }
    }
    item.literal_label = person;
    item.disabled = !row.enabled && item.type == ItemType::kAction;
    item.focus_subtitle_color = color::kOnFocus;
    item.value_max_width = std::max(1.0f, available * 0.28f);
    if (item.type==ItemType::kAction && kind!=SocialRowKind::Friend &&
        kind!=SocialRowKind::PlaySteam && kind!=SocialRowKind::PlayIP) {
      item.value_text="Select"; item.value_action=Action::kAccept;
    }
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
  const bool interactive = active && state_.open() && !just_opened_ && !editing();
  // Tab stays in the drawer; arrows/stick/d-pad use the launcher's own list model.
  if (interactive && (in.Pressed(Action::kNextTab) || in.Pressed(Action::kPrevTab))) {
    in.Inject(in.Pressed(Action::kPrevTab) ? Action::kUp : Action::kDown, in.device());
  }
  const float top = o.y + 100*s + note_height;
  const auto before_focus=focus_;
  DrawList(dl, items, list_, ImVec2(left, top), ImVec2(x + width - 24 * s, end.y - 106 * s), m, in,
           interactive, &glyphs, set);
  if (list_.focus >= 0 && list_.focus < int(rows.size())) {
    if (focus_==before_focus) focus_ = rows[size_t(list_.focus)].key;
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
  if (editing()) DrawEditor(dl,ImVec2(x,o.y),ImVec2(width,size.y),m,in,glyphs,set,service);
  dl->PopClipRect();
  if (animating()) RequestUiPaint();
}
void SocialPanel::DrawChapters(ImDrawList* dl,ImVec2 o,ImVec2 size,const Metrics& m,Input& in,
                                Glyphs& glyphs,const std::string& set,ISocialService& service,bool active) {
  const auto f=GetFonts(); const float s=m.s, left=o.x+24*s, width=size.x-48*s;
  dl->AddText(f.display,28*s,ImVec2(left,o.y+24*s),color::kText,T("Chapter select"));
  const auto policy_name=[](int entry) {
    const auto policy=kChapterRules[size_t(entry)].policy;
    return policy==ChapterPolicy::Together ? "Together" : policy==ChapterPolicy::SoloRegroup ? "Separate then regroup" : "Watch";
  };
  const auto name=[](int entry) {
    const auto& chapter=play::Chapters()[size_t(entry)];
    return chapter.name.empty() ? F("Chapter {0}",{std::to_string(entry+1)}) : std::string(T(chapter.name));
  };
  auto select=[&](int entry,bool resume) {
    const auto before=service.Snapshot().choice;
    const play::StartRequest request{resume ? play::StartKind::kContinue : play::StartKind::kChapter,
        resume ? chapters_.continue_slot : chapters_.slot,entry,{}};
    if (SelectSessionStart(request)) {
      chapter_screen_=false;
      notice_=before.valid() && before.chapter==entry && before.continue_game==resume ? "" : "Chapter changed. Both players must be ready again.";
    }
    else notice_="Could not select chapter";
    notice_until_=ImGui::GetTime()+5;
  };
  std::vector<Item> items;
  Item resume; resume.type=ItemType::kAction; resume.label="Continue"; resume.tall=true;
  resume.disabled=chapters_.continue_slot<0 || chapters_.continue_chapter<0 || chapters_.continue_chapter>=int(play::Chapters().size());
  resume.subtitle=resume.disabled ? T("No saved game") : name(chapters_.continue_chapter);
  resume.literal_subtitle=true; resume.focus_subtitle_color=color::kOnFocus;
  resume.on_activate=[&,entry=chapters_.continue_chapter] { select(entry,true); };
  items.push_back(std::move(resume));
  for (const auto& chapter : play::Chapters()) {
    Item item; item.type=ItemType::kAction; item.label=name(chapter.entry); item.literal_label=true;
    item.tall=true; item.subtitle=policy_name(chapter.entry); item.focus_subtitle_color=color::kOnFocus;
    item.on_activate=[&,entry=chapter.entry] { select(entry,false); };
    items.push_back(std::move(item));
  }
  const bool interactive=active && !just_opened_;
  DrawList(dl,items,chapter_list_,ImVec2(left,o.y+304*s),ImVec2(left+width,o.y+size.y-102*s),m,in,interactive,&glyphs,set);
  const int entry=chapter_list_.focus==0 ? chapters_.continue_chapter : chapter_list_.focus-1;
  if (entry>=0 && entry<int(play::Chapters().size())) {
    float aspect=16.f/9.f;
    auto* picture=chapters_.picture ? chapters_.picture(entry,&aspect) : nullptr;
    const float height=std::min(154*s,width/std::max(0.1f,aspect));
    const ImVec2 a(left,o.y+76*s),b(left+height*aspect,a.y+height);
    dl->AddRectFilled(a,b,color::kBase);
    if (picture) dl->AddImage(Tex(picture),a,b);
    dl->AddText(f.text,17*s,ImVec2(left,o.y+240*s),color::kText,
        Ellipsize(f.text,17*s,name(entry),width).c_str());
    dl->AddText(f.text,14*s,ImVec2(left,o.y+268*s),color::kAccent,T(policy_name(entry)));
  }
  const auto clicked=DrawPrompts(dl,glyphs,set,{{Action::kAccept,"Select"},{Action::kBack,"Back"}},
      o.x+size.x-24*s,o.y+size.y-31*s,m,interactive);
  if (clicked==Action::kBack) chapter_screen_=false;
  if (clicked==Action::kAccept && chapter_list_.focus>=0 && chapter_list_.focus<int(items.size()) &&
      !items[size_t(chapter_list_.focus)].disabled) items[size_t(chapter_list_.focus)].on_activate();
  in.ConsumeAll();
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
    focus_=row->key; Activate(*row,service); return true;
  }
  if (command=="host" || command=="join" || command.starts_with("edit-")) { ip_path_=true; direct_mode_=command=="host" ? 1 : 2; }
  if (command.starts_with("address:")) { direct_address_=command.substr(8,64); return true; }
  if (command.starts_with("port:")) { direct_port_=command.substr(5,5); return true; }
  if (command.starts_with("code:")) { direct_code_=NormalizeJoinCode(command.substr(5,6)); return true; }
  if (command=="chapters") {
    const auto rows=SocialRows(service.Snapshot());
    for (const auto& row : rows) if (row.key.kind==SocialRowKind::ChapterSelect && row.enabled) { Activate(row,service); return true; }
    return false;
  }
  const auto kind=command=="host" ? SocialRowKind::DirectHost : command=="join" ? SocialRowKind::DirectJoin :
      command=="ready" ? SocialRowKind::Ready : command=="edit-address" ? SocialRowKind::DirectAddress :
      command=="edit-port" ? SocialRowKind::DirectPort : command=="edit-code" ? SocialRowKind::DirectCode :
      command=="leave" ? SocialRowKind::Leave : SocialRowKind::Start;
  const auto rows=SocialRows(service.Snapshot(),direct_mode_,ip_path_);
  const auto row=std::find_if(rows.begin(),rows.end(),[&](const auto& r) { return r.key.kind==kind; });
  if (row==rows.end() || !row->enabled) return false;
  focus_=row->key; Activate(*row,service); return true;
}
#endif
}  // namespace kk::ui
