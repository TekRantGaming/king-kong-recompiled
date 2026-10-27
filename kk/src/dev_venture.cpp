// Developer-only: KK_DEV_VENTURE_TRACE=1 logs the Venture opening's camera
// tutorial (Chaloupe_CineD1, sub_82432CD8: "Right stick to aim", then the
// vertical sighting choice) twice a second: its gates (+12, +592, +596,
// +600, +660 of the script's variables) and the right stick's y, to see why
// the choice is or is not taken.
#if defined(KK_DEV_TOOLS)

#include <chrono>
#include <cstdlib>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"
#include "coop_partner_body.h"

// KK_DEV_VENTURE_PASS=1: once the vertical sighting choice is made, count the
// "look at the boat" step as done (as if the target had been in view for
// 0.1 s), so scripted runs reach the rowing whatever the stick did to the
// camera.
REX_EXTERN(__imp__sub_82432CD8);
REX_HOOK_RAW(sub_82432CD8) {
  using namespace kk::hfr;
  static const bool on = std::getenv("KK_DEV_VENTURE_TRACE") != nullptr;
  static const bool pass = std::getenv("KK_DEV_VENTURE_PASS") != nullptr;
  if (pass) {
    const uint32_t ai = Load32(base, 0x82CCBF64);
    const uint32_t v = ai ? Load32(base, ai + 64) : 0;
    if (v && Load32(base, v + 660) == 1 && Load32(base, v + 596) == 0) Store32(base, v + 596, 1);
  }
  if (!on) {
    __imp__sub_82432CD8(ctx, base);
    return;
  }
  const uint32_t ai = Load32(base, 0x82CCBF64);
  const uint32_t v = ai ? Load32(base, ai + 64) : 0;
  auto gates = [&] {
    return v ? fmt::format("12={} 592={} 596={} 600={} 660={} cb={:08X} [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                           Load32(base, v + 12), Load32(base, v + 592), Load32(base, v + 596), Load32(base, v + 600),
                           Load32(base, v + 660), Load32(base, v + 280), Load32(base, v + 348), Load32(base, v + 352),
                           Load32(base, v + 356), Load32(base, v + 360), Load32(base, v + 364), Load32(base, v + 368),
                           Load32(base, v + 372), Load32(base, v + 376))
             : std::string("no vars");
  };
  const std::string before = gates();
  __imp__sub_82432CD8(ctx, base);
  static auto last = std::chrono::steady_clock::now();
  static std::string last_after;
  const std::string after = gates();
  // Stick block (sub_8272C610's source): 0x82C638D8, right stick at +16/+20.
  const float sy = LoadF(base, 0x82C638D8 + 20);
  if (after != last_after || std::chrono::steady_clock::now() - last > std::chrono::milliseconds(500)) {
    last = std::chrono::steady_clock::now();
    last_after = after;
    REXLOG_INFO("KK dev venture: before [{}] after [{}] stick y {:.2f} dt {:.4f} ret {}", before, after, sy,
                FrameTime(base), int32_t(ctx.r3.u32));
  }
}

#endif

#if defined(KK_DEV_TOOLS)
// KK_DEV_ACTION_TRACE=1: every request to change an object's animation action
// (sub_82850100(obj, action | flags, ...), the engine's action set) that
// differs from the current one, with the caller, and every animation event
// key that calls into the scripts (sub_82702518(key)) for the object being
// animated, to see what moves the rowers from one action to the next.
namespace {
bool ActionTrace() {
  static const bool on = std::getenv("KK_DEV_ACTION_TRACE") != nullptr;
  return on;
}
uint16_t CurrentAction(const uint8_t* base, uint32_t obj) {
  using namespace kk::hfr;
  const uint32_t b = obj ? Load32(base, obj + 20) : 0;
  const uint32_t anim = b ? Load32(base, b) : 0;
  return anim ? Load16(base, anim + 156) : 0xFFFF;
}
}  // namespace

static uint32_t g_outer_lr = 0;  // the script that called the AI's set-action helper sub_823F80A8

REX_EXTERN(__imp__sub_823F80A8);
REX_HOOK_RAW(sub_823F80A8) {
  const uint32_t saved = g_outer_lr;
  g_outer_lr = uint32_t(ctx.lr);
  __imp__sub_823F80A8(ctx, base);
  g_outer_lr = saved;
}

REX_EXTERN(__imp__sub_82850100);
REX_HOOK_RAW(sub_82850100) {
  kk::PartnerBodyObserveAction(ctx,base);
  if (ActionTrace()) {
    const uint32_t obj = ctx.r3.u32, req = ctx.r4.u32;
    const uint16_t cur = CurrentAction(base, obj);
    if ((req & 0x1FFFFFFF) != cur)
      REXLOG_INFO("KK dev action: obj {:08X} {} -> {} (flags {:X}) from {:08X} via {:08X} dt {:.4f}", obj, cur,
                  req & 0x1FFFFFFF, req >> 29, uint32_t(ctx.lr), g_outer_lr, kk::hfr::FrameTime(base));
  }
  __imp__sub_82850100(ctx, base);
}

REX_EXTERN(__imp__sub_823F8AF0);
REX_EXTERN(__imp__sub_82702518);
REX_HOOK_RAW(sub_82702518) {
  if (ActionTrace() && kk::hfr::FrameTime(base) != 0.0f) {  // (pose evaluations run with a zero step)
    using namespace kk::hfr;
    const uint32_t key = ctx.r3.u32;
    const uint32_t obj = Load32(base, 0x82CCBF8C);  // the object being animated (-16500)
    int32_t frame = -1;
    if (obj) {
      const uint32_t b = Load32(base, obj + 20), anim = b ? Load32(base, b) : 0, layer = anim ? Load32(base, anim + 132) : 0;
      if (layer && Load32(base, layer + 4)) {
        PPCContext saved = ctx;
        ctx.r3.u64 = obj;
        ctx.r4.u64 = 0;
        __imp__sub_823F8AF0(ctx, base);
        frame = int32_t(ctx.r3.u32);
        ctx = saved;
      }
    }
    REXLOG_INFO("KK dev action: event key {:08X} [{:04X} {:04X} {:08X}] obj {:08X} action {} frame {} dt {:.4f}", key,
                Load16(base, key), Load16(base, key + 2), Load32(base, key + 4), obj, CurrentAction(base, obj), frame,
                FrameTime(base));
  }
  __imp__sub_82702518(ctx, base);
}
#endif

#if defined(KK_DEV_TOOLS)
// KK_DEV_KEYS_OBJ=421329F0: the event key player (sub_8271BA18(track list,
// state, ?, out)) for that object's main track: key index and count, time in
// the key before and after, the time step, and what it returns (1: the end of
// the animation, which counts a loop and can move to the next action).
REX_EXTERN(__imp__sub_8271BA18);
REX_HOOK_RAW(sub_8271BA18) {
  using namespace kk::hfr;
  static const uint32_t want = [] {
    const char* v = std::getenv("KK_DEV_KEYS_OBJ");
    return v && *v ? uint32_t(std::stoul(v, nullptr, 16)) : 0u;
  }();
  const uint32_t tracks = ctx.r3.u32, state = ctx.r4.u32;
  const uint32_t obj = Load32(base, 0x82CCBF88);  // -16504: the object being animated
  const uint32_t main_track = Load32(base, 0x82CCBF94) ? Load32(base, Load32(base, 0x82CCBF94)) : 0;  // [[-16492]]
  const bool trace = want && obj == want && FrameTime(base) != 0.0f && Load16(base, tracks + 16) <= 3;
  const uint16_t i0 = trace ? Load16(base, state + 8) : 0;
  const float t0 = trace ? LoadF(base, state) : 0.0f;
  const float step = FrameTime(base);
  __imp__sub_8271BA18(ctx, base);
  if (!trace) return;
  REXLOG_INFO("KK dev keys: track {:08X}{} key {}->{} of {} t {:.5f}->{:.5f} step {:.5f} ret {} action {}", tracks,
              tracks == main_track ? " (main)" : "", i0, Load16(base, state + 8), Load16(base, tracks + 16), t0,
              LoadF(base, state), step, int32_t(ctx.r3.u32), CurrentAction(base, obj));
}
#endif

#if defined(KK_DEV_TOOLS)
// KK_DEV_GFX_TRACE=1: once a second, how many effects were added to the world's
// effect list (sub_827F9978) by each caller and effect type, and the frames in
// that second, to find effects created once a frame.
#include <map>
#include <utility>

REX_EXTERN(__imp__sub_827F9978);
REX_HOOK_RAW(sub_827F9978) {
  using namespace kk::hfr;
  static const bool on = std::getenv("KK_DEV_GFX_TRACE") != nullptr;
  if (on) {
    static std::map<std::pair<uint32_t, uint32_t>, int> counts;
    static auto start = std::chrono::steady_clock::now();
    // Frames with at least one add: the game clock (as sub_825845E0 reads it) changes once a frame.
    static int frames = 0;
    static uint32_t last_clock = 0;
    const uint32_t clock = Load32(base, 0x82CCAE1C);
    if (clock != last_clock) ++frames, last_clock = clock;
    ++counts[{uint32_t(ctx.lr), ctx.r4.u32}];
    if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(1)) {
      std::string line;
      for (const auto& [k, n] : counts) line += fmt::format(" {:08X}/{}={}", k.first, k.second, n);
      REXLOG_INFO("KK dev gfx adds ({} frames with adds, dt {:.4f}):{}", frames, FrameTime(base), line);
      counts.clear(), frames = 0, start = std::chrono::steady_clock::now();
    }
  }
  __imp__sub_827F9978(ctx, base);
}
#endif
