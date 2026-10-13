// Developer-only: menu page traces (never shipped). Findings:
// native-renderer/docs/chapter-jump.md, "Replacing the pause menu".
//
// KK_DEV_PAGE_TRACE=1: log when each menu page script (the IntMIG_* functions
// of the AI function table at 0x828CB140, one runs every frame while its page
// is up) starts running, and every page switch request (sub_826FA3B0).
// (IntMIG_Page_StartMain, IntMIG_loop and IntMIG_init are hooked elsewhere:
// cheats.cpp, fov.cpp, dev_chapter_jump.cpp; IntMIG_wait, IntMIG_Page_Main,
// IntMIG_Page_MessageYesNo and IntMIG_Page_ScoreNew by the pause takeover,
// pause_hooks.cpp.)
#if defined(KK_DEV_TOOLS)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

namespace kk::pause {
namespace {
bool Tracing() {
  static const bool on = std::getenv("KK_DEV_PAGE_TRACE") != nullptr;
  return on;
}
uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void PageRuns(const char* name, uint8_t* base) {
  static std::mutex m;
  static std::unordered_map<const char*, std::chrono::steady_clock::time_point> last;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(m);
  auto& t = last[name];
  if (now - t > std::chrono::milliseconds(400)) {
    const uint32_t ai = Load32(base, 0x82CCBF64), v = ai ? Load32(base, ai + 0x40) : 0;
    REXLOG_INFO("KK dev page: {} runs (vars {:08X}, item {})", name, v, v ? int32_t(Load32(base, v + 0x3C)) : -1);
  }
  t = now;
}
}  // namespace
}  // namespace kk::pause

#define KK_PAGE_HOOK(addr, name)                                REX_EXTERN(__imp__sub_##addr);                                REX_HOOK_RAW(sub_##addr) {                                      if (kk::pause::Tracing()) kk::pause::PageRuns(name, base);     __imp__sub_##addr(ctx, base);                               }

KK_PAGE_HOOK(8249ED88, "Interface_MenuInGame")
KK_PAGE_HOOK(825ECC98, "interface_MenuInGame_tools")
KK_PAGE_HOOK(824C5050, "IntMIG_Page_DemoXenon_Extras")
KK_PAGE_HOOK(824C4948, "IntMIG_Page_DemoXenon_Main")
KK_PAGE_HOOK(824C46D8, "IntMIG_Page_Xbox360_save_game_with_no_device")
KK_PAGE_HOOK(824C3F90, "IntMIG_Page_Xbox360_Invalid_Storage_Device")
KK_PAGE_HOOK(824C3CB0, "IntMIG_Page_Xbox360_Controller_Check")
KK_PAGE_HOOK(824C39B0, "IntMIG_Xbox360_Profile_Loaded_Before_Gamplay")
KK_PAGE_HOOK(824C3618, "IntMIG_Page_Xbox360_Profile_Changed_During_StartMenu")
KK_PAGE_HOOK(824C30C8, "IntMIG_Page_Xbox360_Profile_Changed_During_Gameplay")
KK_PAGE_HOOK(824D2F20, "IntMIG_Page_OptStart_Langue")
KK_PAGE_HOOK(824C1A48, "IntMIG_Page_CheatFinal")
KK_PAGE_HOOK(824C17F8, "IntMIG_Page_Memcard_TestError")
KK_PAGE_HOOK(824CF400, "IntMIG_Page_OptStart_Control")
KK_PAGE_HOOK(824CE4A0, "IntMIG_Page_OptStart_Video")
KK_PAGE_HOOK(824CD320, "IntMIG_Page_OptStart_Audio")
KK_PAGE_HOOK(824C0F60, "IntMIG_Page_OptStart")
KK_PAGE_HOOK(824A0E70, "IntMIG_Background")
KK_PAGE_HOOK(824AC7E8, "IntMIG_afairetoutletemps")
KK_PAGE_HOOK(824C0A30, "IntMIG_Page_E3Menu")
KK_PAGE_HOOK(8266F908, "interface_MenuIGame_memcard")
KK_PAGE_HOOK(824C02B0, "IntMIG_Page_ProfileDelete")
KK_PAGE_HOOK(824BF288, "IntMIG_Page_ProfileCreate")
KK_PAGE_HOOK(824BDC28, "IntMIG_Page_Profile")
KK_PAGE_HOOK(824CBFE8, "IntMIG_Page_Xtra")
KK_PAGE_HOOK(824BD150, "IntMIG_Page_Map")
KK_PAGE_HOOK(824B4DF0, "IntMIG_Page_load")
KK_PAGE_HOOK(824CB738, "IntMIG_Page_Memcard")
KK_PAGE_HOOK(8249CE68, "IntMIG_Page_Cheat")
KK_PAGE_HOOK(8249EBD8, "IntMIG_colorize")
KK_PAGE_HOOK(824BA188, "IntMIG_Page_Xbox360_NotEnoughSpace")
KK_PAGE_HOOK(824B9F50, "IntMIG_Page_Xbox360_Quit_Demo")

// For the pages hooked elsewhere (pause_hooks.cpp).
namespace kk {
void DevPageRuns(const char* name, uint8_t* base) {
  if (pause::Tracing()) pause::PageRuns(name, base);
}
}  // namespace kk

// Page switch requests: sub_826FA3B0(mode, page) from a page script.
REX_EXTERN(__imp__sub_826FA3B0);
REX_HOOK_RAW(sub_826FA3B0) {
  if (kk::pause::Tracing())
    REXLOG_INFO("KK dev page: switch r3={} r4={:08X} lr={:08X}", int32_t(ctx.r3.u32), ctx.r4.u32, uint32_t(ctx.lr));
  __imp__sub_826FA3B0(ctx, base);
}

#endif
