#pragma once

#include <functional>

#include <rex/ui/overlay/debug_overlay.h>

namespace kk {

// Measured guest (game) frame rate, updated once per second.
rex::ui::FrameStats GetGuestFrameStats();

// Runs `fn` once, on the game thread, `seconds` after the first guest frame
// (several may be scheduled).
void RunAfterFirstFrame(double seconds, std::function<void()> fn);

// Runs `fn` once, on the game thread, `seconds` from now.
void RunAfterDelay(double seconds, std::function<void()> fn);

}  // namespace kk
