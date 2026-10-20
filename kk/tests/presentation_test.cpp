#include <cstdlib>
#include <cstdio>

#include "presentation.h"

namespace {

void Expect(kk::PresentationChoice actual, kk::PresentationMode mode, bool letterbox) {
  if (actual.mode != mode || actual.letterbox_16_9 != letterbox) {
    std::fprintf(stderr, "presentation mismatch: got mode=%d letterbox=%d; expected mode=%d letterbox=%d\n",
                 int(actual.mode), int(actual.letterbox_16_9), int(mode), int(letterbox));
    std::abort();
  }
}

void ExpectBool(bool actual, bool expected) {
  if (actual != expected) {
    std::fprintf(stderr, "boolean policy mismatch: got %d; expected %d\n", int(actual), int(expected));
    std::abort();
  }
}

}  // namespace

int main() {
  using kk::PresentationMode;
  // The selected display mode is 21:9 (present_letterbox=false).
  // Menus follow the selected output aspect too, with the menu camera widened
  // by the same aspect correction as gameplay.
  Expect(kk::ChoosePresentation(false, true, true, false), PresentationMode::kMenu, false);
  Expect(kk::ChoosePresentation(false, true, true, true), PresentationMode::kMenu, true);
  Expect(kk::ChoosePresentation(true, false, true, false), PresentationMode::kCutscene, true);
  Expect(kk::ChoosePresentation(true, false, false, false), PresentationMode::kCutscene, false);
  Expect(kk::ChoosePresentation(true, true, false, false), PresentationMode::kMenu, false);
  Expect(kk::ChoosePresentation(false, false, true, false), PresentationMode::kGameplay, false);

  // A saved 16:9 display preference remains 16:9 during gameplay.
  Expect(kk::ChoosePresentation(false, false, true, true), PresentationMode::kGameplay, true);

  // Inactive menu script means gameplay can use the selected ultrawide mode;
  // active menu script keeps the front end or pause menu at 16:9.
  ExpectBool(kk::KeepMenuFraming(true), true);
  ExpectBool(kk::KeepMenuFraming(false), false);

  // Gameplay camera detection must not depend on Jack's default FOV: interaction
  // and scripted gameplay views can use a different angle.
  ExpectBool(kk::IsGameplayCameraView(false, false), true);
  ExpectBool(kk::IsGameplayCameraView(true, false), false);
  ExpectBool(kk::IsGameplayCameraView(false, true), false);

  // Render at the selected output aspect so screen-space UI is not stretched
  // when the guest frame is scaled to the display.
  if (kk::GameFrameWidth(3440, 1440, false) != 1720 ||
      kk::GameFrameWidth(1920, 1080, false) != 1280 ||
      kk::GameFrameWidth(2560, 1600, false) != 1152 ||
      kk::GameFrameWidth(3440, 1440, true) != 1280 ||
      kk::GameFrameWidth(0, 0, false) != 1280) {
    std::fprintf(stderr, "game frame width policy mismatch\n");
    std::abort();
  }

  // A buffered movie can go quiet on disk reads while it is still displayed.
  // Retain cutscene framing until the player or gameplay camera gives a positive
  // end-of-cutscene signal.
  ExpectBool(kk::KeepCutsceneFraming(false, true, false), true);
  ExpectBool(kk::KeepCutsceneFraming(true, false, true), true);
  ExpectBool(kk::KeepCutsceneFraming(false, true, true), false);
  ExpectBool(kk::KeepCutsceneFraming(false, false, false), false);
}
