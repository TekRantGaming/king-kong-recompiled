// Real Social drawing and SDL polling, with an SDL virtual controller and no game/window.
#include "social_panel.h"
#include "coop_runtime.h"
#include "art.h"
#include "platform.h"
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
      auto frame=[&] {
        ImGui::NewFrame(); ImGui::Begin("Social test"); input.set_social_triggers(true);
        input.set_keyboard_enabled(!panel.editing()); input.Update();
        panel.Update(true,SocialPage::Page,input,service);
        input.set_social_blocking(panel.blocking());
        panel.Draw(ImGui::GetWindowDrawList(),{0,0},{1280,720},metrics,input,glyphs,"xbox360",nullptr,service,true);
        ImGui::End(); ImGui::Render();
      };
      auto button=[&](SDL_GamepadButton key) {
        SDL_SetJoystickVirtualButton(joystick,key,true); frame();
        SDL_SetJoystickVirtualButton(joystick,key,false); frame();
      };
      auto focus=[&](SocialRowKind kind,int bound=40) {
        // Lists stop at their ends. Start from the top when revisiting a field.
        for (int i=0;i<bound;++i) button(SDL_GAMEPAD_BUTTON_DPAD_UP);
        for (int i=0;i<bound && (!panel.focused_row() || panel.focused_row()->kind!=kind);++i) button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        if (!panel.focused_row() || panel.focused_row()->kind!=kind)
          throw std::runtime_error("pad reaches requested row "+std::to_string(int(kind))+"; actual="+
              (panel.focused_row() ? std::to_string(int(panel.focused_row()->kind)) : "none"));
      };
      frame(); Check(input.gamepad_count()>0,"real SDL enumerate/open path");
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767); frame();
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768); frame(); frame();
      Check(panel.blocking(),"real SDL RT opens Social");
      Check(service.calls==0,"RT opens a dormant drawer without requesting Steam or installing its input hooks");
      Check(panel.focused_row() && panel.focused_row()->kind==SocialRowKind::ConnectSteam,"Steam connection is an explicit row");
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTY,32767); frame();
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTY,0); frame();
      Check(panel.focused_row() && panel.focused_row()->kind==SocialRowKind::AddSteam,"left stick moves Social focus through the ordinary launcher input");
      focus(SocialRowKind::DirectChooseJoin); button(SDL_GAMEPAD_BUTTON_SOUTH);
      focus(SocialRowKind::DirectAddress); button(SDL_GAMEPAD_BUTTON_SOUTH); Check(panel.editing(),"A opens IP keyboard");
      button(SDL_GAMEPAD_BUTTON_EAST); Check(!panel.editing() && panel.blocking(),"B cancels editor without closing Social");
      focus(SocialRowKind::DirectPort); button(SDL_GAMEPAD_BUTTON_SOUTH); Check(panel.editing(),"A opens port keyboard");
      button(SDL_GAMEPAD_BUTTON_EAST);
      focus(SocialRowKind::DirectCode); button(SDL_GAMEPAD_BUTTON_SOUTH); Check(panel.editing(),"A opens code keyboard");
      Check(panel.editor_text().empty(),"guest starts with an empty code, not a generated host code");
      button(SDL_GAMEPAD_BUTTON_SOUTH);
      for (int i=0;i<6;++i) button(SDL_GAMEPAD_BUTTON_WEST);
      Check(panel.editor_text().empty(),"X erases on-screen characters");
      button(SDL_GAMEPAD_BUTTON_DPAD_DOWN); button(SDL_GAMEPAD_BUTTON_DPAD_DOWN); // index 20 = A in QWERTY
      for (int i=0;i<7;++i) button(SDL_GAMEPAD_BUTTON_SOUTH);
      Check(panel.editor_text()=="AAAAAA","pad keyboard enters exactly six characters");
      // index 43 is Keep. All keyboard controls are reachable by d-pad.
      for (int i=0;i<2;++i) button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
      for (int i=0;i<3;++i) button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
      button(SDL_GAMEPAD_BUTTON_SOUTH); Check(!panel.editing(),"pad keeps text");
      Check(joined==1 && joined_code=="AAAAAA","pad confirmation joins with entered code");
      focus(SocialRowKind::DirectJoin); button(SDL_GAMEPAD_BUTTON_SOUTH);
      Check(joined==2,"explicit Join button works");
      // Real ImGui key/text events, no direct editor-text writes. The widget
      // must request OS text input, replace old contents and consume Enter/Esc.
      auto key=[&](ImGuiKey k) { io.AddKeyEvent(k,true); frame(); io.AddKeyEvent(k,false); frame(); };
      for (auto field : {SocialRowKind::DirectAddress,SocialRowKind::DirectPort,SocialRowKind::DirectCode}) {
        focus(field); button(SDL_GAMEPAD_BUTTON_SOUTH); frame();
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
      focus(SocialRowKind::DirectCode); button(SDL_GAMEPAD_BUTTON_SOUTH); frame(); key(ImGuiKey_Escape);
      Check(!panel.editing() && panel.blocking(),"physical Escape cancels just the editor");
      frame(); Check(!io.WantTextInput,"closing editor releases OS text input");
      focus(SocialRowKind::DirectChooseHost); button(SDL_GAMEPAD_BUTTON_SOUTH);
      focus(SocialRowKind::DirectCode); button(SDL_GAMEPAD_BUTTON_SOUTH); frame();
      button(SDL_GAMEPAD_BUTTON_START);
      Check(!panel.editing() && joined==3 && hosted==0,"host code confirmation keeps text without joining");
      focus(SocialRowKind::DirectHost); button(SDL_GAMEPAD_BUTTON_SOUTH);
      Check(hosted==1,"pad Host confirmation is reachable after role selection");
      Check(service.calls==0,"Direct IP editors, Join and Host never request Steam");
      service.model.direct=true; service.model.status=SocialStatus::Lobby;
      service.model.self=service.model.owner=1; service.model.lobby=55;
      service.model.members={{1,"Host",false},{2,"Guest",false}}; frame();
      // These details used to be focusable in DrawList but rejected on the next frame.
      for (int i=0;i<20;++i) button(SDL_GAMEPAD_BUTTON_DPAD_UP);
      Check(panel.focused_row()==SocialRowKey{SocialRowKind::Text,10},"pad reaches first Direct detail and focus survives next frame");
      focus(SocialRowKind::ChapterSelect); button(SDL_GAMEPAD_BUTTON_SOUTH); Check(panel.chapters_open(),"A opens lobby chapter screen");
      button(SDL_GAMEPAD_BUTTON_DPAD_DOWN); button(SDL_GAMEPAD_BUTTON_SOUTH);
      Check(selected==0 && !panel.chapters_open(),"A commits first chapter from inside lobby");
      service.model.choice={1,0,false}; frame();
      focus(SocialRowKind::ChapterSelect); button(SDL_GAMEPAD_BUTTON_SOUTH);
      for (int i=0;i<41;++i) button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
      button(SDL_GAMEPAD_BUTTON_SOUTH); Check(selected==41,"all 42 chapters reachable including last chapter");
      button(SDL_GAMEPAD_BUTTON_SOUTH); Check(panel.chapters_open(),"reopen chapter screen");
      button(SDL_GAMEPAD_BUTTON_EAST); Check(!panel.chapters_open() && panel.blocking(),"B returns from chapters to lobby");
      focus(SocialRowKind::Ready); button(SDL_GAMEPAD_BUTTON_SOUTH);
      Check(service.last.action==SocialAction::SetReady && service.last.choice==service.model.choice,"Ready includes displayed choice revision");
      service.model.can_start=true; service.model.members[0].ready=service.model.members[1].ready=true;
      focus(SocialRowKind::Start); button(SDL_GAMEPAD_BUTTON_SOUTH); Check(starts==1,"Start reachable by pad");
      focus(SocialRowKind::Leave); button(SDL_GAMEPAD_BUTTON_SOUTH); Check(service.last.action==SocialAction::Leave,"Leave reachable by pad");
      button(SDL_GAMEPAD_BUTTON_EAST); frame(); frame(); frame(); Check(!panel.blocking(),"B closes Social after slide");
      service.model.direct=false; service.model.members.resize(1); service.model.friends={{2,"Friend",FriendState::Online}};
      panel.ResetPreview();
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767); frame();
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768); frame();
      focus(SocialRowKind::Friend); button(SDL_GAMEPAD_BUTTON_WEST);
      Check(service.last.action==SocialAction::Invite && service.last.id==2,"X invites focused Steam friend");
      button(SDL_GAMEPAD_BUTTON_SOUTH); Check(service.last.action==SocialAction::Invite,"A also selects friend invite");
      button(SDL_GAMEPAD_BUTTON_EAST); frame(); frame(); frame();
      service.model={}; service.calls=0; panel.ResetPreview();
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767); frame();
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768); frame();
      Check(service.calls==0,"reopening dormant Social still leaves Steam alone");
      focus(SocialRowKind::ConnectSteam); button(SDL_GAMEPAD_BUTTON_SOUTH);
      Check(service.calls==1 && service.last.action==SocialAction::Open,"only explicit Connect to Steam requests initialization");
    }
    SDL_CloseJoystick(joystick); SDL_DetachVirtualJoystick(id); SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    ImGui::DestroyContext(); std::cout<<"Social SDL pad: RT, rows, keyboard, join, chapters, Ready, Start, Leave and B passed\n";
  } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
