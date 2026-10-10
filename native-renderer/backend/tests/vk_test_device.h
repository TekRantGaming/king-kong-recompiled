// The standalone test host's GPU: a Vulkan instance and device of its own
// (on lavapipe, Mesa's CPU driver, when present: no GPU needed), NVRHI
// created on that existing device the same way the plugin creates it on the
// SDK provider's, and helpers to read an image back.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/tests/readback.h"

namespace nr::test {

class VulkanTestDevice {
 public:
  // Validation: the Khronos validation layer when installed, plus NVRHI's
  // validation layer. Errors from either are counted (and printed).
  static std::unique_ptr<VulkanTestDevice> Create(bool validation = true);
  ~VulkanTestDevice();

  nvrhi::IDevice* device() const { return nvrhi_device_; }
  const std::string& device_name() const { return device_name_; }
  bool khronos_validation() const { return khronos_validation_; }
  // Validation errors so far (Vulkan layer + NVRHI).
  int errors() const;

  void Execute(nvrhi::ICommandList* command_list);
  void WaitIdle();

  using Pixels = nr::test::Pixels;
  Pixels ReadBack(nvrhi::ITexture* texture) { return ReadBackTexture(nvrhi_device_, texture); }

 private:
  VulkanTestDevice() = default;
  bool Init(bool validation);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  nvrhi::DeviceHandle nvrhi_device_;
  std::string device_name_;
  bool khronos_validation_ = false;
};


}  // namespace nr::test
