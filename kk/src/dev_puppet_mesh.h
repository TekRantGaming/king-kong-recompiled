#pragma once
#if defined(KK_DEV_TOOLS)
#include <cstdint>
namespace kk {
void DevPuppetMeshArm(const uint8_t* base, uint32_t source, uint32_t copy, uint64_t epoch);
void DevPuppetMeshReset();
void DevPuppetMeshFrame(uint64_t frame);
void DevPuppetMeshTick(const uint8_t* base);
// Used around native D3D forwarding. Saves/restores the optional plugin's scope.
uint64_t DevPuppetMeshDrawBegin(const uint8_t* base);
void DevPuppetMeshDrawEnd(uint64_t previous);
}  // namespace kk
#endif
