#include "backend/tests/reference_motion_history.h"
#include "backend/tests/check.h"
#include "backend/tests/hotspot_bench.h"
#include <bit>
#include <random>
using namespace nr;
using MotionData = std::vector<VsConstants>;

TEST(MotionSearchMatchesOldBytesForFiniteAndExceptionalInputs) {
  std::mt19937 rng(340);
  const uint32_t edges[] = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc01234, 0x7f801234,
                             1, 0x80000001, 0x7f7fffff, 0xff7fffff, 0x3f800000};
  for (unsigned pass = 0; pass < 3000; ++pass) {
    MotionHistory next;
    nr::test::ReferenceMotionHistory old;
    MotionDrawKey key; key.vs_hash = pass;
    old.BeginFrame(10); next.BeginFrame(10);
    const unsigned count = 2 + rng() % 40, first = rng() % 1024, last = first + rng() % (1025 - first);
    std::vector<VsConstants> data(count);
    VsConstants common{};
    for (auto& f : common) f = pass % 3 ? float(int(rng() % 8) - 4) : std::bit_cast<float>(edges[rng() % std::size(edges)]);
    for (auto& c : data) {
      c = common;
      for (unsigned i = first; i < last; ++i)
        c[i] = pass % 5 ? float(int(rng() % 8) - 4) : std::bit_cast<float>(uint32_t(rng()));
      old.Record(key, c); next.Record(key, c);
    }
    old.BeginFrame(11); next.BeginFrame(11);
    for (unsigned q = 0; q < 8; ++q) {
      auto cur = data[rng() % count];
      if (q % 2) cur[rng() % 1024] = float(int(rng() % 8) - 4);
      if (q % 3 == 0) cur[1023] = std::bit_cast<float>(edges[rng() % std::size(edges)]);
      const unsigned ordinal = rng() % (count + 3), final_count = q % 4 == 0 ? count : 0;
      auto* a = old.FindPrevious(key, ordinal, cur, final_count);
      auto* b = next.FindPrevious(key, ordinal, cur, final_count);
      CHECK(a && b && std::memcmp(a->data(), b->data(), sizeof(VsConstants)) == 0);
    }
    // Missed keys and frame gaps must remain empty; same-frame BeginFrame is a no-op.
    old.BeginFrame(11); next.BeginFrame(11);
    old.BeginFrame(13); next.BeginFrame(13);
    CHECK(old.FindPrevious(key, 0, common) == next.FindPrevious(key, 0, common));
  }
}

TEST(MotionSharedRangesAreInvalidatedWhenStorageIsRecycled) {
  MotionHistory next;
  nr::test::ReferenceMotionHistory old;
  std::mt19937 rng(340);
  uint64_t frame = 0;
  for (unsigned pass = 0; pass < 200; ++pass) {
    frame += pass % 11 ? 1 : 2;
    old.BeginFrame(frame); next.BeginFrame(frame);
    for (unsigned group = 0; group < 4; ++group) {
      MotionDrawKey key; key.vs_hash = group + 1;
      for (unsigned draw = 0; draw < 2 + pass % 8; ++draw) {
        VsConstants c{};
        const unsigned begin = (pass * 317) % 1024;
        c.fill(float(pass % 5));
        for (unsigned k = begin; k < std::min(begin + 32, 1024u); ++k) c[k] = float(rng() % 8);
        const auto aord = old.Record(key, c), bord = next.Record(key, c);
        CHECK_EQ(aord, bord);
        const auto* a = old.FindPrevious(key, aord, c);
        const auto* b = next.FindPrevious(key, bord, c);
        CHECK((a == nullptr) == (b == nullptr));
        if (a && b) CHECK(std::memcmp(a->data(), b->data(), sizeof(c)) == 0);
      }
    }
  }
}

template<class History>
NR_NOINLINE uint64_t MotionRun(const MotionData& constants, bool move_camera) {
  History h;
  uint64_t sum = 0;
  for (uint64_t frame = 0; frame < 100; ++frame) {
    h.BeginFrame(frame);
    for (unsigned i = 0; i < 256; ++i) {
      MotionDrawKey key; key.vs_hash = 1 + i / 8;
      auto c = constants[i];
      if (move_camera) c[0] = float(frame) * 0.125f;
      const auto ordinal = h.Record(key, c);
      if (const auto* p = h.FindPrevious(key, ordinal, c)) sum += std::bit_cast<uint32_t>((*p)[64]);
    }
  }
  return sum;
}

template<class History>
NR_NOINLINE uint64_t LookupRun(History& h, const MotionData& constants) {
  uint64_t sum = 0;
  for (unsigned loop = 0; loop < 1000; ++loop) for (unsigned i = 0; i < 256; ++i) {
    MotionDrawKey key; key.vs_hash = 1 + i / 8;
    if (const auto* p = h.FindPrevious(key, i % 8, constants[i]))
      sum += std::bit_cast<uint32_t>((*p)[64]);
  }
  return sum;
}

int main(int argc, char** argv) {
  if (nr::test::RunAll(1, argv)) return 1;
  if (!nr::test::BenchmarkRequested(argc, argv)) return 0;
  MotionData constants(256);  // SceneAo's exact 256-draw cap, 4 KiB per draw; keep off Windows' 1 MiB stack
  for (unsigned i = 0; i < 256; ++i) {
    for (unsigned k = 0; k < 16; ++k) constants[i][k] = float(k) / 16;
    for (unsigned k = 64; k < 128; ++k) constants[i][k] = float(i % 8) + float(k) / 1024;
  }
  for (bool moving : {false, true}) {
    nr::test::Benchmark(moving ? "motion-256-moving-camera" : "motion-256-static-camera", 100 * 256,
      [&] { return MotionRun<nr::test::ReferenceMotionHistory>(constants, moving); },
      [&] { return MotionRun<MotionHistory>(constants, moving); });
    nr::test::ReferenceMotionHistory old;
    MotionHistory next;
    old.BeginFrame(0); next.BeginFrame(0);
    for (unsigned i = 0; i < 256; ++i) {
      MotionDrawKey key; key.vs_hash = 1 + i / 8;
      old.Record(key, constants[i]); next.Record(key, constants[i]);
    }
    old.BeginFrame(1); next.BeginFrame(1);
    auto current = constants;
    if (moving) for (auto& c : current) c[0] += 0.125f;
    nr::test::Benchmark(moving ? "motion-lookup-moving-camera" : "motion-lookup-static-camera", 1000 * 256,
      [&] { return LookupRun(old, current); }, [&] { return LookupRun(next, current); });
  }
  // A control case with no shared finite ranges: the optional cache must not rely on sparse data.
  for (unsigned i = 0; i < 256; ++i)
    for (unsigned k = 0; k < 1024; ++k) constants[i][k] = float(i % 8) + float(k) / 1024;
  nr::test::Benchmark("motion-256-dense-control", 100 * 256,
    [&] { return MotionRun<nr::test::ReferenceMotionHistory>(constants, false); },
    [&] { return MotionRun<MotionHistory>(constants, false); });
}
