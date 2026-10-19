#pragma once
#include <cstdint>
struct PPCContext;
namespace kk::coop {
void StoryGameFrame(uint8_t* base);
void StoryLoaded(uint8_t* base);
bool StoryBehind();
bool StoryNeedsMove();
// Game thread only, after a successful native soft catch-up teleport.
void StoryPartnerMoved();
// The existing partner-body PRG hook remains the sole owner of this entry point.
bool StoryLoop(PPCContext& ctx, uint8_t* base);
}
