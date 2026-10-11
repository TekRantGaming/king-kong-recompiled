// A VkPipelineCache kept in a file under the cache root (DriverPipelineCache on Vulkan).
//
// The application makes the cache before NVRHI's device and hands it over in
// nvrhi::vulkan::DeviceDesc::pipelineCache (a change to the vendored NVRHI, thirdparty/NOTES.md),
// so every pipeline the device creates goes through it. The file is a header of ours, then the
// data vkGetPipelineCacheData returned:
//
//   magic 'KVPC', version, header size, vendor ID, device ID, driver version,
//   pipelineCacheUUID[16], driverUUID[16], data size, a 64-bit hash of the data
//
// The data starts with Vulkan's own header (VkPipelineCacheHeaderVersionOne: vendor ID, device
// ID, pipelineCacheUUID). A file is used only when our header and Vulkan's both match the device:
// the same vendor and device, the same pipelineCacheUUID, the same driverUUID and driver version.
// Otherwise the driver changed (or the file is damaged): the cache starts empty and is filled
// again by the pipelines the run makes (the prewarm makes them all in the background), and the
// next save replaces the file.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "backend/driver_pipeline_cache.h"

namespace nr {

class VulkanPipelineCache final : public DriverPipelineCache {
 public:
  // What a saved cache must have been made by.
  struct Identity {
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t driver_version = 0;
    uint8_t pipeline_cache_uuid[VK_UUID_SIZE] = {};
    uint8_t driver_uuid[VK_UUID_SIZE] = {};
    bool operator==(const Identity&) const = default;
  };
  // Through vulkan.hpp's default dispatcher (vulkan_dispatch.h), initialised for the instance.
  static Identity QueryIdentity(VkPhysicalDevice physical_device);

  // An empty cache on `device`; null when it cannot be made. The dispatcher must be initialised for
  // the device.
  static std::unique_ptr<VulkanPipelineCache> Create(VkPhysicalDevice physical_device, VkDevice device);
  // Destroys the cache: after NVRHI's device (which only borrows it).
  ~VulkanPipelineCache() override;
  VulkanPipelineCache(const VulkanPipelineCache&) = delete;
  VulkanPipelineCache& operator=(const VulkanPipelineCache&) = delete;

  VkPipelineCache handle() const { return cache_; }
  const Identity& identity() const { return identity_; }

  Load LoadFile(const std::string& path, std::string* note) override;
  bool SaveFile(const std::string& path, size_t* bytes, std::string* note) override;
  // The driver's data as vkGetPipelineCacheData returns it now.
  std::vector<uint8_t> Data();

  // The file format without a device (tests).
  static std::vector<uint8_t> MakeFile(const Identity& identity, const std::vector<uint8_t>& data);
  // kLoaded with the data's offset in `file`, or why the file cannot be used here.
  static Load CheckFile(const std::vector<uint8_t>& file, const Identity& identity, std::string* note,
                        size_t* data_offset);

 private:
  VulkanPipelineCache() = default;
  VkDevice device_ = VK_NULL_HANDLE;
  VkPipelineCache cache_ = VK_NULL_HANDLE;
  Identity identity_;
  std::mutex save_mutex_;  // one save at a time (workers and shutdown)
};

}  // namespace nr
