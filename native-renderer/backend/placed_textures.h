// Task 304: textures in non-zeroed memory (D3D12 --native_d3d12_placed_textures, d3d12_host.cpp).
// A committed texture starts as zeros; a placed one in a reused range holds an old texture's texels. A
// guest texture whose conversion leaves some levels or layers unwritten (no mip data, a level below the
// fetch's base, a layer that could not be read) would then sample garbage there instead of black, so
// the game renderer writes zeros into those subresources when this flag is on (game_resources.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

namespace nr {

inline std::atomic<bool> g_texture_memory_not_zeroed{false};

// The (level, layer) pairs of a levels x layers texture that `written` (level, layer pairs) leaves out.
inline std::vector<std::pair<uint32_t, uint32_t>> MissingSubresources(
    uint32_t levels, uint32_t layers, const std::vector<std::pair<uint32_t, uint32_t>>& written) {
  std::vector<std::pair<uint32_t, uint32_t>> missing;
  for (uint32_t level = 0; level < levels; ++level)
    for (uint32_t layer = 0; layer < layers; ++layer) {
      bool found = false;
      for (const auto& w : written) found = found || (w.first == level && w.second == layer);
      if (!found) missing.emplace_back(level, layer);
    }
  return missing;
}

}  // namespace nr
