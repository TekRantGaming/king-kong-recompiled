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
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "backend/ambient_occlusion.h"
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

class ShaderLibrary;

// Phase 2 settings (the plugin's cvars).
struct GameSettings {
  bool enabled = true;          // the game renderer; off: the placeholder pipeline
  std::string shader_pack;      // a kkshaders pack for the host API
  std::string dxc;              // DXC's folder or library, for shaders missing from the pack
  bool element_endian = true;  // see GameRenderer::Options
  bool flip_front_face = false;
  bool texture_tail_mips = false;
  int32_t dump_frame = -1;
  uint32_t show_texture = 0;  // see GameRenderer::Options
  uint32_t debug = 0;
  bool async_pipelines = true;
  bool pipeline_wait = false;  // see GameRenderer::Options
  bool dynamic_vertex_buffers = false;
  int32_t anisotropic_override = -1;
  uint32_t fxaa = 0;
  double pipeline_wait_budget_ms = 30.0;
  std::string frame_log;       // REX_DEV_FRAME_LOG: "<seconds>[,<frames>]", empty = off
  // Render scale (backend/render_scale.h): "1", "1.5", "2", "1920x1080". shadow_scale: 0 like the
  // render scale, else its own (a number).
  std::string render_scale = "1";
  float shadow_scale = 0.0f;
  // Ambient occlusion settings, read at every resolve (null: off). The plugin reads ao_mode and
  // ao_strength, the Xenos plugin's cvars.
  std::function<AoSettings()> ao_settings;
};

class Backend {
 public:
  Backend(std::unique_ptr<HostDevice> host, rex::memory::Memory* memory);
  ~Backend();

  bool Initialize(uint32_t width, uint32_t height, const GameSettings& game = {});
  void Shutdown();

  // The game renderer's pipeline cache file (GameRenderer::SetPipelineCache); false without a game
  // renderer or when the file cannot be opened.
  bool SetPipelineCache(const std::string& path);
  // The shader pack when it is found after Initialize (the plugin learns the cache root later,
  // before the game creates its first shader). False without a game renderer, when a pack is
  // already open, or when this one does not open (logged).
  bool OpenShaderPack(const std::string& path);
  bool has_shader_pack() const;
  void set_test_picture(TestPicture picture) { test_picture_ = picture; }
  Renderer& renderer() { return *renderer_; }
  DrawTracker* draws() { return draws_.get(); }

  // At the guest's swap, from the ring skimmer's thread. With set_async_present the work is
  // handed to a thread of its own (the newest swap wins) and this returns at once.
  void Present(rex::ui::Presenter* presenter, uint32_t frontbuffer_width,
               uint32_t frontbuffer_height, uint32_t display_width, uint32_t display_height);
  void set_async_present(bool on);

 private:
  std::unique_ptr<HostDevice> host_;
  RuntimeGuestMemory guest_memory_;
  rex::memory::Memory* memory_;
  std::unique_ptr<ShaderLibrary> shaders_;
  void* invalidation_handle_ = nullptr;
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
  // --native_present_unlocked: the presenter's refresh of a game frame runs outside mutex_.
  bool unlocked_present_ = false;
  // --native_async_submit: the frames' command lists are submitted by a thread of their own, so
  // the game's thread does not wait for the queue (the presenter holds it for milliseconds).
  class Submitter;
  std::unique_ptr<Submitter> submitter_;
  // --native_async_present: the presenter's refresh runs on this thread, so the ring skimmer
  // (and through the ring, the game) does not wait the presenter's 5 to 8 ms a frame.
  class PresentThread;
  std::unique_ptr<PresentThread> present_thread_;
  void PresentNow(rex::ui::Presenter* presenter, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
                  uint32_t display_width, uint32_t display_height);
 public:
  void set_unlocked_present(bool on) { unlocked_present_ = on; }
  void set_async_submit(bool on);
 private:
};

}  // namespace nr
