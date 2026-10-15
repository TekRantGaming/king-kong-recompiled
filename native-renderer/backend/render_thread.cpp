#include "backend/render_thread.h"

#include <algorithm>
#include <cstring>

#include "backend/cpu_profile.h"
#include "backend/guest_device.h"

namespace nr {

namespace {

thread_local const GuestSnapshot* t_snapshot = nullptr;

// How far the game may run ahead of the render thread: max_queued_presents frames (by default one: two
// made unlimited frame rates more uneven), or this many calls.
constexpr size_t kMaxQueuedCommands = 8000;

// The parts of the device the renderer reads (guest_device.h): fetch, float, bool and loop constants;
// the register images; the bound objects, streams and strides, viewport and clip planes; the front
// and back buffers; the gamma ramp. A call whose device differs from the previous call's elsewhere
// only (the library's dirty bits and counters change at every call) shares the previous copy.
constexpr uint32_t kReadRanges[][2] = {{1152, 10272}, {11424, 12000}, {12532, 12944}, {13960, 13968}, {14060, 15596}};

// Commands kept for reuse at most (each holds a few kilobytes of room).
constexpr size_t kMaxFreeCommands = 1024;

// Object sizes copied (d3d-structs.md): surfaces are 64 bytes, a texture's fetch constant ends at
// +40, a vertex buffer's at +20; a declaration has its count at +8 and 12-byte elements from +36.
constexpr uint32_t kSurfaceBytes = 64;
constexpr uint32_t kTextureBytes = 64;
constexpr uint32_t kVertexBufferBytes = 32;
constexpr uint32_t kMaxDeclarationElements = 64;

uint64_t Micros(std::chrono::steady_clock::time_point from) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from).count());
}

}  // namespace

void GuestSnapshot::AddObject(const GuestMemory& memory, uint32_t address, uint32_t size) {
  if (!address || !size) return;
  for (const Object& o : objects)
    if (o.address == address && o.size >= size) return;
  const uint8_t* p = memory.Virtual(address);
  if (!p) return;
  Object o{address, size, uint32_t(bytes.size())};
  bytes.insert(bytes.end(), p, p + size);
  objects.push_back(o);
}

const uint8_t* GuestSnapshot::Find(uint32_t address) const {
  if (device && address >= device_address && address - device_address < device_size)
    return device + (address - device_address);
  for (const Object& o : objects)
    if (address >= o.address && address - o.address < o.size) return bytes.data() + o.offset + (address - o.address);
  return nullptr;
}

const uint8_t* SnapshotGuestMemory::Virtual(uint32_t address) const {
  if (const GuestSnapshot* s = t_snapshot) {
    if (const uint8_t* p = s->Find(address)) return p;
  }
  return live_.Virtual(address);
}

void SnapshotGuestMemory::SetCurrent(const GuestSnapshot* snapshot) { t_snapshot = snapshot; }

ThreadedSink::ThreadedSink(DrawSink& inner, const GuestMemory& memory)
    : inner_(inner),
      memory_(memory),
      sent_device_(dev::kSize, 0),
      device_image_(dev::kSize, 0),
      thread_([this] { Run(); }) {}

ThreadedSink::~ThreadedSink() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_all();
  thread_.join();
}

const std::vector<ThreadedSink::DeviceBlock>& ThreadedSink::DeviceBlocks() {
  static const std::vector<DeviceBlock> blocks = [] {
    std::vector<DeviceBlock> b;
    for (const auto& r : kReadRanges)
      for (uint32_t o = r[0]; o < r[1]; o += 64) b.push_back({uint16_t(o), uint16_t(std::min<uint32_t>(64, r[1] - o))});
    return b;
  }();
  return blocks;
}

std::unique_ptr<ThreadedSink::Command> ThreadedSink::NewCommand(Kind kind) {
  std::unique_ptr<Command> c = next_ ? std::move(next_) : std::make_unique<Command>();
  c->kind = kind;
  c->snapshot.device = nullptr;
  c->snapshot.device_size = 0;
  c->snapshot.device_address = 0;
  c->snapshot.bytes.clear();
  c->snapshot.objects.clear();
  c->device_address = 0;
  c->device_blocks.clear();
  c->device_bytes.clear();
  return c;
}

bool ThreadedSink::CaptureDevice(Command& c, uint32_t device) {
  const uint8_t* live = device ? memory_.Virtual(device) : nullptr;
  if (!live) return false;
  // The blocks that differ from what the render thread will have (all of them for another device).
  // Comparing only the parts the renderer reads: the library's dirty bits and counters change at
  // every call elsewhere.
  const bool all = sent_device_address_ != device;
  const std::vector<DeviceBlock>& blocks = DeviceBlocks();
  uint8_t* sent = sent_device_.data();
  for (size_t i = 0; i < blocks.size(); ++i) {
    const DeviceBlock& b = blocks[i];
    const bool same = !all && (b.size == 64 ? SameBytes64(live + b.offset, sent + b.offset)
                                            : std::memcmp(live + b.offset, sent + b.offset, b.size) == 0);
    if (same) continue;
    std::memcpy(sent + b.offset, live + b.offset, b.size);
    c.device_blocks.push_back(uint16_t(i));
    c.device_bytes.insert(c.device_bytes.end(), live + b.offset, live + b.offset + b.size);
  }
  sent_device_address_ = device;
  c.device_address = device;
  if (!c.device_blocks.empty()) device_copies_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

uint32_t ThreadedSink::SentDeviceWord(uint32_t offset) const { return LoadBE32(sent_device_.data() + offset); }

void ThreadedSink::CaptureSurface(GuestSnapshot& s, uint32_t surface) {
  s.AddObject(memory_, surface, kSurfaceBytes);
}

void ThreadedSink::OnClear(const ClearCall& call) {
  const auto start = std::chrono::steady_clock::now();
  std::unique_ptr<Command> c = NewCommand(Kind::kClear);
  c->clear = call;
  if (CaptureDevice(*c, call.device)) {
    for (uint32_t i = 0; i < 4; ++i) CaptureSurface(c->snapshot, SentDeviceWord(dev::kRenderTargets + 4 * i));
  }
  CaptureSurface(c->snapshot, call.render_target0);
  CaptureSurface(c->snapshot, call.depth_stencil);
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnDraw(const DrawCall& call) {
  const auto start = std::chrono::steady_clock::now();
  std::unique_ptr<Command> c = NewCommand(Kind::kDraw);
  c->draw = call;
  const bool device = CaptureDevice(*c, call.device);
  for (uint32_t rt : call.render_targets) CaptureSurface(c->snapshot, rt);
  CaptureSurface(c->snapshot, call.depth_stencil);
  if (const uint8_t* decl = call.vertex_declaration ? memory_.Virtual(call.vertex_declaration) : nullptr) {
    const uint32_t count = std::min(LoadBE32(decl + 8), kMaxDeclarationElements);
    c->snapshot.AddObject(memory_, call.vertex_declaration, 36 + 12 * count);
  }
  if (device) {
    for (uint32_t s = 0; s < 16; ++s)
      c->snapshot.AddObject(memory_, SentDeviceWord(dev::kStreamBuffer + 8 * s), kVertexBufferBytes);
  }
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnResolve(const ResolveCall& call) {
  const auto start = std::chrono::steady_clock::now();
  std::unique_ptr<Command> c = NewCommand(Kind::kResolve);
  c->resolve = call;
  CaptureDevice(*c, call.device);
  for (uint32_t rt : call.render_targets) CaptureSurface(c->snapshot, rt);
  CaptureSurface(c->snapshot, call.render_target0);
  CaptureSurface(c->snapshot, call.depth_stencil);
  c->snapshot.AddObject(memory_, call.dest_texture, kTextureBytes);
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnPresent(uint32_t frame, uint32_t render_target0, uint32_t device) {
  const auto start = std::chrono::steady_clock::now();
  // The calling (game) thread's CPU time, for the frame timing summary (one query a frame).
  const uint64_t game_cpu_us = CurrentThreadCpuMicroseconds();
  {
    std::lock_guard lock(mutex_);
    stats_.game_cpu_us = game_cpu_us;
  }
  std::unique_ptr<Command> c = NewCommand(Kind::kPresent);
  c->present_frame = frame;
  c->present_rt0 = render_target0;
  c->present_device = device;
  const bool has_device = CaptureDevice(*c, device);
  CaptureSurface(c->snapshot, render_target0);
  if (has_device) {
    CaptureSurface(c->snapshot, SentDeviceWord(dev::kBackBuffer));
    c->snapshot.AddObject(memory_, SentDeviceWord(dev::kFrontBuffer), kTextureBytes);
  }
  Enqueue(std::move(c), start);
}

void ThreadedSink::Enqueue(std::unique_ptr<Command> command, std::chrono::steady_clock::time_point start) {
  const bool present = command->kind == Kind::kPresent;
  std::unique_lock lock(mutex_);
  const auto copied = std::chrono::steady_clock::now();
  room_cv_.wait(lock, [&] {
    return stop_ || (queue_.size() < kMaxQueuedCommands && (!present || queued_presents_ < max_queued_presents));
  });
  stats_.backpressure_us += Micros(copied);
  queue_.push_back(std::move(command));
  if (present) ++queued_presents_;
  // The next call's command: one the render thread has run, if any.
  if (!next_ && !free_.empty()) {
    next_ = std::move(free_.back());
    free_.pop_back();
  }
  stats_.max_backlog = std::max<uint64_t>(stats_.max_backlog, queue_.size());
  ++stats_.commands;
  queued_.fetch_add(1, std::memory_order_release);
  stats_.enqueue_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(copied - start).count());
  LongFrame lf;
  bool report = false;
  if (present) {
    ++stats_.presents;
    const auto now = std::chrono::steady_clock::now();
    if (last_present_.time_since_epoch().count() != 0) {
      lf.frame_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - last_present_).count());
      report = on_long_frame && lf.frame_us > long_frame_us;
      lf.copy_us = stats_.enqueue_us - frame_start_stats_.enqueue_us;
      lf.room_us = stats_.backpressure_us - frame_start_stats_.backpressure_us;
      lf.fence_waits = stats_.fence_waits - frame_start_stats_.fence_waits;
      lf.fence_wait_us = stats_.fence_wait_us - frame_start_stats_.fence_wait_us;
      lf.calls = stats_.commands - frame_start_stats_.commands;
      lf.backlog = queue_.size();
    }
    last_present_ = now;
    frame_start_stats_ = stats_;
  }
  lock.unlock();
  work_cv_.notify_one();
  if (report) on_long_frame(lf);
}

void ThreadedSink::WaitDone(uint64_t ticket) {
  std::unique_lock lock(mutex_);
  if (done_ >= ticket) return;
  const auto start = std::chrono::steady_clock::now();
  done_cv_.wait(lock, [&] { return done_ >= ticket || stop_; });
  ++stats_.fence_waits;
  stats_.fence_wait_us += Micros(start);
}

ThreadedSink::Stats ThreadedSink::stats() const {
  std::lock_guard lock(mutex_);
  Stats s = stats_;
  s.device_copies = device_copies_.load(std::memory_order_relaxed);
  return s;
}

void ThreadedSink::Run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    if (queue_.empty() && !stop_) {
      // Nothing queued: the render thread waits for the game (the summary's idle time).
      const auto idle = std::chrono::steady_clock::now();
      work_cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      stats_.render_idle_us += Micros(idle);
    }
    if (queue_.empty()) return;  // stopping, and nothing left
    std::unique_ptr<Command> c = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    room_cv_.notify_all();
    // The device as this call saw it: the copy brought up to date with the call's changes.
    if (c->device_address) {
      const std::vector<DeviceBlock>& blocks = DeviceBlocks();
      const uint8_t* bytes = c->device_bytes.data();
      for (uint16_t i : c->device_blocks) {
        std::memcpy(device_image_.data() + blocks[i].offset, bytes, blocks[i].size);
        bytes += blocks[i].size;
      }
      c->snapshot.device = device_image_.data();
      c->snapshot.device_size = uint32_t(device_image_.size());
      c->snapshot.device_address = c->device_address;
    }
    SnapshotGuestMemory::SetCurrent(&c->snapshot);
    switch (c->kind) {
      case Kind::kClear: inner_.OnClear(c->clear); break;
      case Kind::kDraw: inner_.OnDraw(c->draw); break;
      case Kind::kResolve: inner_.OnResolve(c->resolve); break;
      case Kind::kPresent: inner_.OnPresent(c->present_frame, c->present_rt0, c->present_device); break;
    }
    SnapshotGuestMemory::SetCurrent(nullptr);
    const bool was_present = c->kind == Kind::kPresent;
    lock.lock();
    if (free_.size() < kMaxFreeCommands) free_.push_back(std::move(c));
    if (was_present) --queued_presents_;
    ++done_;
    done_cv_.notify_all();
    room_cv_.notify_all();
  }
}

}  // namespace nr
