// The game's draws with the real thing (Phase 2): translated vertex and pixel
// shaders, textures and samplers through the resources library, vertex data
// pulled from guest memory, the full pipeline state from the device's
// register images, render targets keyed by their EDRAM placement, resolves as
// copies into textures, clears, and the back buffer into the frame image at
// Present.
//
// Recorded into the Renderer's frame command list on the game's render
// thread; everything here runs under the backend's lock except
// InvalidateRange (CPU writes to watched guest memory, from any thread).
//
// Binding model (shaders/include/kkshaders/abi.h): set 0 holds the three
// constant buffers (volatile: rewritten per draw), sets 1-6 the bindless
// tables (2D, 3D, cube and 2D array textures, samplers, vertex data buffers).
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/draw_sink.h"
#include "backend/guest_device.h"
#include "backend/guest_memory.h"
#include "backend/shader_library.h"
#include "kknr/guest_texture.h"
#include "kknr/texture_cache.h"
#include "kknr/texture_convert.h"
#include "kkshaders/abi.h"

namespace nr {

class GameRenderer {
 public:
  struct Options {
    // Read each vertex element with its declaration's own endian field
    // instead of the vertex buffer's (the hardware uses the fetch constant's,
    // which the XDK builds from the buffer: 8in32).
    bool element_endian = false;
    // Flip the guest's front face (in case the host's winding came out the
    // other way round).
    bool flip_front_face = false;
    // Small textures whose whole mip chain is in the base level's packed tail
    // and that are bound with mip address 0: read levels 1+ from that tail
    // (kknr::TextureOptions::mips_from_base_tail). Off: level 0 only, as the
    // SDK.
    bool texture_tail_mips = false;
    // Log every draw of this game frame (-1 none).
    int32_t dump_frame = -1;
    // Debugging aids (bits): 1 the game's clears are green, 2 the frame image
    // is magenta before the back buffer is copied in.
    uint32_t debug = 0;
  };

  struct Stats {
    uint64_t draws = 0;
    uint64_t skipped_shader = 0;     // a shader could not be made
    uint64_t skipped_target = 0;     // nothing to draw into
    uint64_t skipped_primitive = 0;  // primitive type not handled
    uint64_t skipped_pipeline = 0;   // pipeline creation failed
    uint64_t skipped_device = 0;     // no device struct
    uint64_t texture_uploads = 0;
    uint64_t texture_failures = 0;
    uint64_t buffer_uploads = 0;
    uint64_t clears = 0;
    uint64_t resolves = 0;
    uint64_t resolve_failures = 0;
    uint64_t pipelines = 0;
    uint64_t invalidations = 0;
    uint64_t presents_missing = 0;  // no host target for the back buffer
  };

  GameRenderer(nvrhi::IDevice* device, const GuestMemory& memory, kknr::GuestMemory physical,
               ShaderLibrary* shaders);
  ~GameRenderer();

  bool Initialize();
  Options& options() { return options_; }
  const Stats& stats() const { return stats_; }

  // Enables CPU write notifications for a physical range (the SDK's watches);
  // the notifications come back through InvalidateRange.
  void set_watch(std::function<void(uint32_t physical, uint32_t bytes)> watch);
  // Guest memory was written by the CPU (any thread).
  void InvalidateRange(uint32_t physical, uint32_t bytes);

  void Draw(nvrhi::ICommandList* cl, const DrawCall& call);
  void Clear(nvrhi::ICommandList* cl, const ClearCall& call);
  void Resolve(nvrhi::ICommandList* cl, const ResolveCall& call);
  // Copies the game's back buffer into target (the frame image). False when
  // there is nothing to show yet.
  bool Present(nvrhi::ICommandList* cl, uint32_t device, nvrhi::ITexture* target);
  // After the frame's command list was submitted.
  void EndFrame();

 private:
  // ---- bindless tables
  enum Table : uint32_t { kTex2D, kTex3D, kTexCube, kTex2DArray, kSamplers, kBuffers, kTableCount };
  struct DescriptorTable {
    nvrhi::BindingLayoutHandle layout;
    nvrhi::DescriptorTableHandle table;
    uint32_t capacity = 0;
    uint32_t next = 1;  // slot 0 is the dummy
    std::vector<uint32_t> free;
    std::deque<std::pair<uint64_t, uint32_t>> retired;  // (frame, slot)
  };
  uint32_t AllocateSlot(Table table);
  void RetireSlot(Table table, uint32_t slot);
  void WriteSlot(Table table, const nvrhi::BindingSetItem& item);

  // ---- render targets: one host texture per EDRAM placement
  struct HostTarget {
    nvrhi::TextureHandle texture;
    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    uint32_t width = 0, height = 0;
    bool depth = false;
    uint64_t key = 0;
  };
  HostTarget* GetTarget(bool depth, uint32_t edram_base, uint32_t pitch, uint32_t format,
                        uint32_t height);
  HostTarget* FindTarget(bool depth, uint32_t edram_base, uint32_t pitch, uint32_t format);
  static nvrhi::Format ColorTargetFormat(uint32_t format);
  kknr::TextureOptions TextureOptions() const {
    kknr::TextureOptions o;
    o.mips_from_base_tail = options_.texture_tail_mips;
    return o;
  }
  nvrhi::IFramebuffer* GetFramebuffer(const std::array<HostTarget*, 4>& colors, HostTarget* depth);
  // The attachment formats, for pipeline keys.
  static uint64_t FormatSignature(const std::array<HostTarget*, 4>& colors, const HostTarget* depth);

  // ---- textures, samplers, buffers
  struct HostTexture {
    nvrhi::TextureHandle texture;
    kknr::HostTexturePlan plan;
    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    bool render_target = false;  // made by a resolve (usable as a blit target)
    std::unordered_map<uint32_t, uint32_t> views;  // view key -> descriptor index
  };
  // Descriptor index (with bit 31 for a 2D array) for a texture fetch slot.
  // base_map: a sampler of the slot uses the base level only (mip filter
  // "base map"), so the view holds one level.
  uint32_t BindTexture(nvrhi::ICommandList* cl, const uint32_t words[6],
                       kkshaders::TextureDimension dimension, bool base_map);
  HostTexture* UploadTexture(nvrhi::ICommandList* cl, kknr::TextureCache::Entry& entry,
                             const kknr::TextureFetch& fetch);
  HostTexture* CreateHostTexture(const kknr::HostTexturePlan& plan, bool render_target);
  void ReleaseHostTexture(HostTexture* host);
  uint32_t TextureView(HostTexture* host, const kknr::TextureFetch& fetch,
                       kkshaders::TextureDimension dimension, bool base_map);
  uint32_t BindSampler(const uint32_t words[6], const kkshaders::SamplerBinding& binding);
  struct HostBuffer {
    nvrhi::BufferHandle buffer;
    uint32_t slot = 0;  // kBuffers descriptor (vertex data)
    uint32_t bytes = 0;
  };
  HostBuffer* BindVertexBuffer(nvrhi::ICommandList* cl, uint32_t physical, uint32_t bytes);
  HostBuffer* BindIndexBuffer(nvrhi::ICommandList* cl, uint32_t physical, uint32_t bytes,
                              bool index32);
  void ProcessInvalidations();

  // ---- pipelines
  nvrhi::IGraphicsPipeline* GetPipeline(const nvrhi::GraphicsPipelineDesc& desc,
                                        nvrhi::IFramebuffer* framebuffer, uint64_t key);

  // ---- passes
  struct ViewportSetup {
    nvrhi::Viewport viewport;
    nvrhi::Rect scissor;
    float ndc_scale[3] = {1, 1, 1};
    float ndc_offset[3] = {0, 0, 0};
  };
  ViewportSetup ComputeViewport(const dev::DeviceView& d, uint32_t width, uint32_t height) const;
  void ClearRect(nvrhi::ICommandList* cl, HostTarget* color, HostTarget* depth, const nvrhi::Rect& rect,
                 const float rgba[4], bool clear_color, bool clear_depth, bool clear_stencil, float z,
                 uint32_t stencil);
  bool Blit(nvrhi::ICommandList* cl, nvrhi::ITexture* source, int32_t sx, int32_t sy, int32_t w,
            int32_t h, nvrhi::ITexture* dest, uint32_t dest_level, uint32_t dest_slice, int32_t dx,
            int32_t dy, const uint32_t channels[4], bool gamma_ramp = false);
  bool Dump(uint32_t frame) const {
    return options_.dump_frame >= 0 && uint32_t(options_.dump_frame) == frame;
  }

  nvrhi::IDevice* device_;
  const GuestMemory& memory_;
  kknr::GuestMemory physical_;
  ShaderLibrary* shaders_;
  Options options_;
  Stats stats_;
  uint64_t frame_ = 0;

  std::array<DescriptorTable, kTableCount> tables_;
  nvrhi::BindingLayoutHandle constants_layout_;
  nvrhi::BindingSetHandle constants_set_;
  nvrhi::BufferHandle vs_constants_, ps_constants_, draw_constants_;
  std::vector<nvrhi::BindingLayoutHandle> pipeline_layouts_;  // set 0 + the six tables

  nvrhi::TextureHandle dummy_2d_, dummy_3d_, dummy_cube_, dummy_array_;
  nvrhi::SamplerHandle dummy_sampler_;
  nvrhi::BufferHandle dummy_buffer_;
  bool dummies_uploaded_ = false;

  std::unordered_map<uint64_t, std::unique_ptr<HostTarget>> targets_;
  std::unordered_map<uint64_t, nvrhi::FramebufferHandle> framebuffers_;
  std::unordered_map<uint64_t, nvrhi::GraphicsPipelineHandle> pipelines_;

  kknr::TextureCache textures_;
  kknr::BufferCache vertex_buffers_;
  kknr::BufferCache index_buffers_;
  std::unordered_map<uint64_t, std::pair<nvrhi::SamplerHandle, uint32_t>> samplers_;
  std::function<void(uint32_t, uint32_t)> watch_;
  std::mutex invalidation_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> invalidations_;

  // Static index patterns for non-indexed quad lists and fans, and a ring for
  // indexed ones.
  nvrhi::BufferHandle quad_indices_, fan_indices_, index_ring_;
  uint64_t index_ring_used_ = 0;
  static constexpr uint32_t kPatternVertices = 65536;
  static constexpr uint64_t kIndexRingBytes = 8ull << 20;
  bool patterns_uploaded_ = false;
  std::vector<uint32_t> scratch_indices_;

  // Built-in passes.
  nvrhi::ShaderHandle blit_vs_, blit_ps_, clear_vs_, clear_ps_;
  nvrhi::BindingLayoutHandle blit_layout_, clear_layout_;
  nvrhi::BindingSetHandle clear_set_;
  // The display gamma ramp (the device's copy of SetGammaRamp's table), as
  // the Xenos plugin applies it at the swap.
  nvrhi::TextureHandle gamma_ramp_;
  std::array<uint16_t, 768> gamma_ramp_values_ = {};
  bool gamma_ramp_uploaded_ = false;
  std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> blit_sets_;

  // The previous draw's constants, to rewrite the volatile buffers only when
  // they changed (and once per command list).
  std::array<float, 1024> last_vs_ = {}, last_ps_ = {};
  bool constants_valid_ = false;
  nvrhi::ICommandList* constants_list_ = nullptr;
  uint64_t constants_frame_ = 0;
};

}  // namespace nr
