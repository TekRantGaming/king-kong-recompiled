#include "input_diag.h"
#include "play_internal.h"
#include "debug_overlay.h"
#include "coop_observer.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <mutex>
#include <map>
#include <string>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#endif

REXCVAR_DEFINE_INT32(kk_input_diag, 0, "KK/Diagnostics", "Log native input and filter stages once per second (0 off, 1 on)");
namespace kk::input_diag {
namespace {
std::mutex mutex;
std::string stages;
std::string active_stages;
std::map<std::string,uint64_t> zeroed;
bool previous_nonzero=false, sample_active=false;
bool launcher_active = false, social_open = false, editor_open = false;
int launcher_pads = 0;
int32_t title_active_pad=-1;
uint32_t title_native_block=0;
uint64_t reads = 0;
bool Nonzero(const uint8_t* p) {
  if (p) for (int i=4;i<16;++i) if (p[i]) return true;
  return false;
}
std::string Pad(const uint8_t* p) {
  if (!p) return "none";
  auto w=[&](int n) { return uint16_t(uint16_t(p[n])<<8|p[n+1]); };
  return fmt::format("{:04X},t={}/{},l={}/{},r={}/{}", w(4),p[6],p[7],int16_t(w(8)),int16_t(w(10)),int16_t(w(12)),int16_t(w(14)));
}
struct KeyProbe : rex::ui::WindowInputListener {
  uint32_t held=0,seen=0;
  void OnKeyDown(rex::ui::KeyEvent& e) override { Key(e,true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { Key(e,false); }
  void Key(rex::ui::KeyEvent& e,bool down) {
    if (!Enabled()) return;
    constexpr int codes[]={'W','A','S','D',0x26,0x28,0x25,0x27,0x0D,0x1B,0x20,0x73};
    for (int i=0;i<int(std::size(codes));++i) if (int(e.virtual_key())==codes[i]) {
      if (down) { held|=1u<<i; seen|=1u<<i; } else held&=~(1u<<i);
    }
  }
};
class Diagnostics final : public rex::ui::ImGuiDialog, public KeyProbe, public rex::ui::WindowListener {
 public:
  Diagnostics(rex::ui::ImGuiDrawer* d,rex::ui::Window* w) : ImGuiDialog(d),window_(w) {
    window_->AddInputListener(this, 2000); // observe only, before all consumers
    window_->AddInputListener(&game_keys_, 0); // events that reach the MnK driver's layer
    window_->AddListener(this);
    REXLOG_INFO("KK input diag: enabled; masks W A S D Up Down Left Right Enter Esc Space F4 = bits 0..11; pad buttons,t=LT/RT,l=LX/LY,r=RX/RY; text contents are never logged");
  }
  ~Diagnostics() override {
    window_->RemoveInputListener(this);
    window_->RemoveInputListener(&game_keys_);
    window_->RemoveListener(this);
    for (auto* pad : pads_) SDL_CloseGamepad(pad);
    if (sdl_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
  bool NeedsContinuousPaint() override { return Enabled(); }
  void OnLostFocus(rex::ui::UISetupEvent&) override { held=game_keys_.held=0; }
 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!Enabled()) return;
    const auto now=std::chrono::steady_clock::now();
    if (now<next_) return;
    next_=now+std::chrono::seconds(1);
    // UI thread only. This is the app SDL, not the runtime SDL; label both
    // domains explicitly rather than pretending its keyboard owns our HWND.
    if (!sdl_) {
#if defined(_WIN32)
      SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
#endif
      sdl_=SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }
    SDL_UpdateGamepads();
    int count=0; auto* ids=SDL_GetGamepads(&count);
    for (int i=0;i<count;++i) {
      bool found=false;
      for (auto* p : pads_) found |= SDL_GetGamepadID(p)==ids[i];
      if (!found) if (auto* p=SDL_OpenGamepad(ids[i])) pads_.push_back(p);
    }
    SDL_free(ids);
    std::string raw;
    for (auto it=pads_.begin();it!=pads_.end();) {
      auto* p=*it;
      if (!SDL_GamepadConnected(p)) { SDL_CloseGamepad(p); it=pads_.erase(it); continue; }
      uint32_t buttons=0;
      for (int b=0;b<SDL_GAMEPAD_BUTTON_COUNT;++b) if (SDL_GetGamepadButton(p,SDL_GamepadButton(b))) buttons|=1u<<b;
      raw+=fmt::format(" [{}:{:X},t={}/{},l={}/{},r={}/{}]",SDL_GetGamepadID(p),buttons,
          SDL_GetGamepadAxis(p,SDL_GAMEPAD_AXIS_LEFT_TRIGGER),SDL_GetGamepadAxis(p,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER),
          SDL_GetGamepadAxis(p,SDL_GAMEPAD_AXIS_LEFTX),SDL_GetGamepadAxis(p,SDL_GAMEPAD_AXIS_LEFTY),
          SDL_GetGamepadAxis(p,SDL_GAMEPAD_AXIS_RIGHTX),SDL_GetGamepadAxis(p,SDL_GAMEPAD_AXIS_RIGHTY)); ++it;
    }
    constexpr SDL_Scancode codes[]={SDL_SCANCODE_W,SDL_SCANCODE_A,SDL_SCANCODE_S,SDL_SCANCODE_D,SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,
        SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,SDL_SCANCODE_RETURN,SDL_SCANCODE_ESCAPE,SDL_SCANCODE_SPACE,SDL_SCANCODE_F4};
    const bool* keys=SDL_GetKeyboardState(nullptr); uint32_t sdl_keys=0;
    for (int i=0;i<int(std::size(codes));++i) if (keys[codes[i]]) sdl_keys|=1u<<i;
    uintptr_t own=reinterpret_cast<uintptr_t>(window_->GetNativeWindowHandle()),foreground=0;
    unsigned long own_pid=0,window_pid=0,front_pid=0; bool native_focus=window_->HasFocus(); std::string xinput;
#if defined(_WIN32)
    own_pid=GetCurrentProcessId(); auto front=GetForegroundWindow(); foreground=reinterpret_cast<uintptr_t>(front);
    GetWindowThreadProcessId(reinterpret_cast<HWND>(own),&window_pid);
    GetWindowThreadProcessId(front,&front_pid); native_focus=front==reinterpret_cast<HWND>(own);
    // System DLL only; no SDK or third-party service. XInput is shared across instances.
    using Read=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
    static auto module=LoadLibraryExW(L"xinput1_4.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto read=module ? reinterpret_cast<Read>(GetProcAddress(module,"XInputGetState")) : nullptr;
    for (DWORD i=0;read && i<4;++i) {
      XINPUT_STATE p{};
      if (read(i,&p)==ERROR_SUCCESS) xinput+=fmt::format(" [{}:{:04X},t={}/{},l={}/{},r={}/{}]",i,p.Gamepad.wButtons,
          p.Gamepad.bLeftTrigger,p.Gamepad.bRightTrigger,p.Gamepad.sThumbLX,p.Gamepad.sThumbLY,p.Gamepad.sThumbRX,p.Gamepad.sThumbRY);
    }
#endif
    std::string trace,blocked; uint64_t n; bool active,social,editing; int lp; int32_t title_pad; uint32_t title_block;
    { std::lock_guard lock(mutex); trace=active_stages.empty() ? stages : active_stages; active_stages.clear();
      for (const auto& [name,times] : zeroed) blocked+=fmt::format(" {}:{}",name,times);
      zeroed.clear(); n=reads; reads=0; active=launcher_active; social=social_open; editing=editor_open; lp=launcher_pads;
      title_pad=title_active_pad; title_block=title_native_block; }
    REXLOG_INFO("KK input diag: own={:X}/window-pid={} process={} startup-child={} foreground={:X}/pid={} window-focus={} native-focus={} launcher-active={} launcher-pads={} social={} editor={} capture-k/m/text={}/{}/{} backend={} mnk={}/{} cover={} pause={} debug={} observer={}",
        own,window_pid,own_pid,rex::cvar::GetFlagByName("kk_startup_child"),foreground,front_pid,window_->HasFocus(),native_focus,active,lp,social,editing,
        io.WantCaptureKeyboard,io.WantCaptureMouse,io.WantTextInput,rex::cvar::GetFlagByName("input_backend"),
        rex::cvar::GetFlagByName("mnk_mode"),rex::cvar::GetFlagByName("mnk_mouse"),
        play::CoverScreen(),play::PauseMenuOpen(),DebugOverlayOpen(),coop::observer::Active());
    REXLOG_INFO("KK input diag: app-SDL-pads={}{} XInput={}{} keys app-SDL={:03X} runtime-window={:03X}/{:03X} game-events={:03X}/{:03X} (held/seen) title-active-pad={} title-block={} game-native/filters reads={} zeroed={} sample={}",
        count,raw,xinput.empty() ? "none" : "",xinput,sdl_keys,held,seen,game_keys_.held,game_keys_.seen,title_pad,title_block,n,blocked.empty() ? "none" : blocked,
        n ? trace : "no game pad read this second");
    seen=game_keys_.seen=0;
  }
 private:
  KeyProbe game_keys_;
  rex::ui::Window* window_;
  bool sdl_=false;
  std::vector<SDL_Gamepad*> pads_;
  std::chrono::steady_clock::time_point next_{};
};
}
bool Enabled() { return REXCVAR_GET(kk_input_diag)!=0; }
void Install(rex::ui::ImGuiDrawer* d,rex::ui::Window* w) { if (Enabled() && d && w) new Diagnostics(d,w); }
void Begin(uint32_t user,uint32_t status,const uint8_t* state,int32_t active_pad,uint32_t native_block) {
  if (!Enabled() || user) return;
  std::lock_guard lock(mutex); ++reads; previous_nonzero=sample_active=!status && Nonzero(state);
  title_active_pad=active_pad; title_native_block=native_block;
  stages=fmt::format("native(status={})={}",status,Pad(state));
}
void Stage(uint32_t user,const char* stage,const uint8_t* state,uint32_t status) {
  if (!Enabled() || user) return;
  std::lock_guard lock(mutex);
  const bool nonzero=!status && Nonzero(state);
  if (previous_nonzero && !nonzero) ++zeroed[stage];
  previous_nonzero=nonzero; sample_active|=nonzero;
  stages+=fmt::format(" > {}(status={})={}",stage,status,Pad(state));
  if (std::string_view(stage)=="game-final" && sample_active) active_stages=stages;
}
void Launcher(bool active,int pads,bool social,bool editing) {
  if (!Enabled()) return;
  std::lock_guard lock(mutex); launcher_active=active; launcher_pads=pads; social_open=social; editor_open=editing;
}
}
