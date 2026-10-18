// When the port may apply a loaded save's original video modes (horizontal flip G+0x4DA0) with the game's own
// sub_825EE130, and the game's "Old movie" filter (G+0x4DA4), which is never applied.
#pragma once
#include <cstdint>

namespace kk::play {
// sub_825EE130 reads the modes through the profile pointer P (0x82CC8D24) and writes
// the flip to the object at 0x82CCAC30. Neither is set yet when a launcher start loads
// its save on the title screen (the original menus that would call it never run), so
// calling it there reads guest address 0x4DA4 and crashes. The load only marks the modes
// pending; they are applied once both are set. `profile` is P, or the running universe block
// (the same structure) when P stays unset, as in the Credits level.
//
// Task 213 (owner, 18 Oct: Beta 9 loaded with an amber tint that photo mode could not clear): the game's Old
// movie filter (1 sepia, 2 another filter of sub_825EE130's) never applies in normal play, whatever a save
// holds; filters belong to photo mode only. Whenever the profile holds a nonzero Old movie value it is
// written back to 0 and sub_825EE130 runs again (a save just loaded, a level change, a chapter restart).
enum class VideoModeStep { kNothing, kWait, kApply };
inline VideoModeStep NextVideoModeStep(bool pending, uint32_t profile, uint32_t flip_target, uint32_t flip,
                                       uint32_t old_movie) {
  if (!pending && !old_movie) return VideoModeStep::kNothing;
  if (!profile || !flip_target) return VideoModeStep::kWait;
  // Both off is the game's normal state: nothing to apply (the old direct call had the same guard).
  return flip || old_movie ? VideoModeStep::kApply : VideoModeStep::kNothing;
}
}  // namespace kk::play
