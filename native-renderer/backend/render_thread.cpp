#include "backend/render_thread.h"

#include <algorithm>
#include <cstring>

#include "backend/guest_device.h"

namespace nr {

namespace {

thread_local const GuestSnapshot* t_snapshot = nullptr;

// How far the game may run ahead of the render thread: two frames, or this many calls.
constexpr uint32_t kMaxQueuedPresents = 2;
constexpr size_t kMaxQueuedCommands = 20000;

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
  if (device && address >= device_address && address - device_address < device->size())
    return device->data() + (address - device_address);
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
    : inner_(inner), memory_(memory), thread_([this] { Run(); }) {}

ThreadedSink::~ThreadedSink() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_all();
  thread_.join();
}

void ThreadedSink::CaptureDevice(GuestSnapshot& s, uint32_t device) {
  const uint8_t* live = device ? memory_.Virtual(device) : nullptr;
  if (!live) return;
  if (!last_device_ || last_device_address_ != device || std::memcmp(last_device_->data(), live, dev::kSize) != 0) {
    last_device_ = std::make_shared<const std::vector<uint8_t>>(live, live + dev::kSize);
    last_device_address_ = device;
    device_copies_.fetch_add(1, std::memory_order_relaxed);
  }
  s.device = last_device_;
  s.device_address = device;
}

void ThreadedSink::CaptureSurface(GuestSnapshot& s, uint32_t surface) {
  s.AddObject(memory_, surface, kSurfaceBytes);
}

void ThreadedSink::OnClear(const ClearCall& call) {
  const auto start = std::chrono::steady_clock::now();
  Command c;
  c.kind = Kind::kClear;
  c.clear = call;
  CaptureDevice(c.snapshot, call.device);
  if (c.snapshot.device) {
    for (uint32_t i = 0; i < 4; ++i) CaptureSurface(c.snapshot, LoadBE32(c.snapshot.device->data() + dev::kRenderTargets + 4 * i));
  }
  CaptureSurface(c.snapshot, call.render_target0);
  CaptureSurface(c.snapshot, call.depth_stencil);
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnDraw(const DrawCall& call) {
  const auto start = std::chrono::steady_clock::now();
  Command c;
  c.kind = Kind::kDraw;
  c.draw = call;
  CaptureDevice(c.snapshot, call.device);
  for (uint32_t rt : call.render_targets) CaptureSurface(c.snapshot, rt);
  CaptureSurface(c.snapshot, call.depth_stencil);
  if (const uint8_t* decl = call.vertex_declaration ? memory_.Virtual(call.vertex_declaration) : nullptr) {
    const uint32_t count = std::min(LoadBE32(decl + 8), kMaxDeclarationElements);
    c.snapshot.AddObject(memory_, call.vertex_declaration, 36 + 12 * count);
  }
  if (c.snapshot.device) {
    for (uint32_t s = 0; s < 16; ++s)
      c.snapshot.AddObject(memory_, LoadBE32(c.snapshot.device->data() + dev::kStreamBuffer + 8 * s), kVertexBufferBytes);
  }
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnResolve(const ResolveCall& call) {
  const auto start = std::chrono::steady_clock::now();
  Command c;
  c.kind = Kind::kResolve;
  c.resolve = call;
  CaptureDevice(c.snapshot, call.device);
  for (uint32_t rt : call.render_targets) CaptureSurface(c.snapshot, rt);
  CaptureSurface(c.snapshot, call.render_target0);
  CaptureSurface(c.snapshot, call.depth_stencil);
  c.snapshot.AddObject(memory_, call.dest_texture, kTextureBytes);
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnPresent(uint32_t frame, uint32_t render_target0, uint32_t device) {
  const auto start = std::chrono::steady_clock::now();
  Command c;
  c.kind = Kind::kPresent;
  c.present_frame = frame;
  c.present_rt0 = render_target0;
  c.present_device = device;
  CaptureDevice(c.snapshot, device);
  CaptureSurface(c.snapshot, render_target0);
  if (c.snapshot.device) {
    CaptureSurface(c.snapshot, LoadBE32(c.snapshot.device->data() + dev::kBackBuffer));
    c.snapshot.AddObject(memory_, LoadBE32(c.snapshot.device->data() + dev::kFrontBuffer), kTextureBytes);
  }
  Enqueue(std::move(c), start);
}

void ThreadedSink::Enqueue(Command&& command, std::chrono::steady_clock::time_point start) {
  const bool present = command.kind == Kind::kPresent;
  std::unique_lock lock(mutex_);
  const auto copied = std::chrono::steady_clock::now();
  room_cv_.wait(lock, [&] {
    return stop_ || (queue_.size() < kMaxQueuedCommands && (!present || queued_presents_ < kMaxQueuedPresents));
  });
  stats_.backpressure_us += Micros(copied);
  queue_.push_back(std::move(command));
  if (present) ++queued_presents_;
  stats_.max_backlog = std::max<uint64_t>(stats_.max_backlog, queue_.size());
  ++stats_.commands;
  queued_.fetch_add(1, std::memory_order_release);
  stats_.enqueue_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(copied - start).count());
  lock.unlock();
  work_cv_.notify_one();
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
    work_cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
    if (queue_.empty()) return;  // stopping, and nothing left
    Command c = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    room_cv_.notify_all();
    SnapshotGuestMemory::SetCurrent(&c.snapshot);
    switch (c.kind) {
      case Kind::kClear: inner_.OnClear(c.clear); break;
      case Kind::kDraw: inner_.OnDraw(c.draw); break;
      case Kind::kResolve: inner_.OnResolve(c.resolve); break;
      case Kind::kPresent: inner_.OnPresent(c.present_frame, c.present_rt0, c.present_device); break;
    }
    SnapshotGuestMemory::SetCurrent(nullptr);
    lock.lock();
    if (c.kind == Kind::kPresent) --queued_presents_;
    ++done_;
    done_cv_.notify_all();
    room_cv_.notify_all();
  }
}

}  // namespace nr
