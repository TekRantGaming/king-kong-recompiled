#pragma once

#include <functional>

namespace kk {
// The app installs a UI-thread marshaling callback for guest-thread state changes.
void SetUiPaintRequest(std::function<void()> request);
void RequestUiPaint();
}  // namespace kk
