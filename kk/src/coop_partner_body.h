#pragma once
#include <cstdint>
#include <functional>
#include <rex/hook.h>
#include "coop_protocol.h"

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
// Only an independently allocated native copy, never an authored donor.
uint32_t PartnerBodyCombatCopy(const uint8_t* base);
std::vector<coop::WorldProbe> PartnerNpcDigest(const uint8_t* base);
// Proxy animation callbacks must not fire another damaging native shot.
bool PartnerBodySuppressShot(const uint8_t* base);
void PartnerBodyObserveAction(const PPCContext& ctx, const uint8_t* base);
void PartnerBodyPoseBlended(PPCContext& ctx, uint8_t* base, uint32_t actor);
#if defined(KK_DEV_TOOLS)
// Dev probe only: every observed level human and where it stands. Read only.
void DevHumanPositions(const uint8_t* base, const std::function<void(uint32_t, float, float, float)>& f);
#endif
}  // namespace kk
