#pragma once

#include "coop_mut.h"
#include <cstdint>
#if defined(KK_COOP_MUTATION_TEST)
#include <chrono>
#include <thread>
#endif

namespace kk::coop {
enum class FrameWaitReason { None, Reconnect, Load, Transition, Chapter, Observer };

// Called once per engine frame. Native world updates are held separately, so
// display, audio, the input pump and the network worker can continue normally.
class FrameWait {
 public:
  bool Poll(FrameWaitReason reason, uint32_t generation, double now) {
#if defined(KK_COOP_MUTATION_TEST)
    // Restore a blocking game thread wait. The elapsed time assertion must
    // catch the sleep itself, rather than treating a test timeout as a catch.
    if (COOP_MUT_IF("reload_blocking_sleep"))
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
#endif
    if (reason_!=reason || generation_!=generation) {
      reason_=reason; generation_=generation; began_=now;
    }
    // Independent chapters may take as long as their partner needs. Connection
    // loss still changes the reason to Reconnect and gets its own finite budget.
    return !COOP_MUT_IF("reload_wait_timeout") && reason!=FrameWaitReason::Chapter &&
        reason!=FrameWaitReason::Observer && now-began_>=120;
  }
  void Clear() { reason_=FrameWaitReason::None; }
  FrameWaitReason Reason() const { return reason_; }
 private:
  FrameWaitReason reason_=FrameWaitReason::None;
  uint32_t generation_=0;
  double began_=0;
};
}  // namespace kk::coop
