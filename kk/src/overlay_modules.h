#pragma once

// Task 368 (owner, 23 Oct; crash reports G5, G6): third party overlays and hooks found loaded in the game's process
// are named in the crash report's text (crash_dump.cpp), so the server data shows when a crash sits in one.
// Nothing is shown to players. Only file names are used, never a folder.

#include <string>
#include <string_view>

namespace kk {

// `name`: a loaded module's file name in lower case ("rtsshooks64.dll"). `beside_game`: it lives in the game's own
// folder (a proxy dxgi.dll there is how ReShade and Special K are often installed). Returns what it is, or "".
inline std::string OverlayModule(std::string_view name, bool beside_game) {
  const auto ends = [&](std::string_view s) { return name.size() >= s.size() && name.substr(name.size() - s.size()) == s; };
  const auto starts = [&](std::string_view s) { return name.substr(0, s.size()) == s; };
  if (name == "reshade64.dll" || name == "reshade32.dll" || name == "reshade.dll") return "ReShade";
  if (ends(".addon64") || ends(".addon32") || ends(".addon"))
    return starts("renodx") ? "RenoDX (ReShade addon)" : "ReShade addon";
  if (name == "rtsshooks64.dll" || name == "rtsshooks.dll") return "RivaTuner Statistics Server";
  if (starts("ow-graphics") || name.find("overwolf") != std::string_view::npos) return "Overwolf";
  if (name == "specialk64.dll" || name == "specialk32.dll") return "Special K";
  if (name == "discordhook64.dll") return "Discord overlay";
  if (name == "gameoverlayrenderer64.dll") return "Steam overlay";
  if (name == "graphics-hook64.dll") return "OBS game capture";
  if (name == "nvspcap64.dll") return "NVIDIA overlay";
  if (name == "fraps64.dll") return "Fraps";
  if (beside_game && (name == "dxgi.dll" || name == "d3d11.dll" || name == "d3d12.dll" || name == "d3d9.dll" ||
                      name == "opengl32.dll" || name == "dinput8.dll"))
    return "a proxy DLL beside the game (often ReShade or Special K)";
  return {};
}

}  // namespace kk
