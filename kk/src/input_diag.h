#pragma once
#include <cstdint>
namespace rex::ui { class ImGuiDrawer; class Window; }
namespace kk::input_diag {
bool Enabled();
void Install(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);
// No pad values or event work is collected with the switch off.
void Begin(uint32_t user, uint32_t status, const uint8_t* state, int32_t active_pad, uint32_t native_block);
void Stage(uint32_t user, const char* stage, const uint8_t* state, uint32_t status = 0);
void Launcher(bool active, int pads, bool social, bool editing);
}
