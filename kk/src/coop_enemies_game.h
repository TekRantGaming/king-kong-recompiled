#pragma once
#include <cstdint>
#include <rex/hook.h>

namespace kk::coop {
void EnemyBeforeLoad();
void EnemyAfterLoad(const uint8_t* base, uint32_t game);
void EnemyGameFrame();
} // namespace kk::coop
