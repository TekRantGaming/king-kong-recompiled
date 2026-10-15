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
    }
  for (int fps : {30, 60, 120, 144, 240, 1000}) {
    Camera c;
    c.yaw = 1.5707963f;
    for (int i = 0; i < fps; ++i)
      c.Move(1, 0, 0, false, false, 1.f / fps);
    Check(Near(c.position.y, 10), "real-time movement across frame rates");
  }
  Camera diagonal;
  diagonal.Move(1, 1, 1, false, false, .1f);
  Check(Near(Length(diagonal.position), 1), "diagonal speed normalized");
  Camera fast;
  fast.Move(1, 0, 0, true, false, .1f);
  Check(Near(fast.position.x, 4), "shift speed");
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
