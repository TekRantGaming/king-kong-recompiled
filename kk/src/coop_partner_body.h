#pragma once
#include <cstdint>
#include <rex/hook.h>

namespace kk {
// Game-thread body service, included in shipping builds. Dev selectors use the
// same census, lifetime checks and driver. Auto can use a native rigid copy;
// explicit spawn selectors and rendering probes remain KK_DEV_TOOLS only.
void PartnerBodyGameFrame(uint8_t* base);
void PartnerBodyBeforeLoad();
void PartnerBodyAfterLoad(const uint8_t* base, uint32_t game);
void PartnerBodyBeforeAi(PPCContext& ctx, uint8_t* base, uint32_t world);
void PartnerBodyAfterAi(PPCContext& ctx, uint8_t* base, uint32_t world);
bool PartnerBodyHold(PPCContext& ctx, uint8_t* base);
void PartnerBodyLateWorld(PPCContext& ctx, uint8_t* base, uint32_t world);
// Before a trigger callback runs, release an auto body mentioned by its payload.
void PartnerBodyTriggerReference(const uint8_t* base, uint32_t record);
// Current validated borrowed or native-copy partner. Zero after release/load/loss. Used by
// the one stimulus hook to protect its authored state and route native bites.
uint32_t PartnerBodyCombatActor(const uint8_t* base);
// Proxy animation callbacks must not fire another damaging native shot.
bool PartnerBodySuppressShot(const uint8_t* base);
}  // namespace kk
