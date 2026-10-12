// --native_render_thread: the renderer's work on a thread of its own, as the Xenos plugin's command
// processor does it.
//
// Without it the hooks call the renderer on the game's thread, so the game waits for every
// pipeline, texture upload and queue submission of its draws; at a chapter's start those stalls
// changed the game's own timing (Jack landed 0.15 units higher in chap12, see backend.md). With
// it the hooks only copy what each call needs and queue it:
//
// - The D3D device struct (20,608 bytes: register images, constants, fetch constants, the bound
//   objects' addresses), whole, because the game changes it right after the call. Consecutive
//   calls with an unchanged device share one copy.
// - The small guest objects a call names (render target and depth surfaces, the vertex
//   declaration, the vertex buffer objects of the 16 streams, a resolve's destination texture,
//   the back and front buffers): their headers can be rewritten for the next call
//   (XGSetVertexBufferHeader and the like).
// - Not the data (vertices, indices, textures): the renderer reads guest memory when it runs the
//   call, as the Xenos plugin's command processor does. The game rewrites such memory only after
//   the GPU has passed a fence (D3D's Lock waits for the resource's fence, dynamic buffers are
//   fenced rings), and the ring skimmer writes a fence, a swap or an interrupt only after the
//   render thread has run every call queued before the ring was kicked (WaitDone). So the data a
//   queued draw reads is still the data the game drew with. CPU writes outside that discipline
//   reach the renderer through the write watches, as before.
//
// The renderer's guest reads go through SnapshotGuestMemory: on the render thread, an address
// inside a copied block reads the copy; everything else, and every other thread, reads live
// memory.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "backend/draw_sink.h"
#include "backend/guest_memory.h"

namespace nr {

// Guest bytes copied at a call on the game's thread.
struct GuestSnapshot {
  std::shared_ptr<const std::vector<uint8_t>> device;  // the device struct, shared between calls
  uint32_t device_address = 0;
  std::vector<uint8_t> bytes;  // the objects, back to back
  struct Object {
    uint32_t address = 0, size = 0, offset = 0;
  };
  std::vector<Object> objects;

  // Copies `size` bytes at a guest virtual address (nothing when unmapped or already copied).
  void AddObject(const GuestMemory& memory, uint32_t address, uint32_t size);
  // The copy of the byte at `address`, or null when no block holds it.
  const uint8_t* Find(uint32_t address) const;
};

// GuestMemory that reads the render thread's current snapshot first.
class SnapshotGuestMemory final : public GuestMemory {
 public:
  explicit SnapshotGuestMemory(const GuestMemory& live) : live_(live) {}
  const uint8_t* Virtual(uint32_t address) const override;
  const uint8_t* Physical(uint32_t address) const override { return live_.Physical(address); }
  // The snapshot of the call the calling thread is running (null: live memory).
  static void SetCurrent(const GuestSnapshot* snapshot);

 private:
  const GuestMemory& live_;
};

class ThreadedSink final : public DrawSink {
 public:
  struct Stats {
    uint64_t commands = 0;
    uint64_t device_copies = 0;      // calls whose device differed from the previous call's
    uint64_t enqueue_us = 0;         // the game's thread copying and queueing
    uint64_t backpressure_us = 0;    // the game's thread waiting for room in the queue
    uint64_t fence_waits = 0;        // WaitDone calls that had to wait
    uint64_t fence_wait_us = 0;      // the ring skimmer waiting for the render thread
    uint64_t max_backlog = 0;        // commands
  };

  // `inner` runs the calls on the render thread; `memory` is live guest memory, read on the game's
  // thread for the copies.
  ThreadedSink(DrawSink& inner, const GuestMemory& memory);
  ~ThreadedSink() override;  // runs what is queued, then stops

  void OnClear(const ClearCall& call) override;
  void OnDraw(const DrawCall& call) override;
  void OnResolve(const ResolveCall& call) override;
  void OnPresent(uint32_t frame, uint32_t render_target0, uint32_t device) override;
  // Shader creation stays on the calling thread (the shader library has its own lock).
  void OnShaderCreated(uint32_t kind, uint32_t container, uint32_t object) override {
    inner_.OnShaderCreated(kind, container, object);
  }
  // The library's patch of a vertex shader is read at once (the microcode is patched again for the
  // next declaration): `on_patched` runs on the calling thread.
  void OnVertexShaderPatched(uint32_t shader, uint32_t microcode, uint32_t declaration) override {
    if (on_patched) on_patched(shader, microcode, declaration);
  }
  std::function<void(uint32_t shader, uint32_t microcode, uint32_t declaration)> on_patched;

  // A game frame (Present to Present on the calling thread) longer than long_frame_us: where its
  // time went, for the log. Runs on the calling thread.
  struct LongFrame {
    uint64_t frame_us = 0;
    uint64_t copy_us = 0;          // the game's thread copying calls
    uint64_t room_us = 0;          // the game's thread waiting for room in the queue
    uint64_t fence_waits = 0;      // the ring skimmer waiting for the render thread
    uint64_t fence_wait_us = 0;
    uint64_t calls = 0;            // calls queued in the frame
    uint64_t backlog = 0;          // calls waiting when it ended
  };
  std::function<void(const LongFrame&)> on_long_frame;
  uint64_t long_frame_us = 50000;
  // Frames the game may queue before its Present waits for the render thread (set before the first call).
  uint32_t max_queued_presents = 1;

  // Calls queued so far (a ticket), and a wait until the render thread has run that many.
  uint64_t Ticket() const { return queued_.load(std::memory_order_acquire); }
  void WaitDone(uint64_t ticket);
  Stats stats() const;

 private:
  enum class Kind : uint8_t { kClear, kDraw, kResolve, kPresent };
  struct Command {
    Kind kind = Kind::kDraw;
    ClearCall clear;
    DrawCall draw;
    ResolveCall resolve;
    uint32_t present_frame = 0, present_rt0 = 0, present_device = 0;
    GuestSnapshot snapshot;
  };
  void CaptureDevice(GuestSnapshot& s, uint32_t device);
  void CaptureSurface(GuestSnapshot& s, uint32_t surface);
  void Enqueue(Command&& command, std::chrono::steady_clock::time_point start);
  void Run();

  DrawSink& inner_;
  const GuestMemory& memory_;
  // Device copies that no call holds any more, for reuse.
  struct DevicePool {
    std::mutex mutex;
    std::vector<std::vector<uint8_t>*> free;
    ~DevicePool() {
      for (auto* p : free) delete p;
    }
  };
  std::shared_ptr<DevicePool> device_pool_ = std::make_shared<DevicePool>();
  // The last device copy (game's thread only).
  std::shared_ptr<const std::vector<uint8_t>> last_device_;
  uint32_t last_device_address_ = 0;

  mutable std::mutex mutex_;
  std::condition_variable work_cv_, room_cv_, done_cv_;
  std::deque<Command> queue_;
  uint32_t queued_presents_ = 0;
  std::atomic<uint64_t> queued_{0};
  std::atomic<uint64_t> device_copies_{0};
  uint64_t done_ = 0;
  bool stop_ = false;
  Stats stats_;
  Stats frame_start_stats_;  // at the last Present
  std::chrono::steady_clock::time_point last_present_{};
  std::thread thread_;
};

}  // namespace nr
