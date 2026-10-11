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
#include <atomic>
#include <condition_variable>
#include <thread>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/ambient_occlusion.h"
#include "backend/draw_sink.h"
#include "backend/frame_log.h"
#include "backend/pipeline_cache.h"
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
    // Read each vertex element with its declaration's own endian field (the
    // game's SHORT4N normals and tangents are 8in16 in 8in32 streams) rather
    // than the vertex buffer's. Proven on the game's frames: with the
    // buffer's endian, Kong's fur shells and the V-Rex's lighting follow
    // scrambled normals.
    bool element_endian = true;
    // Flip the guest's front face (in case the host's winding came out the
    // other way round).
    bool flip_front_face = false;
    // Small textures whose whole mip chain is in the base level's packed tail
    // and that are bound with mip address 0: read levels 1+ from that tail
    // (kknr::TextureOptions::mips_from_base_tail). Off: level 0 only, as the
    // SDK.
    bool texture_tail_mips = false;
    // Vertex buffers the game rewrites every frame stop being watched: each draw uploads the
    // vertices it reads instead. Watching them cost a write fault per 4 KB page and frame (340 a
    // frame in Venture: the game writes a 4 MB ring of particle vertices between its draws).
    bool dynamic_vertex_buffers = false;
    // The port's anisotropic_override (the launcher's texture filtering): -1 the game's own, else the
    // Xenos AnisoFilter value every sampler uses (0 off, 1 1:1, 2 2:1, 3 4:1, 4 8:1, 5 16:1), as the
    // Xenos plugin applies it.
    int32_t anisotropic_override = -1;
    // The port's swap_post_effect, which the Xenos plugin applies in its swap: 0 none, 1 "fxaa",
    // 2 "fxaa_extreme" (more edge search steps). Applied to the frame image at Present.
    uint32_t fxaa = 0;
    // Create the game's pipelines on a worker thread; a draw whose pipeline
    // is not ready yet is skipped (a missing object for a frame or two the
    // first time a state combination appears) instead of stalling the render
    // thread for the driver's compile. Vulkan only (NVRHI's D3D12 backend
    // caches root signatures without a lock).
    bool async_pipelines = true;
    // A draw whose pipeline is still being made: false skips the draw (the Xenos plugin can do
    // the same), true waits for the worker to finish it. Only with async_pipelines.
    bool pipeline_wait = false;
    // Without pipeline_wait: the time a frame may spend waiting for pipelines being made before
    // its draws are skipped (milliseconds, all draws of the frame together; 0 skips at once).
    // The workers usually finish a pipeline in a few milliseconds, so most new pipelines are
    // drawn in their first frame, and the frame the game leaves on screen during a load (the
    // V-Rex loading screen) is complete, while a burst of hundreds at a chapter start still
    // costs one long frame at most rather than a stall.
    double pipeline_wait_budget_ms = 30.0;
    // Render scale: host pixels per guest pixel of the colour and depth targets (1 = the
    // console's size, bit for bit what the renderer always did). Resolved textures and the
    // screen copies follow. docs/backend.md, "Render scale". Set before Initialize.
    float render_scale_x = 1.0f, render_scale_y = 1.0f;
    // The same for shadow maps (colour targets k_32_FLOAT whose pitch is not the frame's);
    // 0: like the render scale.
    float shadow_scale = 0.0f;
    // The guest frame's width (the back buffer's pitch): a k_32_FLOAT target of any other pitch
    // is a shadow map.
    uint32_t guest_frame_width = 1280;
    // Debugging: show the texture a resolve wrote at this guest address instead of the frame (0 off).
    uint32_t show_texture = 0;
    // Ambient occlusion (docs/backend.md, "Ambient occlusion"): the settings,
    // asked for at every depth and colour resolve, so the launcher's change of
    // ao_mode / ao_strength applies at once. Unset (or mode 0): off, and nothing
    // of it runs.
    std::function<AoSettings()> ao_settings;
    // Log every draw of this game frame (-1 none).
    int32_t dump_frame = -1;
    // Debugging aids (bits): 1 the game's clears are green, 2 the frame image
    // is magenta before the back buffer is copied in, 4 no gamma ramp, 16 plain pixel shaders at a
    // render scale (not the scale-aware ones), 32 only frame-wide targets scale, 64 watches armed every
    // time, 128 no widening of a written range, 8 log the pages CPU writes
    // invalidate most (every 300 frames).
    uint32_t debug = 0;
  };

  struct Stats {
    uint64_t draws = 0;
    uint64_t skipped_shader = 0;     // a shader could not be made
    uint64_t skipped_target = 0;     // nothing to draw into
    uint64_t skipped_primitive = 0;  // primitive type not handled
    uint64_t skipped_pipeline = 0;   // pipeline creation failed
    uint64_t skipped_pending = 0;    // pipeline still being created (async_pipelines)
    uint64_t pipeline_waits = 0;     // draws that waited for their pipeline (pipeline_wait)
    double pipeline_wait_ms = 0;
    uint64_t pipeline_budget_waits = 0;  // draws drawn after waiting within pipeline_wait_budget_ms
    uint64_t dynamic_vertex_buffers = 0;  // vertex buffers turned dynamic
    uint64_t dynamic_vertex_bytes = 0;    // bytes their draws uploaded
    uint64_t cache_recorded = 0;     // descriptions appended to the pipeline cache file
    uint64_t prewarm_queued = 0;     // cached descriptions sent to the workers
    uint64_t prewarm_deferred = 0;   // cached descriptions whose shaders were not available yet
    uint64_t prewarm_failed = 0;     // cached descriptions that could not be built or created
    uint64_t skipped_device = 0;     // no device struct
    uint64_t texture_uploads = 0;
    uint64_t texture_failures = 0;
    uint64_t buffer_uploads = 0;
    uint64_t clears = 0;
    uint64_t resolves = 0;
    uint64_t resolve_failures = 0;
    uint64_t pipelines = 0;
    uint64_t draw_pipelines = 0;     // of those, the game draws' (and the prewarm's), not the built-in passes'
    uint64_t invalidations = 0;
    uint64_t presents_missing = 0;  // no host target for the back buffer
    uint64_t ao_applied = 0;        // scene copies the ambient occlusion went into
    uint64_t aliases = 0;           // textures converted from a resolve through another format
    uint64_t alias_failures = 0;
    double pipeline_ms = 0;         // time spent creating pipelines
    double texture_ms = 0;          // time spent converting and uploading textures
  };

  GameRenderer(nvrhi::IDevice* device, const GuestMemory& memory, kknr::GuestMemory physical,
               ShaderLibrary* shaders);
  ~GameRenderer();

  bool Initialize();
  Options& options() { return options_; }
  // Any target or texture at a scale other than 1 (the pixel shaders must be render scale aware).
  bool scaled() const {
    return options_.render_scale_x != 1.0f || options_.render_scale_y != 1.0f ||
           (options_.shadow_scale > 0.0f && options_.shadow_scale != 1.0f);
  }
  const Stats& stats() const { return stats_; }

  // Enables CPU write notifications for a physical range (the SDK's watches);
  // the notifications come back through InvalidateRange.
  void set_watch(std::function<void(uint32_t physical, uint32_t bytes)> watch);
  // Guest memory was written by the CPU (any thread).
  void InvalidateRange(uint32_t physical, uint32_t bytes);
  // The SDK's notification of a CPU write to watched memory (any thread, inside its fault handler):
  // invalidates and returns the range that may be unwatched, which is every watched range the write
  // touched, whole. A game that rewrites a 7 MB vertex buffer then faults once for it instead of
  // once per 4 KB page (1,800 faults a frame in Venture, 18 instead of 30 frames a second).
  std::pair<uint32_t, uint32_t> OnGuestWrite(uint32_t physical, uint32_t bytes);

  void Draw(nvrhi::ICommandList* cl, const DrawCall& call);
  void Clear(nvrhi::ICommandList* cl, const ClearCall& call);
  void Resolve(nvrhi::ICommandList* cl, const ResolveCall& call);
  // Copies the game's back buffer into target (the frame image). False when
  // there is nothing to show yet.
  bool Present(nvrhi::ICommandList* cl, uint32_t device, nvrhi::ITexture* target);
  // After the frame's command list was submitted.
  void EndFrame();

  // The pipeline cache file (pipeline_cache.h): reads the descriptions of earlier runs and
  // starts creating them on the worker threads, then appends each new description the draws
  // need. Needs async_pipelines on Vulkan (the workers); otherwise the file is only recorded.
  // Call before the first draw; may be called from another thread.
  bool SetPipelineCache(const std::string& path);
  // Blocks until the worker threads have finished the queued prewarm (tests, shutdown).
  void WaitForPrewarm();

  // REX_DEV_FRAME_LOG: the draw and resolve lines of frame_log.h, from the game's
  // calls. Off until set; emit null = the warning log (the harness reads it there).
  void SetFrameLog(const FrameLogConfig& config, FrameLog::Emit emit = nullptr);
  FrameLog* frame_log() { return frame_log_.get(); }
  // The microcode hash the Xenos log prints for a vertex shader bound with this declaration
  // and these streams: the library patches the fetches (kkshaders/vertex_patch.h).
  uint64_t ObjectVertexShaderHash(uint32_t object_address, uint32_t* bytes_out);
  // NrApi::vertex_shader_patched: the hash of the microcode the library patched for the shader's
  // declaration (what the GPU runs and the Xenos log hashes), for the frame log.
  void VertexShaderPatched(uint32_t shader, uint32_t microcode, uint32_t declaration);
  uint64_t PatchedVertexShaderHash(const GameShader& vs, uint32_t declaration, const dev::DeviceView& d);

  // The host render targets made so far (tests and debugging).
  struct TargetInfo {
    bool depth = false;
    uint32_t edram_base = 0, pitch = 0, format = 0;  // the key
    uint32_t width = 0, height = 0;           // the guest's size
    uint32_t host_width = 0, host_height = 0;  // the texture's
    nvrhi::Format host_format = nvrhi::Format::UNKNOWN;
    nvrhi::ITexture* texture = nullptr;
  };
  std::vector<TargetInfo> DebugTargets() const;

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
    uint32_t width = 0, height = 0;  // in guest pixels (the pitch and the rows drawn so far)
    uint32_t host_width = 0, host_height = 0;  // the texture's: the guest's times the scale
    float scale_x = 1.0f, scale_y = 1.0f;
    bool depth = false;
    uint64_t key = 0;
  };
  struct Scale {
    float x = 1.0f, y = 1.0f;
  };
  // The scale of the targets with this pitch (shadow maps have their own).
  Scale ScaleFor(bool depth, uint32_t pitch, uint32_t format);
  static int32_t ScaleCoord(int32_t v, float s) { return s == 1.0f ? v : int32_t(std::lround(double(v) * s)); }
  static nvrhi::Rect ScaleRect(const nvrhi::Rect& r, float sx, float sy) {
    return nvrhi::Rect(ScaleCoord(r.minX, sx), ScaleCoord(r.maxX, sx), ScaleCoord(r.minY, sy),
                       ScaleCoord(r.maxY, sy));
  }
  std::unordered_set<uint32_t> shadow_pitches_;
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
    // Resolved from a scaled target: the host texture is the guest's size times this.
    float scale_x = 1.0f, scale_y = 1.0f;
    std::unordered_map<uint32_t, uint32_t> views;  // view key -> descriptor index
    // Resolves: bumped on every write, and whether the texels are depth
    // values (a depth source into R32F).
    uint64_t serial = 0;
    bool depth = false;
    // A texture whose memory a resolve wrote through another texture object
    // (other format or size): the converted copy and the write it holds.
    const HostTexture* alias_source = nullptr;
    uint64_t alias_serial = 0;
  };
  // The texture at a resolved texture's address seen through another fetch
  // constant (the light shafts read the depth resolve's k_24_8 memory as
  // k_8_8_8_8): a converted copy of the resolve's texture.
  HostTexture* AliasTexture(nvrhi::ICommandList* cl, kknr::TextureCache::Entry& entry,
                            const kknr::TextureFetch& fetch, const HostTexture* source);
  // Descriptor index (with bit 31 for a 2D array) for a texture fetch slot.
  // base_map: a sampler of the slot uses the base level only (mip filter
  // "base map"), so the view holds one level.
  uint32_t BindTexture(nvrhi::ICommandList* cl, const uint32_t words[6],
                       kkshaders::TextureDimension dimension, bool base_map);
  HostTexture* UploadTexture(nvrhi::ICommandList* cl, kknr::TextureCache::Entry& entry,
                             const kknr::TextureFetch& fetch);
  HostTexture* CreateHostTexture(const kknr::HostTexturePlan& plan, bool render_target, float scale_x = 1.0f,
                                 float scale_y = 1.0f);
  // The scale of the texture BindTexture bound last (1 when none).
  float bound_scale_x_ = 1.0f, bound_scale_y_ = 1.0f;
  void ReleaseHostTexture(HostTexture* host);
  // Textures and buffers the caches let go of, kept for as many frames as a retired descriptor
  // slot. The frames still on the GPU may sample them through the bindless tables, and the open
  // command list may hold their state without a reference (NVRHI's Vulkan clears and the tables
  // do not add one): destroying them at once freed images the command list then put a barrier on
  // at its close (a crash in the NVIDIA driver at the start of the Venture chapter).
  void RetireResource(nvrhi::IResource* resource);
  nvrhi::TextureHandle fxaa_source_;  // Options::fxaa: the frame before the anti-aliasing
  std::deque<std::pair<uint64_t, nvrhi::ResourceHandle>> retired_resources_;  // (frame, resource)
  uint32_t TextureView(HostTexture* host, const kknr::TextureFetch& fetch,
                       kkshaders::TextureDimension dimension, bool base_map);
  uint32_t BindSampler(const uint32_t words[6], const kkshaders::SamplerBinding& binding);
  struct HostBuffer {
    nvrhi::BufferHandle buffer;
    uint32_t slot = 0;  // kBuffers descriptor (vertex data)
    uint32_t bytes = 0;
    // Dynamic vertex data (Options::dynamic_vertex_buffers): re-uploaded in consecutive frames.
    uint64_t last_upload_frame = ~0ull;
    uint32_t upload_streak = 0;
    bool dynamic = false;
    kknr::BufferCache::Entry* entry = nullptr;
  };
  // used_begin / used_end: the bytes of the buffer this draw reads (a dynamic buffer uploads only
  // those); the whole buffer when unknown.
  HostBuffer* BindVertexBuffer(nvrhi::ICommandList* cl, uint32_t physical, uint32_t bytes, uint32_t used_begin = 0,
                               uint32_t used_end = ~0u);
  HostBuffer* BindIndexBuffer(nvrhi::ICommandList* cl, uint32_t physical, uint32_t bytes,
                              bool index32);
  void ProcessInvalidations();
  void LogInvalidationHistogram();
  std::unordered_map<uint32_t, uint64_t> invalidation_histogram_;  // --native_debug=8
  uint64_t invalidation_bytes_ = 0;

  // ---- pipelines
  // async: the game's draws (may return null while the worker creates it);
  // the built-in passes create theirs at once.
  nvrhi::IGraphicsPipeline* GetPipeline(const nvrhi::GraphicsPipelineDesc& desc,
                                        nvrhi::IFramebuffer* framebuffer, uint64_t key, bool async = false) {
    return GetPipeline(desc, framebuffer->getFramebufferInfo(), key, async);
  }
  nvrhi::IGraphicsPipeline* GetPipeline(const nvrhi::GraphicsPipelineDesc& desc,
                                        const nvrhi::FramebufferInfo& framebuffer, uint64_t key, bool async = false);
  void BuildPipelineDesc(const PipelineRecord& record, nvrhi::IShader* vs, nvrhi::IShader* ps,
                         nvrhi::GraphicsPipelineDesc& desc, nvrhi::FramebufferInfo& framebuffer) const;
  bool IsPending(uint64_t key);
  void EnsureWorkers();
  // A description the draws need: appended to the cache file the first time.
  void NoteRecord(const PipelineRecord& record, uint64_t key);
  // Queues the description of a cached record for the workers; false when a shader is not there
  // yet (the record waits in deferred_ for the next try).
  bool QueuePrewarm(const PipelineRecord& record);
  void RetryDeferred();
  void PipelineWorker();
  struct PipelineJob {
    uint64_t key = 0;
    nvrhi::GraphicsPipelineDesc desc;
    nvrhi::FramebufferInfo framebuffer;
  };
  std::vector<std::thread> pipeline_threads_;
  std::condition_variable pipeline_done_cv_;
  PipelineCacheFile cache_file_;
  std::mutex cache_mutex_;                    // the file and deferred_ (SetPipelineCache comes from another thread)
  std::vector<PipelineRecord> deferred_;
  std::atomic<bool> cache_recording_{false};
  std::unordered_set<uint64_t> noted_keys_;   // render thread only
  int pipelines_active_ = 0;                  // jobs a worker is compiling (pipeline_mutex_)
  uint64_t deferred_shader_count_ = 0;        // shaders registered when deferred_ was last tried
  std::mutex pipeline_mutex_;
  std::condition_variable pipeline_cv_;
  std::deque<PipelineJob> pipeline_jobs_;
  std::vector<std::pair<uint64_t, nvrhi::GraphicsPipelineHandle>> pipelines_done_;
  std::unordered_map<uint64_t, bool> pipelines_pending_;
  bool pipeline_stop_ = false;
  double pipeline_worker_ms_ = 0;

  // ---- frame log (game_frame_log.cpp)
  // --native_dump_frame: the three candidates for the Xenos log's vs hash (docs/backend.md, "The vertex
  // shader hash"): the database's microcode, the microcode in the shader object's memory right now (the
  // library may patch in place), and the reproduced patching.
  void DumpVertexShaderHashes(const DrawCall& call, const dev::DeviceView& d, const GameShader& vs);
  void LogDraw(const DrawCall& call, const dev::DeviceView& d, const GameShader& vs, const GameShader* ps);
  void LogResolve(const ResolveCall& call, const dev::DeviceView& d);
  void LogClear(const ClearCall& call, const dev::DeviceView& d);
  void LogSwap(const dev::DeviceView& d);
  // A draw's line waits for the next call the library forwards (draw, clear, resolve, present):
  // the library patches the vertex shader's fetches for the draw's declaration inside its draw,
  // after the hook, so the object holds this draw's microcode only once the draw returned.
  void FlushPendingDraw();
  void NoteDrawNotDrawn(bool pending);
  bool has_pending_draw_ = false;
  FrameLogDraw pending_draw_;
  uint32_t pending_vs_object_ = 0;
  uint64_t pending_vs_template_ = 0;
  int pending_not_drawn_ = -1;  // -1 drawn, 0 pipeline missing, 1 pipeline not ready
  // The library keeps a patched copy per (shader, declaration, strides) and patches again only for
  // a new combination, so the hash is kept per combination (VertexShaderPatched).
  std::unordered_map<uint64_t, uint64_t> patched_vs_hash_;
  std::mutex patched_mutex_;  // the hook's thread writes it, the render thread reads it
  uint64_t pending_vs_key_ = 0;
  std::unique_ptr<FrameLog> frame_log_;
  std::unordered_map<uint64_t, uint64_t> patched_hashes_;

  // ---- passes
  struct ViewportSetup {
    nvrhi::Viewport viewport;
    nvrhi::Rect scissor;
    float ndc_scale[3] = {1, 1, 1};
    float ndc_offset[3] = {0, 0, 0};
  };
  // width, height: the guest's target size (the vertex shaders' clip space is in guest pixels);
  // the viewport and scissor are in host pixels (the guest's times the scale).
  ViewportSetup ComputeViewport(const dev::DeviceView& d, uint32_t width, uint32_t height, float scale_x,
                                float scale_y) const;
  void ClearRect(nvrhi::ICommandList* cl, HostTarget* color, HostTarget* depth, const nvrhi::Rect& rect,
                 const float rgba[4], bool clear_color, bool clear_depth, bool clear_stencil, float z,
                 uint32_t stencil);
  bool Blit(nvrhi::ICommandList* cl, nvrhi::ITexture* source, int32_t sx, int32_t sy, int32_t w,
            int32_t h, nvrhi::ITexture* dest, uint32_t dest_level, uint32_t dest_slice, int32_t dx,
            int32_t dy, const uint32_t channels[4], uint32_t blit_flags = 0, uint32_t blit_arg = 0);
  static constexpr uint32_t kBlitGammaRamp = 1;   // r, g, b through the display gamma ramp
  static constexpr uint32_t kBlitFxaa = 16;       // FXAA (blit_arg: edge search steps)
  static constexpr uint32_t kBlitDepthBytes = 2;  // a depth value as the D24S8 word's bytes
  static constexpr uint32_t kBlitBytesToDepth = 4;  // 8:8:8:8 colour holding those bytes back to depth
  static constexpr uint32_t kBlitTexelBytesToDepth = 8;  // the same from a texture's X, Y, Z, W bytes
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
  nvrhi::BufferHandle vs_constants_, ps_constants_, draw_constants_, scale_constants_;
  std::vector<nvrhi::BindingLayoutHandle> pipeline_layouts_;  // set 0 + the six tables

  nvrhi::TextureHandle dummy_2d_, dummy_3d_, dummy_cube_, dummy_array_;
  nvrhi::SamplerHandle dummy_sampler_;
  nvrhi::BufferHandle dummy_buffer_;
  bool dummies_uploaded_ = false;

  std::unordered_map<uint64_t, std::unique_ptr<HostTarget>> targets_;
  std::unordered_map<uint64_t, nvrhi::FramebufferHandle> framebuffers_;
  std::unordered_map<uint64_t, nvrhi::GraphicsPipelineHandle> pipelines_;

  // Ambient occlusion (null until Initialize, and when its shaders are missing).
  std::unique_ptr<AmbientOcclusion> ao_;

  kknr::TextureCache textures_;
  // GPU-written (resolved) textures by base address, for other fetch
  // constants over the same memory.
  std::unordered_map<uint32_t, kknr::TextureCache::Entry*> resolved_by_base_;
  uint64_t resolve_serial_ = 0;
  kknr::BufferCache vertex_buffers_;
  kknr::BufferCache index_buffers_;
  std::unordered_map<uint64_t, std::pair<nvrhi::SamplerHandle, uint32_t>> samplers_;
  std::function<void(uint32_t, uint32_t)> watch_;
  // The ranges watched now (start -> end, overlapping ones merged), for OnGuestWrite.
  std::mutex watched_mutex_;
  std::map<uint32_t, uint32_t> watched_buffers_, watched_textures_;
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
  std::array<uint8_t, 4096> last_vs_raw_ = {}, last_ps_raw_ = {};  // the guest bytes of those
  bool constants_valid_ = false;
  nvrhi::ICommandList* constants_list_ = nullptr;
  uint64_t constants_frame_ = 0;
  uint64_t wait_budget_frame_ = ~0ull;  // the frame frame_wait_ms_ counts for
  double frame_wait_ms_ = 0;
};

}  // namespace nr
