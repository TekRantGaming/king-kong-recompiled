// Guest memory as the backend sees it: big-endian data at 32-bit guest
// addresses. The plugin implements this over rex::memory::Memory; the tests
// over a synthetic buffer. Nothing here depends on the SDK.
#pragma once

#include <cstdint>
#include <cstring>

namespace nr {

class GuestMemory {
 public:
  virtual ~GuestMemory() = default;
  // Host pointer for a guest virtual address (what the engine passes around),
  // or null when the address is not mapped.
  virtual const uint8_t* Virtual(uint32_t address) const = 0;
  // Host pointer for a GPU physical address (vertex / index / texture data).
  virtual const uint8_t* Physical(uint32_t address) const = 0;
};

inline uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint16_t LoadBE16(const uint8_t* p) { return uint16_t((uint32_t(p[0]) << 8) | p[1]); }
inline float LoadBEFloat(const uint8_t* p) {
  uint32_t u = LoadBE32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
inline void StoreBE16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
// Whether the 64 bytes at a and b are equal, with inline loads (a memcmp of them is a library call;
// the renderer compares shader constants and the device in such blocks at every draw).
inline bool SameBytes64(const uint8_t* a, const uint8_t* b) {
  uint64_t x[8], y[8];
  std::memcpy(x, a, sizeof(x));
  std::memcpy(y, b, sizeof(y));
  uint64_t differ = 0;
  for (int i = 0; i < 8; ++i) differ |= x[i] ^ y[i];
  return differ == 0;
}
inline void StoreBEFloat(uint8_t* p, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  StoreBE32(p, u);
}

}  // namespace nr
