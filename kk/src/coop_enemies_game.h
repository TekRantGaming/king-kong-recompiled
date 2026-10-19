#pragma once
#include <cstdint>
#include <rex/hook.h>
#include "coop_protocol.h"
#include <optional>

namespace kk::coop {
std::optional<PlayerCondition> LocalPlayerCondition(const uint8_t* base);
bool ReviveTeleport(PPCContext& ctx, uint8_t* base, const PlayerPose& partner);
void EnemyBeforeLoad();
void EnemyAfterLoad(const uint8_t* base, uint32_t game);
void EnemyGameFrame();
// Called by the single 8271E4E8 hook, before C25's general reference observer.
// Returns true when the event was handled without native body/local damage.
bool EnemyDamageEnqueue(PPCContext& ctx, uint8_t* base, uint32_t body);
bool EnemyCombatReference(const uint8_t* base, uint32_t owner, uint32_t ai,
                          uint32_t vars, uint32_t offset);
bool PartnerBodyGrabRefusal(const uint8_t* base, uint32_t victim);
bool EnemyPartnerCombatReady();
#if defined(KK_DEV_TOOLS)
std::optional<ShotVector> EnemyAimTarget(const uint8_t* base, ShotVector origin);
// Solo dev placement: initialized, living, unscheduled allowlisted actor only.
std::optional<ShotVector> EnemyDormantTarget(const uint8_t* base, uint32_t world, ShotVector origin);
#endif
} // namespace kk::coop
