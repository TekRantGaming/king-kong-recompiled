// Photo mode's entry point (plugin/native_photo.h, task 208): the game sends its settings each frame while
// photo mode is on, and once with active = 0 when it ends.

#include <rex/system/gpu_plugin.h>

#include "backend/draw_tags.h"
#include "backend/menu_scene_light.h"
#include "backend/photo_config.h"
#include "plugin/native_photo.h"

// The ultrawide mod's draw tags (backend/draw_tags.h, task 231): called on the game's thread around the draws.
extern "C" REX_GPU_PLUGIN_EXPORT void nr_set_draw_tags(uint32_t tags, float hud_width_scale) {
  nr::SetDrawTags(tags, hud_width_scale);
}

// Task 330: the live menu scene behind the launcher draws with lighter effects (backend/menu_scene_light.h).
extern "C" REX_GPU_PLUGIN_EXPORT void nr_set_menu_scene_light(uint32_t on) { nr::SetMenuSceneLight(on != 0); }

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t nr_photo_set(const NrPhotoSettings* settings) {
  if (!settings || settings->size != sizeof(NrPhotoSettings)) return 0;
  nr::SetPhotoSettings(*settings);
  return 1;
}
