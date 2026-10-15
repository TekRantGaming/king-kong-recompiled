#include "settings.h"
#include "platform.h"
#include "hardware_recommendation.h"
#include <cstdlib>
#include <iostream>

// Only settings/cvar logic. No window, GPU, game or system detection.
namespace kk {
GpuInfo DetectGpu() { return {}; }
HardwareProfile DetectHardware() { return {}; }
}

void Equals(const char* name, const char* value) {
  if (rex::cvar::GetFlagByName(name) != value) {
    std::cerr << "Unexpected " << name << '\n';
    std::exit(1);
  }
}

int main() {
  rex::cvar::ResetAllToDefaults();
  rex::cvar::SetFlagFromCommandLine("d3d12_allow_variable_refresh_rate_and_tearing", "true");
  rex::cvar::SetFlagFromCommandLine("vulkan_allow_present_mode_immediate", "false");
  rex::cvar::SetFlagFromCommandLine("vulkan_allow_present_mode_mailbox", "true");
  rex::cvar::SetFlagFromCommandLine("vulkan_allow_present_mode_fifo_relaxed", "false");
  rex::cvar::SetFlagByName("kk_frame_rate", "144");
  kk::ApplyVSyncSettings();
  Equals("vulkan_allow_present_mode_immediate", "false");
  Equals("vulkan_allow_present_mode_mailbox", "true");
  Equals("vulkan_allow_present_mode_fifo_relaxed", "false");
  Equals("kk_frame_rate", "144");
  rex::cvar::SetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing", "false");
  kk::ApplyVSyncSettings();
  Equals("vulkan_allow_present_mode_immediate", "false");
  Equals("vulkan_allow_present_mode_mailbox", "false");
  Equals("vulkan_allow_present_mode_fifo_relaxed", "false");
  Equals("kk_frame_rate", "144");
  rex::cvar::SetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing", "true");
  kk::ApplyVSyncSettings();
  Equals("vulkan_allow_present_mode_immediate", "true");
  Equals("vulkan_allow_present_mode_mailbox", "true");
  Equals("vulkan_allow_present_mode_fifo_relaxed", "true");
  std::cout << "VSync mode/override tests passed\n";
}
