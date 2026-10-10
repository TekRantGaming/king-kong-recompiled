// Developer-tool output: DDS files in the host format (every level and layer, viewable in RenderDoc, Visual
// Studio or texconv), uncompressed PNG previews, and a host-data -> RGBA8 decoder for the previews.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kknr/texture_convert.h"

namespace kknr_tools {

bool WritePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
bool WriteDds(const std::string& path, const kknr::HostTextureData& texture);
uint32_t DxgiFormat(kknr::HostFormat format);

// One depth slice of a host subresource as RGBA8, with the plan's view swizzle applied (what a shader
// sampling the SRV would see, before the shader's own sign / gamma / exp fix-ups). Floats are clamped to
// [0, 1]; signed values map -1..1 to 0..255; integers clamp to 0..255.
std::vector<uint8_t> HostToRgba8(const kknr::HostTextureData& texture, const kknr::HostSubresource& sub, uint32_t z);

// FNV-1a of the host data, to compare conversions across runs and machines.
uint64_t Fnv1a64(const uint8_t* data, size_t size);

}  // namespace kknr_tools
