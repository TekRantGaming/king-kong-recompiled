// The NVRHI renderer: owns the frame images and pipelines, draws the
// milestone test pictures (a clear colour, a triangle) and, as a DrawSink,
// turns the draw-state tracker's records into NVRHI work (milestone 3: every
// game draw through the placeholder pipeline, a flat colour per draw).
//
// API-neutral: runs on whatever nvrhi::IDevice it is given (the plugin's,
// created on the SDK provider's device, or the test host's on lavapipe).
// Commands for the game's frame are recorded into one command list between
// two Presents; `submit` receives it closed, at Present.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/draw_sink.h"

namespace nr {

class GameRenderer;

// The placeholder pipeline's push constants (shaders/placeholder.hlsl).
struct PlaceholderConstants {
  float wvp[16];  // row-major, rows = guest c0..c3
  float color[4];
  uint32_t stride;
  uint32_t pos_offset;
  uint32_t vertex_base;  // byte offset of the draw's first vertex in the vertex ring
  uint32_t pad;
};
static_assert(sizeof(PlaceholderConstants) == 96);

// What reached NVRHI, for tests and logging.
class SubmitObserver {
 public:
  virtual ~SubmitObserver() = default;
  virtual void OnClearRecorded(const ClearCall& /*call*/, const nvrhi::Color& /*color*/) {}
  virtual void OnDrawRecorded(const DrawCall& /*call*/, const nvrhi::GraphicsState& /*state*/,
                              const nvrhi::DrawArguments& /*args*/,
                              const PlaceholderConstants& /*constants*/,
                              std::span<const uint32_t> /*indices*/) {}
  virtual void OnFrameSubmitted(uint32_t /*frame*/, nvrhi::ITexture* /*image*/) {}
};

class Renderer final : public DrawSink {
 public:
  static constexpr nvrhi::Format kFrameFormat = nvrhi::Format::R10G10B10A2_UNORM;
  // Each frame image has a depth buffer, so the placeholder draws hide each
  // other as the game's depth states say (without it the sky dome, drawn late,
  // covers the frame).
  static constexpr nvrhi::Format kDepthFormat = nvrhi::Format::D32;

  struct Options {
    // Draw only into the surface that was bound at the last Present (the
    // frame's main surface); render-to-texture passes are skipped until
    // render targets are pooled (Phase 2).
    bool only_main_surface = true;
    // Use c0..c3 as columns instead of rows.
    bool transpose_wvp = false;
    // Draw only depth-tested draws (compare function other than always) that
    // write depth or are opaque (RB_BLENDCONTROL0 ONE, ZERO): the depth
    // pre-pass and the colour pass that follows it with depth writes off.
    // Full-screen post effects, fog, the HUD and particles (sprites the
    // vertex shader expands, which the placeholder cannot) would otherwise
    // cover the frame with flat colour.
    bool only_depth_tested = true;
  };

  struct Stats {
    uint64_t draws_recorded = 0;
    uint64_t draws_skipped_target = 0;     // other render target than the main surface
    uint64_t draws_skipped_primitive = 0;  // points, lines, rectangles
    uint64_t draws_skipped_range = 0;      // indices outside the vertex buffer
    uint64_t draws_skipped_overlay = 0;    // not part of the main pass (only_depth_tested)
    uint64_t draws_skipped_no_wvp = 0;     // no perspective matrix in the vertex constants
    uint64_t draws_skipped_format = 0;     // position not 32-bit float x, y, z
    uint64_t clears_recorded = 0;
    uint64_t frames_submitted = 0;
  };

  explicit Renderer(nvrhi::IDevice* device);
  ~Renderer() override;

  // Creates the two frame images (width x height, kFrameFormat) and the
  // pipelines. Returns false on any failure (logged through the device's
  // message callback).
  bool Initialize(uint32_t width, uint32_t height);
  void Shutdown();

  Options& options() { return options_; }
  const Stats& stats() const { return stats_; }
  void set_observer(SubmitObserver* observer) { observer_ = observer; }
  // Receives the closed frame command list at Present; must execute it.
  void set_submit(std::function<void(nvrhi::ICommandList*)> submit) { submit_ = std::move(submit); }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  // The image the game's current frame is drawn into.
  nvrhi::ITexture* recording_image() const { return frames_[recording_]; }
  // The last image completed by a Present (null before the first).
  nvrhi::ITexture* presented_image() const {
    return presented_ < 0 ? nullptr : frames_[presented_].Get();
  }

  // Milestone test pictures, recorded into an open command list. `target`
  // must have kFrameFormat and be a render target.
  void RecordClear(nvrhi::ICommandList* command_list, nvrhi::ITexture* target,
                   const nvrhi::Color& color);
  void RecordTestTriangle(nvrhi::ICommandList* command_list, nvrhi::ITexture* target);

  // DrawSink.
  void OnClear(const ClearCall& call) override;
  void OnDraw(const DrawCall& call) override;
  void OnResolve(const ResolveCall& call) override;
  void OnPresent(uint32_t frame, uint32_t render_target0, uint32_t device) override;

  // Phase 2: the game's draws through the real shaders, textures and state
  // (GameRenderer) instead of the placeholder pipeline.
  void EnableGame(std::unique_ptr<GameRenderer> game);
  GameRenderer* game() { return game_.get(); }

  // Milestone 1's picture: a colour that changes every frame (a slow cycle
  // through the hues, so a stalled frame loop is visible).
  static nvrhi::Color TestClearColor(uint32_t frame);

  // A stable colour per (vertex shader, pixel shader) pair, so draws of the
  // same material look alike.
  static nvrhi::Color DrawColor(uint32_t vertex_shader, uint32_t pixel_shader);

 private:
  nvrhi::IFramebuffer* FramebufferFor(nvrhi::ITexture* target);
  nvrhi::ITexture* DepthFor(nvrhi::ITexture* target);
  nvrhi::IGraphicsPipeline* PlaceholderPipeline(const RenderStates& states);
  nvrhi::ICommandList* FrameCommandList();
  bool OnMainSurface(uint32_t render_target0) const;
  nvrhi::Viewport ViewportFor(const Viewport& v) const;

  nvrhi::IDevice* device_;
  Options options_;
  Stats stats_;
  SubmitObserver* observer_ = nullptr;
  std::function<void(nvrhi::ICommandList*)> submit_;

  uint32_t width_ = 0, height_ = 0;
  std::array<nvrhi::TextureHandle, 2> frames_;
  int recording_ = 0;
  int presented_ = -1;
  std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> framebuffers_;
  // Depth buffers by colour target (the frame images, and any test target).
  std::unordered_map<nvrhi::ITexture*, nvrhi::TextureHandle> depth_;
  // Placeholder pipelines by depth state: bit 4 test, bit 3 write, bits 0-2
  // the Xenos compare function.
  std::unordered_map<uint32_t, nvrhi::GraphicsPipelineHandle> placeholder_pipelines_;
  nvrhi::GraphicsPipelineDesc placeholder_desc_;

  nvrhi::ShaderHandle triangle_vs_, triangle_ps_, placeholder_vs_, placeholder_ps_;
  nvrhi::BindingLayoutHandle placeholder_layout_;
  nvrhi::BindingSetHandle placeholder_set_;
  nvrhi::GraphicsPipelineHandle triangle_pipeline_, placeholder_pipeline_;

  // Per-frame rings for the game's vertex and index data (reset at Present;
  // a frame larger than a ring drops the rest of its draws).
  static constexpr uint64_t kVertexRingBytes = 64ull << 20;
  static constexpr uint64_t kIndexRingBytes = 16ull << 20;
  nvrhi::BufferHandle vertex_ring_, index_ring_;
  uint64_t vertex_ring_used_ = 0, index_ring_used_ = 0;

  std::unique_ptr<GameRenderer> game_;
  nvrhi::CommandListHandle frame_command_list_;
  bool frame_open_ = false;
  uint32_t main_surface_ = 0;  // render target 0 at the last Present
  std::vector<uint32_t> scratch_indices_;
};

}  // namespace nr
