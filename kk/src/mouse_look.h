#pragma once

// Keyboard & mouse: the mouse turns Jack's camera directly (mouse_look.cpp).

namespace rex::ui {
class Window;
}

namespace kk::mouse_look {

// Listens to the window's mouse movement (call once the window exists).
void Install(rex::ui::Window* window);

// CM_Cam's right-stick read, x and y in -1..1 (game thread): takes the mouse
// movement since the last frame and, on an axis it moved, nudges the stick
// just past the camera's deadzone so CM_Cam turns that axis this frame.
void OnCameraStick(float& x, float& y);

// CM_Cam's yaw and pitch turns this frame (radians, the stick's part in):
// the mouse's part added.
double Yaw(double angle);
double Pitch(double angle);

}  // namespace kk::mouse_look
