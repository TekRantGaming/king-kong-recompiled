#pragma once
#include <cstdint>
#include <rex/hook.h>
#include "coop_protocol.h"
#include "coop_desync.h"
#include <optional>

namespace kk::coop {
bool EnemySpawnerStep(PPCContext& ctx,uint8_t* base);
bool EnemyHoldDestroy(PPCContext& ctx,uint8_t* base);
void EnemyDestroying(const PPCContext& ctx,uint8_t* base);
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
bool PartnerBodyGrabRefusal(const uint8_t* base, uint32_t victim,uint32_t kind);
bool EnemyPartnerCombatReady();
std::optional<uint16_t> EnemyPartnerCapabilities();
// Conservative checkpoint-offer guard: any living scheduled shared enemy
// within the radius counts, regardless of which peer owns its AI.
bool EnemyNearPlayer(const uint8_t* base,const PlayerPose& player,float radius);
std::vector<WorldEnemy> EnemyWorldDigest(const uint8_t* base);
EnemyWorld EnemyCurrentWorld();
// Game thread, fresh generation-scoped host digest only. Empty cancels it.
void EnemyGuardRecoveryWorld(EnemyWorld world);
desync::Evidence EnemyCompareDigest(uint8_t* base,const desync::State& state,bool reconcile);
#if defined(KK_DEV_TOOLS)
bool EnemyDevWorldDrift(uint8_t* base);
bool EnemyDevWorldGenerationDrift();
void EnemyDevGameFrame(PPCContext& ctx, uint8_t* base);
std::optional<ShotVector> EnemyAimTarget(const uint8_t* base, ShotVector origin);
// Solo dev placement: initialized, living, unscheduled allowlisted actor only.
std::optional<ShotVector> EnemyDormantTarget(const uint8_t* base, uint32_t world, ShotVector origin);
#endif
} // namespace kk::coop
