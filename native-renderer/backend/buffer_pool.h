// Task 309: guest vertex and index buffers suballocated from large shared buffers (D3D12
// --native_d3d12_buffer_pool). NVRHI makes every buffer a committed resource on D3D12, ~2.5x Vulkan's cost
// per buffer (task 304: buffer_create 115-155 ms a run vs 51 on Vulkan).
//
// One pool per kind (vertex data: raw SRVs; indices: index buffers), blocks of kBlockBytes made on
// demand, ranges 256-byte aligned (raw views need 16, index offsets 4). A range the caches let go of is
// retired and only reused kRetireFrames frames later, the rule the descriptor slots of the same buffers
// already follow (GameRenderer::RetireSlot): the renderer never has more than two frames on the GPU
// (Renderer::WaitForFrameImage), so by then no submitted work reads it. Thread safe (workers allocate
// while preparing uploads; the render thread retires and releases).
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "thirdparty/nvrhi/src/d3d12/d3d12-placed-ranges.h"

namespace nr {

inline std::atomic<bool> g_buffer_pool_enabled{false};

class BufferPool {
 public:
  static constexpr uint64_t kBlockBytes = 32ull << 20;
  static constexpr uint64_t kAlignment = 256;
  static constexpr uint64_t kRetireFrames = 8;

  struct Range {
    nvrhi::IBuffer* buffer = nullptr;  // the block (the pool keeps it alive)
    nvrhi::d3d12::PlacedRanges::Allocation allocation;
    bool valid() const { return buffer != nullptr; }
  };
  // make_block(bytes): one shared buffer of that size, or null.
  explicit BufferPool(std::function<nvrhi::BufferHandle(uint64_t)> make_block, uint64_t block_bytes = kBlockBytes)
      : make_block_(std::move(make_block)), ranges_(block_bytes) {}

  // A range of at least `bytes`; false (the caller makes its own buffer) when it is larger than a
  // quarter block or no block can be made.
  bool Allocate(uint64_t bytes, Range& out) {
    if (!bytes || bytes > ranges_.blockSize() / 4) return false;
    const uint64_t size = (bytes + kAlignment - 1) & ~(kAlignment - 1);
    std::lock_guard lock(mutex_);
    if (failed_) return false;
    nvrhi::d3d12::PlacedRanges::Allocation a;
    if (!ranges_.allocate(size, kAlignment, a)) {
      nvrhi::BufferHandle block = make_block_(ranges_.blockSize());
      if (!block) {
        failed_ = true;
        return false;
      }
      const uint32_t index = ranges_.addBlock();
      if (index >= blocks_.size()) blocks_.resize(index + 1);
      blocks_[index] = block;
      ++blocks_made_;
      if (!ranges_.allocate(size, kAlignment, a)) return false;
    }
    out.buffer = blocks_[a.block];
    out.allocation = a;
    ++live_;
    return true;
  }
  // The caches let go of the range in `frame`; reused from frame + kRetireFrames + 1.
  void Retire(const Range& range, uint64_t frame) {
    if (!range.valid()) return;
    std::lock_guard lock(mutex_);
    retired_.emplace_back(frame, range.allocation);
  }
  // Returns the ranges retired long enough ago. Empty blocks are kept (one burst's worth of memory).
  void Release(uint64_t frame) {
    std::lock_guard lock(mutex_);
    while (!retired_.empty() && retired_.front().first + kRetireFrames < frame) {
      ranges_.release(retired_.front().second);
      retired_.pop_front();
      --live_;
    }
  }
  uint64_t live() const { std::lock_guard lock(mutex_); return live_; }
  uint64_t retired() const { std::lock_guard lock(mutex_); return retired_.size(); }
  uint64_t blocks_made() const { std::lock_guard lock(mutex_); return blocks_made_; }

 private:
  std::function<nvrhi::BufferHandle(uint64_t)> make_block_;
  mutable std::mutex mutex_;
  nvrhi::d3d12::PlacedRanges ranges_;
  std::vector<nvrhi::BufferHandle> blocks_;
  std::deque<std::pair<uint64_t, nvrhi::d3d12::PlacedRanges::Allocation>> retired_;
  uint64_t live_ = 0, blocks_made_ = 0;
  bool failed_ = false;
};

}  // namespace nr
