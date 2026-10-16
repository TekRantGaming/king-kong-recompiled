// Live adapter budget, in bytes. Vulkan reports heap-wide usage; DXGI reports
// this process's usage against its process budget. Never mix those domains.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace nr {
struct MemoryBudget {
  uint64_t budget = 0, usage = 0;
  bool valid = false;
};

inline uint64_t MemoryBudgetReserve(uint64_t budget) {
  return std::min<uint64_t>(256ull << 20, budget / 10);
}
inline bool MemoryPressure(const MemoryBudget& m) {
  if (!m.valid || !m.budget) return false;
  const uint64_t reserve = MemoryBudgetReserve(m.budget);
  return m.usage >= m.budget - reserve;
}
inline bool TighterMemoryBudget(const MemoryBudget& a, const MemoryBudget& b) {
  if (!a.valid || !a.budget) return false;
  if (!b.valid || !b.budget) return true;
  auto margin = [](const MemoryBudget& m) {
    return (double(m.budget) - double(m.usage)) / double(std::max<uint64_t>(MemoryBudgetReserve(m.budget), 1));
  };
  return margin(a) < margin(b);
}
inline uint64_t ParseMemoryBudgetCap(const char* v) {
  if (!v) return 0;
  if (std::strcmp(v, "4") == 0) return 4ull << 30;
  if (std::strcmp(v, "6") == 0) return 6ull << 30;
  if (std::strcmp(v, "8") == 0) return 8ull << 30;
  return 0;
}

// Only a supported live query can activate adaptation. A synthetic cap never
// invents usage, and cannot raise the real budget.
inline MemoryBudget CapMemoryBudget(MemoryBudget m, uint64_t cap) {
  if (m.valid && cap) m.budget = std::min(m.budget, cap);
  return m;
}

// Player replacement textures (DECISIONS 16 Oct: 4x packs) may hold at most a
// quarter of the effective budget, and never more than the replacements already
// resident plus what is free after the normal reserve and 16 MiB of upload strip
// headroom. Without a live budget query the size and use of the card are unknown,
// so a conservative fixed cap applies (originals render beyond it).
constexpr uint64_t kTexturePackFallbackLimit = 256ull << 20;
inline uint64_t TexturePackLimit(const MemoryBudget& m, uint64_t resident) {
  if (!m.valid || !m.budget) return kTexturePackFallbackLimit;
  const uint64_t reserve = MemoryBudgetReserve(m.budget) + (16ull << 20);
  const uint64_t spare = m.budget > m.usage && m.budget - m.usage > reserve ? m.budget - m.usage - reserve : 0;
  return std::min(m.budget / 4, resident + spare);
}

inline bool CanEvictClearTarget(bool clear_only, bool has_external_writer,
                               uint64_t used_frame, uint64_t frame) {
  return clear_only && !has_external_writer && used_frame != UINT64_MAX &&
         frame > used_frame && frame - used_frame > 120;
}

inline float ReducedMemoryScale(float current, float floor, float factor = 0.75f) {
  return std::max(floor, current * factor);
}
}  // namespace nr
