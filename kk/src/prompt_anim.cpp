// Keyboard prompts for the game's animated stick pictures (task 328, Beta 12).
//
// The game's texts draw a moving stick as an animated picture: "\P16\\b1315\F\P0\ to aim" (the right stick) and
// "\P16\\b12131415\A\P0\ to move" (the left stick). sub_827BB110 reads one "\x...\" code of a text into the
// structure at 0x82977B64 (+0 the kind, +4 its number); "\b<digits>\" is kind 13 and its digits are the frames
// the picture after it steps through, read when the text is set up. On the game's button sheet frame 1 is the
// stick at rest (the cell the stick clicks also use), 2-5 the pushed stick (one cell, turned), 7 the L and 9 the
// R letter (seen in game, task 328).
//
// With the keyboard prompts (glyphs.cpp) the stick cells hold keys, so the animation cycled through the crouch
// key and a turned W. Here the animations become one still frame: aiming shows frame 9, where the keyboard sheet
// has the mouse in place of the R; every other stick animation (moving) shows frame 3, where it has W A S D. The texts themselves
// are not changed, and with a controller nothing is.

#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>

#include "glyphs.h"
#include "hfr.h"

REX_EXTERN(__imp__sub_827BB110);  // reads one "\x...\" code of a text

namespace kk {
namespace {

constexpr uint32_t kCode = 0x82977B64;  // +0 kind, +4 number
constexpr uint32_t kFrames = 13;        // "\b<digits>\"
constexpr uint32_t kAimFrames = 1315;   // "Right stick to aim": at rest, pushed up, at rest, pushed down
constexpr uint32_t kMouseFrame = 9;     // the R: glyphs.cpp's mouse_move cell
constexpr uint32_t kKeysFrame = 3;      // the pushed stick upright: glyphs.cpp's stick_move cell (W A S D)

// Whether the frames show the pushed stick (digits 2-5).
bool MovesStick(uint32_t frames) {
  for (; frames; frames /= 10)
    if (const uint32_t d = frames % 10; d >= 2 && d <= 5) return true;
  return false;
}

}  // namespace

// The still frame the keyboard prompts show for these frames, or 0 to keep them.
uint32_t KeyboardStickFrame(uint32_t frames) {
  if (frames == kAimFrames) return kMouseFrame;
  return MovesStick(frames) ? kKeysFrame : 0;
}

}  // namespace kk

REX_HOOK_RAW(sub_827BB110) {
  using namespace kk::hfr;
  __imp__sub_827BB110(ctx, base);
  if (ctx.r3.u32 == 0 || Load32(base, kk::kCode) != kk::kFrames) return;
  const uint32_t frames = Load32(base, kk::kCode + 4);
#if defined(KK_DEV_TOOLS)
  // KK_DEV_ANIM_FRAMES=<digits>: the aim picture's frames instead (to see what each frame draws).
  if (const char* dev = std::getenv("KK_DEV_ANIM_FRAMES"); dev && frames == kk::kAimFrames) {
    Store32(base, kk::kCode + 4, uint32_t(std::strtoul(dev, nullptr, 10)));
    return;
  }
#endif
  if (!kk::KeyboardPromptsShown()) return;
  if (const uint32_t still = kk::KeyboardStickFrame(frames)) {
    Store32(base, kk::kCode + 4, still);
    static int logged = 0;  // texts are read once when shown; a few lines at most
    if (logged < 20) {
      ++logged;
      REXLOG_INFO("KK: keyboard prompts: stick picture frames {} shown as {}", frames, still);
    }
  }
}
