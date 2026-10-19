#pragma once

#if defined(KK_DEV_TOOLS)
#include <cstdint>
#include <rex/hook.h>

namespace kk {
// All calls run on the guest update thread. Shared AI hooks live in coop_enemies_game.cpp.
void DevPuppetGameFrame(uint8_t* base);
void DevPuppetBeforeLoad();
void DevPuppetAfterLoad(const uint8_t* base, uint32_t game);
void DevPuppetBeforeAi(PPCContext& ctx, uint8_t* base, uint32_t world);
void DevPuppetAfterAi(PPCContext& ctx, uint8_t* base, uint32_t world);
bool PuppetHold(PPCContext& ctx, uint8_t* base);
void DevPuppetLateWorld(PPCContext& ctx, uint8_t* base, uint32_t world);
}  // namespace kk
#endif
