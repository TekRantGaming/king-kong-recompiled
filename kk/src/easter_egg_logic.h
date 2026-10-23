// Task 357 (owner, 22 Oct): a famous controller code on the launcher's Home screen starts the owner's little
// Easter egg game. This is the code itself, with no window or controller (kk-launcher-tests check it).
#pragma once

#include <cstddef>

namespace kk::egg {

// Buttons by their position (SDL gamepad positions): A bottom, B right, X left, Y top, whatever the controller
// prints on them (PlayStation Cross, Circle, Square, Triangle are A, B, X, Y).
enum class Pad { kUp, kDown, kLeft, kRight, kB, kA, kStart, kOther, kX, kY };

// Up, Up, Down, Down, Left, Right, Left, Right, B, A, Start.
inline constexpr Pad kCode[] = {Pad::kUp,   Pad::kUp,    Pad::kDown, Pad::kDown, Pad::kLeft, Pad::kRight,
                                Pad::kLeft, Pad::kRight, Pad::kB,    Pad::kA,    Pad::kStart};
inline constexpr int kCodeLength = int(sizeof(kCode) / sizeof(kCode[0]));

enum class Step {
  kNone,      // not part of the code here: the press does what it always does (a wrong press starts again)
  kMatched,   // the next press of the code
  kComplete,  // the last press: start the game
};

// One press against a code of `length` presses (at most 16) of which `progress` are matched: the new progress
// (0 after the last press) and in *step what the press was. A wrong press starts the code again; it may itself
// begin it again (a third Up after Up, Up keeps Up, Up).
inline int Advance(const Pad* code, int length, int progress, Pad p, Step* step) {
  if (p == code[progress]) {
    if (++progress == length) {
      *step = Step::kComplete;
      return 0;
    }
    *step = Step::kMatched;
    return progress;
  }
  *step = Step::kNone;
  // The longest start of the code that ends the presses so far (the matched part, then p).
  Pad seen[16];
  for (int i = 0; i < progress; ++i) seen[i] = code[i];
  seen[progress] = p;
  const int n = progress + 1;
  for (int k = n - 1; k > 0; --k) {
    bool same = true;
    for (int i = 0; i < k && same; ++i) same = seen[n - k + i] == code[i];
    if (same) return k;
  }
  return 0;
}

class Code {
 public:
  // One controller press (Advance).
  Step Press(Pad p) {
    Step step;
    progress_ = Advance(kCode, kCodeLength, progress_, p, &step);
    return step;
  }
  void Reset() { progress_ = 0; }
  int progress() const { return progress_; }

 private:
  int progress_ = 0;
};

// Task 364 (owner, 23 Oct): the original game's cheat menu codes start the Easter egg too (the egg only, never
// the old cheat menu), on the launcher's Home like the code above, from any controller. Each is entered while
// shoulder buttons are held down; letting go of them early starts that code again.
enum HoldBit : unsigned {
  kHoldLB = 1u << 0,
  kHoldRB = 1u << 1,
  kHoldLT = 1u << 2,  // a trigger counts once pressed past halfway (an analog axis or a digital trigger)
  kHoldRT = 1u << 3,
};

struct HeldCode {
  const char* name;  // for the log
  // Every group needs at least one of its buttons held (code 2: a left one and a right one, bumper or trigger).
  unsigned hold[4];
  int hold_groups;
  Pad presses[8];
  int length;
};

inline constexpr HeldCode kHeldCodes[] = {
    // PS2: hold L1 and R1, then Down, Circle, Up, Square, Down, Down, Up, Up.
    {"PS2",
     {kHoldLB, kHoldRB},
     2,
     {Pad::kDown, Pad::kB, Pad::kUp, Pad::kX, Pad::kDown, Pad::kDown, Pad::kUp, Pad::kUp},
     8},
    // Xbox and GameCube: hold L and R (the bumpers or the triggers), then Down, X, Up, Y, Down, Down, Up, Up.
    {"Xbox and GameCube",
     {kHoldLB | kHoldLT, kHoldRB | kHoldRT},
     2,
     {Pad::kDown, Pad::kX, Pad::kUp, Pad::kY, Pad::kDown, Pad::kDown, Pad::kUp, Pad::kUp},
     8},
    // Xbox 360: hold LB, RB, LT and RT, then Down, Up, Y, X, Down, Down, Y, Y.
    {"Xbox 360",
     {kHoldLB, kHoldRB, kHoldLT, kHoldRT},
     4,
     {Pad::kDown, Pad::kUp, Pad::kY, Pad::kX, Pad::kDown, Pad::kDown, Pad::kY, Pad::kY},
     8},
};
inline constexpr int kHeldCodeCount = int(sizeof(kHeldCodes) / sizeof(kHeldCodes[0]));

inline bool HoldMet(const HeldCode& c, unsigned held) {
  for (int g = 0; g < c.hold_groups; ++g)
    if (!(held & c.hold[g])) return false;
  return true;
}

// The three codes side by side (all held buttons down tracks all three at once).
class HeldCodes {
 public:
  // The hold buttons down now (HoldBit), each frame before its presses: a code whose hold is not down starts
  // again (letting go early resets it).
  void SetHeld(unsigned held) {
    held_ = held;
    for (int i = 0; i < kHeldCodeCount; ++i)
      if (!HoldMet(kHeldCodes[i], held)) progress_[i] = 0;
  }
  // Some code's hold is down: Home keeps the controller to the codes then (nothing else happens).
  bool Holding() const {
    for (const HeldCode& c : kHeldCodes)
      if (HoldMet(c, held_)) return true;
    return false;
  }
  // One press that is not a hold button (directions, A, B, X, Y, Start, Back as kOther). Only codes whose hold
  // is down take it. kComplete when it ends a code (completed() names it; every code starts again then).
  Step Press(Pad p) {
    Step result = Step::kNone;
    for (int i = 0; i < kHeldCodeCount; ++i) {
      const HeldCode& c = kHeldCodes[i];
      if (!HoldMet(c, held_)) continue;
      Step step;
      progress_[i] = Advance(c.presses, c.length, progress_[i], p, &step);
      if (step == Step::kComplete) {
        completed_ = i;
        Reset();
        return Step::kComplete;
      }
      if (step == Step::kMatched) result = Step::kMatched;
    }
    return result;
  }
  void Reset() {
    for (int& p : progress_) p = 0;
  }
  int progress(int code) const { return progress_[code]; }
  int completed() const { return completed_; }

 private:
  unsigned held_ = 0;
  int progress_[kHeldCodeCount] = {};
  int completed_ = -1;
};

// Whether the launcher leaves this press alone (B, A and Start while they are the code's next press, so B does
// not go back, A does not press a button and Start does not open Play). Directions always move as usual.
inline bool Swallowed(Pad p, Step step) {
  return step != Step::kNone && (p == Pad::kB || p == Pad::kA || p == Pad::kStart);
}

// Task 358 (owner, 23 Oct): the code from the keyboard too, mixed with the controller in one code (one Code
// takes both): the arrow keys are the directions, the B and A keys are B and A, Enter is Start; any other key
// is a wrong press.
enum class Key { kUp, kDown, kLeft, kRight, kB, kA, kEnter, kOther };

inline Pad FromKey(Key k) {
  switch (k) {
    case Key::kUp: return Pad::kUp;
    case Key::kDown: return Pad::kDown;
    case Key::kLeft: return Pad::kLeft;
    case Key::kRight: return Pad::kRight;
    case Key::kB: return Pad::kB;
    case Key::kA: return Pad::kA;
    case Key::kEnter: return Pad::kStart;
    default: return Pad::kOther;
  }
}

// Whether the launcher leaves this key alone: Enter (the launcher's Accept) while it is the code's next press,
// as Start is. The arrow keys always move as usual; the B and A keys stand for nothing in the launcher.
inline bool KeySwallowed(Key k, Step step) { return step != Step::kNone && k == Key::kEnter; }

}  // namespace kk::egg
