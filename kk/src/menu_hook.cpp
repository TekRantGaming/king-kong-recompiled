// Detects the game's save menu: sub_821074C0 is the title's wrapper around
// XamContentCreateEnumerator, which it calls to list save games as the menu
// opens. Used to grab launcher art from the menu backdrop.

#include "menu_hook.h"

#include <atomic>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

namespace kk {
namespace {

std::mutex g_mutex;
std::atomic<bool> g_seen{false};

// Function-local so it can be used during static initialisation.
std::vector<std::function<void()>>& Callbacks() {
  static std::vector<std::function<void()>> callbacks;
  return callbacks;
}

}  // namespace

void OnSaveMenuShown(std::function<void()> fn) {
  std::lock_guard lock(g_mutex);
  Callbacks().push_back(std::move(fn));
}

}  // namespace kk

REX_EXTERN(__imp__sub_821074C0);
REX_HOOK_RAW(sub_821074C0) {
  if (!kk::g_seen.exchange(true)) {
    REXLOG_INFO("KK: save menu opened");
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard lock(kk::g_mutex);
      callbacks = std::move(kk::Callbacks());
    }
    for (auto& fn : callbacks) fn();
  }
  __imp__sub_821074C0(ctx, base);
}
