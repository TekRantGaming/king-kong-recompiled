#include "debug_overlay.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <rex/logging.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include "frame_stats.h"
#include "launcher_ui.h"
#include "play_internal.h"
#include "settings.h"
#if defined(KK_DEV_TOOLS)
#include "art.h"
namespace kk { double DevSecondsSinceMenu(); }
#endif
#if defined(KK_NATIVE_RENDERER)
#include "hooks/kk_native_ui.h"
#endif

namespace kk {
namespace {
std::atomic<bool> open{false};
std::mutex saved_mutex;
std::map<std::string, std::string, std::less<>> saved;
std::filesystem::path settings_path;
std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
void Change(const char* name, const std::string& value) {
  {
    std::lock_guard lock(saved_mutex);
    const auto [entry, inserted] = saved.try_emplace(name, Get(name));
    if (inserted) SetTemporarySettingBaseline(name, entry->second);
  }
  if (rex::cvar::SetFlagByName(name, value)) SetTemporarySettingTrialValue(name, Get(name));
}
void Reset() {
  std::lock_guard lock(saved_mutex);
  for (const auto& [name, value] : saved) {
    rex::cvar::SetFlagByName(name, value);
    SetTemporarySettingTrialValue(name, value);
  }
  saved.clear();
}
void Check(const char* label, const char* name, const char* on = "true", const char* off = "false") {
  bool value = Get(name) == on;
  if (ImGui::Checkbox(label, &value)) Change(name, value ? on : off);
}
void Choice(const char* label, const char* name, const char* labels, const char* const* values, int count) {
  int selected = 0;
  const auto current = Get(name);
  for (int i = 0; i < count; ++i) if (current == values[i]) selected = i;
  if (ImGui::Combo(label, &selected, labels)) Change(name, values[selected]);
}
void Strength(const char* label, const char* name, float maximum) {
  float value = float(std::atof(Get(name).c_str()));
  if (ImGui::SliderFloat(label, &value, 0, maximum, "%.2f")) Change(name, std::to_string(value));
}

class DebugOverlay final : public rex::ui::ImGuiDialog, public rex::ui::WindowInputListener {
 public:
  DebugOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, std::filesystem::path config)
      : ImGuiDialog(drawer), window_(window), config_(std::move(config)) {
    // Above the SDK input driver: explicitly feed ImGui, consume panel mouse events only.
    window_->AddInputListener(this, 1200);
    rex::ui::RegisterBind("bind_kk_graphics_debug", "F6", "Toggle graphics debug overlay", [] {
      if (play::SessionActive() && !play::CoverScreen() && !play::PauseMenuOpen()) open.store(!open.load());
    });
  }
  ~DebugOverlay() override {
    window_->RemoveInputListener(this);
    rex::ui::UnregisterBind("bind_kk_graphics_debug");
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (!DebugOverlayOpen()) return;
    held_[int(e.button())] = true;
    static_cast<rex::ui::WindowInputListener*>(imgui_drawer())->OnMouseDown(e);
    e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (!held_[int(e.button())]) return;  // release a button pressed in the game normally
    held_[int(e.button())] = false;
    static_cast<rex::ui::WindowInputListener*>(imgui_drawer())->OnMouseUp(e);
    e.set_handled(true);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!DebugOverlayOpen()) return;
    static_cast<rex::ui::WindowInputListener*>(imgui_drawer())->OnMouseMove(e);
    e.set_handled(true);
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!DebugOverlayOpen()) return;
    static_cast<rex::ui::WindowInputListener*>(imgui_drawer())->OnMouseWheel(e);
    e.set_handled(true);
  }
 protected:
  void OnDraw(ImGuiIO& io) override {
    if (config_.empty()) {
      std::lock_guard lock(saved_mutex);
      config_ = settings_path;
    }
#if defined(KK_DEV_TOOLS)
    TestEffects();
#endif
    if (!play::SessionActive() || play::CoverScreen() || play::PauseMenuOpen()) open.store(false);
    if (!DebugOverlayOpen()) {
      if (was_open_) {
        window_->SetCursorVisibility(cursor_before_);
        if (!nav_no_capture_before_) io.ConfigFlags &= ~ImGuiConfigFlags_NavNoCaptureKeyboard;
        was_open_ = false;
      }
      return;
    }
    if (!was_open_) {
      cursor_before_ = window_->GetCursorVisibility();
      nav_no_capture_before_ = (io.ConfigFlags & ImGuiConfigFlags_NavNoCaptureKeyboard) != 0;
      was_open_ = true;
    }
    window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;
    ImGui::SetNextWindowPos(ImVec2(24, 48), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(490, 610), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::color::kBase);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::color::kText);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ui::color::kAccent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ui::color::kAccent);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ui::color::kBase);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::color::kHairline);
    ImGui::PushStyleColor(ImGuiCol_Separator, ui::color::kHairline);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::color::kPanel);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(48, 42, 32, 230));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(65, 53, 35, 230));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(48, 42, 32, 230));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(65, 53, 35, 230));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(91, 71, 43, 230));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(48, 42, 32, 230));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(65, 53, 35, 230));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(91, 71, 43, 230));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ui::color::kAccent);
    bool visible = true;
    if (ImGui::Begin("Graphics effects (F6)", &visible, ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::TextWrapped("Changes apply while you play. Save only when you want to keep them.");
#if defined(KK_NATIVE_RENDERER)
      const bool native = Get("kk_renderer") == "native";
      ImGui::BeginDisabled(!native);
      Check("Ambient occlusion", "ao_mode", "1", "0");
      static const char* samples[] = {"1", "2", "3"};
      Choice("AO quality", "native_ao_samples", "Low\0Medium\0High\0", samples, 3);
      static const char* resolution[] = {"auto", "half", "full"};
      Choice("AO resolution", "native_ao_quality", "Auto\0Half\0Full\0", resolution, 3);
      int strength = std::atoi(Get("ao_strength").c_str());
      if (ImGui::SliderInt("AO strength", &strength, 1, 3)) Change("ao_strength", std::to_string(strength));
      Check("AO noise smoothing", "native_ao_temporal");
      ImGui::Separator();
      static const char* gi[] = {"off", "low", "medium", "high"};
      Choice("Global illumination", "native_gi", "Off\0Low\0Medium\0High\0", gi, 4);
      Strength("GI intensity", "native_gi_intensity", 4);
      static const char* shadow[] = {"original", "high", "ultra"};
      Choice("Sharper shadows", "native_shadow_quality", "Original\0High\0Ultra\0", shadow, 3);
      Check("Water reflections (SSR)", "native_water_reflections", "ssr", "original");
      Strength("Reflection strength", "native_ssr_strength", 4);
      static const char* shafts[] = {"off", "low", "high"};
      Choice("Volumetric light shafts", "native_volumetrics", "Off\0Low\0High\0", shafts, 3);
      Strength("Light shaft strength", "native_volumetric_strength", 4);
      ImGui::EndDisabled();
      if (!native) ImGui::TextWrapped("These effects require the native renderer.");
      ImGui::Separator();
      const auto frame = GetGuestFrameStats();
      ImGui::Text("%.1f FPS   %.2f ms", frame.fps, frame.frame_time_ms);
      const auto times = nr::kk::EffectTimes();
      auto timing = [](const char* label, double ms, bool enabled) {
        if (!enabled) return;
        if (ms < 0) ImGui::Text("%s: waiting for GPU sample", label);
        else ImGui::Text("%s: %.3f ms GPU", label, ms);
      };
      timing("AO", times.ao_ms, Get("ao_mode") != "0");
      timing("GI", times.gi_ms, Get("native_gi") != "off");
      timing("Shadows", times.shadows_ms, Get("native_shadow_quality") != "original");
      timing("Water", times.water_ms, Get("native_water_reflections") == "ssr");
      timing("Light shafts", times.volumetric_ms, Get("native_volumetrics") != "off");
      if (times.preparing) ImGui::TextUnformatted("Preparing effects...");
#else
      ImGui::TextUnformatted("These effects require a native renderer build.");
#endif
      if (ImGui::Button("Reset to my settings")) { Reset(); status_ = "Restored your settings."; }
      ImGui::SameLine();
      if (ImGui::Button("Save as my settings")) {
        SaveAsSettings();
      }
      if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor(17);
    if (!visible) open.store(false);
    // Keep all keyboard keys available to the running game.
    io.WantCaptureKeyboard = false;
  }
 private:
  void SaveAsSettings() {
    if (SaveSettings(config_, true)) {
      std::lock_guard lock(saved_mutex);
      saved.clear();
      ClearTemporarySettingBaselines();
      status_ = "Settings saved.";
    } else status_ = "Could not save settings.";
  }
#if defined(KK_DEV_TOOLS)
  void TestEffects() {
    const char* folder = std::getenv("KK_DEV_EFFECT_TEST");
    if (!folder || !*folder || !play::SessionActive() || play::CoverScreen()) return;
    const double now = DevSecondsSinceMenu();
    if (now < 5 || test_done_) return;
    struct Step { const char* name; const char* value; };
    static const Step steps[] = {{"ao_mode", "1"}, {"native_ao_samples", "1"}, {"native_ao_samples", "2"},
      {"native_ao_samples", "3"}, {"native_ao_quality", "half"}, {"native_ao_quality", "full"},
      {"ao_strength", "3"}, {"native_ao_temporal", "false"}, {"native_ao_temporal", "true"},
      {"native_gi", "low"}, {"native_gi", "medium"}, {"native_gi", "high"}, {"native_gi_intensity", "2"},
      {"native_shadow_quality", "high"}, {"native_shadow_quality", "ultra"}, {"native_shadow_quality", "original"},
      {"native_water_reflections", "ssr"}, {"native_ssr_strength", "2"}, {"native_water_reflections", "original"},
      {"native_volumetrics", "low"}, {"native_volumetrics", "high"}, {"native_volumetric_strength", "2"},
      {"native_volumetrics", "off"}, {"ao_mode", "0"}};
    if (test_next_ == 0) {
      open.store(true);
      SaveSettings(config_);
      test_next_ = now;
    }
    if (now < test_next_) return;
    const auto capture = [&](const char* suffix) {
      art::CaptureFrame(std::filesystem::path(folder) / ("effect-" + std::to_string(test_step_) + "-" + suffix + ".png"));
    };
    if (test_step_ < int(std::size(steps))) {
      if (!test_after_) {
        capture("before");
        const auto& step = steps[test_step_];
        Change(step.name, step.value);
        REXLOG_INFO("KK effect test: step {} {}={} at {:.3f} tick {:.6f}", test_step_, step.name, step.value, now,
          std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        test_after_ = true;
        test_next_ = now + 2;
      } else {
#if defined(KK_NATIVE_RENDERER)
        if (nr::kk::EffectTimes().preparing) { test_next_ = now + 0.25; return; }
#endif
        capture("after");
        // An unrelated menu save must preserve every trial's saved value.
        if (std::string_view(steps[test_step_].name) == "native_gi" && std::string_view(steps[test_step_].value) == "high")
          SaveSettings(config_);
        ++test_step_;
        test_after_ = false;
        test_next_ = now + 1;
      }
    } else if (test_step_ == int(std::size(steps))) {
      Reset();
      SaveSettings(config_);
      ++test_step_;
      test_next_ = now + 2;
    } else if (test_step_ <= int(std::size(steps)) + 50) {
      if (test_after_) {
        capture("after");
        test_after_ = false;
        ++test_step_;
        test_next_ = now + 0.5;
        return;
      }
      capture("before");
      const bool enabled = (test_step_ - int(std::size(steps))) % 2 == 0;
      Change("ao_mode", enabled ? "1" : "0");
      Change("native_ao_samples", enabled ? "3" : "1");
      Change("native_ao_temporal", enabled ? "true" : "false");
      Change("native_gi", enabled ? "high" : "off");
      Change("native_shadow_quality", enabled ? "ultra" : "original");
      Change("native_water_reflections", enabled ? "ssr" : "original");
      Change("native_volumetrics", enabled ? "high" : "off");
      REXLOG_INFO("KK effect test: stress toggle {} at {:.3f}", test_step_ - int(std::size(steps)), now);
      test_after_ = true;
      test_next_ = now + 0.5;
    } else {
      Reset();
      Change("native_gi_intensity", "1.25");
      SaveAsSettings();
      Change("native_gi_intensity", "2.5");
      SaveSettings(config_);
      Reset();
      SaveSettings(config_);
      open.store(false);
      test_done_ = true;
      REXLOG_INFO("KK effect test: done at {:.3f}", now);
    }
  }
  int test_step_ = 0;
  double test_next_ = 0;
  bool test_after_ = false, test_done_ = false;
#endif
  rex::ui::Window* window_;
  std::filesystem::path config_;
  rex::ui::Window::CursorVisibility cursor_before_ = rex::ui::Window::CursorVisibility::kVisible;
  bool was_open_ = false, nav_no_capture_before_ = false;
  std::map<int, bool> held_;
  std::string status_;
};
}
bool DebugOverlayOpen() { return open.load(std::memory_order_relaxed); }
void SetDebugOverlayConfigPath(const std::filesystem::path& config) {
  std::lock_guard lock(saved_mutex);
  settings_path = config;
}
void CreateDebugOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, const std::filesystem::path& config) {
  static bool created = false;
  if (created || !window) return;
  created = true;
  new DebugOverlay(drawer, window, config);
}
}  // namespace kk
