// The game's pipelines made before the game runs: the prewarm the launcher starts when it opens
// (the plugin's nr_prewarm_start, docs/backend.md, "Prewarm from the launcher").
//
// A game renderer with no game, on a device of its own, given the pipeline files to read (the
// player's cache file and the shipped list, never written here) and making every description on
// its pipeline workers. The pipelines it makes belong to its device and go with it; what the game
// keeps is the driver's own disk cache (most drivers keep one), and with a DriverPipelineCache the
// file that cache is saved to when the prewarm stops, which the game's device reads at its start.
// For those to hold the game's pipelines, the settings must be the game's: the same shader pack,
// render scale and face flip (the plugin reads them from the same cvars as the game).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/lighting_config.h"

namespace nr {

class DriverPipelineCache;
class GameRenderer;
class ShaderLibrary;

class PipelinePrewarm {
 public:
  struct Settings {
    std::string shader_pack;  // the pack the game uses (the prewarm makes shaders from it alone)
    std::string dxc;
    float render_scale_x = 1.0f, render_scale_y = 1.0f;  // render_scale.h, ResolveRenderScale
    float shadow_scale = 0.0f;
    bool shaders_aware = false;
    bool flip_front_face = false;
    // The lighting options (docs/lighting.md): they add b5 to the game's layout and change shaders.
    LightingConfig lighting;
    // Pipeline files to make, read only, in this order (the player's cache file, the shipped list).
    std::vector<std::string> lists;
    // The driver's pipeline cache of the prewarm's device and its file (null: none): read at Start,
    // saved at Stop.
    DriverPipelineCache* driver_cache = nullptr;
    std::string driver_cache_path;
  };
  struct Progress {
    uint64_t listed = 0;      // descriptions in the files
    uint64_t queued = 0;      // sent to the workers
    uint64_t made = 0;        // made by the workers
    uint64_t waiting = 0;     // whose shaders are not in the pack (the game makes those)
    uint64_t other_kind = 0;  // of another render scale or face flip
    bool done = false;        // the workers have nothing left
  };

  explicit PipelinePrewarm(nvrhi::IDevice* device);
  ~PipelinePrewarm();  // Stop()
  PipelinePrewarm(const PipelinePrewarm&) = delete;
  PipelinePrewarm& operator=(const PipelinePrewarm&) = delete;

  // Makes the renderer and queues the files' descriptions; returns at once. False when the game
  // renderer or its shader library could not be made (nothing runs then).
  bool Start(const Settings& settings);
  Progress progress();
  // Waits until the workers are done or `seconds` passed; true when done.
  bool Wait(double seconds);
  // Stops the workers once their current pipeline is made (the rest is left to the game) and saves
  // the driver's pipeline cache. Idempotent.
  void Stop();

 private:
  nvrhi::IDevice* device_;
  class NoGuestMemory;
  std::unique_ptr<NoGuestMemory> memory_;
  std::unique_ptr<ShaderLibrary> shaders_;
  std::unique_ptr<GameRenderer> renderer_;
  uint64_t listed_ = 0;
};

}  // namespace nr
