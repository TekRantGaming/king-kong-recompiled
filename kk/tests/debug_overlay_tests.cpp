#include "debug_overlay_logic.h"
#include "launcher_text.h"
#include <rex/cvar.h>
#include <iostream>
#include <string_view>

REXCVAR_DEFINE_INT32(user_language, 1, "Test", "Test");
REXCVAR_DEFINE_STRING(native_upscaler, "off", "Test", "Test");
namespace {
int checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void PadChecks() {
  using namespace kk::panel;
  PadCapture filter;
  PadState p{0x0040};
  Check(!filter.Filter(p, true, false) && p.buttons == 0x0040, "single crouch click remains the game's");
  p.buttons = 0x0080;
  Check(!filter.Filter(p, true, false) && p.buttons == 0x0080, "single zoom click remains the game's");
  p.buttons = 0x00C0;
  Check(filter.Filter(p, true, false) && p.Neutral(), "opening chord suppressed before panel paint");
  Check(filter.TakeToggle(), "opening chord requests toggle");
  p.buttons = 0x00C0;
  Check(filter.Filter(p, true, true) && !filter.TakeToggle(), "held chord never repeats");
  p = {};
  filter.Filter(p, true, true);
  for (uint16_t b : {0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020,
                     0x0040, 0x0080, 0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000}) {
    p = {b, 255, 255, 32767, -32768, -32768, 32767};
    Check(filter.Filter(p, true, true) && p.Neutral(), "open panel hides all game controls");
    Check(filter.latest.buttons == b && filter.latest.lx == 32767, "UI gets original physical state");
  }
  p = {0x00C0};
  Check(filter.Filter(p, true, true) && filter.TakeToggle(), "second chord requests close");
  p = {0x2000, 255, 255, 32767, 0, 0, -32768};
  Check(filter.Filter(p, true, false) && p.Neutral(), "close blocks held buttons, triggers and sticks");
  p = {0, 0, 0, 0, 0, 16000, 0};
  Check(filter.Filter(p, true, false) && p.Neutral(), "held camera stick cannot leak after close");
  p = {};
  Check(!filter.Filter(p, true, false), "neutral releases capture");
  p = {0x1000, 0, 255};
  Check(!filter.Filter(p, true, false) && p.buttons == 0x1000 && p.rt == 255, "game controls resume after neutral");
  p = {0x00C0};
  Check(!filter.Filter(p, false, false) && !filter.TakeToggle(), "no panel chord in launcher, menus or load screen");
  filter.Filter(p, true, false);
  Check(!filter.TakeToggle(), "a chord held across menu exit is not a new chord");
}
void CapabilityChecks() {
  using namespace kk::panel;
  const UpscalerCaps yes{true, true, true, true, true};
  for (uint32_t api : {0u, 1u, 2u}) {
    for (uint32_t vendor : {0u, 0x10DEu, 0x1002u, 0x8086u}) {
      for (bool known : {false, true}) for (bool supported : {false, true}) {
        auto c = yes; c.known = known; c.fsr4 = supported;
        const auto offered = OfferedUpscalers(api, vendor, c);
        const bool expected = api == 2 && vendor == 0x1002 && known && supported;
        Check((std::find(offered.begin(), offered.end(), 2) != offered.end()) == expected,
              "FSR 4 only with DirectX 12, AMD and positive SDK capability");
        // Follow the same ordered list and navigation helper as the live panel.
        int at = 0;
        bool selected = false;
        for (size_t i = 0; i < offered.size(); ++i) {
          at = NextChoice(at, int(offered.size()), 1);
          rex::cvar::SetFlagByName("native_upscaler", kUpscalers[offered[at]].value);
          selected |= rex::cvar::GetFlagByName("native_upscaler") == "fsr4";
        }
        Check(selected == expected && at == 0, "controller selection reaches FSR 4 exactly when offered");
      }
    }
  }
  Check(OfferedUpscalers(2, 0x1002, yes, false) == std::vector<int>{0}, "non-Windows offers no temporal upscaler");
  Check(OfferedUpscalers(1, 0x10DE, {}) == std::vector<int>({0, 1, 3, 4}), "unknown SDK retains existing Vulkan defaults");
  Check(OfferedUpscalers(2, 0x1002, {true}) == std::vector<int>{0}, "known unsupported capabilities offer off only");
  Check(NextChoice(-1, 3, 1) == 0 && NextChoice(-1, 3, -1) == 2, "unavailable current value enters supported choices");
  Check(NextChoice(0, 3, -1) == 2 && NextChoice(2, 3, 1) == 0, "choices wrap in both directions");
}
struct Entry { const char *en, *de, *es, *fr, *it; };
const Entry entries[] = {
#include "debug_overlay_text_table.inc"
};
void TextChecks() {
  const char* languages[] = {"1", "3", "5", "4", "6"};
  for (int lang = 0; lang < 5; ++lang) {
    Check(rex::cvar::SetFlagByName("user_language", languages[lang]), "set actual language cvar");
    for (const auto& e : entries) {
      const char* values[] = {e.en, e.de, e.es, e.fr, e.it};
      Check(std::string_view(kk::text::T(e.en)) == values[lang], "panel translation through actual launcher lookup");
    }
    const auto formatted = kk::text::F("Unsaved changes: {0}", {"17"});
    Check(formatted.find("17") != std::string::npos && formatted.find("{0}") == std::string::npos,
          "translated template substitutes its arguments");
  }
}
}
int main() {
  PadChecks(); CapabilityChecks(); TextChecks();
  std::cout << checks << " graphics panel checks passed\n";
}
