// A joined batch of first-use resources. Workers never share a command list or
// read live guest memory. The caller publishes results and submits the closed
// lists only after every job has joined. In-place updates do not use this path.
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <exception>
#include <functional>
#include <mutex>
#include <vector>
#include <nvrhi/nvrhi.h>
#include "kknr/texture_convert.h"

namespace nr {

class ResourceUploadWorkers {
 public:
  struct Task {
    std::function<bool()> prepare;
    std::function<void(nvrhi::ICommandList*)> record;
    bool ready = false;
    std::exception_ptr error;
    uint64_t bytes = 0;
  };
  struct Scratch {
    std::vector<uint8_t> source, indices;
    kknr::HostTextureData texture;
  };

  ResourceUploadWorkers(nvrhi::IDevice* device, unsigned workers) : device_(device), workers_(workers) {}
  unsigned WorkerCount() const { return workers_.WorkerCount(); }
  Scratch& scratch(size_t index) {
    // Preallocate every requested index before Run; workers only access those
    // existing elements and cannot resize the scratch vector concurrently.
    if (scratch_.size() <= index) scratch_.resize(index + 1);
    return scratch_[index];
  }
  uint64_t ScratchBytes() const {
    uint64_t bytes = 0;
    for (const auto& s : scratch_) bytes += s.source.capacity() + s.indices.capacity() +
        s.texture.bytes.capacity() + s.texture.subresources.capacity() * sizeof(kknr::HostSubresource);
    return bytes;
  }
  void TrimScratch() {
    // Input and output budgets limit a batch; also bound capacities retained
    // across different large-resource placements in later draws/chapters.
    constexpr uint64_t limit = 64ull << 20;
    if (ScratchBytes() <= limit) return;
    for (auto& s : scratch_) {
      s = Scratch{};
      if (ScratchBytes() <= limit) break;
    }
  }

  // No submission, garbage collection or other resource uploads may run during
  // Run. NVRHI D3D12's queue recording counters are not atomic: serialize list
  // open/record/close across workers, while creation/conversion/hash work overlaps.
  // The calling thread participates, just as in texture slab conversion.
  void Run(std::vector<Task>& tasks) {
    for (auto& slot : slots_) { assert(!slot.recorded); slot.bytes = 0; }
    for (auto& task : tasks) { task.ready = false; task.error = nullptr; }
    const unsigned participants = std::min(unsigned(tasks.size()), WorkerCount() + 1);
    if (!participants) return;
    struct Context { ResourceUploadWorkers* self; std::vector<Task>* tasks; unsigned participants; };
    Context context{this, &tasks, participants};
    workers_.Run(participants, &context, [](void* arg, unsigned participant) {
      auto& c = *static_cast<Context*>(arg);
      auto& self = *c.self;
      auto& slot = self.slots_[participant];
      bool open = false;
      for (size_t i = participant; i < c.tasks->size(); i += c.participants) {
        Task& task = (*c.tasks)[i];
        try {
          if (!task.prepare()) continue;
          std::lock_guard lock(self.record_mutex_);
          if (!slot.list) {
            nvrhi::CommandListParameters p;
            p.enableImmediateExecution = false;
            slot.list = self.device_->createCommandList(p);
          }
          if (!slot.list) continue;
          if (!open) { slot.list->open(); open = true; }
          task.record(slot.list);
          task.ready = true;
          slot.bytes += task.bytes;
        } catch (...) {
          task.error = std::current_exception();
        }
      }
      if (open) {
        std::lock_guard lock(self.record_mutex_);
        slot.list->close();
        slot.recorded = true;
      }
    });
  }
  template <typename Submit> void SubmitAll(Submit&& submit) {
    // Even a stale snapshot's copies must be submitted before reusing its list:
    // this gives NVRHI's staging allocations their normal completion lifetime.
    for (auto& slot : slots_) if (slot.recorded) {
      submit(slot.list.Get(), slot.bytes);
      slot.recorded = false;
    }
  }

 private:
  struct Slot { nvrhi::CommandListHandle list; bool recorded = false; uint64_t bytes = 0; };
  nvrhi::IDevice* device_;
  kknr::TextureConversionWorkers workers_;
  std::mutex record_mutex_;
  std::array<Slot, 4> slots_;
  std::vector<Scratch> scratch_;
};

}  // namespace nr
