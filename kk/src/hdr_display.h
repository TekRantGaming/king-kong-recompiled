// HDR follows Windows: reads the game's screen (Windows HDR on or off, the brightness DXGI reports, Windows'
// SDR content brightness and the Windows HDR Calibration app's colour profile) and applies kk_hdr and
// kk_hdr_levels to the presenter's present_hdr, present_hdr_peak and present_hdr_paper_white (rules:
// hdr_logic.h). Linux: SDL's HDR flag for the screen, no brightness values (the defaults are used).
#pragma once

#include <functional>
#include <string>

#include "hdr_logic.h"

namespace kk::hdr {

// The game's window (a native handle), for the screen it is on; without one the primary screen is used.
void SetWindowProvider(std::function<void*()> provider);

// Reads the screen now. Windows: DXGI (IDXGIOutput6::GetDesc1), DisplayConfig (advanced colour, SDR white
// level) and mscms (ColorProfileGetDisplayDefault). KK_DEV_SCREEN_HDR=1/0 (tests) fakes the HDR state.
DisplayInfo QueryDisplay(void* native_window);

// Reads the screen and sets the presenter's HDR values from kk_hdr and kk_hdr_levels; logs when anything
// changed. `why` is logged.
void Apply(const char* why);
// As Apply, at most every two seconds (the launcher's and the pause menu's settings pages call it each frame,
// so values follow Windows, and the calibration app, while they are open).
void ApplyIfStale();
// The last reading (Apply or ApplyIfStale).
DisplayInfo LastDisplay();

// Opens the Windows HDR Calibration app, or its Microsoft Store page when it is not installed. Returns false
// when neither could be opened (or not on Windows). The next ApplyIfStale reads the new values.
bool OpenCalibrationApp(bool* opened_store = nullptr);
// Whether the Windows HDR Calibration app is installed (Windows only; cached).
bool CalibrationAppInstalled();

}  // namespace kk::hdr
