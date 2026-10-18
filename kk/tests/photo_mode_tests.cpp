// Task 208: photo mode's logic without the game or a GPU. The settings it sends the renderer (defaults
// change nothing, the player's own settings stay in charge until a row is changed), the camera's limits
// (radius, roll), the controller mapping, the picture names, and the renderer's side: which draws it leaves
// out (only Jack's arms, never by shader alone) and the photo grade's colour function (the GPU pass does the
// same, shaders/photo.hlsl).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "backend/photo_config.h"
#include "freecam_math.h"
#include "photo_mode_logic.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}
bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }
float Luma(const std::array<float, 3>& c) { return c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f; }
}  // namespace

int main() {
  using namespace kk::photo;
  // Fresh values: the player's picture settings are the Picture rows' defaults; nothing overrides them.
  const Values v = Fresh(1, 2, 12.0f, 1.2f, 7);
  const NrPhotoSettings s = ToRenderer(v, true);
  Check(s.size == sizeof(NrPhotoSettings), "size");
  Check(s.active == 1 && s.hide == 3, "active hides the arms and the pause cover");
  Check(s.cel == -1 && s.sharpness == -1 && std::isnan(s.brightness) && std::isnan(s.gamma),
        "untouched Picture rows keep the player's settings");
  Check(!nr::PhotoGradeNeeded(nr::SanitizePhotoSettings(s)), "defaults need no grade pass");
  Values changed = v;
  changed.cel = 0;
  changed.brightness = -5;
  const NrPhotoSettings c = ToRenderer(changed, true);
  Check(c.cel == 0 && Near(c.brightness, -5) && c.sharpness == -1, "a changed row overrides only that setting");
  Values no_arms = v;
  no_arms.hide_arms = false;
  Check(ToRenderer(no_arms, true).hide == 2, "arms shown: only the pause cover is left out");
  Check(ToRenderer(v, false).active == 0 && ToRenderer(v, false).hide == 0, "inactive hides nothing");
  Values look = v;
  look.look = 2;
  look.letterbox = 3;
  look.dof = 50;
  const NrPhotoSettings l = ToRenderer(look, true);
  Check(l.look == 2 && Near(l.letterbox, 2.39f) && Near(l.dof, 0.5f), "look, letterbox and depth of field");
  Check(nr::PhotoGradeNeeded(nr::SanitizePhotoSettings(l)), "a look needs the grade pass");
  // Field of view: the game's until set, then the player's.
  Values f = v;
  Check(Near(FieldOfViewRadians(f, 1.2f), 1.2f), "field of view follows the game until set");
  f.fov = 90;
  Check(Near(FieldOfViewRadians(f, 1.2f), 3.14159265f / 2, 1e-4f), "field of view in degrees");
  // Picture names.
  std::tm t{};
  t.tm_year = 126, t.tm_mon = 9, t.tm_mday = 18, t.tm_hour = 3, t.tm_min = 4, t.tm_sec = 5;
  Check(PictureName(t, 42) == "kk-photo-20261018-030405-042.png", "picture name");

  // The camera: clamped to its radius, roll keeps the axes at right angles.
  using kk::freecam::Vec;
  const Vec centre{1, 2, 3};
  const Vec far = kk::freecam::ClampToRadius({1, 2, 103}, centre, 15);
  Check(Near(far.z, 18) && Near(far.x, 1), "camera clamped to the radius");
  const Vec inside = kk::freecam::ClampToRadius({2, 2, 3}, centre, 15);
  Check(Near(inside.x, 2), "camera inside the radius untouched");
  kk::freecam::Camera cam;
  cam.yaw = 0.7f, cam.pitch = 0.2f;
  for (float roll : {0.0f, 0.5f, -1.0f}) {
    const Vec r = kk::freecam::RollAxis(cam.Right(), cam.Up(), roll, true);
    const Vec u = kk::freecam::RollAxis(cam.Right(), cam.Up(), roll, false);
    const Vec fw = cam.Forward();
    Check(Near(r.x * u.x + r.y * u.y + r.z * u.z, 0, 1e-4f), "rolled right and up stay at right angles");
    Check(Near(r.x * fw.x + r.y * fw.y + r.z * fw.z, 0, 1e-4f), "rolled right stays across the forward axis");
    Check(Near(kk::freecam::Length(r), 1, 1e-4f), "rolled right stays unit length");
  }
  const Vec r0 = kk::freecam::RollAxis(cam.Right(), cam.Up(), 0, true);
  Check(Near(r0.x, cam.Right().x) && Near(r0.y, cam.Right().y), "no roll is the free camera's own axis");
  // The controller: triggers down / up, stick clicks faster / slower.
  uint8_t pad[16] = {};
  pad[7] = 255;  // right trigger
  pad[5] = 0x40;  // left stick click
  auto e = kk::freecam::PhotoPadFromState(pad);
  Check(Near(e.vertical, 1) && e.fast && !e.slow, "right trigger goes up, left stick click is faster");
  std::memset(pad, 0, sizeof(pad));
  pad[6] = 255;
  pad[5] = 0x80;
  pad[4] = 0x10;  // A: the panel's (take picture), never the camera's
  e = kk::freecam::PhotoPadFromState(pad);
  Check(Near(e.vertical, -1) && e.slow && !e.fast, "left trigger goes down, right stick click is slower");
  kk::freecam::PadInput in;
  in.vertical = 1;  // what ConsumePad makes of A
  kk::freecam::ApplyPhotoPad(e, in);
  Check(Near(in.vertical, -1), "A does not move the photo camera");

  // The renderer: only Jack's arms, by geometry and index count (never by shader alone).
  Check(nr::PhotoHidesMesh(0x7EB7E914947CEC40ull, 14310) && nr::PhotoHidesMesh(0xD69009DC0D6F9433ull, 2928),
        "Jack's arms and sleeves are left out");
  Check(!nr::PhotoHidesMesh(0x7EB7E914947CEC40ull, 2928), "the count must match too");
  // Ann's face, skin and clothing (task 072) and anything else are never left out.
  for (uint64_t ann : {0xBE2A0AED8846FA65ull, 0x4602AF0699809BC4ull, 0x5E97B95CF5FEFC4Eull, 0x40022003D57903FBull})
    for (uint32_t n : {7779u, 3024u, 2901u, 5298u, 14310u, 2928u}) Check(!nr::PhotoHidesMesh(ann, n), "Ann stays");
  Check(!nr::PhotoHidesMesh(0, 14310), "an unhashed draw is never left out");
  Check(nr::PhotoIsPauseCover(nr::kPauseCoverVs, nr::kPauseCoverPs, 4) &&
            !nr::PhotoIsPauseCover(nr::kPauseCoverVs, nr::kPauseCoverPs, 6) &&
            !nr::PhotoIsPauseCover(0x2E3B3ECDD3BC8D62ull, 0xB2328F814932E231ull, 4),
        "the pause cover: its vertex and pixel shader and one quad");

  // The grade's colour function.
  NrPhotoSettings g = nr::NeutralPhotoSettings();
  g.active = 1;
  for (float x : {0.0f, 0.2f, 0.5f, 0.9f, 1.0f}) {
    const auto o = nr::PhotoGradeColour(g, {x, x * 0.5f, 1 - x});
    Check(Near(o[0], x, 1e-3f) && Near(o[1], x * 0.5f, 1e-3f) && Near(o[2], 1 - x, 1e-3f), "neutral grade is identity");
  }
  g.look = 1;
  auto bw = nr::PhotoGradeColour(g, {0.8f, 0.3f, 0.1f});
  Check(Near(bw[0], bw[1], 1e-4f) && Near(bw[1], bw[2], 1e-4f), "black and white is grey");
  Check(Near(Luma(bw), Luma({0.8f, 0.3f, 0.1f}), 1e-3f), "black and white keeps the luminance");
  g.look = 2;
  auto sepia = nr::PhotoGradeColour(g, {0.5f, 0.5f, 0.5f});
  Check(sepia[0] > sepia[1] && sepia[1] > sepia[2], "sepia is warm brown");
  Check(Near(Luma(sepia), 0.5f, 2e-3f), "sepia keeps the luminance");
  g.look = 0;
  g.exposure = 1;
  Check(nr::PhotoGradeColour(g, {0.3f, 0.3f, 0.3f})[0] > 0.3f, "positive exposure brightens");
  g.exposure = 0;
  g.saturation = 0;
  auto grey = nr::PhotoGradeColour(g, {0.9f, 0.1f, 0.2f});
  Check(Near(grey[0], grey[2], 1e-4f), "saturation 0 is grey");
  g.saturation = 1;
  g.temperature = 1;
  auto warm = nr::PhotoGradeColour(g, {0.5f, 0.5f, 0.5f});
  Check(warm[0] > warm[2], "warm white balance");
  // Vignette and bars.
  Check(Near(nr::PhotoVignette(1, 0.5f, 0.5f, 16.f / 9), 1), "no vignette in the centre");
  Check(nr::PhotoVignette(1, 0.0f, 0.0f, 16.f / 9) < 0.3f, "dark corners");
  Check(nr::PhotoUnderBars(2.39f, 0.5f, 0.02f, 1920, 1080) && !nr::PhotoUnderBars(2.39f, 0.5f, 0.5f, 1920, 1080),
        "2.39 bars top and bottom on 16:9");
  Check(nr::PhotoUnderBars(1.0f, 0.05f, 0.5f, 1920, 1080) && !nr::PhotoUnderBars(1.0f, 0.5f, 0.5f, 1920, 1080),
        "1:1 bars at the sides");
  // Junk in, sane out.
  NrPhotoSettings junk{};
  junk.contrast = NAN;
  junk.look = 99;
  junk.dof = 7;
  junk.letterbox = 0.2f;
  const NrPhotoSettings clean = nr::SanitizePhotoSettings(junk);
  Check(Near(clean.contrast, 1) && clean.look == 0 && Near(clean.dof, 1) && clean.letterbox == 0, "sanitised");

  std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
