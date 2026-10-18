// The game's side of photo mode in the native renderer (plugin/native_photo.h, task 208): compiled into
// king_kong like kk_native_ui.cpp. Does nothing when the native plugin is not loaded.
#pragma once

#include "plugin/native_photo.h"

namespace nr::kk {
// Sends photo mode's settings (any thread). False when the renderer did not take them.
bool SetPhoto(const NrPhotoSettings& settings);
}  // namespace nr::kk
