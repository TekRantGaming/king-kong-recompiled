// Byte swaps as the Xenos applies them when it reads memory (the endian field of a fetch constant).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "kknr/xenos.h"

namespace kknr {

inline uint16_t Swap16(uint16_t v) { return uint16_t(v << 8 | v >> 8); }
inline uint32_t Swap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// Applies an endian mode to one 32-bit unit as read from memory (little-endian host load of guest bytes).
inline uint32_t ApplyEndian32(uint32_t v, Endian endian) {
  switch (endian) {
    case Endian::k8in16:
      return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
    case Endian::k8in32:
      return Swap32(v);
    case Endian::k16in32:
      return (v >> 16) | (v << 16);
    default:
      return v;
  }
}

// Copies bytes applying the endian mode to every 32-bit unit (bytes must be a multiple of 4, or of 2 for
// k8in16). src and dst may be the same.
void CopySwap(Endian endian, const void* src, void* dst, size_t bytes);

// 16- and 32-bit index buffers: the GPU swaps each index as the draw's endian field says (8in16 for 16-bit
// indices, 8in32 for 32-bit); big-endian guest indices become little-endian host indices.
void CopySwapIndices16(const void* src, void* dst, size_t count);
void CopySwapIndices32(const void* src, void* dst, size_t count);

}  // namespace kknr
