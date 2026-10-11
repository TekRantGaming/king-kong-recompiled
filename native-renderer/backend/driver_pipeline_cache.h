// The driver's own compiled pipelines kept in a file between runs (a VkPipelineCache on Vulkan:
// vk_pipeline_cache.h). The pipeline cache file (pipeline_cache.h) says which pipelines to make;
// this one makes making them fast, since the driver finds the compiled code instead of compiling.
// docs/backend.md, "The driver's pipeline cache".
//
// No Vulkan here: the game renderer uses it on every host API (only Vulkan has one).
#pragma once

#include <cstddef>
#include <string>

namespace nr {

class DriverPipelineCache {
 public:
  enum class Load {
    kLoaded,         // the file's pipelines are in the cache
    kAbsent,         // no file yet (the first run)
    kDriverChanged,  // made by another GPU, driver or driver version: started empty
    kDamaged,        // not a cache file, or cut short: started empty
    kRefused,        // the driver would not take the data: started empty
  };
  virtual ~DriverPipelineCache() = default;

  // Reads `path` into the cache every pipeline is created with. No pipeline may be created while
  // it runs (the caller makes sure: before the first draw). `note` says what was found.
  virtual Load LoadFile(const std::string& path, std::string* note) = 0;
  // Writes what the cache holds now to `path` (a temporary file, renamed over it). Any thread, also
  // while pipelines are being created. `bytes`: the size of the driver's data.
  virtual bool SaveFile(const std::string& path, size_t* bytes, std::string* note) = 0;
};

inline const char* LoadResultName(DriverPipelineCache::Load load) {
  switch (load) {
    case DriverPipelineCache::Load::kLoaded: return "loaded";
    case DriverPipelineCache::Load::kAbsent: return "absent";
    case DriverPipelineCache::Load::kDriverChanged: return "driver changed";
    case DriverPipelineCache::Load::kDamaged: return "damaged";
    case DriverPipelineCache::Load::kRefused: return "refused by the driver";
  }
  return "?";
}

}  // namespace nr
