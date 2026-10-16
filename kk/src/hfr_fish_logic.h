// High frame rate fix "fish" (hfr_fish.cpp): the arithmetic, kept apart so a
// test can check it without the game.

#pragma once

#include <cmath>

namespace kk::hfr {

// The game's frame time is clamped to 0.001 .. 0.063 s (sub_8272CA20); outside
// that (a paused or broken clock) the game's own angle is kept.
inline bool FishWiggleApplies(float dt) { return std::isfinite(dt) && dt >= 0.001f && dt <= 0.063f; }

// PNJ_EatMe_ETAT_Water adds its wiggle angle once a call; the console makes a
// call every 1/30 s. This is the angle for a call dt seconds after the last.
inline float FishWiggle(float angle, float dt) { return angle * (30.0f * dt); }

}  // namespace kk::hfr
