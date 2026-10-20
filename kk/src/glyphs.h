#pragma once

#include <cstdint>
#include <string>

namespace rex::memory {
class Memory;
}

namespace kk {

// Replaces the game's Xbox 360 button prompts with the style picked in
// kk_button_prompts, and follows changes to it while the game runs.
void StartButtonPrompts(rex::memory::Memory* memory);

// Whether the keyboard prompts are the ones drawn now (prompt_anim.cpp).
bool KeyboardPromptsShown();

// The prompt style for a kind of controller (an SDL_GamepadType and the
// controller's name): xbox360, xbox_series, ps2 (also the PS3's), ps5 or
// steam_deck. Anything else gets the game's own Xbox 360 prompts.
std::string PadPromptsFor(int sdl_gamepad_type, const char* name);
// The style for the first connected controller (xbox360 if there is none).
std::string DetectedPadPrompts();
// kk_button_prompts, with "auto" (the default) worked out.
std::string ButtonPromptStyle();

}  // namespace kk
