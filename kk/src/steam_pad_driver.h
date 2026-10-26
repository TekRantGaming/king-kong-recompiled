#pragma once
// C82: the game's input driver for the Steam Input pad (steam_pad.h). It lists one controller only while
// Steam Input holds at least one pad and our actions are loaded; it gives input only while the game's
// window has the focus (like the SDL driver). Every device still feeds player 1 (UseOneControllerSlot).
namespace rex::input { class InputSystem; }
namespace rex::ui { class Window; }
namespace kk::steam_pad {
void InstallDriver(rex::input::InputSystem* input, rex::ui::Window* window);
}
