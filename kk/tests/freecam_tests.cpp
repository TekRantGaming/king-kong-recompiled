#include "freecam_math.h"
#include <cstdio>
#include <cstdlib>
using namespace kk::freecam;
void Check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what);
    std::exit(1);
  }
}
bool Near(float a, float b) { return std::abs(a - b) < .002f; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
int main() {
  // The guest-to-render conversion uses -row0 for screen X and -row1
  // for positive depth. At yaw/pitch zero, W must be world -X and D +Y.
  const Vec expected[] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
                          {0, 1, 0}, {0, 0, 1}, {0, 0, -1}};
  const float commands[][3] = {{1, 0, 0}, {-1, 0, 0}, {0, -1, 0},
                               {0, 1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (int i = 0; i < 6; ++i) {
    Camera c;
    c.Move(commands[i][0], commands[i][1], commands[i][2], false, false, .1f);
    Check(Length(c.position + expected[i] * -1) < .002f,
          "W/S/A/D/up/down follow rendered camera axes");
    uint8_t state[16] = {0x12, 0x34, 0x56, 0x78};
    auto stick = [&](int offset, int16_t v) {
      state[offset] = uint16_t(v) >> 8;
      state[offset + 1] = uint8_t(v);
    };
    if (i < 2) stick(10, i == 0 ? 32767 : -32768);
    else if (i < 4) stick(8, i == 2 ? -32768 : 32767);
    else state[4] = i == 4 ? 0x10 : 0x20;
    const auto p = ConsumePad(state, .15f, false, false);
    Camera pad;
    pad.Move(p.forward, p.side, p.vertical, p.fast, p.slow, .1f);
    Check(Length(pad.position + expected[i] * -1) < .002f,
          "physical left stick and A/B match keyboard directions");
    for (int j = 4; j < 16; ++j)
      Check(state[j] == 0, "pad input never reaches the player");
    Check(state[0] == 0x12 && state[3] == 0x78, "packet number preserved");
  }
  for (bool invert_x : {false, true})
    for (bool invert_y : {false, true}) {
      uint8_t state[16]{};
      state[12] = state[14] = 0x7f;
      state[13] = state[15] = 0xff;
      state[6] = state[7] = 255;
      state[4] = state[5] = 0xff;
      const auto p = ConsumePad(state, .15f, invert_x, invert_y);
      Check((p.look_x < 0) == invert_x && (p.look_y < 0) == invert_y,
            "right stick respects both invert settings");
      Camera look;
      look.Look(p.look_x * .1f, p.look_y * .1f);
      Check((-look.Forward().z > 0) != invert_y,
            "normal stick up raises visible forward; inverted lowers it");
      Check((-look.Forward().y > 0) != invert_x,
            "normal stick right turns visible forward toward screen right");
      Check(p.fast && p.slow && p.vertical == 0, "triggers and opposing A/B");
      for (int j = 4; j < 16; ++j)
        Check(state[j] == 0, "all buttons/triggers/sticks suppressed");
    }
  // Beta 12: the mouse turns the free camera (F9, photo mode) the same way as in play: up looks up, right turns
  // right; kk_mouse_invert_y turns only the vertical round.
  for (bool invert_y : {false, true}) {
    Camera up, right;
    MouseLook(up, 0, -40, .01f, invert_y);  // window y grows downwards: the mouse moved up
    Check((-up.Forward().z > 0) != invert_y, "mouse up raises visible forward; inverted lowers it");
    MouseLook(right, 40, 0, .01f, invert_y);
    Check(-right.Forward().y > 0, "mouse right turns visible forward toward screen right (either invert)");
  }
  uint8_t drift[16]{};
  drift[8] = drift[12] = 1;
  const auto quiet = ConsumePad(drift, .15f, false, false);
  Check(quiet.side == 0 && quiet.look_x == 0, "radial deadzone prevents drift");
  for (float yaw : {-3.f, 0.f, 1.5707963f, 3.f})
    for (float pitch : {-1.5f, 0.f, 1.5f}) {
      Camera c;
      c.yaw = yaw;
      c.pitch = pitch;
      Check(Near(Length(c.Forward()), 1) && Near(Length(c.Right()), 1) &&
                Near(Length(c.Up()), 1),
            "unit camera axes");
      Check(Near(Dot(c.Forward(), c.Right()), 0) &&
                Near(Dot(c.Forward(), c.Up()), 0) &&
                Near(Dot(c.Right(), c.Up()), 0),
            "orthogonal camera axes");
      const Vec start = c.position;
      c.Move(1, 0, 0, false, false, .1f);
      Check(Near(Dot(c.position + start * -1, c.Forward()), -1),
            "W advances positive rendered depth at every yaw and pitch");
      c.position = {};
      c.Move(0, 1, 0, false, false, .1f);
      Check(Near(Dot(c.position, c.Right()), -1),
            "D advances positive rendered screen X at every yaw and pitch");
    }
  for (int fps : {30, 60, 120, 144, 240, 1000}) {
    Camera c;
    c.yaw = 1.5707963f;
    for (int i = 0; i < fps; ++i)
      c.Move(1, 0, 0, false, false, 1.f / fps);
    Check(Near(c.position.y, -10), "real-time movement across frame rates");
  }
  Camera diagonal;
  diagonal.Move(1, 1, 1, false, false, .1f);
  Check(Near(Length(diagonal.position), 1), "diagonal speed normalized");
  Camera fast;
  fast.Move(1, 0, 0, true, false, .1f);
  Check(Near(fast.position.x, -4), "shift speed");
  Camera slow;
  slow.Move(0, 0, -1, false, true, .1f);
  Check(Near(slow.position.z, -.25f), "alt and vertical movement");
  Camera c;
  c.Look(10000, -10000);
  Check(std::isfinite(c.yaw) && c.pitch < 1.571f,
        "mouse rotation stays finite and below pole");
  c.Wheel(10000);
  c.Wheel(10000);
  Check(c.speed <= 1000, "upper speed bound");
  for (int i = 0; i < 10; ++i)
    c.Wheel(-10000);
  Check(c.speed >= .1f, "lower speed bound");
  c.position = {};
  c.Move(1, 0, 0, false, false, -1);
  Check(Length(c.position) == 0, "negative elapsed time rejected");
  std::puts("Free-camera movement and basis checks passed");
}
