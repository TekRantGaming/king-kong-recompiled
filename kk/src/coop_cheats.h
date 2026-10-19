#pragma once
#include <cstdint>
#include <compare>
#include <atomic>
#include <optional>

namespace kk::coop {
// Low eight bits follow kCheats; bit 8 is the master kk_cheats switch.
struct CheatSet {
  uint16_t bits = 0;
  bool Enabled() const { return (bits & 0x100) != 0; }
  bool Chosen(unsigned index) const { return index < 8 && (bits & (1u << index)); }
  auto operator<=>(const CheatSet&) const = default;
};
inline bool ValidCheatSet(CheatSet set) { return (set.bits & ~0x1ffu) == 0; }
// Value-only overlay: neither applying nor restoring ever mutates saved choices.
inline CheatSet SessionCheats(CheatSet own, std::optional<CheatSet> host) { return host.value_or(own); }
class CheatOverride {
 public:
  bool Apply(std::optional<CheatSet> host) {
    if (host && !ValidCheatSet(*host)) return false;
    host_.store(host ? 0x10000u | host->bits : 0); return true;
  }
  std::optional<CheatSet> Snapshot() const {
    const auto bits = host_.load();
    return bits & 0x10000 ? std::optional<CheatSet>(CheatSet{uint16_t(bits)}) : std::nullopt;
  }
 private:
  std::atomic<uint32_t> host_{0};
};
} // namespace kk::coop
