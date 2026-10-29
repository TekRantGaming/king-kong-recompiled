#pragma once
#include "coop_desync.h"
namespace kk::coop {
void WorldGuardReceive(const Message& message);
void WorldGuardFrame(uint8_t* base);
desync::Tier WorldGuardPending();
// RO1: count a withheld automatic repair; true when it should be logged (once a minute per kind).
bool WorldGuardWithhold(desync::Withheld::Kind kind);
uint64_t WorldGuardWithheld(desync::Withheld::Kind kind);
void WorldGuardStarted(desync::Tier tier);
void WorldGuardCompleted(desync::Tier tier);
void WorldGuardCancelled(desync::Tier tier);
void WorldGuardLoaded();
void WorldGuardSummary();
}
