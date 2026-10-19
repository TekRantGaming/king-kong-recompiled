#pragma once
#include <cstdint>
#include <rex/hook.h>
#include "coop_protocol.h"
#include <optional>

namespace kk::coop {
void EnemyBeforeLoad();
void EnemyAfterLoad(const uint8_t* base, uint32_t game);
void EnemyGameFrame();
#if defined(KK_DEV_TOOLS)
std::optional<ShotVector> EnemyAimTarget(const uint8_t* base, ShotVector origin);
#endif
} // namespace kk::coop
