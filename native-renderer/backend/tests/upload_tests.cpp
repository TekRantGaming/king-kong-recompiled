#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>
#include "backend/upload_batch.h"
#include "backend/resource_upload_workers.h"
#include <stdexcept>
#include <thread>
#include "backend/tests/check.h"
#include "backend/tests/vk_test_device.h"
#if NR_HAS_D3D12
#include <wrl/client.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>
#endif

using namespace nr;
using namespace nr::test;

namespace {
struct Messages : nvrhi::IMessageCallback {
  int errors = 0;
  void message(nvrhi::MessageSeverity s, const char* text) override {
    std::fprintf(stderr, "%s\n", text);
    if (s == nvrhi::MessageSeverity::Error || s == nvrhi::MessageSeverity::Fatal) ++errors;
  }
};

nvrhi::CommandListHandle List(nvrhi::IDevice* d) {
  nvrhi::CommandListParameters p;
  p.enableImmediateExecution = false;
  return d->createCommandList(p);
}
nvrhi::BufferHandle Buffer(nvrhi::IDevice* d, size_t bytes, bool readback = false) {
  nvrhi::BufferDesc p;
  p.byteSize = bytes;
  p.initialState = readback ? nvrhi::ResourceStates::CopyDest : nvrhi::ResourceStates::ShaderResource;
  p.keepInitialState = true;
  p.cpuAccess = readback ? nvrhi::CpuAccessMode::Read : nvrhi::CpuAccessMode::None;
  return d->createBuffer(p);
}

void Correctness(nvrhi::IDevice* d) {
  constexpr size_t size = UploadBatch::kMinBytes;
  uint64_t submits = 0;
  UploadBatch batch(d, [&](auto* cl) { d->executeCommandList(cl); ++submits; });
  CHECK(batch.CanPrepare(size, 4));
  CHECK(!batch.CanPrepare(size, UploadBatch::kFrameBatches + 1));
  CHECK(!batch.CanPrepare(UploadBatch::kFrameBytes + 1, 1));
  CHECK(batch.Begin(size - 1) == nullptr);
  CHECK(batch.Begin(UploadBatch::kFrameBytes + 1) == nullptr);
  auto source = Buffer(d, size);
  auto before = Buffer(d, size, true), after = Buffer(d, size, true);
  std::vector<uint8_t> data(size, 0x39), changed(size, 0xA7);
  nvrhi::TextureDesc td;
  td.width = 128; td.height = 128; td.format = nvrhi::Format::RGBA8_UNORM;
  td.initialState = nvrhi::ResourceStates::ShaderResource;
  td.keepInitialState = true;
  auto texture = d->createTexture(td);
  auto draw = List(d);
  draw->open();
  auto* upload = batch.Begin(size);
  CHECK(upload != nullptr);
  if (!upload) return;
  upload->writeBuffer(source, data.data(), data.size());
  upload->setBufferState(source, nvrhi::ResourceStates::ShaderResource);
  upload = batch.Begin(size);
  CHECK(upload != nullptr);
  upload->writeTexture(texture, 0, 0, data.data(), 128 * 4);
  upload->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  draw->beginTrackingTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  // These copies stand for reads by two dependent draws, separated by an in-place
  // update in the same frame. Moving the update to the early list would fail.
  draw->copyBuffer(before, 0, source, 0, size);
  draw->writeBuffer(source, changed.data(), changed.size());
  draw->copyBuffer(after, 0, source, 0, size);
  draw->close();
  batch.Flush();
  CHECK_EQ(submits, 1u);
  CHECK_EQ(batch.bytes(), 2 * size);
  batch.Flush();
  CHECK_EQ(submits, 1u);
  d->executeCommandList(draw);
  d->waitForIdle();
  for (auto result : {before, after}) {
    const auto& want = result == before ? data : changed;
    const void* mapped = d->mapBuffer(result, nvrhi::CpuAccessMode::Read);
    CHECK(mapped != nullptr);
    if (mapped) CHECK(std::memcmp(mapped, want.data(), size) == 0);
    d->unmapBuffer(result);
  }
  const auto pixels = ReadBackTexture(d, texture);
  CHECK_EQ(pixels.width, 128u);
  for (size_t y = 0; y < pixels.height; ++y)
    for (size_t x = 0; x < pixels.width; ++x)
      for (int c = 0; c < 4; ++c) CHECK_NEAR(pixels.at(uint32_t(x), uint32_t(y))[c], 0x39 / 255.0, 0.0001);

  // Exhaust the byte budget without allocating a huge payload; an accepted
  // reservation is sufficient to test policy, then a real write tests reuse.
  batch.EndFrame();
  CHECK(batch.Begin(UploadBatch::kFrameBytes) != nullptr);
  CHECK(batch.Begin(size) == nullptr);
  CHECK(!batch.CanPrepare(size, 1));
  batch.EndFrame();
  for (uint32_t i = 0; i < UploadBatch::kFrameBatches; ++i) {
    auto* cl = batch.Begin(size);
    CHECK(cl != nullptr);
    cl->writeBuffer(source, data.data(), size);
    cl->setBufferState(source, nvrhi::ResourceStates::ShaderResource);
    batch.Flush();
  }
  CHECK(batch.Begin(size) == nullptr);
  CHECK(!batch.CanPrepare(size, 1));
  batch.EndFrame();
  CHECK(batch.Begin(size) != nullptr);
  batch.EndFrame();
  d->waitForIdle();
  d->runGarbageCollection();
}

void PreparedCorrectness(nvrhi::IDevice* d, unsigned workers) {
  ResourceUploadWorkers pool(d, workers);
  constexpr size_t size = 64u << 10;
  std::vector<nvrhi::BufferHandle> readbacks;
  std::vector<uint8_t> expected;
  for (unsigned round = 0; round < 4; ++round) {
    std::vector<nvrhi::BufferHandle> buffers(5);
    std::vector<ResourceUploadWorkers::Task> tasks(5);
    std::vector<std::thread::id> threads(5);
    std::vector<std::vector<uint8_t>> inputs(5, std::vector<uint8_t>(size));
    for (unsigned i = 0; i < tasks.size(); ++i) {
      std::fill(inputs[i].begin(), inputs[i].end(), uint8_t(7 + round * 5 + i));
      tasks[i].prepare = [&, i] {
        threads[i] = std::this_thread::get_id();
        if (round == 1 && i == 3) throw std::runtime_error("injected creation failure");
        buffers[i] = Buffer(d, size);
        return bool(buffers[i]);
      };
      tasks[i].record = [&, i](auto* cl) { cl->writeBuffer(buffers[i], inputs[i].data(), size); };
      tasks[i].bytes = size;
    }
    pool.Run(tasks);
    CHECK(threads[0] == std::this_thread::get_id());
    if (pool.WorkerCount()) CHECK(threads[1] != threads[0]);
    for (unsigned i = 0; i < tasks.size(); ++i) {
      CHECK_EQ(tasks[i].ready, !(round == 1 && i == 3));
      CHECK_EQ(bool(tasks[i].error), round == 1 && i == 3);
      // The upload list owns captured staging bytes even after caller input is overwritten.
      std::fill(inputs[i].begin(), inputs[i].end(), 0xFC);
    }
    uint64_t batches = 0;
    pool.SubmitAll([&](auto* list, uint64_t bytes) { CHECK(bytes > 0); d->executeCommandList(list); ++batches; });
    CHECK(batches <= pool.WorkerCount() + 1);
    pool.SubmitAll([&](auto*, uint64_t) { CHECK(false); });
    auto read = List(d); read->open();
    for (unsigned i = 0; i < tasks.size(); ++i) if (tasks[i].ready) {
      auto result = Buffer(d, size, true);
      read->copyBuffer(result, 0, buffers[i], 0, size);
      readbacks.push_back(result); expected.push_back(uint8_t(7 + round * 5 + i));
    }
    read->close(); d->executeCommandList(read);
  }
  // Lists are reopened with prior submissions still in flight; their staging
  // memory may be recycled only after its GPU completion, without a CPU idle.
  d->waitForIdle();
  for (size_t i = 0; i < readbacks.size(); ++i) {
    const auto* p = static_cast<const uint8_t*>(d->mapBuffer(readbacks[i], nvrhi::CpuAccessMode::Read));
    CHECK(p != nullptr);
    if (p) CHECK(std::all_of(p, p + size, [&](uint8_t b) { return b == expected[i]; }));
    d->unmapBuffer(readbacks[i]);
  }
  d->runGarbageCollection();
  pool.scratch(0).source.reserve(33u << 20);
  pool.scratch(1).source.reserve(33u << 20);
  CHECK(pool.ScratchBytes() > (64u << 20));
  pool.TrimScratch();
  CHECK(pool.ScratchBytes() <= (64u << 20));
}

void BenchResources(nvrhi::IDevice* d, const char* api) {
  if (!std::getenv("NR_BENCH_RESOURCES")) return;
  const unsigned count = std::getenv("NR_BENCH_RESOURCES_SERIAL") ? 0 : 3;
  ResourceUploadWorkers pool(d, count);
  kknr::TextureConversionWorkers slabs(3);
  constexpr size_t size = 1u << 20;
  kknr::TextureFetchDesc fetch_desc;
  fetch_desc.width = fetch_desc.height = 512; fetch_desc.tiled = true;
  fetch_desc.format = kknr::TextureFormat::k_8_8_8_8;
  fetch_desc.base_address = 4096; fetch_desc.endian = kknr::Endian::k8in32;
  const auto fetch = kknr::MakeTextureFetch(fetch_desc);
  kknr::GuestTextureImage image;
  std::vector<uint8_t> data(size, 0x73);
  CHECK(kknr::EncodeGuestTexture(fetch, [&](uint32_t, uint32_t) { return data.data(); }, image));
  const kknr::GuestMemory memory{image.base.data(), image.base.size(), 4096};
  std::vector<double> record, complete;
  for (unsigned iteration = 0; iteration < 28; ++iteration) {
    std::vector<nvrhi::BufferHandle> buffers(8);
    std::vector<nvrhi::TextureHandle> textures(8);
    std::vector<kknr::HostTextureData> converted(8);
    std::vector<ResourceUploadWorkers::Task> tasks(16);
    for (unsigned i = 0; i < 16; ++i) {
      tasks[i].bytes = size;
      tasks[i].prepare = [&, i] {
        if (i < 8) { buffers[i] = Buffer(d, size); return bool(buffers[i]); }
        const auto n = i - 8;
        if (!kknr::ConvertTexture(fetch, memory, converted[n], nullptr, {}, count ? nullptr : &slabs)) return false;
        nvrhi::TextureDesc td;
        td.width = td.height = 512; td.format = nvrhi::Format::RGBA8_UNORM;
        td.initialState = nvrhi::ResourceStates::ShaderResource; td.keepInitialState = true;
        textures[n] = d->createTexture(td);
        return bool(textures[n]);
      };
      tasks[i].record = [&, i](auto* cl) {
        if (i < 8) cl->writeBuffer(buffers[i], data.data(), size);
        else cl->writeTexture(textures[i-8], 0, 0, converted[i-8].bytes.data(), 512 * 4);
      };
    }
    const auto start = std::chrono::steady_clock::now();
    pool.Run(tasks);
    pool.SubmitAll([&](auto* list, uint64_t) { d->executeCommandList(list); });
    const auto recorded = std::chrono::steady_clock::now();
    d->waitForIdle();
    const auto done = std::chrono::steady_clock::now();
    for (auto& task : tasks) CHECK(task.ready && !task.error);
    if (iteration >= 7) {
      record.push_back(std::chrono::duration<double, std::micro>(recorded - start).count());
      complete.push_back(std::chrono::duration<double, std::micro>(done - start).count());
    }
    if (iteration == 27) {
      auto copy = List(d); copy->open();
      std::vector<nvrhi::BufferHandle> results;
      for (auto& b : buffers) {
        results.push_back(Buffer(d, size, true)); copy->copyBuffer(results.back(), 0, b, 0, size);
      }
      copy->close(); d->executeCommandList(copy); d->waitForIdle();
      for (auto& b : results) {
        const void* mapped = d->mapBuffer(b, nvrhi::CpuAccessMode::Read);
        CHECK(mapped && !std::memcmp(mapped, data.data(), size)); d->unmapBuffer(b);
      }
      for (auto& t : textures) {
        const auto pixels = ReadBackTexture(d, t);
        for (uint32_t y = 0; y < pixels.height; ++y)
          for (uint32_t x = 0; x < pixels.width; ++x)
            for (unsigned c = 0; c < 4; ++c) CHECK_NEAR(pixels.at(x,y)[c], 0x73 / 255.0, 0.0001);
      }
    }
    d->runGarbageCollection();
  }
  std::sort(record.begin(), record.end()); std::sort(complete.begin(), complete.end());
  std::printf("BENCH resources %s %s: record_us %.3f p90_us %.3f complete_us %.3f bytes %zu\n",
      api, count ? "batch" : "serial", record[10], record[18], complete[10], size * 16);
}

void Bench(nvrhi::IDevice* d, const char* api) {
  if (!std::getenv("NR_BENCH")) return;
  // NR_BENCH_UPLOAD_EARLY selects matched controls in separate invocations.
  const bool early = std::getenv("NR_BENCH_UPLOAD_EARLY") != nullptr;
  constexpr size_t size = 1u << 20;
  std::vector<uint8_t> data(size, 0x73);
  UploadBatch batch(d, [&](auto* cl) { d->executeCommandList(cl); });
  auto draw = List(d);
  std::vector<double> record, complete;
  for (int iteration = 0; iteration < 28; ++iteration) {
    std::vector<nvrhi::BufferHandle> buffers;
    std::vector<nvrhi::TextureHandle> textures;
    for (int i = 0; i < 8; ++i) {
      buffers.push_back(Buffer(d, size));
      nvrhi::TextureDesc td;
      td.width = 512; td.height = 512; td.format = nvrhi::Format::RGBA8_UNORM;
      td.initialState = nvrhi::ResourceStates::ShaderResource; td.keepInitialState = true;
      textures.push_back(d->createTexture(td));
    }
    const auto start = std::chrono::steady_clock::now();
    draw->open();
    for (size_t i = 0; i < buffers.size(); ++i) {
      auto* cl = early ? batch.Begin(size * 2) : draw.Get();
      CHECK(cl != nullptr);
      cl->writeBuffer(buffers[i], data.data(), size);
      cl->setBufferState(buffers[i], nvrhi::ResourceStates::ShaderResource);
      cl->writeTexture(textures[i], 0, 0, data.data(), 512 * 4);
      cl->setTextureState(textures[i], nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
      if (early) {
        draw->beginTrackingTextureState(textures[i], nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        batch.Flush();
      }
    }
    draw->close();
    d->executeCommandList(draw);
    const auto recorded = std::chrono::steady_clock::now();
    d->waitForIdle();
    const auto done = std::chrono::steady_clock::now();
    if (iteration >= 7) {
      record.push_back(std::chrono::duration<double, std::micro>(recorded - start).count());
      complete.push_back(std::chrono::duration<double, std::micro>(done - start).count());
    }
    batch.EndFrame();
    d->runGarbageCollection();
  }
  std::sort(record.begin(), record.end()); std::sort(complete.begin(), complete.end());
  std::printf("BENCH upload %s %s: record_us %.3f p90_us %.3f complete_us %.3f bytes %zu\n",
              api, early ? "early" : "inline", record[10], record[18], complete[10], size * 16);
}
}

TEST(Uploads_Vulkan) {
  auto gpu = VulkanTestDevice::Create(true);
  CHECK(gpu != nullptr);
  if (!gpu) return;
  Correctness(gpu->device());
  PreparedCorrectness(gpu->device(), 0);
  PreparedCorrectness(gpu->device(), 3);
  BenchResources(gpu->device(), "Vulkan");
  Bench(gpu->device(), "Vulkan");
  CHECK_EQ(gpu->errors(), 0);
}

#if NR_HAS_D3D12
TEST(Uploads_D3D12) {
  Microsoft::WRL::ComPtr<ID3D12Device> native;
  CHECK(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&native))));
  if (!native) return;
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
  CHECK(SUCCEEDED(native->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))));
  Messages messages;
  nvrhi::d3d12::DeviceDesc desc;
  desc.pDevice = native.Get(); desc.pGraphicsCommandQueue = queue.Get(); desc.errorCB = &messages;
  auto backend = nvrhi::d3d12::createDevice(desc);
  CHECK(backend != nullptr);
  if (!backend) return;
  auto device = nvrhi::validation::createValidationLayer(backend);
  Correctness(device);
  PreparedCorrectness(device, 0);
  PreparedCorrectness(device, 3);
  BenchResources(device, "D3D12");
  Bench(device, "D3D12");
  CHECK_EQ(messages.errors, 0);
}
#endif
NR_TEST_MAIN()
