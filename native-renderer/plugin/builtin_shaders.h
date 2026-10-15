#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace nr {
// The D3D library's vertex and two pixel containers are in the player's XEX, not xeshaders.bin.
std::vector<std::vector<uint8_t>> BuiltinShaderContainers(std::span<const uint8_t> xex);
std::vector<std::vector<uint8_t>> ReadBuiltinShaderContainers(const std::filesystem::path& xex);
}  // namespace nr
