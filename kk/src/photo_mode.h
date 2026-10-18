// Photo mode (task 208, owner decision 18 Oct): opened from the pause menu. The game stays paused
// its own way while the 3D world keeps drawing; a free camera (freecam.cpp) flies around Jack within
// a radius, Jack's first-person arms and weapon and the HUD are hidden, a photo grade (renderer) and
// the existing picture settings apply, and A saves a full-resolution picture without the UI.
#pragma once

#include <cstdint>

namespace kk::photo {

// Once a frame on the game thread (sub_8272CA20's hook, hfr_frame_time.cpp).
void Frame(uint8_t* base);

}  // namespace kk::photo
