// C110: human strike state with no saved state to go back to (see coop_human_return.h).
// Two hooks, both on the game thread:
//   H_ETAT_IA_frappe 826A3818: before the strike state runs, fill the saved
//     state its end will read when that slot is empty (cause fix).
//   ai_set_track 826FA3B0: refuse a null function that would be dereferenced
//     (second line of defence for every caller). The developer page trace of
//     dev_pause.cpp is called from here (one hook per function).
#include "coop_human_return.h"
#include "coop_pose.h"
#include "hfr.h"
#include <cstdint>
#include <rex/hook.h>
#include <rex/logging.h>

REX_EXTERN(__imp__sub_826A3818);
REX_EXTERN(__imp__sub_826FA3B0);

#if defined(KK_DEV_TOOLS)
namespace kk { void DevPageSwitch(PPCContext& ctx); }
#endif

namespace kk::human_guard {
namespace {
namespace hr = kk::coop::human_return;
using kk::hfr::Load32;
using kk::hfr::Store32;

struct Counters { uint64_t fills = 0, refused = 0, help_entries = 0; };
Counters& Count() { static Counters c; return c; }

// One line for the first few events of a kind, then one in every 64.
bool Report(uint64_t n) { return n <= 8 || (n % 64) == 0; }

void BeforeFrappe(uint8_t* b) {
  const uint32_t ai = Load32(b, hr::kAiRunning);
  if (!kk::coop::PoseAddress(ai, 0x58)) return;
  const uint32_t vars = Load32(b, ai + hr::kAiVars);
  if (!kk::coop::PoseAddress(vars, hr::kVarsFriend + 4)) return;
  hr::Slots s;
  s.friend_object = Load32(b, vars + hr::kVarsFriend);
  s.b04 = Load32(b, vars + hr::kVarsB04);
  s.afc = Load32(b, vars + hr::kVarsAfc);
  s.native_default = Load32(b, hr::kDefaultTrack2);
  const uint32_t state = Load32(b, vars + hr::kVarsState);
  auto& c = Count();
#if defined(KK_DEV_TOOLS)
  // Evidence: every entry into the strike state, with the slots its end reads.
  static uint64_t entries = 0, empty = 0;
  const bool empty_end = hr::NativeEndChoice(s) == 0;
  if (state != hr::kStateFrappe && (Report(++entries) || (empty_end && ++empty <= 200))) {
    REXLOG_INFO("KK dev human strike: entry ai={:08X} actor={:08X} from-state={:X} old={:X} friend={:08X} b04={:08X} afc={:08X} b00={:08X} end-reads={:08X} n={}",
        ai, Load32(b, hr::kAiObject), state, Load32(b, vars + hr::kVarsOldState), s.friend_object, s.b04, s.afc,
        Load32(b, vars + hr::kVarsB00), hr::NativeEndChoice(s), entries);
  }
#endif
  if (state == hr::kStateHelp && Report(++c.help_entries)) {
    REXLOG_INFO("KK co-op human: strike from help ai={:08X} actor={:08X} friend={:08X} b04={:08X} afc={:08X} n={}",
        ai, Load32(b, hr::kAiObject), s.friend_object, s.b04, s.afc, c.help_entries);
  }
  if (hr::DevReplayNoFill()) return;
  const auto fill = hr::FrappeFill(s);
  if (!fill.needed) return;
  Store32(b, vars + fill.slot, fill.value);
  if (Report(++c.fills)) {
    REXLOG_WARN("KK co-op human: strike had no state to go back to ai={:08X} actor={:08X} state={:X} old={:X} friend={:08X} "
        "b04={:08X} afc={:08X}; slot {} given {:08X} (n={})",
        ai, Load32(b, hr::kAiObject), state, Load32(b, vars + hr::kVarsOldState), s.friend_object, s.b04, s.afc,
        fill.slot == hr::kVarsB04 ? "B04" : "AFC", fill.value, c.fills);
  }
}

// True when the call must not run.
bool RefuseSetTrack(PPCContext& ctx, uint8_t* b) {
  if (ctx.r4.u32 != 0) return false;
  const uint32_t track = ctx.r3.u32;
  const uint32_t ai = Load32(b, hr::kAiTarget);
  if (track >= hr::kAiTracks || !kk::coop::PoseAddress(ai, 0x58)) return false;
  const uint32_t current = Load32(b, ai + hr::AiFunctionSlot(track));
  if (!hr::SetTrackRefused(track, 0, current)) return false;
  // Keep the native side effect that precedes the store: the target goes
  // back to the running instance and object. The track keeps its state.
  Store32(b, hr::kAiTarget, Load32(b, hr::kAiRunning));
  Store32(b, hr::kAiTargetObject, Load32(b, hr::kAiObject));
  auto& c = Count();
  if (Report(++c.refused)) {
    const uint32_t vars = Load32(b, ai + hr::kAiVars);
    const bool v = kk::coop::PoseAddress(vars, hr::kVarsFriend + 4);
    REXLOG_WARN("KK co-op human: refused a null state ai={:08X} track={} current={:08X} lr={:08X} state={:X} old={:X} "
        "friend={:08X} b04={:08X} afc={:08X} (n={})",
        ai, track, current, uint32_t(ctx.lr), v ? Load32(b, vars + hr::kVarsState) : 0,
        v ? Load32(b, vars + hr::kVarsOldState) : 0, v ? Load32(b, vars + hr::kVarsFriend) : 0,
        v ? Load32(b, vars + hr::kVarsB04) : 0, v ? Load32(b, vars + hr::kVarsAfc) : 0, c.refused);
  }
  return true;
}
}  // namespace
}  // namespace kk::human_guard

REX_HOOK_RAW(sub_826A3818) {
  kk::human_guard::BeforeFrappe(base);
  __imp__sub_826A3818(ctx, base);
}

REX_HOOK_RAW(sub_826FA3B0) {
#if defined(KK_DEV_TOOLS)
  kk::DevPageSwitch(ctx);
#endif
  if (kk::human_guard::RefuseSetTrack(ctx, base)) return;
  __imp__sub_826FA3B0(ctx, base);
}
