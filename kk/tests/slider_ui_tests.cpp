// The real launcher/pause DrawList and controller repeat, with an in-memory
// ImGui frame. Art is unused; no window, game, saves or physical input.
#include "launcher_ui.h"
#include "art.h"
#include "platform.h"
#include <rex/ui/immediate_drawer.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace kk {
const UiFonts& GetUiFonts() { static UiFonts fonts{}; return fonts; }
void KeepTextureAlive(std::unique_ptr<rex::ui::ImmediateTexture>) {}
std::string PadPromptsFor(int,const char*) { return "xbox360"; }
namespace art { Image LoadImage(const std::filesystem::path&) { return {}; } }
}
int main() {
  try {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.DisplaySize = {1280,720}; io.DeltaTime = 0.01f;
    io.IniFilename = nullptr;
    unsigned char* pixels; int w,h;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    int count = 0;
    // Every numeric slider range in launcher/pause/F8 (special Auto values
    // remain checked by graphics-panel tests).
    for (auto range : std::vector<std::array<float,3>>{{0,100,1},{0,100,5},{69,110,1},{10,200,5},
         {25,300,5},{0,40,1},{0.1f,5,0.05f},{0,1,0.05f},{0,4,0.05f},{0.25f,4,0.05f},
         {1,16,1},{-2,1,0.05f},{0.3f,1,0.05f}}) {
      kk::ui::Input input(false);
      kk::ui::ListState state;
      float value = range[0]; int changes = 0;
      auto frame = [&](uint16_t buttons) {
        ImGui::NewFrame(); ImGui::Begin("test");
        input.FeedPad(buttons,0,0,0,0); input.Update();
        kk::ui::Item item; item.type = kk::ui::ItemType::kSlider;
        item.label = "Volume"; item.lo = range[0]; item.hi = range[1]; item.step = range[2]; item.value = value;
        item.on_value = [&](float v) { value = v; ++changes; };
        std::vector<kk::ui::Item> items{item};
        kk::ui::DrawList(ImGui::GetWindowDrawList(),items,state,{20,20},{1000,500},{},input,true);
        ImGui::End(); ImGui::Render();
      };
      frame(0); frame(8);
      if (changes != 1 || std::abs(value-(range[0]+range[2])) > 1e-4f)
        throw std::runtime_error("first pad press did not move one step");
      for (int i=0;i<20;++i) frame(8);
      if (changes != 1) throw std::runtime_error("repeat started before delay");
      for (int i=0;i<20000 && value<range[1];++i) frame(8);
      if (value != range[1]) throw std::runtime_error("held right failed to reach maximum");
      frame(0);
      for (int i=0;i<20000 && value>range[0];++i) frame(4);
      if (value != range[0]) throw std::runtime_error("held left failed to reach minimum");
      frame(0); const int before = changes;
      frame(8); frame(0); frame(8);
      if (changes != before+2) throw std::runtime_error("quick separate presses were lost");
      ++count;
    }
    {
      // Steam can type an arrow a frame before its corresponding pad sample.
      // Pair the duplicate once; a second rapid physical press must still count.
      kk::ui::Input input(false); kk::ui::ListState state; float value = 50;
      auto frame = [&](uint16_t pad,bool key) {
        io.AddKeyEvent(ImGuiKey_LeftArrow,key);
        ImGui::NewFrame(); ImGui::Begin("echo test");
        input.FeedPad(pad,0,0,0,0); input.Update();
        kk::ui::Item item; item.type=kk::ui::ItemType::kSlider; item.label="Volume";
        item.lo=0; item.hi=100; item.step=1; item.value=value; item.on_value=[&](float v){value=v;};
        std::vector<kk::ui::Item> items{item};
        kk::ui::DrawList(ImGui::GetWindowDrawList(),items,state,{20,20},{1000,500},{},input,true);
        ImGui::End(); ImGui::Render();
      };
      frame(0,true); frame(4,true);
      if(value!=49) throw std::runtime_error("keyboard-first Steam echo counted twice");
      frame(0,false); frame(0,true); frame(4,true);
      if(value!=48) throw std::runtime_error("second quick Steam controller press was lost");
      frame(0,false); frame(4,false);
      if(value!=47) throw std::runtime_error("third quick pad press was lost");
      frame(0,false); frame(4,true);
      if(value!=46) throw std::runtime_error("pad-first Steam echo counted twice");
      frame(0,false);
    }
    ImGui::DestroyContext();
    std::cout << count << " production slider ranges: press, repeat and endpoints passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
