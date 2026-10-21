#pragma once
#include "coop_desync.h"
namespace kk::coop {
void WorldGuardReceive(const Message& message);
void WorldGuardFrame(uint8_t* base);
desync::Tier WorldGuardPending();
void WorldGuardStarted(desync::Tier tier);
void WorldGuardCompleted(desync::Tier tier);
void WorldGuardCancelled(desync::Tier tier);
void WorldGuardLoaded();
void WorldGuardSummary();
}
