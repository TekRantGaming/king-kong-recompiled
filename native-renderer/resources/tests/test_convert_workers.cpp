#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

#include "kknr/texture_convert.h"
#include "test.h"

using namespace kknr;

namespace {

struct ConvertCase {
  const char* name;
  TextureFormat format;
  uint32_t width, height, depth = 1;
  Dimension dimension = Dimension::k2D;
  uint32_t max_level = 0;
};

const ConvertCase kBenchCases[] = {
    {"small-rgba", TextureFormat::k_8_8_8_8, 64, 64},
    {"dxt1-1024-mips", TextureFormat::k_DXT1, 1024, 1024, 1, Dimension::k2D, 10},
    {"dxt5-1024-mips", TextureFormat::k_DXT4_5, 1024, 1024, 1, Dimension::k2D, 10},
    {"rgba-1024-mips", TextureFormat::k_8_8_8_8, 1024, 1024, 1, Dimension::k2D, 10},
    {"565-1024", TextureFormat::k_5_6_5, 1024, 1024},
    {"dxt5-unaligned", TextureFormat::k_DXT4_5, 1023, 1023},
    {"cube-rgba-256", TextureFormat::k_8_8_8_8, 256, 256, 6, Dimension::kCube, 8},
    {"volume-rgba", TextureFormat::k_8_8_8_8, 128, 128, 16, Dimension::k3D, 7},
};

TextureFetch MakeCase(const ConvertCase& c) {
  TextureFetchDesc d;
  d.format = c.format;
  d.width = c.width;
  d.height = c.height;
  d.depth_or_layers = c.depth;
  d.dimension = c.dimension;
  d.base_address = 0x1000;
  d.mip_address = 0x1000000;
  d.max_level = c.max_level;
  d.tiled = GetFormatInfo(c.format).BytesPerBlock() != 12;
  d.packed_mips = true;
  d.endian = Endian::k8in32;
  return MakeTextureFetch(d);
}

uint64_t Hash(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t b : bytes) hash = (hash ^ b) * 1099511628211ull;
  return hash;
}

bool Same(const HostTextureData& a, const HostTextureData& b) {
  const auto& p = a.plan;
  const auto& q = b.plan;
  if (p.format != q.format || p.conversion != q.conversion || p.dimension != q.dimension ||
      p.width != q.width || p.height != q.height || p.depth != q.depth || p.layers != q.layers ||
      p.levels != q.levels || p.min_level != q.min_level || p.view_swizzle != q.view_swizzle ||
      p.shader_flags != q.shader_flags || p.decompressed != q.decompressed) return false;
  if (a.bytes != b.bytes || a.subresources.size() != b.subresources.size()) return false;
  for (size_t i = 0; i < a.subresources.size(); ++i) {
    const auto& x = a.subresources[i];
    const auto& y = b.subresources[i];
    if (x.level != y.level || x.layer != y.layer || x.width != y.width || x.height != y.height ||
        x.depth != y.depth || x.row_pitch != y.row_pitch || x.depth_pitch != y.depth_pitch ||
        x.offset != y.offset || x.size != y.size) return false;
  }
  return true;
}

void Compare(const TextureFetch& fetch, const GuestMemory& memory, TextureConversionWorkers& workers,
             const TextureOptions& options = {}) {
  HostTextureData serial, parallel;
  std::string why_serial, why_parallel;
  const bool expected = ConvertTexture(fetch, memory, serial, &why_serial, options);
  const bool actual = ConvertTexture(fetch, memory, parallel, &why_parallel, options, &workers);
  CHECK_EQ(actual, expected);
  CHECK(why_parallel == why_serial);
  if (actual && !Same(serial, parallel)) {
    kknr_test::Fail(__FILE__, __LINE__, std::string("worker bytes/metadata differ: ") +
                    GetFormatInfo(fetch.Format()).name);
  }
}

}  // namespace

TEST(bench_texture_conversion) {
  if (!std::getenv("NR_BENCH")) return;
  kknr_test::FakeGuest guest;
  kknr_test::Rng(91).Fill(guest.bytes.data(), guest.bytes.size());
  const char* worker_env = std::getenv("NR_BENCH_CONVERT_WORKERS");
  const unsigned count = worker_env ? unsigned(std::strtoul(worker_env, nullptr, 10)) : 0;
  std::unique_ptr<TextureConversionWorkers> workers;
  for (const auto& c : kBenchCases) {
    const TextureFetch fetch = MakeCase(c);
    const auto ranges = GetTextureRanges(fetch);
    const bool use_workers = count && uint64_t(ranges.base_bytes) + ranges.mip_bytes >= (256u << 10);
    if (use_workers && !workers) workers = std::make_unique<TextureConversionWorkers>(count);
    std::vector<double> times;
    HostTextureData out;
    for (unsigned i = 0; i < 28; ++i) {
      const auto start = std::chrono::steady_clock::now();
      const bool ok = ConvertTexture(fetch, GuestMemory{guest.bytes.data(), guest.bytes.size()}, out,
                                     nullptr, {}, use_workers ? workers.get() : nullptr);
      const auto end = std::chrono::steady_clock::now();
      CHECK(ok);
      if (i >= 7) times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    std::sort(times.begin(), times.end());
    std::printf("NR_BENCH texture %s median_us=%.3f p90_us=%.3f bytes=%zu hash=%016llx\n", c.name,
                times[times.size() / 2], times[times.size() * 9 / 10], out.bytes.size(),
                static_cast<unsigned long long>(Hash(out.bytes)));
  }
}

TEST(guest_block_row_slabs_match_whole_levels) {
  kknr_test::Rng rng(73);
  // Includes 12-byte blocks, every format footprint, unaligned dimensions, mip tails, volume slices,
  // cube layers, missing bytes, and empty regions. Each slab is compared with the original whole read.
  for (unsigned fi = 0; fi < 64; ++fi)
    for (unsigned dim = 0; dim < 3; ++dim)
      for (unsigned tiled = 0; tiled < 2; ++tiled) {
        const auto format = TextureFormat(fi);
        if (tiled && GetFormatInfo(format).BytesPerBlock() == 12) continue;
        TextureFetchDesc desc;
        desc.format = format;
        desc.width = 127;
        desc.height = 69;
        desc.depth_or_layers = dim == 0 ? 1 : (dim == 1 ? 5 : 6);
        desc.dimension = dim == 0 ? Dimension::k2D : (dim == 1 ? Dimension::k3D : Dimension::kCube);
        desc.base_address = 0x1000;
        desc.mip_address = 0x1000000;
        desc.max_level = 7;
        desc.packed_mips = fi % 2;
        desc.tiled = tiled;
        const auto layout = ComputeGuestLayout(MakeTextureFetch(desc));
        std::vector<uint8_t> base(layout.base_extent_bytes), mips(layout.mips_extent_bytes);
        rng.Fill(base.data(), base.size());
        rng.Fill(mips.data(), mips.size());
        GuestRegions regions{base.data(), base.size() / 2, mips.data(), mips.size() / 2};
        const uint32_t bpb = GetFormatInfo(format).BytesPerBlock();
        for (uint32_t level = 0; level <= layout.max_level; ++level)
          for (uint32_t layer = 0; layer < layout.layers; ++layer) {
            std::vector<uint8_t> whole;
            BlockExtent extent;
            if (!ReadGuestBlocks(layout, regions, level, layer, whole, extent)) continue;
            const uint32_t rows = extent.height_blocks * extent.depth;
            uint8_t guard = 0xCD;
            CHECK(!ReadGuestBlockRows(layout, regions, level, layer, rows, 1, &guard));
            CHECK_EQ(guard, 0xCD);
            CHECK(ReadGuestBlockRows(layout, regions, level, layer, rows, 0, nullptr));
            for (uint32_t first = 0; first < rows; first += 3) {
              const uint32_t count = std::min(3u, rows - first);
              const size_t bytes = size_t(count) * extent.width_blocks * bpb;
              std::vector<uint8_t> slab(bytes + 16, 0xCD);
              CHECK(ReadGuestBlockRows(layout, regions, level, layer, first, count, slab.data() + 8));
              CHECK(std::equal(slab.begin() + 8, slab.end() - 8,
                               whole.begin() + size_t(first) * extent.width_blocks * bpb));
              CHECK(std::all_of(slab.begin(), slab.begin() + 8, [](uint8_t b) { return b == 0xCD; }));
              CHECK(std::all_of(slab.end() - 8, slab.end(), [](uint8_t b) { return b == 0xCD; }));
            }
          }
      }
}

TEST(texture_workers_match_serial_formats_and_shapes) {
  kknr_test::FakeGuest guest;
  kknr_test::Rng(29).Fill(guest.bytes.data(), guest.bytes.size());
  TextureConversionWorkers workers(3);
  for (unsigned fi = 0; fi < 64; ++fi) {
    ConvertCase c{"all-format", TextureFormat(fi), 1023, 513, 1, Dimension::k2D, 9};
    auto fetch = MakeCase(c);
    Compare(fetch, {guest.bytes.data(), guest.bytes.size()}, workers);
    // Signed format selection and truncated swap units (remaining guest reads must be zero).
    fetch.words[0] |= 0x55u << 2;
    Compare(fetch, {guest.bytes.data(), guest.bytes.size()}, workers);
    c.max_level = 0;
    fetch = MakeCase(c);
    fetch.words[0] |= 0x55u << 2;
    Compare(fetch, {guest.bytes.data(), 0x1000 + 123457}, workers);
  }
  for (const auto& c : kBenchCases) {
    Compare(MakeCase(c), {guest.bytes.data(), guest.bytes.size()}, workers);
    Compare(MakeCase(c), {guest.bytes.data(), 0x1000000 + 123}, workers);
  }
  TextureFetchDesc tail;
  tail.format = TextureFormat::k_8_8_8_8;
  tail.width = tail.height = 16;
  tail.base_address = 0x1000;
  tail.max_level = 4;
  tail.packed_mips = tail.tiled = true;
  Compare(MakeTextureFetch(tail), {guest.bytes.data(), guest.bytes.size()}, workers);
  tail.width = tail.height = 1024;
  tail.mip_address = 0x1000000;
  tail.min_level = 2;
  tail.max_level = 10;
  Compare(MakeTextureFetch(tail), {guest.bytes.data(), guest.bytes.size()}, workers);
  TextureConversionWorkers inline_only(0);
  Compare(MakeCase(kBenchCases[3]), {guest.bytes.data(), guest.bytes.size()}, inline_only);
}

TEST(texture_workers_join_exceptions_and_reuse) {
  TextureConversionWorkers workers(99);
  CHECK(workers.WorkerCount() <= 3);
  const unsigned count = workers.WorkerCount() + 1;
  std::atomic<unsigned> completed = 0;
  struct FailureContext { std::atomic<unsigned>& completed; unsigned failing; } context{completed, 0};
  for (unsigned failing : {0u, count - 1}) {
    completed = 0;
    context.failing = failing;
    bool caught = false;
    try {
      workers.Run(count, &context, [](void* context, unsigned index) {
        auto& c = *static_cast<FailureContext*>(context);
        ++c.completed;
        if (index == c.failing) throw std::runtime_error("synthetic conversion failure");
      });
    } catch (const std::runtime_error&) {
      caught = true;
    }
    CHECK(caught);
    CHECK_EQ(completed.load(), count);
  }
  for (unsigned iteration = 0; iteration < 30; ++iteration) {
    workers.Run(count, &completed, [](void* context, unsigned) {
      ++*static_cast<std::atomic<unsigned>*>(context);
    });
  }
  CHECK_EQ(completed.load(), count * 31);
  workers.Run(19, &completed, [](void* context, unsigned) {
    ++*static_cast<std::atomic<unsigned>*>(context);
  });
  CHECK_EQ(completed.load(), count * 31 + 19);
}

TEST(texture_workers_shared_pool_contexts_stay_separate) {
  kknr_test::FakeGuest guest;
  kknr_test::Rng(371).Fill(guest.bytes.data(), guest.bytes.size());
  TextureConversionWorkers workers(3);
  HostTextureData outputs[2], expected[2];
  bool ok[2] = {};
  std::thread callers[2];
  for (unsigned i = 0; i < 2; ++i) {
    CHECK(ConvertTexture(MakeCase(kBenchCases[i + 2]), {guest.bytes.data(), guest.bytes.size()}, expected[i]));
    callers[i] = std::thread([&, i] {
      for (unsigned repeat = 0; repeat < 12; ++repeat) {
        ok[i] = ConvertTexture(MakeCase(kBenchCases[i + 2]), {guest.bytes.data(), guest.bytes.size()},
                               outputs[i], nullptr, {}, &workers);
        if (!ok[i] || !Same(outputs[i], expected[i])) { ok[i] = false; return; }
      }
    });
  }
  for (auto& caller : callers) caller.join();
  CHECK(ok[0] && ok[1]);
}
