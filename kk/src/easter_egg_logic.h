// Task 357 (owner, 22 Oct): a famous controller code on the launcher's Home screen starts the owner's little
// Easter egg game. This is the code itself, with no window or controller (kk-launcher-tests check it).
#pragma once

#include <cstddef>

namespace kk::egg {

enum class Pad { kUp, kDown, kLeft, kRight, kB, kA, kStart, kOther };

// Up, Up, Down, Down, Left, Right, Left, Right, B, A, Start.
inline constexpr Pad kCode[] = {Pad::kUp,   Pad::kUp,    Pad::kDown, Pad::kDown, Pad::kLeft, Pad::kRight,
                                Pad::kLeft, Pad::kRight, Pad::kB,    Pad::kA,    Pad::kStart};
inline constexpr int kCodeLength = int(sizeof(kCode) / sizeof(kCode[0]));

enum class Step {
  kNone,      // not part of the code here: the press does what it always does (a wrong press starts again)
  kMatched,   // the next press of the code
  kComplete,  // the last press: start the game
};

class Code {
 public:
  // One controller press. A wrong press starts the code again; it may itself begin it again (a third Up
  // after Up, Up keeps Up, Up).
  Step Press(Pad p) {
    if (p == kCode[progress_]) {
      if (++progress_ == kCodeLength) {
        progress_ = 0;
        return Step::kComplete;
      }
      return Step::kMatched;
    }
    // The longest start of the code that ends the presses so far (the matched part, then p).
    Pad seen[kCodeLength];
    for (int i = 0; i < progress_; ++i) seen[i] = kCode[i];
    seen[progress_] = p;
    const int n = progress_ + 1;
    int keep = 0;
    for (int k = n - 1; k > 0 && keep == 0; --k) {
      bool same = true;
      for (int i = 0; i < k && same; ++i) same = seen[n - k + i] == kCode[i];
      if (same) keep = k;
    }
    progress_ = keep;
    return Step::kNone;
  }
  void Reset() { progress_ = 0; }
  int progress() const { return progress_; }

 private:
  int progress_ = 0;
};

// Whether the launcher leaves this press alone (B, A and Start while they are the code's next press, so B does
// not go back, A does not press a button and Start does not open Play). Directions always move as usual.
inline bool Swallowed(Pad p, Step step) {
  return step != Step::kNone && (p == Pad::kB || p == Pad::kA || p == Pad::kStart);
}

}  // namespace kk::egg
