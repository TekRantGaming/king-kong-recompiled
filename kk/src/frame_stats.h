#pragma once

#include <cstddef>
#include <functional>

#include <rex/ui/overlay/debug_overlay.h>

namespace kk {

// Measured guest (game) frame rate, updated once per second.
rex::ui::FrameStats GetGuestFrameStats();
// The longest guest frame of the last one-second window (ms; the graphics panel's readout).
double GuestWorstFrameMs();
// The last guest frame times in ms, oldest first (up to 256; the Performance metrics graph). Returns how many.
size_t GuestFrameTimes(float* out, size_t max);

// Runs `fn` once, on the game thread, `seconds` after the first guest frame
// (several may be scheduled).
void RunAfterFirstFrame(double seconds, std::function<void()> fn);

// Runs `fn` once, on the game thread, `seconds` from now.
void RunAfterDelay(double seconds, std::function<void()> fn);

}  // namespace kk
