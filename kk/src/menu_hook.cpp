// Detects the game's save menu: sub_821074C0 is the title's wrapper around
// XamContentCreateEnumerator, which it calls to list save games as the menu
// opens. Used to grab launcher art from the menu backdrop.

#include "menu_hook.h"

#include <atomic>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>

namespace kk {
namespace {

std::mutex g_mutex;
std::function<void()> g_on_save_menu;
std::atomic<bool> g_seen{false};

}  // namespace

void OnSaveMenuShown(std::function<void()> fn) {
  std::lock_guard lock(g_mutex);
  g_on_save_menu = std::move(fn);
}

}  // namespace kk

REX_EXTERN(__imp__sub_821074C0);
REX_HOOK_RAW(sub_821074C0) {
  if (!kk::g_seen.exchange(true)) {
    REXLOG_INFO("KK: save menu opened");
    std::function<void()> fn;
    {
      std::lock_guard lock(kk::g_mutex);
      fn = std::move(kk::g_on_save_menu);
    }
    if (fn) fn();
  }
  __imp__sub_821074C0(ctx, base);
}
