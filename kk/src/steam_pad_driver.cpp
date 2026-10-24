#include "steam_pad_driver.h"
#include "steam_pad.h"

#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

#include <cstring>
#include <memory>

namespace kk::steam_pad {
namespace {
using rex::input::DeviceId;
using rex::X_RESULT;
using rex::X_STATUS;
constexpr DeviceId kDevice = static_cast<DeviceId>(0x4B4B5350);  // "KKSP"

class Driver final : public rex::input::InputDriver {
 public:
  explicit Driver(rex::ui::Window* window) : InputDriver(window, 0), window_(window) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void OnWindowAvailable(rex::ui::Window* window) override { window_ = window; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    if (Controllers() <= 0) return;
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Steam Input controller";
    info.guid = "kk_steam_input";
    out.push_back(info);
  }
  X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    State s;
    if (!Read(s, Now())) return X_ERROR_DEVICE_NOT_CONNECTED;
    // Another window has the focus: Steam still sends us the pad, the player is using something else.
    if (!is_active() || !window_ || !window_->HasFocus()) s = {};
    if (!(s == last_)) { last_ = s; ++packet_; }
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->packet_number = packet_;
      out->gamepad.buttons = s.buttons;
      out->gamepad.left_trigger = s.lt;
      out->gamepad.right_trigger = s.rt;
      out->gamepad.thumb_lx = s.lx;
      out->gamepad.thumb_ly = s.ly;
      out->gamepad.thumb_rx = s.rx;
      out->gamepad.thumb_ry = s.ry;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (id != kDevice || Controllers() <= 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = int16_t(0x7FFF);
      caps->gamepad.thumb_ly = int16_t(0x7FFF);
      caps->gamepad.thumb_rx = int16_t(0x7FFF);
      caps->gamepad.thumb_ry = int16_t(0x7FFF);
      caps->vibration.left_motor_speed = 0xFFFF;
      caps->vibration.right_motor_speed = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override {
    if (id != kDevice || Controllers() <= 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (vibration) SetVibration(vibration->left_motor_speed, vibration->right_motor_speed);
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kDevice && Controllers() > 0 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  rex::ui::Window* window_;
  State last_;
  uint32_t packet_ = 0;
};
}  // namespace

void InstallDriver(rex::input::InputSystem* input, rex::ui::Window* window) {
  if (!input) return;
  input->AddDriver(std::make_unique<Driver>(window));
  REXLOG_INFO("KK: Steam Input controller driver ready (used only while Steam manages a controller)");
}
}  // namespace kk::steam_pad
