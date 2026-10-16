#include "backend/memory_budget.h"
#include <cstdio>
#include <cstdlib>

static int cases = 0;
static void Check(bool condition) {
  ++cases;
  if (!condition) { std::fprintf(stderr, "case %d failed\n", cases); std::exit(1); }
}
int main() {
  using namespace nr;
  constexpr uint64_t GiB = 1ull << 30, MiB = 1ull << 20;
  Check(!MemoryPressure({}));
  Check(!MemoryPressure({0, UINT64_MAX, true}));
  Check(!MemoryPressure({4 * GiB, 4 * GiB, false}));
  Check(!MemoryPressure({4 * GiB, 4 * GiB - 256 * MiB - 1, true}));
  Check(MemoryPressure({4 * GiB, 4 * GiB - 256 * MiB, true}));
  Check(MemoryPressure({4 * GiB, 5 * GiB, true}));
  Check(!MemoryPressure({100, 89, true}));
  Check(MemoryPressure({100, 90, true}));
  Check(MemoryPressure({1, 1, true}));
  Check(!MemoryPressure({UINT64_MAX, 0, true}));
  Check(MemoryPressure({UINT64_MAX, UINT64_MAX, true}));
  for (uint64_t cap : {4 * GiB, 6 * GiB, 8 * GiB}) {
    auto m = CapMemoryBudget({12 * GiB, 5 * GiB, true}, cap);
    Check(m.valid && m.budget == cap && m.usage == 5 * GiB);
  }
  Check(CapMemoryBudget({2 * GiB, GiB, true}, 4 * GiB).budget == 2 * GiB);
  Check(CapMemoryBudget({2 * GiB, GiB, true}, 0).budget == 2 * GiB);
  Check(!CapMemoryBudget({}, GiB).valid);
  Check(!CanEvictClearTarget(false, false, 0, 1000)); // rendered contents persist
  Check(!CanEvictClearTarget(true, true, 0, 1000)); // AO/upscale writers
  Check(!CanEvictClearTarget(true, false, UINT64_MAX, 1000));
  Check(!CanEvictClearTarget(true, false, 1000, 1000));
  Check(!CanEvictClearTarget(true, false, 1000, 999)); // no unsigned underflow
  Check(!CanEvictClearTarget(true, false, 1000, 1120));
  Check(CanEvictClearTarget(true, false, 1000, 1121));
  Check(ReducedMemoryScale(2, 1) == 1.5f);
  Check(ReducedMemoryScale(1.125f, 1) == 1);
  Check(ReducedMemoryScale(1, 1) == 1);
  Check(ReducedMemoryScale(0.5f, 0.5f) == 0.5f);
  Check(ReducedMemoryScale(4, 1) == 3);
  Check(ReducedMemoryScale(2, 1, 0.5f) == 1);
  const MemoryBudget large{12 * GiB, 12 * GiB - 600 * MiB, true};
  const MemoryBudget small{256 * MiB, 236 * MiB, true};
  Check(!MemoryPressure(large)); Check(MemoryPressure(small));
  Check(TighterMemoryBudget(small, large)); Check(!TighterMemoryBudget(large, small));
  Check(TighterMemoryBudget(large, {})); Check(!TighterMemoryBudget({}, large));
  Check(ParseMemoryBudgetCap("4") == 4 * GiB && ParseMemoryBudgetCap("6") == 6 * GiB && ParseMemoryBudgetCap("8") == 8 * GiB);
  Check(ParseMemoryBudgetCap("5") == 0 && ParseMemoryBudgetCap(nullptr) == 0);
  // Texture packs: a quarter of the budget (no fixed 512 MiB cap any more), bounded by free space.
  Check(TexturePackLimit({}, 0) == kTexturePackFallbackLimit);
  Check(TexturePackLimit({12 * GiB, 0, false}, 0) == 256 * MiB);
  Check(TexturePackLimit({12 * GiB, 2 * GiB, true}, 0) == 3 * GiB);
  Check(TexturePackLimit({11 * GiB, 1 * GiB, true}, 0) == 11 * GiB / 4);
  Check(TexturePackLimit({24 * GiB, 2 * GiB, true}, 0) == 6 * GiB);
  // 4 GiB budget, 3 GiB used: 1 GiB - 256 MiB reserve - 16 MiB strips, plus what is resident.
  Check(TexturePackLimit({4 * GiB, 3 * GiB, true}, 0) == 752 * MiB);
  Check(TexturePackLimit({4 * GiB, 3 * GiB, true}, 200 * MiB) == 952 * MiB);
  Check(TexturePackLimit({4 * GiB, 4 * GiB, true}, 300 * MiB) == 300 * MiB);
  Check(TexturePackLimit({4 * GiB, 5 * GiB, true}, 0) == 0);
  Check(TexturePackLimit({1 * GiB, 100 * MiB, true}, 0) == 256 * MiB);
  std::printf("memory budget: %d cases passed\n", cases);
}
