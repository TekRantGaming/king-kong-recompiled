#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace kk {

// Replaces the game's Xbox 360 button prompts with the style picked in
// kk_button_prompts (does nothing for the default "xbox360").
void StartButtonPrompts(rex::memory::Memory* memory);

}  // namespace kk
