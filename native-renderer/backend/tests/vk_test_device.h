// The standalone test host's GPU: a Vulkan instance and device of its own
// (on lavapipe, Mesa's CPU driver, when present: no GPU needed), NVRHI
// created on that existing device the same way the plugin creates it on the
// SDK provider's, and helpers to read an image back.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/tests/readback.h"

namespace nr {
class VulkanPipelineCache;
}

namespace nr::test {

class VulkanTestDevice {
 public:
  // Validation: the Khronos validation layer when installed, plus NVRHI's
  // validation layer. Errors from either are counted (and printed).
  // driver_pipeline_cache: NVRHI's device creates its pipelines through a VulkanPipelineCache of
  // ours (the plugin's --native_vk_pipeline_cache), pipeline_cache().
  // upscaler_needs: the instance and device also get what DLSS and XeSS ask for (upscaler_vulkan.h), as
  // the plugin's device does with an upscaler on.
  static std::unique_ptr<VulkanTestDevice> Create(bool validation = true, bool driver_pipeline_cache = false,
                                                  bool upscaler_needs = false);
  ~VulkanTestDevice();

  nvrhi::IDevice* device() const { return nvrhi_device_; }
  nr::VulkanPipelineCache* pipeline_cache() const { return pipeline_cache_.get(); }
  // vulkan.hpp's dispatcher is one for the process: point it at this device again (after another
  // test device was made).
  void MakeCurrent();
  const std::string& device_name() const { return device_name_; }
  bool khronos_validation() const { return khronos_validation_; }
  // Validation errors so far (Vulkan layer + NVRHI).
  int errors() const;

  void Execute(nvrhi::ICommandList* command_list);
  void WaitIdle();

  using Pixels = nr::test::Pixels;
  Pixels ReadBack(nvrhi::ITexture* texture) {
    std::lock_guard lock(queue_mutex_);
    return ReadBackTexture(nvrhi_device_, texture);
  }

 private:
  VulkanTestDevice() = default;
  bool Init(bool validation, bool driver_pipeline_cache, bool upscaler_needs);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<nr::VulkanPipelineCache> pipeline_cache_;  // destroyed after nvrhi_device_
  nvrhi::DeviceHandle nvrhi_device_;
  std::mutex queue_mutex_;
  std::string device_name_;
  bool khronos_validation_ = false;
};


}  // namespace nr::test
