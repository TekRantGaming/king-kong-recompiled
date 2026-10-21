#include "backend/tests/prototype_device_capture.h"
#include "backend/tests/prototype_avx2_hotspots.h"
#include "backend/render_thread.h"
#include "backend/tests/check.h"
#include "backend/tests/hotspot_bench.h"
#include <random>
using namespace nr;

NR_NOINLINE void OldCapture(const uint8_t* live, uint8_t* sent, bool all,
                            std::vector<uint16_t>& out, std::vector<uint8_t>& bytes) {
  const auto& blocks = ThreadedSink::DeviceBlocks();
  for (size_t i = 0; i < blocks.size(); ++i) {
    const auto& b = blocks[i];
    if (!all && (b.size == 64 ? SameBytes64(live + b.offset, sent + b.offset)
                             : std::memcmp(live + b.offset, sent + b.offset, b.size) == 0)) continue;
    std::memcpy(sent + b.offset, live + b.offset, b.size);
    out.push_back(uint16_t(i));
    bytes.insert(bytes.end(), live + b.offset, live + b.offset + b.size);
  }
}
NR_NOINLINE void NewCapture(const uint8_t* live, uint8_t* sent, bool all,
                            std::vector<uint16_t>& out, std::vector<uint8_t>& bytes) {
  nr::test::prototype::CaptureDeviceImage(live, sent, all, out, bytes);
}
NR_NOINLINE void AvxCapture(const uint8_t* live, uint8_t* sent, bool all,
                           std::vector<uint16_t>& out, std::vector<uint8_t>& bytes) {
#if NR_HOTSPOT_AVX2
  if (nr::test::prototype::Avx2Available()) {
    nr::test::prototype::Avx2Capture(live, sent, all, out, bytes);
    return;
  }
#endif
  OldCapture(live, sent, all, out, bytes);
}

TEST(CaptureDeviceBytesAndBlockOrderMatchForRandomAndEveryBoundary) {
  std::array<uint8_t, 20608> live{}, a{}, b{}, c{};
  std::vector<uint16_t> ab, bb, cb;
  std::vector<uint8_t> ad, bd, cd;
  auto check = [&](bool all) {
    ab.clear(); bb.clear(); ad.clear(); bd.clear(); cb.clear(); cd.clear();
    OldCapture(live.data(), a.data(), all, ab, ad);
    NewCapture(live.data(), b.data(), all, bb, bd);
    AvxCapture(live.data(), c.data(), all, cb, cd);
    CHECK(a == b); CHECK(ab == bb); CHECK(ad == bd);
    CHECK(a == c); CHECK(ab == cb); CHECK(ad == cd);
  };
  check(true); check(false);
  for (const auto& block : ThreadedSink::DeviceBlocks()) {
    for (unsigned offset : {unsigned(block.offset - 1), unsigned(block.offset),
                             unsigned(block.offset + block.size - 1), unsigned(block.offset + block.size)}) {
      live[offset] ^= 0xff;
      check(false);
    }
  }
  std::mt19937 rng(340);
  for (unsigned i = 0; i < 20000; ++i) {
    for (unsigned j = 0; j < i % 37; ++j) live[rng() % live.size()] = uint8_t(rng());
    check(i % 997 == 0);  // changed device: all blocks even if byte-identical
  }
}

int main(int argc, char** argv) {
  if (nr::test::RunAll(1, argv)) return 1;
  if (!nr::test::BenchmarkRequested(argc, argv)) return 0;
  std::printf("AVX2 path %s\n", nr::test::prototype::Avx2Available() ? "available" : "unavailable");
  constexpr unsigned calls = 300000;
  for (unsigned changed : {0u, 1u, 4u, 64u}) {
    auto run = [&](auto fn) {
      std::array<uint8_t, 20608> live{}, sent{};
      std::vector<uint16_t> blocks;
      std::vector<uint8_t> bytes;
      uint64_t sum = 0;
      fn(live.data(), sent.data(), true, blocks, bytes);  // retained capacity, as reused commands
      for (unsigned i = 0; i < calls; ++i) {
        for (unsigned j = 0; j < changed; ++j)
          std::memcpy(live.data() + 1152 + j * 64, &i, 4);
        blocks.clear(); bytes.clear();
        fn(live.data(), sent.data(), i % 1572 == 0, blocks, bytes);
        sum += bytes.size() + blocks.size();
      }
      return sum;
    };
    const std::string name = "capture-device-" + std::to_string(changed) + "-changed-blocks";
    nr::test::Benchmark(name.c_str(), calls, [&] { return run(OldCapture); }, [&] { return run(NewCapture); });
    if (nr::test::prototype::Avx2Available())
      nr::test::Benchmark((name + "-avx2").c_str(), calls, [&] { return run(OldCapture); }, [&] { return run(AvxCapture); });
  }
}
