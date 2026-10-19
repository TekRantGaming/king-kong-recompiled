// Task 300: the placed texture pool (DeviceDesc::placedTextureHeapSize) and the larger, not-zeroed
// upload chunks (DeviceDesc::minUploadChunkSize) in the vendored NVRHI. The range bookkeeping is
// checked on its own; then textures are made, written, read back, released and made again in
// reused ranges on the PC's D3D12 device (skipped when there is none).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <wrl/client.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/utils.h>

#include "backend/placed_textures.h"
#include "backend/tests/check.h"
#include "d3d12-placed-ranges.h"

using nvrhi::d3d12::PlacedRanges;

TEST(PlacedRanges_FirstFitAlignsAndSplits) {
  PlacedRanges r(1 << 20);
  PlacedRanges::Allocation a, b, c;
  CHECK(!r.allocate(4096, 4096, a));  // no block yet
  CHECK_EQ(r.addBlock(), 0u);
  CHECK(r.allocate(4096, 4096, a));
  CHECK_EQ(a.offset, 0u);
  CHECK(r.allocate(100, 65536, b));  // aligned up, the gap stays free
  CHECK_EQ(b.offset, 65536u);
  CHECK(r.allocate(4096, 4096, c));  // first fit: the gap after a
  CHECK_EQ(c.offset, 4096u);
  CHECK_EQ(r.freeBytes(0), uint64_t(1 << 20) - 4096 - 100 - 4096);
  CHECK(!r.allocate(2 << 20, 4096, a));  // larger than a block
  CHECK(!r.allocate(4096, 3, a));        // not a power of two
}

TEST(PlacedRanges_ReleaseMergesNeighbours) {
  PlacedRanges r(1 << 20);
  r.addBlock();
  PlacedRanges::Allocation x[4];
  for (auto& a : x) CHECK(r.allocate(65536, 65536, a));
  CHECK(!r.release(x[1]));
  CHECK(!r.release(x[3]));  // merges with the tail
  CHECK_EQ(r.freeRanges(0), 2u);
  CHECK(!r.release(x[2]));  // joins both sides
  CHECK_EQ(r.freeRanges(0), 1u);
  CHECK(r.release(x[0]));  // the block is empty
  CHECK_EQ(r.freeRanges(0), 1u);
  CHECK_EQ(r.freeBytes(0), uint64_t(1 << 20));
  PlacedRanges::Allocation whole;
  CHECK(r.allocate(1 << 20, 65536, whole));  // one range again, not fragments
  CHECK_EQ(whole.offset, 0u);
}

TEST(PlacedRanges_BlocksAreAddedAndReused) {
  PlacedRanges r(1 << 20);
  r.addBlock();
  PlacedRanges::Allocation a, b;
  CHECK(r.allocate(1 << 20, 4096, a));
  CHECK(!r.allocate(4096, 4096, b));
  CHECK_EQ(r.addBlock(), 1u);
  CHECK(r.allocate(4096, 4096, b));
  CHECK_EQ(b.block, 1u);
  CHECK_EQ(r.emptyBlocks(), 0u);
  CHECK(r.release(a));
  CHECK_EQ(r.emptyBlocks(), 1u);
  r.removeBlock(0);
  CHECK_EQ(r.liveBlocks(), 1u);
  CHECK_EQ(r.addBlock(), 0u);  // the dead slot
  CHECK(!r.release(a));        // a stale range of a removed block is ignored
}

TEST(PlacedTextures_MissingSubresourcesAreListed) {
  CHECK(nr::MissingSubresources(3, 2, {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {2, 1}}).empty());
  const auto missing = nr::MissingSubresources(3, 1, {{0, 0}});  // no mip data
  CHECK_EQ(missing.size(), 2u);
  CHECK(missing[0] == std::make_pair(1u, 0u));
  CHECK(missing[1] == std::make_pair(2u, 0u));
  CHECK_EQ(nr::MissingSubresources(1, 6, {{0, 0}, {0, 2}}).size(), 4u);  // cube faces that were not read
}

namespace {
struct Messages final : nvrhi::IMessageCallback {
  int errors = 0;
  void message(nvrhi::MessageSeverity severity, const char* text) override {
    if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) {
      ++errors;
      std::fprintf(stderr, "%s\n", text);
    }
  }
};

uint32_t Texel(uint32_t seed, uint32_t x, uint32_t y) { return seed * 2654435761u ^ (x * 73856093u) ^ (y * 19349663u); }
}  // namespace

TEST(D3D12PlacedTextures_WriteReadBackAndReuse) {
  Microsoft::WRL::ComPtr<ID3D12Device> native;
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&native)))) {
    std::puts("SKIP: no D3D12 device");
    return;
  }
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
  CHECK(SUCCEEDED(native->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));
  Messages messages;
  nvrhi::d3d12::DeviceDesc desc;
  desc.pDevice = native.Get();
  desc.pGraphicsCommandQueue = queue.Get();
  desc.errorCB = &messages;
  desc.placedTextureHeapSize = 4ull << 20;  // small heaps: the test fills several
  desc.minUploadChunkSize = 1ull << 20;
  nvrhi::DeviceHandle device = nvrhi::d3d12::createDevice(desc);
  CHECK(bool(device));
  if (!device) return;
  nvrhi::CommandListHandle cl = device->createCommandList();

  // Three rounds: make textures (placed, sizes from 4 KiB-aligned to over a quarter heap, which is
  // committed), write them, read every one back, release half; the next round reuses those ranges.
  std::vector<std::pair<nvrhi::TextureHandle, uint32_t>> live;
  uint32_t seed = 1;
  for (int round = 0; round < 3; ++round) {
    std::vector<std::pair<nvrhi::TextureHandle, uint32_t>> made;
    for (uint32_t size : {16u, 64u, 128u, 256u, 600u, 16u, 32u}) {
      nvrhi::TextureDesc td;
      td.width = td.height = size;
      td.format = nvrhi::Format::RGBA8_UNORM;
      td.initialState = nvrhi::ResourceStates::ShaderResource;
      td.keepInitialState = true;
      td.debugName = "placed test";
      td.allowPlacedMemory = true;
      made.emplace_back(device->createTexture(td), ++seed);
      CHECK(bool(made.back().first));
    }
    cl->open();
    for (auto& [tex, s] : made) {
      const uint32_t w = tex->getDesc().width;
      std::vector<uint32_t> texels(size_t(w) * w);
      for (uint32_t y = 0; y < w; ++y)
        for (uint32_t x = 0; x < w; ++x) texels[size_t(y) * w + x] = Texel(s, x, y);
      cl->writeTexture(tex, 0, 0, texels.data(), size_t(w) * 4);
    }
    cl->close();
    device->executeCommandList(cl);
    for (auto& m : made) live.push_back(m);
    for (auto& [tex, s] : live) {
      nvrhi::TextureDesc sd = tex->getDesc();
      auto staging = device->createStagingTexture(sd, nvrhi::CpuAccessMode::Read);
      cl->open();
      cl->copyTexture(staging, nvrhi::TextureSlice(), tex, nvrhi::TextureSlice());
      cl->close();
      device->executeCommandList(cl);
      device->waitForIdle();
      size_t pitch = 0;
      auto* bytes = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(),
                                                                           nvrhi::CpuAccessMode::Read, &pitch));
      CHECK(bytes != nullptr);
      if (!bytes) continue;
      uint32_t bad = 0;
      for (uint32_t y = 0; y < sd.height; ++y)
        for (uint32_t x = 0; x < sd.width; ++x) {
          uint32_t v;
          std::memcpy(&v, bytes + y * pitch + x * 4, 4);
          bad += v != Texel(s, x, y);
        }
      device->unmapStagingTexture(staging);
      CHECK_EQ(bad, 0u);
    }
    // Release every other texture (its range is free once the GPU is done, as for a committed one).
    std::vector<std::pair<nvrhi::TextureHandle, uint32_t>> kept;
    for (size_t i = 0; i < live.size(); ++i)
      if (i % 2) kept.push_back(live[i]);
    live = std::move(kept);
    device->waitForIdle();
    device->runGarbageCollection();
  }
  // Information, not a check (timing): making and releasing 200 small textures, pooled and committed.
  for (bool pooled : {true, false}) {
    nvrhi::d3d12::DeviceDesc d2 = desc;
    d2.placedTextureHeapSize = pooled ? (64ull << 20) : 0;
    nvrhi::DeviceHandle dev = nvrhi::d3d12::createDevice(d2);
    std::vector<nvrhi::TextureHandle> many;
    nvrhi::TextureDesc td;
    td.width = td.height = 256;
    td.format = nvrhi::Format::BC1_UNORM;
    td.mipLevels = 9;
    td.allowPlacedMemory = true;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) many.push_back(dev->createTexture(td));
    const auto t1 = std::chrono::steady_clock::now();
    many.clear();
    const auto t2 = std::chrono::steady_clock::now();
    std::printf("200 BC1 256x256 textures %s: made in %.2f ms, released in %.2f ms\n", pooled ? "pooled" : "committed",
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count());
  }
  // A render target is never placed in the pool (its heap only allows sampled textures).
  nvrhi::TextureDesc rt;
  rt.width = rt.height = 64;
  rt.format = nvrhi::Format::RGBA8_UNORM;
  rt.isRenderTarget = true;
  rt.initialState = nvrhi::ResourceStates::RenderTarget;
  rt.keepInitialState = true;
  CHECK(bool(device->createTexture(rt)));
  live.clear();
  device->waitForIdle();
  device->runGarbageCollection();
  CHECK_EQ(messages.errors, 0);
}

NR_TEST_MAIN()
