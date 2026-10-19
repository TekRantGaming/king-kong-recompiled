// High frame rate fix: scripts that never step less than 0.016 s
// (kk_hfr_fixes, "dt_floor").
//
// Some of Kong's fight scripts and the BC creature's path script clamp the
// frame time to 0.016 .. 0.048 s before using it as their time step (the
// constant 0.016 at 0x820B0D1C):
//   k_ETAT_grab (sub_825CA350), k_ETAT_grab_mashing (sub_8259D998),
//   k_ETAT_finished (sub_825B3288), k_ETAT_cine (sub_825B62E0),
//   KT_ETAT_Finish (sub_824876F0), KBC_ETAT_KK_Finished (sub_8252B1D0),
//   BC_exec_MOVE_beziers (sub_8263E1B8).
// At 30 FPS the clamp does nothing. Above 62.5 FPS every frame counts as
// 0.016 s, so these run faster than real time: about 1.9x at 120 FPS, 3.8x at
// 240.
//
// While one of them runs, the constant is lowered to this frame's time (when
// that is under 0.016), so the clamp keeps the real frame time. The same
// constant is the step of the "decay per 0.016 s" loops in a few of them;
// with a step equal to the frame time those still apply their factor once,
// as they do at that frame rate without the fix (hfr_step_decay.cpp corrects
// those). Only these scripts, the raft (Raft_exec_compute_position, a fixed
// 0.016 s step that must keep it), the step-decay scripts and SFA_init read
// the constant, all on the game thread, and it is put back after each call.
// It lives in the executable's read-only data, so Setup() makes its page
// writable.

#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "hfr.h"

namespace kk::hfr {
namespace {

constexpr uint32_t kFloor = 0x820B0D1C;  // 0.016f
bool g_writable = false;

template <typename Fn>
void WithRealStep(PPCContext& ctx, uint8_t* base, Fn original) {
  const float dt = FrameTime(base);
  if (!g_writable || !On("dt_floor") || !(dt > 0.0f && dt < 0.016f)) {
    original(ctx, base);
    return;
  }
  Count("dt_floor calls with a frame under 0.016 s");
  const uint32_t saved = Load32(base, kFloor);
  StoreF(base, kFloor, dt);
  original(ctx, base);
  Store32(base, kFloor, saved);
}

}  // namespace

// For a hook that also wraps one of these scripts (k_ETAT_grab, see
// hfr_step_decay.cpp).
void WithRealStepFn(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  WithRealStep(ctx, base, original);
}

void SetupDtFloor(rex::memory::Memory* memory) {
  auto* heap = memory ? memory->LookupHeap(kFloor) : nullptr;
  g_writable = heap && heap->Protect(kFloor, 4, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  if (!g_writable) REXLOG_WARN("KK: high frame rate fix dt_floor unavailable (could not unprotect {:08X})", kFloor);
}

}  // namespace kk::hfr

#define KK_DT_FLOOR_HOOK(addr)                                                    \
  REX_EXTERN(__imp__sub_##addr);                                                  \
  REX_HOOK_RAW(sub_##addr) { kk::hfr::WithRealStep(ctx, base, __imp__sub_##addr); }

// k_ETAT_grab (sub_825CA350) is hooked in hfr_step_decay.cpp and k_ETAT_grab_mashing (sub_8259D998) in
// hfr_kong_lift.cpp; both call WithRealStepFn.
KK_DT_FLOOR_HOOK(825B3288)  // k_ETAT_finished
KK_DT_FLOOR_HOOK(825B62E0)  // k_ETAT_cine
KK_DT_FLOOR_HOOK(824876F0)  // KT_ETAT_Finish
KK_DT_FLOOR_HOOK(8252B1D0)  // KBC_ETAT_KK_Finished
KK_DT_FLOOR_HOOK(8263E1B8)  // BC_exec_MOVE_beziers
