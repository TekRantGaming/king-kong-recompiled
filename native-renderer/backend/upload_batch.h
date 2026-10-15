// Early graphics-queue transfers for resources that have never been used by a draw.
// Updating an existing resource must stay on the draw list: moving that update ahead
// of earlier draws in the same list would change their picture.
#pragma once

#include <cstdint>
#include <functional>
#include <nvrhi/nvrhi.h>

namespace nr {

class UploadBatch {
 public:
  using Submit = std::function<void(nvrhi::ICommandList*)>;
  static constexpr uint64_t kFrameBytes = 32ull << 20;
  static constexpr uint64_t kMinBytes = 64ull << 10;
  static constexpr uint32_t kFrameBatches = 32;

  UploadBatch(nvrhi::IDevice* device, Submit submit) : device_(device), submit_(std::move(submit)) {}
  bool CanPrepare(uint64_t bytes, uint32_t batches) const {
    return submit_ && !open_ && bytes <= kFrameBytes - frame_bytes_ &&
        batches <= kFrameBatches - frame_batches_;
  }
  // The caller checked CanPrepare before starting a joined worker batch. Every
  // list is closed and published on this thread, using the host's queue locks.
  void SubmitPrepared(nvrhi::ICommandList* list, uint64_t bytes) {
    submit_(list);
    frame_bytes_ += bytes;
    bytes_ += bytes;
    ++frame_batches_;
    ++batches_;
  }
  // A null list means the caller must upload inline. Required work is never deferred
  // or omitted when the early-transfer budget runs out.
  nvrhi::ICommandList* Begin(uint64_t bytes) {
    if (!submit_ || bytes < kMinBytes || bytes > kFrameBytes - frame_bytes_ ||
        frame_batches_ >= kFrameBatches) return nullptr;
    if (!list_) {
      nvrhi::CommandListParameters p;
      p.enableImmediateExecution = false;
      // Share one staging pool across early transfers; retain NVRHI's default
      // chunk size and completion-based reuse of in-flight staging storage.
      list_ = device_->createCommandList(p);
      if (!list_) return nullptr;
    }
    if (!open_) {
      list_->open();
      open_ = true;
    }
    frame_bytes_ += bytes;
    bytes_ += bytes;
    return list_;
  }
  // Called after recording a draw, including early returns, before its graphics
  // list can be submitted. Both lists use the same queue; upload completion and
  // the final resource transitions precede the dependent draw on the GPU.
  void Flush() {
    if (!open_) return;
    list_->close();
    submit_(list_);
    open_ = false;
    ++frame_batches_;
    ++batches_;
  }
  void EndFrame() {
    Flush();
    frame_bytes_ = 0;
    frame_batches_ = 0;
  }
  uint64_t bytes() const { return bytes_; }
  uint64_t batches() const { return batches_; }

 private:
  nvrhi::IDevice* device_;
  Submit submit_;
  nvrhi::CommandListHandle list_;
  bool open_ = false;
  uint64_t frame_bytes_ = 0, bytes_ = 0, batches_ = 0;
  uint32_t frame_batches_ = 0;
};

}  // namespace nr
