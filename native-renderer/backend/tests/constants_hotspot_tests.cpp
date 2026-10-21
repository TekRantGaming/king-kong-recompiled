#include "backend/tests/prototype_shader_constants.h"
#include "backend/tests/prototype_avx2_hotspots.h"
#include "backend/tests/check.h"
#include "backend/tests/hotspot_bench.h"
#include <random>
using namespace nr;
NR_NOINLINE bool OldConstants(const uint8_t* raw, std::array<uint8_t, 4096>& last_raw,
                              std::array<float, 1024>& last) {
  bool changed = false;
  for (uint32_t block = 0; block < 4096; block += 64) {
    if (SameBytes64(raw + block, last_raw.data() + block)) continue;
    changed = true;
    std::memcpy(last_raw.data() + block, raw + block, 64);
    for (uint32_t i = block / 4; i < block / 4 + 16; ++i) last[i] = LoadBEFloat(raw + i * 4);
  }
  return changed;
}
NR_NOINLINE bool NewConstants(const uint8_t* raw, std::array<uint8_t, 4096>& last_raw,
                              std::array<float, 1024>& last) {
  return nr::test::prototype::UpdateShaderConstants(raw, last_raw, last);
}
NR_NOINLINE bool AvxConstants(const uint8_t* raw, std::array<uint8_t, 4096>& last_raw,
                              std::array<float, 1024>& last) {
#if NR_HOTSPOT_AVX2
  if (nr::test::prototype::Avx2Available()) return nr::test::prototype::Avx2Constants(raw, last_raw, last);
#endif
  return OldConstants(raw, last_raw, last);
}

TEST(ConstantsConversionPreservesEveryBitAndChangedFlag) {
  std::array<uint8_t, 4096 + 32> source{};
  std::array<uint8_t, 4096> old_raw{}, new_raw{}, avx_raw{};
  std::array<float, 1024> old_out{}, new_out{}, avx_out{};
  std::mt19937 rng(340);
  const uint32_t edges[] = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc01234, 0x7f801234,
                             1, 0x80000001, 0x7f7fffff, 0xff7fffff, 0x3f800000};
  for (unsigned i = 0; i < 20000; ++i) {
    uint8_t* raw = source.data() + i % 32;
    for (unsigned j = 0; j < i % 128; ++j)
      StoreBE32(raw + (rng() % 1024) * 4, i % 2 ? rng() : edges[rng() % std::size(edges)]);
    const bool changed = OldConstants(raw, old_raw, old_out);
    CHECK(changed == NewConstants(raw, new_raw, new_out));
    CHECK(changed == AvxConstants(raw, avx_raw, avx_out));
    CHECK(old_raw == new_raw);
    CHECK(old_raw == avx_raw);
    CHECK(std::memcmp(old_out.data(), new_out.data(), sizeof(old_out)) == 0);
    CHECK(std::memcmp(old_out.data(), avx_out.data(), sizeof(old_out)) == 0);
    CHECK(!NewConstants(raw, new_raw, new_out));
    CHECK(!AvxConstants(raw, avx_raw, avx_out));
  }
}

int main(int argc, char** argv) {
  if (nr::test::RunAll(1, argv)) return 1;
  if (!nr::test::BenchmarkRequested(argc, argv)) return 0;
  std::printf("AVX2 path %s\n", nr::test::prototype::Avx2Available() ? "available" : "unavailable");
  constexpr unsigned calls = 500000;
  for (unsigned changes : {0u, 1u, 4u, 64u}) {
    auto run = [&](auto fn) {
      std::array<uint8_t, 4096> raw{}, cache{};
      std::array<float, 1024> out{};
      uint64_t sum = 0;
      for (unsigned i = 0; i < calls; ++i) {
        for (unsigned j = 0; j < changes; ++j) StoreBE32(raw.data() + j * 64, i);
        sum += fn(raw.data(), cache, out);
      }
      nr::test::bench_sink = uint64_t(cache[0]);
      return sum;
    };
    const auto name = "constants-4096-" + std::to_string(changes) + "-changed-blocks";
    nr::test::Benchmark(name.c_str(), calls, [&] { return run(OldConstants); }, [&] { return run(NewConstants); });
    if (nr::test::prototype::Avx2Available())
      nr::test::Benchmark((name + "-avx2").c_str(), calls, [&] { return run(OldConstants); }, [&] { return run(AvxConstants); });
  }
}
