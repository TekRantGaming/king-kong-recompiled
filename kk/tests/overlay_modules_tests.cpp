// Task 368: the third party overlays and hooks a crash report names (overlay_modules.h): ReShade and its addons
// (RenoDX), RTSS, Overwolf, Special K and others, and a proxy DLL in the game's folder; the game's own and
// Windows' modules are never named.
// Usage: kk-overlay-modules-tests
#include "overlay_modules.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}
}  // namespace

int main() {
  using kk::OverlayModule;
  Check(OverlayModule("reshade64.dll", false) == "ReShade", "ReShade");
  Check(OverlayModule("renodx-dlss5.addon64", false) == "RenoDX (ReShade addon)", "RenoDX addon (report G6)");
  Check(OverlayModule("some_effect.addon64", false) == "ReShade addon", "other ReShade addon");
  Check(OverlayModule("rtsshooks64.dll", false) == "RivaTuner Statistics Server", "RTSS (reports G3, G6)");
  Check(OverlayModule("ow-graphics-vulkan.dll", false) == "Overwolf", "Overwolf graphics hook");
  Check(OverlayModule("overwolfhelper64.dll", false) == "Overwolf", "Overwolf helper");
  Check(OverlayModule("specialk64.dll", false) == "Special K", "Special K");
  Check(OverlayModule("gameoverlayrenderer64.dll", false) == "Steam overlay", "Steam overlay");
  Check(OverlayModule("discordhook64.dll", false) == "Discord overlay", "Discord overlay");
  // A dxgi.dll in the game's folder is a proxy; the system's dxgi.dll is not.
  Check(!OverlayModule("dxgi.dll", true).empty(), "proxy dxgi.dll beside the game");
  Check(OverlayModule("dxgi.dll", false).empty(), "the system dxgi.dll");
  Check(OverlayModule("d3d12.dll", false).empty(), "the system d3d12.dll");
  // The game's own modules and drivers are never named.
  for (const char* own : {"king_kong.exe", "rexgpu-native.dll", "rexruntime.dll", "d3d12core.dll", "dxcompiler.dll",
                          "amd_fidelityfx_loader_dx12.dll", "nvngx_dlss.dll", "libxess.dll", "igd12um64xel.dll",
                          "nvwgf2umx.dll", "vulkan-1.dll", "ntdll.dll", "kernel32.dll"}) {
    Check(OverlayModule(own, true).empty() && OverlayModule(own, false).empty(), own);
  }
  if (failures) {
    std::fprintf(stderr, "%d of %d overlay module checks failed\n", failures, checks);
    return 1;
  }
  std::printf("PASS: %d overlay module checks\n", checks);
  return 0;
}
