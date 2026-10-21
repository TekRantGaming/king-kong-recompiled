#pragma once
#include <cstdint>
#include "coop_desync.h"
struct PPCContext;
namespace kk::coop {
void StoryGameFrame(uint8_t* base);
void StoryLoaded(uint8_t* base);
bool StoryBehind();
bool StoryNeedsMove();
// Game-thread observations: fresh lag at any stage, or persistent lag after
// a completed soft move. Neither observation submits a shared reload.
bool StoryAnyBehind();
bool StoryCatchupReady();
void StoryHostCheckpoint();
// Game thread only, after a successful native soft catch-up teleport.
void StoryPartnerMoved();
std::vector<WorldScript> StoryWorldDigest(const uint8_t* base);
desync::Evidence StoryCompareDigest(const uint8_t* base,const desync::State& state,bool reconcile);
// The existing partner-body PRG hook remains the sole owner of this entry point.
bool StoryLoop(PPCContext& ctx, uint8_t* base);
}
