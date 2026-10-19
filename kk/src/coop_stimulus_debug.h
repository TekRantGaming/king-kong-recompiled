#pragma once
#include <cstdint>

// Read-only native stimulus diagnostics. Runtime callers are KK_DEV_TOOLS only;
// readers are injected so bounds/generation handling can be checked without a game.
namespace kk::coop::stimulus_debug {
constexpr uint32_t kPools = 0x82C8EEF0, kStride = 116;
inline bool Address(uint32_t p, uint64_t bytes) {
  return p >= 0x40000000 && p < 0xA0000000 && !(p & 3) && bytes <= 0xA0000000ull-p;
}
struct Pool {
  uint32_t type = 0, entries = 0, count = 0, capacity = 0;
  bool valid = false;
};
template <class U32> Pool ReadPool(U32 u32, uint32_t type) {
  Pool p; p.type = type;
  if (!type || type >= 128) return p;
  const auto table = kPools+16*type;
  p.entries = u32(table); p.count = u32(table+8); p.capacity = u32(table+12);
  p.valid = p.count <= p.capacity && p.capacity <= 0x1000000 &&
      Address(p.entries, uint64_t(p.count)*kStride);
  return p;
}
struct Record {
  uint32_t id = UINT32_MAX, generation = 0, address = 0, flags = 0, source = 0, target = 0;
  bool valid = false, enumerable = false;
};
template <class U32> Record ReadSlot(U32 u32, const Pool& p, uint32_t index) {
  Record r;
  if (!p.valid || index >= p.count) return r;
  r.id = (p.type << 24)|index; r.address = p.entries+index*kStride;
  r.generation = u32(r.address+112); r.flags = u32(r.address);
  // 8271E600 zeros generation when freeing; 8271E450 skips pending/free records.
  r.valid = r.generation != 0 && !(r.flags & 4);
  r.enumerable = r.valid && !(r.flags & 1);
  if (r.valid) { r.source = u32(r.address+8); r.target = u32(r.address+12); }
  return r;
}
template <class U32> Record ReadHandle(U32 u32, uint32_t handle) {
  if (!Address(handle, 8)) return {};
  const auto id = u32(handle), generation = u32(handle+4);
  auto r = ReadSlot(u32, ReadPool(u32, id >> 24), id & 0xFFFFFF);
  if (!r.valid || r.generation != generation) return {};
  // Source (8271E968) and selector-0 target (8271E9D0) are DIFFERENT fields.
  return r;
}
} // namespace kk::coop::stimulus_debug
