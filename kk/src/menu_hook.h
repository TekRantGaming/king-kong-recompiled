#pragma once

#include <functional>

namespace kk {

// Runs `fn` once, the first time the game lists save games. It does that when
// its save menu opens, over the moonlit Skull Island backdrop.
// Called on a game thread.
void OnSaveMenuShown(std::function<void()> fn);

}  // namespace kk
