// The co-op pages' Direct IP text editor and row actions (SocialPanel, what is left of the Social drawer after UI1
// removed it), with real SDL polling of an SDL virtual controller, real ImGui key and text events, no game/window.
#include "social_panel.h"
#include "coop_runtime.h"
#include "art.h"
#include "platform.h"
#include "launcher_text.h"
#include <rex/cvar.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>

using namespace kk;
using namespace kk::ui;
using namespace kk::coop;
namespace {
void Check(bool value,const char* what) { if (!value) throw std::runtime_error(what); }
struct Service : ISocialService {
  SocialModel model;
  SocialCommand last;
  int calls=0;
  Service() { model.status=SocialStatus::Dormant; }
  SocialModel Snapshot() const override { return model; }
  bool Act(SocialCommand c) override { ++calls; last=c; return true; }
} service;
int joined=0, hosted=0, selected=-1, starts=0;
std::string joined_code, joined_address;
uint16_t joined_port=0;
// No dash, en dash or em dash, and no hyphenated word, apart from the game term co-op (and Koop, coop...).
bool NoDash(std::string text) {
  for (const char* allowed : {"co-op", "Co-op", "CO-OP", "Co-Op"})
    for (size_t at; (at=text.find(allowed))!=std::string::npos;) text.replace(at,std::string_view(allowed).size(),"coop");
  return text.find('-')==std::string::npos && text.find("\xE2\x80\x93")==std::string::npos &&
      text.find("\xE2\x80\x94")==std::string::npos;
}
}
namespace kk {
const UiFonts& GetUiFonts() { static UiFonts fonts{}; return fonts; }
void KeepTextureAlive(std::unique_ptr<rex::ui::ImmediateTexture>) {}
std::string PadPromptsFor(int,const char*) { return "xbox360"; }
void RequestUiPaint() {}
namespace art { Image LoadImage(const std::filesystem::path&) { return {}; } }
namespace play {
const std::vector<ChapterInfo>& Chapters() {
  static const auto chapters=[] { std::vector<ChapterInfo> out(42);
    for (int i=0;i<42;++i) { out[size_t(i)].entry=i; out[size_t(i)].name="Chapter "+std::to_string(i+1); }
    return out; }(); return chapters;
}
}
namespace coop {
bool HostDirect(uint16_t,std::string) { ++hosted; return true; }
bool JoinDirect(std::string address,uint16_t port,std::string code) { ++joined; joined_code=code; joined_address=address; joined_port=port; return true; }
bool SelectSessionStart(const play::StartRequest& r) { selected=r.chapter; return true; }
bool RequestSessionStart() { ++starts; return true; }
}
}
int main() {
  try {
    bool sdk_settings=false;
    rex::ui::RegisterBind("bind_settings","F4","Test SDK settings",[&] { sdk_settings=true; });
    ReserveSocialHotkey(false);
    rex::ui::KeyEvent before(nullptr,rex::ui::VirtualKey::kF4,1,false,false,false,false,false);
    Check(rex::ui::ProcessKeyEvent(before) && sdk_settings,"co-op off retains the SDK F4 binding");
    sdk_settings=false; ReserveSocialHotkey(true);
    rex::ui::KeyEvent after(nullptr,rex::ui::VirtualKey::kF4,1,false,false,false,false,false);
    Check(!rex::ui::ProcessKeyEvent(after) && !sdk_settings,"co-op F4 cannot open a hidden SDK settings overlay");
    ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.DisplaySize={1280,720}; io.DeltaTime=.05f; io.IniFilename=nullptr;
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    Check(SDL_InitSubSystem(SDL_INIT_GAMEPAD),"SDL init");
    SDL_VirtualJoystickDesc desc{}; SDL_INIT_INTERFACE(&desc); desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1; desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.name="C61 test controller";
    const auto id=SDL_AttachVirtualJoystick(&desc); Check(id!=0,"SDL virtual attach");
    SDL_Joystick* joystick=SDL_OpenJoystick(id); Check(joystick!=nullptr,"SDL virtual open");
    // Virtual trigger axes start released, at their minimum.
    SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768);
    SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);
    {
      Input input; Glyphs glyphs(nullptr,{}); SocialPanel panel; Metrics metrics;
      // The page's frame: the editor over it while it is open (launcher_coop_pages.inc DrawCoopPage).
      auto frame=[&] {
        ImGui::NewFrame(); ImGui::Begin("Co-op editor test");
        input.set_keyboard_enabled(!panel.editing()); input.Update();
        panel.Tick(service.model);
        if (panel.editing())
          panel.DrawPageEditor(ImGui::GetWindowDrawList(),{395,0},{490,720},metrics,input,glyphs,"xbox360",service);
        ImGui::End(); ImGui::Render();
      };
      auto button=[&](SDL_GamepadButton key) {
        SDL_SetJoystickVirtualButton(joystick,key,true); frame();
        SDL_SetJoystickVirtualButton(joystick,key,false); frame();
      };
      // A page row pressed (A on its field or button): Join a game is direct mode 2, Invite (host by IP) mode 1.
      auto press=[&](SocialRowKind kind,int mode) {
        panel.PageActivate({{kind,0},"",{},true},service,true,mode); frame();
      };
      frame(); Check(input.gamepad_count()>0,"real SDL enumerate/open path");
      // The old RT drawer is gone: RT is page down again, never a Social press.
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767); frame();
      Check(input.Pressed(Action::kPageDown),"RT pages down in the launcher again");
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768); frame(); frame();
      Check(service.calls==0,"nothing asks Steam to start");
      panel.PrepareDirect(2);
      press(SocialRowKind::DirectAddress,2); Check(panel.editing(),"A opens the IP keyboard");
      button(SDL_GAMEPAD_BUTTON_EAST); Check(!panel.editing(),"B cancels the editor");
      press(SocialRowKind::DirectPort,2); Check(panel.editing(),"A opens the port keyboard");
      button(SDL_GAMEPAD_BUTTON_EAST);
      press(SocialRowKind::DirectCode,2); Check(panel.editing(),"A opens the code keyboard");
      Check(panel.editor_text().empty(),"guest starts with an empty code, not a generated host code");
      button(SDL_GAMEPAD_BUTTON_SOUTH);
      for (int i=0;i<6;++i) button(SDL_GAMEPAD_BUTTON_WEST);
      Check(panel.editor_text().empty(),"X erases on-screen characters");
      button(SDL_GAMEPAD_BUTTON_DPAD_DOWN); button(SDL_GAMEPAD_BUTTON_DPAD_DOWN); // index 20 = A in QWERTY
      for (int i=0;i<7;++i) button(SDL_GAMEPAD_BUTTON_SOUTH);
      Check(panel.editor_text()=="AAAAAA","pad keyboard enters exactly six characters");
      // index 43 is OK. All keyboard controls are reachable by d-pad.
      for (int i=0;i<2;++i) button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
      for (int i=0;i<3;++i) button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
      button(SDL_GAMEPAD_BUTTON_SOUTH); Check(!panel.editing(),"pad keeps text");
      Check(joined==1 && joined_code=="AAAAAA","pad confirmation joins with entered code");
      press(SocialRowKind::DirectJoin,2);
      Check(joined==2,"the Join game button works");
      // Real ImGui key/text events, no direct editor-text writes. The widget
      // must request OS text input, replace old contents and consume Enter/Esc.
      auto key=[&](ImGuiKey k) { io.AddKeyEvent(k,true); frame(); io.AddKeyEvent(k,false); frame(); };
      for (auto field : {SocialRowKind::DirectAddress,SocialRowKind::DirectPort,SocialRowKind::DirectCode}) {
        press(field,2); frame(); frame();
        Check(io.WantTextInput,"physical text widget requests OS text input");
        io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); frame();
        io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); frame();
        const char* value=field==SocialRowKind::DirectAddress ? "127.0.0.1" : field==SocialRowKind::DirectPort ? "27080" : "ab2cd3";
        io.AddInputCharactersUTF8(value); frame();
        Check(panel.editor_text()==(field==SocialRowKind::DirectCode ? "AB2CD3" : value),"physical typing in IP, port and code");
        key(ImGuiKey_Enter); Check(!panel.editing(),"Enter confirms entry");
      }
      Check(joined==3 && joined_code=="AB2CD3","physical Enter on code confirms Join");
      Check(joined_address=="127.0.0.1" && joined_port==27080,"Enter commits exact physical IP and port text");
      Check(panel.direct_address()=="127.0.0.1" && panel.direct_port()=="27080" && panel.direct_code()=="AB2CD3",
            "the page shows what was typed");
      press(SocialRowKind::DirectCode,2); frame(); key(ImGuiKey_Escape);
      Check(!panel.editing(),"physical Escape cancels just the editor");
      frame(); Check(!io.WantTextInput,"closing editor releases OS text input");
      panel.PrepareDirect(1);
      Check(panel.direct_code().size()==6,"hosting by IP keeps (or makes) a 6 character code");
      press(SocialRowKind::DirectCode,1); frame();
      button(SDL_GAMEPAD_BUTTON_START);
      Check(!panel.editing() && joined==3 && hosted==0,"host code confirmation keeps text without joining");
      press(SocialRowKind::DirectHost,1);
      Check(hosted==1,"Host by IP");
      Check(service.calls==0,"Direct IP editors, Join and Host never request Steam");
      // Lobby rows: Ready carries the shown choice, Start, Leave (all checked against the latest snapshot).
      service.model.status=SocialStatus::Lobby; service.model.self=service.model.owner=1; service.model.lobby=55;
      service.model.members={{1,"Host",false},{2,"Guest",false}}; service.model.choice={1,0,false}; frame();
      panel.PageActivate({{SocialRowKind::Ready,0},"Ready",{},true},service,false,0);
      Check(service.last.action==SocialAction::SetReady && service.last.ready && service.last.choice==service.model.choice,
            "Ready includes the shown choice");
      const int before_start=starts;
      panel.PageActivate({{SocialRowKind::Start,0},"Start",{},true},service,false,0);
      Check(starts==before_start,"Start is refused until both are ready");
      service.model.can_start=true; service.model.members[0].ready=service.model.members[1].ready=true;
      panel.PageActivate({{SocialRowKind::Start,0},"Start",{},true},service,false,0);
      Check(starts==before_start+1,"Start once both are ready");
      panel.PageActivate({{SocialRowKind::Leave,0},"Leave lobby",{},true},service,false,0);
      Check(service.last.action==SocialAction::Leave,"Leave lobby");
      // The partner left: a notice for the page's status line.
      service.model.members.resize(1); frame();
      Check(panel.notice()=="Your partner left","the page says the partner left");
      service.model.members[0].ready=false; service.model.can_start=false;
      service.model.friends={{2,"Friend",FriendState::Online}}; service.model.overlay_available=true; frame();
      panel.PageActivate({{SocialRowKind::Friend,2},"Friend",{},true},service,false,0);
      Check(service.last.action==SocialAction::Invite && service.last.id==2 && panel.invited()==2,"Invite a Steam friend");
      panel.PageActivate({{SocialRowKind::OverlayInvite,0},"",{},true},service,false,0);
      Check(service.last.action==SocialAction::OverlayInvite,"Invite with Steam opens Steam's invite window");
      service.model={}; service.calls=0; panel.ResetPreview(); frame();
      Check(service.calls==0,"showing the pages leaves Steam alone");
      panel.PageActivate({{SocialRowKind::ConnectSteam,0},"",{},true},service,false,0);
      Check(service.calls==1 && service.last.action==SocialAction::Open,"only explicit Connect to Steam requests initialization");
      service.model.status=SocialStatus::Idle; frame();
      panel.PageActivate({{SocialRowKind::Create,0},"",{},true},service,false,0);
      Check(service.calls==2 && service.last.action==SocialAction::CreateLobby,"Host with Steam friends creates the lobby");
      service.model.status=SocialStatus::Creating; frame();
      panel.PageActivate({{SocialRowKind::Create,0},"",{},true},service,false,0);
      Check(service.calls==2,"pending lobby cannot create a duplicate");
      // UI1: every new player text of the co-op pages and the early access warning, in all five languages, without
      // dashes or hyphenated words (the game term co-op is allowed).
      static const char* const kNew[]={
          "Play the story with a friend","Two players, online. One of you hosts the game, the other joins it.",
          "You lead","Friend leads","Pick a chapter or one of your saves, then invite a friend to play it with you.",
          "Your game runs the world","Invite a Steam friend, or share a 6 character code",
          "Play in a friend's game. Accept their invite, join them from your friends list, or type their code.",
          "Invites and friends playing now","Join with a code, no Steam needed","{0} invited you to play",
          "A friend invited you to play","Steam invite","Rejoin your last game","Rejoin","{0} with {1}","How co-op works",
          "The host picks what to play","A chapter, or one of the host's saves.","Invite a friend",
          "Through Steam, or by sharing a 6 character code.","Both press Ready, then start",
          "Both games load the chapter and you play it together.","Good to know","Kong chapters:",
          "each player plays their own Kong part, then you meet up again.","Progress:",
          "chapters you finish together unlock on both saves.","Talking:",
          "there is no voice chat in the game. Use Steam or Discord.","Add to Steam","Connect to Steam",
          "Choose what to play","Both ready, then start","Choose any chapter to play together.","Play together",
          "Apart, then regroup","Watch",
          "You both play the whole chapter in the same world. If one of you goes down, the other can keep going and you come back next to them.",
          "Each of you plays this part on your own, then you meet up again.",
          "The host plays this chapter and the partner watches it.","Change chapter","Choose a chapter",
          "Invite a Steam friend","Opens a lobby only your Steam friends can join. Then invite a friend.",
          "Host with Steam friends","Host with a code",
          "Your friend joins with your IP address and this code. Forward the UDP port on your router.",
          "Code (6 characters)","Port","Host by IP","Join a friend's game",
          "Pick the way that suits you. All three put you in the same lobby as your friend.","Invites",
          "When a friend invites you on Steam, it shows up here.","Steam invites and friends need Steam.",
          "{0} invited you","A friend invited you","Accept","Decline","{0} more invites","No invites right now.",
          "Invites from your Steam friends appear here while the game is open.","Friends playing now",
          "Steam friends who are in King Kong right now.","Connect to Steam to see friends who are playing now.",
          "Playing King Kong","In the game, not hosting","Not hosting","No friends are playing now",
          "Friend not listed? They need Steam open and the same version of King Kong. You can also join with their code.",
          "Join with a code","For games hosted by IP. The host sees their address and code on their lobby screen.",
          "Host's IP address","Leave it as it is unless the host changed it.","Join game",
          "Both players need the same version of King Kong. If joining fails, the screen tells you why and what to try.",
          "Not chosen yet","Choose a chapter or one of your saves.","The host chooses a chapter or a save.",
          "Invite with Steam","Leave lobby","Share these details","IP address","Join code","Players","{0} of {1}",
          "Waiting for your partner to join","Waiting for a friend to join",
          "They join with your IP address, port and code.","Invite a Steam friend to this lobby.","Partner",
          "Not ready yet","Choose a chapter or save first","The host has not chosen what to play yet",
          "Press again if you need more time","Press when you are set to play","You are ready","Start game",
          "Both players are ready","Press Ready first","Waiting for {0} to press Ready","Waiting for both games to be ready",
          "The host starts the game","the host","the chapter",
          "When the host starts, both games load {0} and connect again. It takes a few seconds. If the host changes the chapter, both players press Ready again.",
          "Direct IP lobby","Steam lobby, friends only","Direct IP","Steam is unavailable","Steam not connected",
          "Steam connected","Early access","Alpha","Online co-op is in early access",
          "Online co-op is at an alpha stage. You will run into random issues, and you may not be able to progress the game.",
          "For a smooth experience, play the single player game. Play online co-op to try it early and to help us by reporting bugs.",
          "Known issues","Desync between players","Random teleports out of the map","Random guest reloads",
          "Mission state can get stuck","NPCs not in sync","Some chapters fail to load",
          "Not every chapter has a player model for the guest","Report a bug",
          "Tell us on the Discord and send your logs folder.","Join the Discord","Open logs folder","Back to main menu",
          "Continue ({0})","Continue","1 invite","{0} invites","Switch","Accept invite","Cancel",
          "Enter an IP, port 1 to 65535 and a 6 character code",
          "Chapter selected. Open Play Online Co-op and press Start when both players are Ready.",
          "Choose Continue or a chapter before starting co-op","Your partner left","Inviting...","Starting co-op...",
          "Play Online Co-op","Host a game","Join a game","New game","Load game","Lobby","Ready","Host","You","Back",
          "Select","Invite","Join","Version","Chapter {0}","Not entered yet","No saves yet.","No friends to show",
          "Joining the lobby","Creating lobby","Connecting to Steam","Local dev link selected",
          "Steam is unavailable. You can connect by IP.","Play this save together. Your partner plays it with you.",
          "Continue one of your saves together. Your partner plays it with you.",
          "Chapter changed. Both players must be ready again.","Could not select chapter","Could not join lobby",
          "Waiting for {0} to start","Connecting to the host","your partner",
          // the status lines the pages show (SocialStatusKey, CoopLobbyStatus) and the text editor
          "Both players must be ready","Both ready. Select Start","Checking invited lobby","Choose Steam or Direct IP",
          "Connected. Both players must choose Ready","Connected. Choose a chapter","Connected. Waiting for the host to start",
          "Could not complete Social action","Could not create lobby","Could not enable Steam invites","Could not send invite",
          "Gameplay settings changed; open a new lobby","Lobby compatibility changed","Lobby open. Invite a friend.",
          "Lobby unavailable or incompatible","Partner connection lost","Starting co-op is not ready yet",
          "Steam is needed for co-op","Steam lobby request timed out","Steam not available in this build",
          "Steam overlay is unavailable","Steam requests are still pending; restart to retry","Versions don't match: {0}",
          "Choose Ready when you are set","Choose a chapter or save","Waiting for {0} to choose Ready",
          "Waiting for {0} to choose a chapter","Waiting for {0} to join","{0} is ready","Chapters {0} to {1}","UDP port",
          "Type on your keyboard, or select keys with the controller.","Confirm","Delete","Saves"};
      for (int language : {1,3,4,5,6}) {
        rex::cvar::SetFlagByName("user_language",std::to_string(language)); frame();
        for (const char* label : kNew) {
          const std::string shown=text::T(std::string(label));
          if (language!=1 && shown==label && std::string_view(label)!="Port" && std::string_view(label)!="Host" &&
              std::string_view(label)!="Lobby" && std::string_view(label)!="Version" && std::string_view(label)!="{0} with {1}" &&
              std::string_view(label)!="Alpha" && std::string_view(label)!="Partner" && std::string_view(label)!="Direct IP" && std::string_view(label)!="Steam invite")
            throw std::runtime_error("no translation (language "+std::to_string(language)+"): "+label);
          if (!NoDash(shown)) throw std::runtime_error("a dash in (language "+std::to_string(language)+"): "+shown);
        }
      }
      rex::cvar::SetFlagByName("user_language","1");
    }
    SDL_CloseJoystick(joystick); SDL_DetachVirtualJoystick(id); SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    ImGui::DestroyContext();
    std::cout<<"Co-op pages SDL pad: editor (keys, pad, typing), join, host, Ready, Start, Leave, invites and the new texts passed\n";
  } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
