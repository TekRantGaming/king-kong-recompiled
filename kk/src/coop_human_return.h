#pragma once
#include <cstdint>
#include <cstdlib>
#include "coop_mut.h"

// C110: guest access violation "read of guest 0x00000000" in c104-legacy302
// (28 Oct 06:23:56, level 302, follower human 439754B0, AI 414F5948).
//   * Crash stack: ai_set_track 826FA3B0 +0xb8 < H_ETAT_IA_frappe 826A3818
//     (its only ai_set_track call, return address 826A3CDC) < ai_run_track <
//     ai_run_object < ai_run_world < wor_update. Minidump: r8 = AI 414F5948
//     (the follower), track 2, function 0. ai_set_track stores the new
//     function and then reads its first word (lwz r10,0(r4)): guest 0.
//   * At the end of its strike animation H_ETAT_IA_frappe goes back to the
//     state saved at vars+0xB04 when vars+0xC64 is set, else to vars+0xAFC,
//     with no null check.
//   * H_ETAT_IA_help (826B7818) writes the friend it helps into vars+0xC64
//     every frame and forces the strike (H_exec_validate_tir: state 607),
//     but never sets vars+0xB04. Only H_ETAT_IA_paf, tir, porte, Post and
//     ann_climb write it. A human loaded fresh at a checkpoint that has not
//     yet been hit has vars+0xB04 = 0, so help, strike, end = null state.
//   * The game's own answer to an empty saved state elsewhere (H_ETAT_IA_paf
//     for 0xB04, H_TRACK_Post for 0xB08, H_exec_ch_msg for 0xB08) is the
//     default track 2 function held at 0x82CC8854.
namespace kk::coop::human_return {

constexpr uint32_t kFrappeEndReturn = 0x826A3CDC;  // lr of frappe's ai_set_track(2, ...)
constexpr uint32_t kAiRunning = 0x82CCBF64, kAiTarget = 0x82CCBF68;
constexpr uint32_t kAiObject = 0x82CCBF6C, kAiTargetObject = 0x82CCBF70;
constexpr uint32_t kDefaultTrack2 = 0x82CC8854;    // holds the native default track 2 function
constexpr uint32_t kAiVars = 0x40, kAiTracks = 5;
constexpr uint32_t kVarsState = 2712, kVarsOldState = 2716;  // 0xA98, 0xA9C
constexpr uint32_t kVarsAfc = 2812, kVarsB00 = 2816, kVarsB04 = 2820, kVarsFriend = 3172;  // 0xAFC, 0xB00, 0xB04, 0xC64
constexpr uint32_t kStateFrappe = 0x2C8, kStateHelp = 0x25F, kStateSuivi = 0x258;
constexpr uint32_t AiFunctionSlot(uint32_t track) { return (track + 6) * 4; }

struct Slots {
  uint32_t friend_object = 0;   // vars+0xC64
  uint32_t b04 = 0;             // vars+0xB04
  uint32_t afc = 0;             // vars+0xAFC
  uint32_t native_default = 0;  // *0x82CC8854
};
// What frappe's end reads natively (826A3C9C .. 826A3CD4).
inline uint32_t NativeEndChoice(const Slots& s) { return s.friend_object ? s.b04 : s.afc; }
inline uint32_t NativeEndSlot(const Slots& s) { return s.friend_object ? kVarsB04 : kVarsAfc; }

struct Fill { bool needed = false; uint32_t slot = 0, value = 0; };
// Before frappe runs, give the slot its end will read a state when it is
// empty: the other saved state first (help returns to 0xAFC itself when the
// friend goes away), else the native default. Nothing changes when the
// native slot already holds a state.
inline Fill FrappeFill(const Slots& s) {
  Fill f;
  if (NativeEndChoice(s) && !COOP_MUT_IF("human_return_keep_native")) return f;
  if (COOP_MUT_IF("human_return_skip_fill")) return f;
  f.slot = NativeEndSlot(s);
  if (s.friend_object && s.afc && !COOP_MUT_IF("human_return_default_only")) f.value = s.afc;
  else f.value = s.native_default;
  f.needed = f.value != 0;
  return f;
}

// Second line of defence: ai_set_track(track, 0) dereferences the null
// function whenever the track holds another function (only an equal one
// returns early). Such a call is refused; every other call runs natively.
inline bool SetTrackRefused(uint32_t track, uint32_t fn, uint32_t current) {
  if (COOP_MUT_IF("human_set_track_guard")) return false;
  return track < kAiTracks && fn == 0 && current != 0;
}

// Developer builds only: KK_DEV_C110_REPLAY=1 turns the frappe fill off (the
// ai_set_track guard stays on), so a run shows the native null return and
// the guard catching it instead of the crash.
inline bool DevReplayNoFill() {
#if defined(KK_DEV_TOOLS)
  static const bool on = [] {
#if defined(_MSC_VER)
    char* v = nullptr; size_t n = 0;
    const bool set = _dupenv_s(&v, &n, "KK_DEV_C110_REPLAY") == 0 && v && v[0] == '1';
    std::free(v);
    return set;
#else
    const char* v = std::getenv("KK_DEV_C110_REPLAY");
    return v && v[0] == '1';
#endif
  }();
  return on;
#else
  return false;
#endif
}

}  // namespace kk::coop::human_return
