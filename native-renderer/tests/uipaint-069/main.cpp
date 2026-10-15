// No visible window, GPU, game image, or saves. Exercise the real SDK presenter,
// mailbox, window input propagation and ImGui dialog scheduler with fake I/O.
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/logging.h>
#include <imgui.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>

using namespace rex::ui;
void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
  std::printf("PASS %s\n", message);
}
struct Context : WindowedAppContext {
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};
struct TestSurface : Surface {
  uint32_t w = 1280, h = 720;
  TypeIndex GetType() const override { return kTypeIndex_Win32Hwnd; }
  bool GetSizeImpl(uint32_t& x, uint32_t& y) const override { x=w; y=h; return true; }
};
struct TestWindow : Window {
  explicit TestWindow(Context& c) : Window(c, "no-game scheduling", 1280, 720) {
    // Presenter checks monitor membership. Never show or activate this HWND.
    handle=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"no-game scheduling",WS_OVERLAPPED,
                           0,0,1280,720,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if (!handle) throw std::runtime_error("hidden HWND creation");
  }
  ~TestWindow() override { EnterDestructor(); DestroyWindow(handle); }
  HWND handle=nullptr;
  void* GetNativeWindowHandle() const override { return handle; }
  bool pending = false;
  TestSurface* surface = nullptr;
  bool OpenImpl() override {
    WindowDestructionReceiver receiver(this);
    OnActualSizeUpdate(1280, 720, receiver);
    return true;
  }
  void RequestCloseImpl() override {}
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags) override {
    auto result = std::make_unique<TestSurface>(); surface=result.get(); return result;
  }
  void RequestPaintImpl() override { pending=true; }
  bool Pump() { if (!pending) return false; pending=false; OnPaint(); return true; }
  void Expose() { OnPaint(true); }
  void Resize() {
    surface->w=1024;
    WindowDestructionReceiver receiver(this);
    OnActualSizeUpdate(1024,720,receiver);
  }
  void Key(VirtualKey key) {
    KeyEvent e(this, key, 1, false, false, false, false, false);
    WindowDestructionReceiver receiver(this);
    OnKeyDown(e, receiver);
  }
};
struct Texture : ImmediateTexture { Texture(uint32_t w,uint32_t h):ImmediateTexture(w,h){} };
struct Immediate : ImmediateDrawer {
  int vertices=0;
  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t w,uint32_t h,ImmediateTextureFilter,bool,const uint8_t*) override {
    return std::make_unique<Texture>(w,h);
  }
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override { vertices+=batch.vertex_count; }
  void Draw(const ImmediateDraw&) override {}
  void EndDrawBatch() override {}
};
struct TestPresenter : Presenter {
  explicit TestPresenter(Immediate& i):Presenter(nullptr), immediate(i) {
    if (!InitializeCommonSurfaceIndependent()) throw std::runtime_error("presenter initialization");
  }
  Immediate& immediate;
  int presents=0, connects=0;
  bool outdated=false;
  Surface::TypeFlags GetSupportedSurfaceTypes() const override { return Surface::kTypeFlag_Win32Hwnd; }
  bool CaptureGuestOutput(RawImage&) override { return false; }
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(Surface&,uint32_t,uint32_t,bool,bool& vsync) override {
    ++connects; vsync=true; return SurfacePaintConnectResult::kSuccess;
  }
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override {}
  bool RefreshGuestOutputImpl(uint32_t,uint32_t,uint32_t,std::function<bool(GuestOutputRefreshContext&)> refresher,bool& bpc) override {
    struct RefreshContext : GuestOutputRefreshContext {
      explicit RefreshContext(bool& b):GuestOutputRefreshContext(b){}
    } context(bpc);
    return refresher(context);
  }
  PaintResult PaintAndPresentImpl(bool ui) override {
    if (outdated) { outdated=false; return PaintResult::kNotPresentedConnectionOutdated; }
    immediate.vertices=0;
    if (ui) {
      struct DrawContext : UIDrawContext {
        explicit DrawContext(Presenter& p):UIDrawContext(p,1280,720){}
      } context(*this);
      ExecuteUIDrawersFromUIThread(context);
    }
    ++presents;
    return PaintResult::kPresented;
  }
};
struct Overlay : ImGuiDialog {
  explicit Overlay(ImGuiDrawer* drawer):ImGuiDialog(drawer){}
  bool visible=false, animated=false;
  int ticks=0;
  bool NeedsContinuousPaint() override { return visible && animated; }
  void OnDraw(ImGuiIO&) override {
    if (!visible) return;
    ++ticks;
    ImGui::GetForegroundDrawList()->AddRectFilled({10,10},{50,50},0xFFFFFFFF);
  }
};
// F8/keybinds may consume input before the ImGui listener. Window must still wake.
struct Toggle : WindowInputListener {
  Overlay& overlay;
  explicit Toggle(Overlay& o):overlay(o){}
  void OnKeyDown(KeyEvent& e) override { overlay.visible=!overlay.visible; e.set_handled(true); }
};
int main(int argc, char**) {
  rex::InitLogging();
  std::setvbuf(stdout,nullptr,_IONBF,0);
  if (argc>1) rex::cvar::SetFlagByName("ui_paint_on_demand","false");
  try {
    Context context;
    TestWindow window(context);
    Immediate immediate;
    TestPresenter presenter(immediate);
    window.SetPresenter(&presenter);
    window.Open();
    window.RequestPaint();
    {
      ImGuiDrawer drawer(&window, 10);
      drawer.SetPresenterAndImmediateDrawer(&presenter, &immediate);
      Overlay overlay(&drawer);
      Toggle toggle(overlay);
      window.AddInputListener(&toggle, 100);
      window.Pump();
      Check(!window.pending, "registered idle dialog does not request repeat paints");
      auto before=presenter.presents;
      const auto start=std::chrono::steady_clock::now();
      const bool refreshed=presenter.RefreshGuestOutput(1280,720,16,9,[](auto&){return true;});
      Check(window.pending || presenter.presents==before+1, "new guest output wakes UI immediately");
      window.Pump();
      Check(presenter.presents==before+1 && std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100), "new guest output paints promptly once");
      for (auto key : {VirtualKey::kF2, VirtualKey::kF8, VirtualKey::kEscape}) {
        overlay.animated = key!=VirtualKey::kF2;
        window.Key(key); window.Pump();
        Check(immediate.vertices>0, "handled overlay open input wakes and draws");
        if (overlay.animated) {
          const auto ticks=overlay.ticks;
          for(int n=0;n<4;++n) window.Pump();
          Check(overlay.ticks==ticks+4, "visible UI animates with guest output stopped");
        } else Check(!window.pending, "static F2 overlay does not drive monitor refresh");
        window.Key(key); window.Pump();
        Check(immediate.vertices==0 && !window.pending, "overlay close paints clear then becomes idle");
      }
      before=presenter.presents;
      window.Expose();
      Check(presenter.presents==before+1, "expose forces quiet-frame paint");
      before=presenter.connects;
      window.Resize(); window.Pump();
      Check(presenter.connects>before, "resize reconnects and paints while idle");
      presenter.outdated=true;
      window.RequestPresenterUIPaintFromUIThread(); window.Pump();
      Check(window.pending, "outdated surface schedules recovery");
      before=presenter.presents;
      window.Pump();
      Check(presenter.presents==before+1 && !window.pending, "recovery repaints and returns idle");
      window.RemoveInputListener(&toggle);
    }
    window.SetPresenter(nullptr);
    std::puts("All no-game presenter scheduling checks passed.");
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
}
