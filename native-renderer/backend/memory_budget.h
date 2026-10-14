// Live adapter budget, in bytes. Vulkan reports heap-wide usage; DXGI reports
// this process's usage against its process budget. Never mix those domains.
#pragma once
#include <algorithm>
#include <cstdint>

namespace nr {
struct MemoryBudget {
  uint64_t budget = 0, usage = 0;
  bool valid = false;
};

inline bool MemoryPressure(const MemoryBudget& m) {
  if (!m.valid || !m.budget) return false;
  const uint64_t reserve = std::min<uint64_t>(256ull << 20, m.budget / 10);
  return m.usage >= m.budget - reserve;
}

// Only a supported live query can activate adaptation. A synthetic cap never
// invents usage, and cannot raise the real budget.
inline MemoryBudget CapMemoryBudget(MemoryBudget m, uint64_t cap) {
  if (m.valid && cap) m.budget = std::min(m.budget, cap);
  return m;
}

inline bool CanEvictClearTarget(bool clear_only, bool has_external_writer,
                               uint64_t used_frame, uint64_t frame) {
  return clear_only && !has_external_writer && used_frame != UINT64_MAX &&
         frame > used_frame && frame - used_frame > 120;
}
}  // namespace nr
