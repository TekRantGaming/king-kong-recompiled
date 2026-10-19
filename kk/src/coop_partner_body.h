#pragma once
#include <cstdint>
#include <rex/hook.h>

namespace kk {
// Game-thread body service, included in shipping builds. Dev selectors use the
// same census, lifetime checks and driver; spawn/probes remain KK_DEV_TOOLS only.
void PartnerBodyGameFrame(uint8_t* base);
void PartnerBodyBeforeLoad();
void PartnerBodyAfterLoad(const uint8_t* base, uint32_t game);
void PartnerBodyBeforeAi(PPCContext& ctx, uint8_t* base, uint32_t world);
void PartnerBodyAfterAi(PPCContext& ctx, uint8_t* base, uint32_t world);
bool PartnerBodyHold(PPCContext& ctx, uint8_t* base);
void PartnerBodyLateWorld(PPCContext& ctx, uint8_t* base, uint32_t world);
// Before a trigger callback runs, release an auto body mentioned by its payload.
void PartnerBodyTriggerReference(const uint8_t* base, uint32_t record);
}  // namespace kk
