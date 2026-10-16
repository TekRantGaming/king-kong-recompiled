// When the port may apply a loaded save's original video modes ("Old movie" filter
// G+0x4DA4, horizontal flip G+0x4DA0) with the game's own sub_825EE130.
#pragma once
#include <cstdint>

namespace kk::play {
// sub_825EE130 reads the modes through the profile pointer P (0x82CC8D24) and writes
// the flip to the object at 0x82CCAC30. Neither is set yet when a launcher start loads
// its save on the title screen (the original menus that would call it never run), so
// calling it there reads guest address 0x4DA4 and crashes. The load only marks the modes
// pending; they are applied once both pointers are set.
enum class VideoModeStep { kNothing, kWait, kApply };
inline VideoModeStep NextVideoModeStep(bool pending, uint32_t profile, uint32_t flip_target, uint32_t flip,
                                       uint32_t old_movie) {
  if (!pending) return VideoModeStep::kNothing;
  if (!profile || !flip_target) return VideoModeStep::kWait;
  // Both off is the game's normal state: nothing to apply (the old direct call had the same guard).
  return flip || old_movie ? VideoModeStep::kApply : VideoModeStep::kNothing;
}
}  // namespace kk::play
