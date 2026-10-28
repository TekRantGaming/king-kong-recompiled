#pragma once
#include <cstdint>
#include "coop_mut.h"

// C119: host access violation right after "PUPPET spawn allocated" (c118-beach-fix6-20261028-142100, 14:22:28,
// level 101 start 4 after the second host private reload, copy of armed crew 41D649B0).
//   * SpawnCopy checks that the new copy shares no component with any actor of the active AI table, by reading
//     every one of them with ReadRig. Most of those actors are not humans (crabs, spawners, props).
//   * ReadRig follows the human rig chain actor+20 -> +0 (animation) -> +132 (layer) -> +4. For the non human
//     actor 43AF3604 that chain ran into a live crab block (43B02C00, 0x68 before crab 43B02C68) whose word at
//     +132 is 9A0000E0: a 4 byte aligned value inside the old 40000000..A0000000 test, but no guest page is
//     committed there, so the next read faulted (minidump: rdx 43B02C00, r9 9A0000E0, read of 9A0000E4).
//   * The value depends on the state of whichever crab happens to sit there, so it is rare and only shows on
//     levels with crabs; each host private reload makes a new copy and runs the check again.
// Fix: the alias checks read other actors under a probe scope in which every guarded span must also lie in
// committed, readable guest pages (the runtime's own page table). An actor whose chain leaves mapped memory is
// simply not a human rig: ReadRig fails for it, exactly as for any other non human chain.
namespace kk::coop::guest_probe {

struct Region {
  uint32_t base = 0, size = 0;  // pages with the same state, from base
  bool readable = false;        // committed and readable
};

// Query(address, Region&) fills the region holding address and returns false when no heap holds it.
// The whole span [p, p + bytes) must be readable; it may cross several regions.
template<class Query>
bool Readable(uint32_t p, uint32_t bytes, Query query) {
  if (COOP_MUT_IF("probe_off")) return true;
  uint64_t at = p;
  const uint64_t end = uint64_t(p) + (bytes ? bytes : 1);
  while (at < end) {
    Region r;
    if (at > 0xFFFFFFFFull || !query(uint32_t(at), r) || !r.readable || !r.size) return false;
    const uint64_t region_end = uint64_t(r.base) + r.size;
    if (r.base > at || region_end <= at) return false;  // a malformed answer is never proof
    if (COOP_MUT_IF("probe_first_region")) return true;
    at = region_end;
  }
  return true;
}

}  // namespace kk::coop::guest_probe
