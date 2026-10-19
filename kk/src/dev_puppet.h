#pragma once

#if defined(KK_DEV_TOOLS)
#include <cstdint>
#include <rex/hook.h>

namespace kk {
// All calls run on the guest update thread. The existing hooks retain ownership.
void DevPuppetGameFrame(uint8_t* base);
void DevPuppetBeforeLoad();
void DevPuppetAfterLoad(const uint8_t* base, uint32_t game);
void DevPuppetBeforeAi(PPCContext& ctx, uint8_t* base, uint32_t world);
void DevPuppetAfterAi(PPCContext& ctx, uint8_t* base, uint32_t world);
void DevPuppetLateWorld(PPCContext& ctx, uint8_t* base, uint32_t world);
}  // namespace kk
#endif
