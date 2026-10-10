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

  // Reads a 2D R10G10B10A2_UNORM or RGBA8_UNORM image back as RGBA floats.
  struct Pixels {
    uint32_t width = 0, height = 0;
    std::vector<float> rgba;  // width * height * 4
    const float* at(uint32_t x, uint32_t y) const { return &rgba[(size_t(y) * width + x) * 4]; }
  };
  Pixels ReadBack(nvrhi::ITexture* texture);

 private:
  VulkanTestDevice() = default;
  bool Init(bool validation);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  nvrhi::DeviceHandle nvrhi_device_;
  std::string device_name_;
  bool khronos_validation_ = false;
};

// Writes an image as a binary PPM (for looking at failures).
bool WritePpm(const std::string& path, const VulkanTestDevice::Pixels& pixels);

}  // namespace nr::test
