// Real Social drawing and SDL polling, with an SDL virtual controller and no game/window.
#include "social_panel.h"
#include "coop_runtime.h"
#include "art.h"
#include "platform.h"
#include <rex/ui/immediate_drawer.h>
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
  Service() { model.status=SocialStatus::Unavailable; }
  SocialModel Snapshot() const override { return model; }
  bool Act(SocialCommand c) override { last=c; return true; }
} service;
int joined=0, selected=-1, starts=0;
std::string joined_code;
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
bool HostDirect(uint16_t,std::string) { return true; }
bool JoinDirect(std::string,uint16_t,std::string code) { ++joined; joined_code=code; return true; }
bool SelectSessionStart(const play::StartRequest& r) { selected=r.chapter; return true; }
bool RequestSessionStart() { ++starts; return true; }
}
}
int main() {
  try {
    ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.DisplaySize={1280,720}; io.DeltaTime=.05f; io.IniFilename=nullptr;
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
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
        ImGui::NewFrame(); ImGui::Begin("Social test"); input.set_social_triggers(true); input.Update();
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
        for (int i=0;i<bound && (!panel.focused_row() || panel.focused_row()->kind!=kind);++i) button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        Check(panel.focused_row() && panel.focused_row()->kind==kind,"pad reaches requested row");
      };
      frame(); Check(input.gamepad_count()>0,"real SDL enumerate/open path");
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767); frame();
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768); frame(); frame();
      Check(panel.blocking(),"real SDL RT opens Social");
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTY,32767); frame();
      SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTY,0); frame();
      Check(panel.focused_row() && panel.focused_row()->kind==SocialRowKind::DirectAddress,"left stick moves Social focus");
      focus(SocialRowKind::DirectAddress); button(SDL_GAMEPAD_BUTTON_SOUTH); Check(panel.editing(),"A opens IP keyboard");
      button(SDL_GAMEPAD_BUTTON_EAST); Check(!panel.editing() && panel.blocking(),"B cancels editor without closing Social");
      focus(SocialRowKind::DirectPort); button(SDL_GAMEPAD_BUTTON_SOUTH); Check(panel.editing(),"A opens port keyboard");
      button(SDL_GAMEPAD_BUTTON_EAST);
      focus(SocialRowKind::DirectCode); button(SDL_GAMEPAD_BUTTON_SOUTH); Check(panel.editing(),"A opens code keyboard");
      for (int i=0;i<6;++i) button(SDL_GAMEPAD_BUTTON_WEST);
      Check(panel.editor_text().empty(),"X erases generated six-character code");
      button(SDL_GAMEPAD_BUTTON_DPAD_DOWN); // index 10 = A
      for (int i=0;i<7;++i) button(SDL_GAMEPAD_BUTTON_SOUTH);
      Check(panel.editor_text()=="AAAAAA","pad keyboard enters exactly six characters");
      // index 43 is Keep. All keyboard controls are reachable by d-pad.
      for (int i=0;i<3;++i) button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
      for (int i=0;i<3;++i) button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
      button(SDL_GAMEPAD_BUTTON_SOUTH); Check(!panel.editing(),"pad keeps text");
      focus(SocialRowKind::DirectJoin); button(SDL_GAMEPAD_BUTTON_SOUTH);
      Check(joined==1 && joined_code=="AAAAAA","pad-driven join uses entered code");
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
    }
    SDL_CloseJoystick(joystick); SDL_DetachVirtualJoystick(id); SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    ImGui::DestroyContext(); std::cout<<"Social SDL pad: RT, rows, keyboard, join, chapters, Ready, Start, Leave and B passed\n";
  } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
