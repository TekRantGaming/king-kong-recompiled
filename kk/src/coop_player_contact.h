#pragma once
#include "coop_mut.h"
#include <cstdint>

namespace kk::coop::contact {
// The peer's driven copy is a player, not a moving obstacle for local Jack.
// Keep every other pair, including both players against the terrain/enemies.
inline bool PeerPair(uint32_t a,uint32_t b,uint32_t local,uint32_t peer) {
  if (!a || !b || !local || !peer || local==peer) return false;
  if (COOP_MUT_IF("partner_contact_filter")) return false;
  if (COOP_MUT_IF("partner_contact_world")) return a==local || b==local;
  return (a==local && b==peer) || (a==peer && b==local);
}
}
