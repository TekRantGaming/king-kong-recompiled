// Photo mode's entry point (plugin/native_photo.h, task 208): the game sends its settings each frame while
// photo mode is on, and once with active = 0 when it ends.

#include <rex/system/gpu_plugin.h>

#include "backend/photo_config.h"
#include "plugin/native_photo.h"

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t nr_photo_set(const NrPhotoSettings* settings) {
  if (!settings || settings->size != sizeof(NrPhotoSettings)) return 0;
  nr::SetPhotoSettings(*settings);
  return 1;
}
