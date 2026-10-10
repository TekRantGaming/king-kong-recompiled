// The plugin's backend: the NVRHI renderer on the SDK provider's device (via
// HostDevice), the draw-state tracker the D3D hooks feed, and the hand-over
// of finished frames to the SDK presenter.
//
// Frame flow:
// - The hooks drive the tracker on the game's render thread; the renderer
//   records into one command list per frame and submits it at the game's
//   Present (EndFrame).
// - At the guest's swap (the ring skimmer sees VdSwap) Present() copies the
//   last submitted frame into the presenter's guest output. If no game frame
//   was submitted since the last swap (hooks not built in, or --native_draws
//   off), it draws the test picture instead: milestone 1 (a clear colour that
//   changes every frame) or milestone 2 (the triangle over it).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "backend/draw_state.h"
#include "backend/guest_memory.h"
#include "backend/host_device.h"
#include "backend/renderer.h"

namespace rex::memory {
class Memory;
}

namespace nr {

// GuestMemory over the runtime's memory (null for unmapped addresses).
class RuntimeGuestMemory final : public GuestMemory {
 public:
  explicit RuntimeGuestMemory(rex::memory::Memory* memory) : memory_(memory) {}
  const uint8_t* Virtual(uint32_t address) const override;
  const uint8_t* Physical(uint32_t address) const override;

 private:
  rex::memory::Memory* memory_;
};

enum class TestPicture { kClear, kTriangle };

class Backend {
 public:
  Backend(std::unique_ptr<HostDevice> host, rex::memory::Memory* memory);
  ~Backend();

  bool Initialize(uint32_t width, uint32_t height);
  void Shutdown();

  void set_test_picture(TestPicture picture) { test_picture_ = picture; }
  Renderer& renderer() { return *renderer_; }
  DrawTracker* draws() { return draws_.get(); }

  // At the guest's swap, from the ring skimmer's thread.
  void Present(rex::ui::Presenter* presenter, uint32_t frontbuffer_width,
               uint32_t frontbuffer_height, uint32_t display_width, uint32_t display_height);

 private:
  std::unique_ptr<HostDevice> host_;
  RuntimeGuestMemory guest_memory_;
  std::unique_ptr<Renderer> renderer_;
  std::unique_ptr<DrawSink> sink_;  // the renderer, under mutex_
  std::unique_ptr<DrawTracker> draws_;
  TestPicture test_picture_ = TestPicture::kTriangle;
  // The renderer is used from the game's thread (hooks) and the skimmer's
  // (swap): one at a time.
  std::mutex mutex_;
  uint64_t submitted_frames_ = 0;
  uint64_t shown_frames_ = 0;
  uint32_t swaps_ = 0;
};

}  // namespace nr
