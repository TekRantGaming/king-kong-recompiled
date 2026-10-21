#include "backend/tests/check.h"
#include "backend/tests/hotspot_bench.h"
#include <random>

NR_NOINLINE void OldCopy(uint8_t* dest, const uint8_t* source, size_t bytes) { std::memcpy(dest, source, bytes); }
// A deliberately test-only prototype: byte comparison can save a copy only if the destination
// already owns all its bytes. It cannot be used for fresh/uninitialised upload storage.
NR_NOINLINE void ComparedCopy(uint8_t* dest, const uint8_t* source, size_t bytes) {
  if (std::memcmp(dest, source, bytes)) std::memcpy(dest, source, bytes);
}
TEST(ComparedCopyMatchesMemcpyIncludingUnalignedInputsAndSizes) {
  std::mt19937 rng(340);
  std::vector<uint8_t> source(16384 + 64), a(source.size()), b(source.size());
  for (unsigned i = 0; i < 20000; ++i) {
    const size_t bytes = i < 512 ? i : rng() % 16385, offset = rng() % 64;
    for (size_t j = 0; j < bytes; ++j) source[j + offset] = uint8_t(rng());
    OldCopy(a.data() + offset, source.data() + offset, bytes);
    ComparedCopy(b.data() + offset, source.data() + offset, bytes);
    CHECK(a == b);
    ComparedCopy(b.data() + offset, source.data() + offset, bytes);
    CHECK(a == b);
  }
}
int main(int argc, char** argv) {
  if (nr::test::RunAll(1, argv)) return 1;
  if (!nr::test::BenchmarkRequested(argc, argv)) return 0;
  for (size_t bytes : {size_t(64), size_t(4096)}) for (unsigned changed_percent : {0u, 60u, 100u}) {
    constexpr unsigned calls = 1000000;
    auto run = [&](auto fn) {
      // Cycling records, rather than repeatedly writing one destination: reused frame records.
      std::vector<uint8_t> source(bytes * 256), dest(source.size());
      uint64_t sum = 0;
      for (unsigned i = 0; i < calls; ++i) {
        const size_t at = (i % 256) * bytes;
        if (i % 100 < changed_percent) std::memcpy(source.data() + at, &i, 4);
        fn(dest.data() + at, source.data() + at, bytes);
        sum += dest[at];
      }
      return sum;
    };
    const auto name = "copy-" + std::to_string(bytes) + "-changed-" + std::to_string(changed_percent) + "pct";
    nr::test::Benchmark(name.c_str(), calls, [&] { return run(OldCopy); }, [&] { return run(ComparedCopy); });
  }
}
