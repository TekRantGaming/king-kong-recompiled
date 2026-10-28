#pragma once
#include <cstdint>
#include "coop_mut.h"
#include "coop_stimulus_debug.h"
#include <cstdlib>

// C101: events that co-op code adds to the native event pool must follow the
// native pool's own cadence. Root cause of the 26 Oct 21:03 host crash:
//   * 827098B8 (world pause, used by the game's pause page) sets 0x82CCAAA0
//     and freezes the actors; 8271E600 then stops ageing every event.
//   * The partner body's perception publisher (8242AA88, run by co-op code
//     after the AI pass) still made one type 1 event per frame, so about 450
//     events of 0.001 s stayed alive during a 3.8 s pause.
//   * After Resume, a bat's BC_exec_check_vision (8262FE20) appended every
//     visible type 1 event to its list at vars+2492 (count +2484) with no
//     bound. Entry 74 reaches vars+3084 and entry 77 writes handle 0100005C
//     over the player pointer at vars+3108, which 824E8348 then passed to
//     group_instance_find (8286A660): access violation at guest 0100005C.
namespace kk::coop::event_budget {

constexpr uint32_t kPoolPauseWord = 0x82CCAAA0;  // nonzero: 8271E600 ages nothing
constexpr uint32_t kVisionListFirst = 2492, kVisionListCount = 2484, kVisionStateWord = 3084;
// Entries the bat vision list holds before it overwrites its own state words.
constexpr uint32_t kVisionListRoom = (kVisionStateWord - kVisionListFirst) / 8;
static_assert(kVisionListRoom == 74);
// Natively one H publisher makes one 0.001 s event per dispatch, so at most
// two of its events are alive (the new one, flag 1, and last frame's).
constexpr uint32_t kOwnerLiveLimit = 2;

struct PublishInput {
  bool pool_paused = false;  // kPoolPauseWord is set
  bool dispatched = false;   // the native scheduler offered the body's AI this frame
  uint32_t owner_live = 0;   // live events of this owner already in the bucket
};
enum class Decision : uint8_t { Publish, PoolPaused, NotDispatched, OwnerFull };
inline const char* DecisionName(Decision d) {
  switch (d) {
    case Decision::Publish: return "publish";
    case Decision::PoolPaused: return "pool-paused";
    case Decision::NotDispatched: return "not-dispatched";
    case Decision::OwnerFull: return "owner-full";
  }
  return "?";
}
// A publisher that stands in for a frozen native one: publish only when the
// native one would run (actor dispatched), only while the pool ages its
// events, and never beyond the native number of live events.
inline Decision DecidePublish(const PublishInput& in) {
  if (in.pool_paused && !COOP_MUT_IF("event_pause_gate")) return Decision::PoolPaused;
  if (!in.dispatched && !COOP_MUT_IF("event_dispatch_gate")) return Decision::NotDispatched;
  if (in.owner_live >= kOwnerLiveLimit && !COOP_MUT_IF("event_owner_cap")) return Decision::OwnerFull;
  return Decision::Publish;
}
// Event sources that are not tied to an actor's own dispatch (network shots,
// relayed damage) wait while the pool does not age, then run normally.
inline bool DeferWhilePaused(bool pool_paused) { return pool_paused && !COOP_MUT_IF("event_network_defer"); }
// Second line of defence: a scan that can append every visible type 1 event
// must never run when that number exceeds the list's room.
inline bool VisionScanSafe(uint32_t enumerable) {
  return COOP_MUT_IF("event_vision_guard") || enumerable <= kVisionListRoom;
}

// Developer builds only: KK_DEV_C101_REPLAY=1 restores the 26a behaviour (no
// publish gate, no network deferral, no vision guard) to reproduce the crash
// conditions in a two window run. The C90 group lookup guard stays on and
// then logs the corrupt player pointer instead of crashing.
inline bool DevReplay26a() {
#if defined(KK_DEV_TOOLS)
  static const bool on = [] {
#if defined(_MSC_VER)
    char* v = nullptr; size_t n = 0;
    const bool set = _dupenv_s(&v, &n, "KK_DEV_C101_REPLAY") == 0 && v && v[0] == '1';
    std::free(v);
    return set;
#else
    const char* v = std::getenv("KK_DEV_C101_REPLAY");
    return v && v[0] == '1';
#endif
  }();
  return on;
#else
  return false;
#endif
}

struct BucketCount {
  uint32_t enumerable = 0;   // what 8271E450 visits: neither flag 1 nor 4
  uint32_t owner_live = 0;   // not freed, made by the given owner
  bool valid = false;
};
template <class U32> BucketCount Count(U32 u32, uint32_t type, uint32_t owner) {
  BucketCount c;
  const auto pool = stimulus_debug::ReadPool(u32, type);
  if (!pool.valid) return c;
  c.valid = true;
  for (uint32_t i = 0; i < pool.count; ++i) {
    const auto r = stimulus_debug::ReadSlot(u32, pool, i);
    if (!r.valid) continue;
    if (r.enumerable) ++c.enumerable;
    if (owner && r.source == owner) ++c.owner_live;
  }
  return c;
}

}  // namespace kk::coop::event_budget
