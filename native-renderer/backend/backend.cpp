#include "backend/backend.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <algorithm>
#include <chrono>
#include <thread>
#include <deque>
#include <condition_variable>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "backend/api_binding.h"
#include "backend/diagnostics.h"
#include "backend/cpu_profile.h"
#include "backend/frame_log.h"
#include "backend/gpu_loss.h"
#include "backend/log.h"
#include "backend/render_scale.h"
#if NR_GAME_RENDERER
#include "backend/game_renderer.h"
#include "backend/shader_library.h"
#include "backend/upscaler.h"
#else
namespace nr {
class ShaderLibrary {};  // never made without the game renderer; Backend's unique_ptr needs a type
}  // namespace nr
#endif

REXCVAR_DECLARE(int32_t, native_dump_frame);
REXCVAR_DECLARE(int32_t, native_debug);
REXCVAR_DECLARE(bool, native_diagnostics);

namespace nr {

const uint8_t* RuntimeGuestMemory::Virtual(uint32_t address) const {
  if (!address || !memory_->LookupHeap(address)) return nullptr;
  return memory_->TranslateVirtual(address);
}

const uint8_t* RuntimeGuestMemory::Physical(uint32_t address) const {
  return memory_->TranslatePhysical(address);
}

namespace {

// Time the game's thread spends in the renderer's calls (lock wait included), in microseconds,
// for the periodic statistics line: draws, clears, resolves, presents.
std::atomic<uint64_t> g_call_us[5];  // + 4: watching guest memory (inside the others)
std::atomic<uint64_t> g_watch_calls;
// The swap's own time (the ring skimmer's thread): waiting for the frame's submission, and the
// presenter's refresh of its guest output.
std::atomic<uint64_t> g_swap_flush_us, g_swap_refresh_us;
std::atomic<uint64_t> g_swap_refresh_cpu_us;

// The slowest frames since the last statistics line (the game thread's interval between two
// Presents) and where their time went, for finding stutters.
struct FrameRecord {
  uint32_t interval_us = 0, renderer_us = 0, present_us = 0, pipeline_wait_us = 0, texture_us = 0, buffer_us = 0,
           target_us = 0;
  uint32_t draws = 0;
};
std::mutex g_frames_mutex;
std::vector<FrameRecord> g_worst_frames;
// --native_stutter_stats: the counter after the last Present, for the overlay.
std::mutex g_stutter_mutex;
std::atomic<bool> g_stutter_on{false};
bool g_stutter_valid = false;
StutterSnapshot g_stutter;
std::chrono::steady_clock::time_point g_last_present;
uint64_t g_last_calls_us = 0, g_frame_draws = 0;
struct CallTimer {
  explicit CallTimer(int kind)
      : kind_(kind), enabled_(DiagnosticsEnabled()),
        start_(enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
  ~CallTimer() {
    if (!enabled_) return;
    g_call_us[kind_].fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - start_).count()),
                               std::memory_order_relaxed);
  }
  int kind_;
  bool enabled_;
  std::chrono::steady_clock::time_point start_;
};

// Records every renderer call under the backend's lock. Task 268: each call is guarded (gpu_loss.h): a Vulkan
// error thrown inside NVRHI on the calling thread (the game's when the render thread is off) goes to the
// device-loss path with the lock already released; nothing changes while nothing fails.
class LockedSink final : public DrawSink {
 public:
  LockedSink(DrawSink& inner, std::mutex& mutex, const GuestMemory& memory, ShaderLibrary* shaders)
      : inner_(inner), mutex_(mutex), memory_(memory), shaders_(shaders) {}
  // Shader creation is the shader library's (its own lock): it may come from
  // loader threads while the render thread draws.
  void OnShaderCreated(uint32_t kind, uint32_t container, uint32_t object) override {
#if NR_GAME_RENDERER
    if (shaders_) shaders_->OnCreated(kind, container, object);
#else
    (void)kind;
    (void)container;
    (void)object;
#endif
  }
  void OnVertexShaderPatched(uint32_t shader, uint32_t microcode, uint32_t declaration) override {
    RunGuarded("renderer (shader patch)", [&] {
      std::lock_guard lock(mutex_);
      inner_.OnVertexShaderPatched(shader, microcode, declaration);
    });
  }
  void OnClear(const ClearCall& c) override {
    if (Dump(c.frame)) {
      REXLOG_INFO("rexgpu-native: frame {} clear flags {:X} rgba {} {} {} {} z {} rt0 {:08X} ds {:08X}",
                  c.frame, c.flags, c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3], c.z,
                  c.render_target0, c.depth_stencil);
    }
    CallTimer timer(1);
    RunGuarded("renderer (clear)", [&] {
      std::lock_guard lock(mutex_);
      inner_.OnClear(c);
    });
  }
  void OnDraw(const DrawCall& d) override {
    if (Dump(d.frame)) {
      const auto& m = d.vs_c0_c3;
      REXLOG_INFO(
          "rexgpu-native: frame {} draw {} {} prim {} start {} count {} base {} stride {} pos {}{} "
          "vbytes {} vs {:08X} ps {:08X} rt0 {:08X} ds {:08X} vp {},{} {}x{} z {}..{} "
          "blend {:08X} {} postype {:X} wvp c{} vsobj {} psobj {} zstate {}/{}/{} cull {} c0 {} {} {} {} c1 {} {} {} {} c2 {} {} {} {} c3 {} {} {} {}",
          d.frame, d.index_in_frame, d.indexed ? "indexed" : "plain", int(d.primitive), d.start,
          d.count, d.base_vertex, d.stride, d.position_offset,
          d.position_from_declaration ? "" : " (no decl)", d.vertex_data_size, d.vertex_shader,
          d.pixel_shader, d.render_targets[0], d.depth_stencil, d.viewport.x, d.viewport.y,
          d.viewport.width, d.viewport.height, d.viewport.min_z, d.viewport.max_z,
          d.blend_control[0], FirstVertex(d), d.position_type, d.wvp_register, ShaderKind(d.vertex_shader), ShaderKind(d.pixel_shader), d.states.z_enable,
          d.states.z_write_enable, d.states.z_func, d.states.cull_mode, m[0], m[1],
          m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14],
          m[15]);
    }
    CallTimer timer(0);
    RunGuarded("renderer (draw)", [&] {
      std::lock_guard lock(mutex_);
      inner_.OnDraw(d);
    });
  }
  void OnResolve(const ResolveCall& r) override {
    if (Dump(r.frame)) {
      REXLOG_INFO("rexgpu-native: frame {} resolve flags {:X} to {:08X} rt0 {:08X}", r.frame,
                  r.flags, r.dest_texture, r.render_target0);
    }
    CallTimer timer(2);
    RunGuarded("renderer (resolve)", [&] {
      std::lock_guard lock(mutex_);
      inner_.OnResolve(r);
    });
  }
  void OnPresent(uint32_t frame, uint32_t rt0, uint32_t device) override {
    if (Dump(frame)) {
      REXLOG_INFO("rexgpu-native: frame {} present rt0 {:08X}", frame, rt0);
    }
    const auto start = DiagnosticsEnabled() ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    {
      CallTimer timer(3);
      RunGuarded("renderer (present)", [&] {
        std::lock_guard lock(mutex_);
        inner_.OnPresent(frame, rt0, device);
      });
    }
    RecordFrame(start);
  }

  // Keeps the eight slowest frames of the window (see g_worst_frames).
  void RecordFrame(std::chrono::steady_clock::time_point present_start) {
#if NR_GAME_RENDERER
    // The player overlay is independent of detailed timing.
    if (g_stutter_on.load(std::memory_order_relaxed)) {
      if (const GameRenderer* g = renderer_game_ ? renderer_game_() : nullptr) {
        const GameRenderer::StutterStats st = g->stutter();
        std::lock_guard lock(g_stutter_mutex);
        g_stutter = {st.frames,        st.stutter_frames,   st.frames_waited, st.frames_skipped,
                     st.draws_skipped, st.worst_wait_frame, st.wait_ms,       st.worst_wait_ms};
        g_stutter_valid = true;
      }
    }
#endif
    if (!DiagnosticsEnabled()) return;
    const auto now = std::chrono::steady_clock::now();
    uint64_t calls = 0;
    for (int i = 0; i < 4; ++i) calls += g_call_us[i].load(std::memory_order_relaxed);
    FrameRecord r;
    if (g_last_present.time_since_epoch().count() != 0) {
      r.interval_us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now - g_last_present).count());
    }
    r.renderer_us = uint32_t(calls - g_last_calls_us);
    r.present_us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now - present_start).count());
#if NR_GAME_RENDERER
    if (const GameRenderer* g = renderer_game_ ? renderer_game_() : nullptr) {
      const GameRenderer::Stats& s = g->stats();
      r.pipeline_wait_us = uint32_t((s.pipeline_wait_ms - last_wait_ms_) * 1000.0);
      r.texture_us = uint32_t((s.texture_ms - last_texture_ms_) * 1000.0);
      r.buffer_us = uint32_t((s.buffer_ms - last_buffer_ms_) * 1000.0);
      r.target_us = uint32_t((s.target_ms - last_target_ms_) * 1000.0);
      last_buffer_ms_ = s.buffer_ms;
      last_target_ms_ = s.target_ms;
      r.draws = uint32_t(s.draws - last_draws_);
      last_wait_ms_ = s.pipeline_wait_ms;
      last_texture_ms_ = s.texture_ms;
      last_draws_ = s.draws;
    }
#endif
    g_last_present = now;
    g_last_calls_us = calls;
    std::lock_guard lock(g_frames_mutex);
    g_worst_frames.push_back(r);
    std::sort(g_worst_frames.begin(), g_worst_frames.end(),
              [](const FrameRecord& a, const FrameRecord& b) { return a.interval_us > b.interval_us; });
    if (g_worst_frames.size() > 8) g_worst_frames.resize(8);
  }

 public:
  std::function<const GameRenderer*()> renderer_game_;

 private:
  double last_wait_ms_ = 0, last_texture_ms_ = 0, last_buffer_ms_ = 0, last_target_ms_ = 0;
  uint64_t last_draws_ = 0;

 private:
  // Which kind of shader object `object` is, from the container copy that
  // follows its header: a 52-byte header puts the container's flags word at
  // +52, a 592-byte one at +592 (d3d-structs.md). Settles which setter is
  // which (the shader stream found the create functions swapped).
  std::string ShaderKind(uint32_t object) const {
    if (!object) return "none";
    const uint8_t* p = memory_.Virtual(object);
    if (!p) return "unmapped";
    return fmt::format("[+52 {:08X} +592 {:08X}]", LoadBE32(p + 52), LoadBE32(p + 592));
  }

  // The draw's first vertex: its position and where the found matrix puts it
  // (x/w, y/w, z/w, w).
  static std::string FirstVertex(const DrawCall& d) {
    if (!d.vertex_data || d.stride == 0) return "v0 none";
    uint32_t first = d.indexed ? 0 : d.start;
    if (d.indexed && d.index_data) {
      first = d.index_info.index32 ? LoadBE32(d.index_data + size_t(d.start) * 4)
                                   : LoadBE16(d.index_data + size_t(d.start) * 2);
      first = uint32_t(int64_t(first) + d.base_vertex);
    }
    const uint64_t at = uint64_t(first) * d.stride + d.position_offset;
    if (at + 12 > d.vertex_data_size) return "v0 out of range";
    float p[4] = {LoadBEFloat(d.vertex_data + at), LoadBEFloat(d.vertex_data + at + 4),
                  LoadBEFloat(d.vertex_data + at + 8), 1.0f};
    float c[4];
    for (int r = 0; r < 4; ++r) {
      c[r] = d.wvp[r * 4] * p[0] + d.wvp[r * 4 + 1] * p[1] + d.wvp[r * 4 + 2] * p[2] + d.wvp[r * 4 + 3];
    }
    return fmt::format("v0 ({:.3g} {:.3g} {:.3g}) clip ({:.3g} {:.3g} {:.3g} w {:.3g})", p[0], p[1],
                       p[2], c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]);
  }

  // --native_dump_frame=N logs every call of the game's frame N.
  static bool Dump(uint32_t frame) {
    const int32_t wanted = REXCVAR_GET(native_dump_frame);
    return wanted >= 0 && uint32_t(wanted) == frame;
  }

  DrawSink& inner_;
  std::mutex& mutex_;
  const GuestMemory& memory_;
  ShaderLibrary* shaders_;
};

void LogToRuntime(LogLevel level, const char* text) {
  switch (level) {
    case LogLevel::kDebug:
      REXGPU_DEBUG("{}", text);
      break;
    case LogLevel::kInfo:
      // In the core category: the app keeps only warnings of the gpu one.
      REXLOG_INFO("{}", text);
      break;
    case LogLevel::kWarning:
      REXLOG_WARN("{}", text);
      break;
    default:
      REXLOG_ERROR("{}", text);
      break;
  }
}

#if NR_GAME_RENDERER
// CPU writes to watched guest memory (any thread, inside the SDK's fault
// handling): the textures and buffers there must be checked again.
std::pair<uint32_t, uint32_t> InvalidationThunk(void* context, uint32_t start, uint32_t length,
                                                bool /*exact_range*/) {
  auto* game = static_cast<GameRenderer*>(context);
  return game->OnGuestWrite(start, length);
}
#endif

class LogCallback final : public nvrhi::IMessageCallback {
 public:
  void message(nvrhi::MessageSeverity severity, const char* text) override {
    switch (severity) {
      case nvrhi::MessageSeverity::Info:
        REXGPU_DEBUG("NVRHI: {}", text);
        break;
      case nvrhi::MessageSeverity::Warning:
        REXGPU_WARN("NVRHI: {}", text);
        break;
      default:
        REXGPU_ERROR("NVRHI: {}", text);
        break;
    }
  }
};

}  // namespace

void InstallRuntimeLog() { SetLogSink(LogToRuntime); }

bool ReadStutterSnapshot(StutterSnapshot* out) {
  std::lock_guard lock(g_stutter_mutex);
  if (!g_stutter_on || !g_stutter_valid) return false;
  if (out) *out = g_stutter;
  return true;
}

// Command lists recorded on several threads at once (the frame's, the presenter's): not "immediate", a
// DX11 notion that NVRHI's validation layer enforces for every API (two open at once is an error there).
nvrhi::CommandListParameters NonImmediateList() { return nvrhi::CommandListParameters().setEnableImmediateExecution(false); }

nvrhi::IMessageCallback* GetMessageCallback() {
  static LogCallback callback;
  return &callback;
}

// Submits closed command lists in order on a thread of its own. At most three wait (the renderer
// keeps four frame lists in turn, so the one it opens next has been submitted).
class Backend::Submitter {
 public:
  explicit Submitter(HostDevice& host) : host_(host), thread_([this] { Run(); }) {}
  ~Submitter() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  void Enqueue(nvrhi::ICommandList* list, nvrhi::IEventQuery* completion) {
    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [&] { return pending_.size() + busy_ < 3; });
    const uint64_t ticket = ++queued_;
    query_tickets_[completion] = ticket;
    pending_.push_back({list, completion, ticket});
    cv_.notify_one();
  }
  // Before recycling one frame's image/query, only its own submission must
  // have assigned the query. Flushing newer frames serializes recording with
  // their presenter queue-lock waits even when this older frame is complete.
  void WaitSubmitted(nvrhi::IEventQuery* completion) {
    std::unique_lock lock(mutex_);
    auto found = query_tickets_.find(completion);
    if (found == query_tickets_.end()) return;
    const uint64_t wanted = found->second;
    done_cv_.wait(lock, [&] { return submitted_ >= wanted; });
  }
  // Waits until everything enqueued so far was submitted.
  void Flush() {
    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [&] { return pending_.empty() && !busy_; });
  }

 private:
  void Run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      cv_.wait(lock, [&] { return stop_ || !pending_.empty(); });
      if (pending_.empty()) return;  // stopping, and nothing left
      auto frame = std::move(pending_.front());
      pending_.pop_front();
      busy_ = 1;
      lock.unlock();
      const uint64_t ticket = frame.ticket;
      host_.ExecuteFrameCommandList(frame.list, frame.completion);
      frame = {};
      lock.lock();
      busy_ = 0;
      submitted_ = ticket;
      done_cv_.notify_all();
    }
  }
  HostDevice& host_;
  std::mutex mutex_;
  std::condition_variable cv_, done_cv_;
  struct Frame {
    nvrhi::CommandListHandle list;
    nvrhi::EventQueryHandle completion;
    uint64_t ticket = 0;
  };
  std::deque<Frame> pending_;
  std::unordered_map<nvrhi::IEventQuery*, uint64_t> query_tickets_;
  uint64_t queued_ = 0, submitted_ = 0;
  uint32_t busy_ = 0;
  bool stop_ = false;
  std::thread thread_;
};

// The swap's presenter work on a thread of its own: the newest request wins (a swap that comes
// while one is being presented replaces the waiting one).
class Backend::PresentThread {
 public:
  explicit PresentThread(Backend& backend) : backend_(backend), thread_([this] { Run(); }) {}
  ~PresentThread() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  void Request(rex::ui::Presenter* presenter, uint32_t fw, uint32_t fh, uint32_t dw, uint32_t dh) {
    {
      std::lock_guard lock(mutex_);
      request_ = {presenter, fw, fh, dw, dh};
      pending_ = true;
    }
    cv_.notify_one();
  }

 private:
  struct Args {
    rex::ui::Presenter* presenter = nullptr;
    uint32_t fw = 0, fh = 0, dw = 0, dh = 0;
  };
  void Run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      cv_.wait(lock, [&] { return stop_ || pending_; });
      if (stop_) return;
      const Args a = request_;
      pending_ = false;
      lock.unlock();
      RunGuarded("present thread", [&] { backend_.PresentNow(a.presenter, a.fw, a.fh, a.dw, a.dh); });
      lock.lock();
    }
  }
  Backend& backend_;
  std::mutex mutex_;
  std::condition_variable cv_;
  Args request_;
  bool pending_ = false, stop_ = false;
  std::thread thread_;
};

Backend::Backend(std::unique_ptr<HostDevice> host, rex::memory::Memory* memory)
    : host_(std::move(host)), guest_memory_(memory), memory_(memory) {}


bool Backend::Initialize(uint32_t width, uint32_t height, const GameSettings& game) {
  SetDiagnosticsEnabled(REXCVAR_GET(native_diagnostics));
  SetLogSink(LogToRuntime);
  RenderScale scale;
  float shadow_scale = 0.0f;
  bool shaders_aware = false;
#if NR_GAME_RENDERER
  if (game.enabled) {
    const RenderScaleSetup setup = ResolveRenderScale(game.render_scale, game.shadow_scale, game.debug, width, height);
    scale = setup.scale;
    shadow_scale = setup.shadow_scale;
    shaders_aware = setup.shaders_aware;
    if (!scale.valid) {
      REXGPU_ERROR("rexgpu-native: render scale '{}' not understood (a number, or WxH): using 1", game.render_scale);
    }
    if (!scale.unit() || shadow_scale > 0.0f) {
      REXLOG_INFO("rexgpu-native: render scale {:.4g} x {:.4g}, shadow maps {:.4g} (frame image {}x{})", scale.x, scale.y,
                  shadow_scale > 0.0f ? shadow_scale : scale.x, ScaledSize(width, scale.x), ScaledSize(height, scale.y));
    }
  }
#endif
  // The frame image is the scaled size: it is what the presenter gets (its upscalers and FXAA
  // work from it).
  renderer_ = std::make_unique<Renderer>(host_->device());
  renderer_->options().frame_wait_at_open = game.frame_wait_at_open;
  REXLOG_INFO("rexgpu-native: two-frame GPU bound waits {}",
              game.frame_wait_at_open ? "before a frame is recorded (native_frame_wait=open)"
                                      : "before a frame is submitted (native_frame_wait=submit)");
  if (!renderer_->Initialize(ScaledSize(width, scale.x), ScaledSize(height, scale.y))) {
    REXGPU_ERROR("rexgpu-native: renderer initialisation failed ({}x{})", width, height);
    return false;
  }
#if NR_GAME_RENDERER
  if (game.enabled) {
    shaders_ = std::make_unique<ShaderLibrary>(host_->device(), guest_memory_);
    // --native_debug bit 16: the plain pixel shaders at a scale too (to tell the aware translation's part).
    // An upscaler other than native AA draws the scene at another size than the output: aware shaders.
    // With live settings (the graphics panel) an upscaler, its mode or the shadows can change while the
    // game runs: the aware translations from the start (the pack holds them; at a scale of 1 they draw
    // the same picture), and the water's reflection shaders are made when the option is turned on.
    const bool reduced_tables = host_->table_capacities() != kNativeTableCapacities;
    const bool upscale_enabled = game.upscale.enabled() && !reduced_tables;
    const bool upscale_scaled = upscale_enabled && game.upscale.quality != UpscaleQuality::kNative && !(game.debug & 16);
    const bool live = bool(game.effect_settings);
    shaders_->SetRenderScaleAware(shaders_aware || upscale_scaled || (live && !(game.debug & 16)));
    shaders_->SetWaterReflections(!live && game.lighting.water_on());
    // The upscalers' waits for the GPU take the host's queue lock (upscaler.h).
    nvrhi::IDevice* device = host_->device();
    HostDevice* host = host_.get();
    SetUpscalerIdleWait(device, [host] { host->WaitForIdle(); });
    SetUpscalerExclusive(device, [host](const std::function<void()>& fn) { host->RunExclusive(fn); });
    shaders_->Initialize(game.shader_pack, game.dxc);
    kknr::GuestMemory physical;
    physical.base = memory_->TranslatePhysical(0);
    physical.size = 0x20000000;
    physical.origin = 0;
    auto renderer = std::make_unique<GameRenderer>(host_->device(), snapshot_memory_, physical, shaders_.get());
    renderer->options().upload_submit = [this](nvrhi::ICommandList* cl) { host_->ExecuteCommandList(cl); };
    renderer->options().tessellation_supported = host_->tessellation_supported();
    renderer->options().table_capacities = host_->table_capacities();
    const auto descriptor_completion = host_->device()->createEventQuery();
    if (!descriptor_completion) {
      REXGPU_ERROR("rexgpu-native: could not create the descriptor recycling completion query");
      return false;
    }
    renderer->options().drain_descriptors = [this, descriptor_completion](nvrhi::ICommandList* cl) {
      cl->close();
      // All earlier game draws precede this ticket on the graphics queue.
      // Their descriptor references are complete once the ticket completes.
      host_->ExecuteFrameCommandList(cl, descriptor_completion);
      host_->WaitFrame(descriptor_completion);
      cl->open();
    };
    renderer->options().memory_budget = [this] { return host_->QueryMemoryBudget(); };
    renderer->options().render_scale_x = scale.x;
    renderer->options().render_scale_y = scale.y;
    renderer->options().shadow_scale = shadow_scale;
    renderer->options().guest_frame_width = width;
    renderer->options().guest_frame_height = height;
    renderer->options().ao = game.ao;  // before Initialize: the ambient-only AO adds a binding
    renderer->options().gi = game.gi;  // before Initialize (GiInitialize)
    renderer->options().cel = game.cel;  // before Initialize (CelInitialize)
    if (upscale_enabled && (game.ao.method == AoMethod::kLegacy || game.ao.apply == AoApply::kResolve)) {
      // The legacy AO (and GTAO at the resolve moment) darkens the scene copy the post effects read, after
      // the upscaler took its input: with an upscaler the occlusion goes into the scene at the end of its
      // opaque pass instead (docs/upscalers.md, "Ambient occlusion").
      renderer->options().ao.method = AoMethod::kGtao;
      renderer->options().ao.apply = AoApply::kOpaque;
      REXLOG_INFO("rexgpu-native: upscaler on: ambient occlusion by GTAO at the end of the opaque pass");
    }
    // Temporal upscalers (docs/upscalers.md): before Initialize (the input size changes the layouts).
    renderer->options().upscale = game.upscale;
    if (reduced_tables && game.upscale.enabled()) {
      // Motion replay saves descriptor indices until the upscale point.
      // Reusing those indices after a mid-frame drain would corrupt replay.
      // Use the existing native render size until replay can rebind resources.
      renderer->options().upscale.kind = UpscalerKind::kOff;
      REXGPU_WARN("rexgpu-native: reduced Vulkan descriptor tables: temporal upscaling disabled because deferred motion replay requires stable descriptor indices; using native render size");
    }
    renderer->options().fxaa = game.fxaa;
    renderer->options().lighting = game.lighting;  // before Initialize too (docs/lighting.md)
    renderer->options().effect_settings = game.effect_settings;
    if (renderer->Initialize()) {
      shaders_->SetAmbientOcclusion(renderer->ao_ambient_table());
      if (!game.effect_settings) shaders_->SetReplacements(renderer->LightingReplacements());
      renderer->options().element_endian = game.element_endian;
      renderer->options().flip_front_face = game.flip_front_face;
      renderer->options().texture_tail_mips = game.texture_tail_mips;
      renderer->options().dump_frame = game.dump_frame;
      renderer->options().show_texture = game.show_texture;
      renderer->options().debug = game.debug;
      renderer->options().async_pipelines = game.async_pipelines;
      renderer->options().pipeline_wait = game.pipeline_wait;
      renderer->options().dynamic_vertex_buffers = game.dynamic_vertex_buffers;
      renderer->options().anisotropic_override = game.anisotropic_override;
      renderer->options().fxaa = game.fxaa;
      renderer->options().effect_buffers = game.effect_buffers;
      renderer->options().sharpness = game.sharpness;
      renderer->options().pipeline_wait_budget_ms = game.pipeline_wait_budget_ms;
      renderer->options().pipeline_must_wait_ms = game.pipeline_must_wait_ms;
      if (game.pipeline_must_wait_ms < 0) renderer->options().pipeline_loading_wait_ms = -1.0;
      renderer->options().chapter_preload_mib = game.chapter_preload_mib;
      renderer->options().chapter_trickle_mib = game.chapter_trickle_mib;
      REXLOG_INFO("rexgpu-native: pipelines not ready: {}", !game.async_pipelines ? "made on the draw thread"
                  : game.pipeline_wait ? "every draw waits (native_pipeline_wait / KK_DEV_PIPELINE_BLOCK=1)"
                                       : "scene draws skipped until ready, dependent draws wait (task 179)");
      renderer->SetFrameLog(FrameLogConfig::Parse(game.frame_log.c_str()));
      renderer->options().ao_settings = game.ao_settings;
      rex::memory::Memory* memory = memory_;
      // --native_debug bit 256: no write watches at all (debugging: a CPU write never reaches a
      // texture or buffer already uploaded).
      if (!(game.debug & 256)) {
        renderer->set_watch([memory](uint32_t physical_address, uint32_t bytes) {
          CallTimer timer(4);
          if (DiagnosticsEnabled()) g_watch_calls.fetch_add(1, std::memory_order_relaxed);
          memory->EnablePhysicalMemoryAccessCallbacks(physical_address, bytes, true, false);
        });
      }
      invalidation_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, renderer.get());
      renderer_->EnableGame(std::move(renderer));
      stutter_stats_ = game.stutter_stats;
      {
        std::lock_guard lock(g_stutter_mutex);
        g_stutter_on = game.stutter_stats;
        g_stutter_valid = false;
      }
      REXLOG_INFO("rexgpu-native: game renderer ready");
    } else {
      REXGPU_ERROR("rexgpu-native: game renderer initialisation failed. Choose the other native API in the launcher and update your graphics driver.");
      shaders_.reset();
      return false;
    }
  }
#else
  (void)game;
  (void)shaders_aware;
#endif
  renderer_->set_submit([this](nvrhi::ICommandList* cl) {
    if (submitter_) {
      submitter_->Enqueue(cl, renderer_->completion_query());
    } else {
      host_->ExecuteFrameCommandList(cl, renderer_->completion_query());
    }
    ++submitted_frames_;
  });
  renderer_->set_frame_wait([this](nvrhi::IEventQuery* completion) {
    if (submitter_) submitter_->WaitSubmitted(completion);
    host_->WaitFrame(completion);
  });
  {
    auto sink = std::make_unique<LockedSink>(*renderer_, mutex_, guest_memory_, shaders_.get());
#if NR_GAME_RENDERER
    sink->renderer_game_ = [this]() -> const GameRenderer* { return renderer_ ? renderer_->game() : nullptr; };
#endif
    sink_ = std::move(sink);
  }
  DrawSink* sink = sink_.get();
  if (game.render_thread) {
    threaded_ = std::make_unique<ThreadedSink>(*sink_, guest_memory_);
    threaded_->max_queued_presents = std::max<uint32_t>(1, game.render_thread_presents);
#if NR_GAME_RENDERER
    // The library patches a shader's microcode again for the next declaration: hashed at once.
    if (GameRenderer* g = renderer_->game()) {
      threaded_->on_patched = [g](uint32_t shader, uint32_t microcode, uint32_t declaration) {
        g->VertexShaderPatched(shader, microcode, declaration);
      };
    }
#endif
    // Long game frames in the log, with where the time went (the game's clock takes at most 63 ms a
    // frame, so a long frame moves the game against real time).
    threaded_->on_long_frame = [](const ThreadedSink::LongFrame& f) {
      REXLOG_INFO(
          "rexgpu-native: long game frame {:.1f} ms: {} calls ({:.1f} ms copying, {:.1f} ms waiting for room, {} "
          "waiting at its end); ring skimmer waited {} times, {:.1f} ms; room: commands {:.1f} ms, Present {:.1f} ms",
          f.frame_us / 1000.0, f.calls, f.copy_us / 1000.0, f.room_us / 1000.0, f.backlog, f.fence_waits,
          f.fence_wait_us / 1000.0, f.command_room_us / 1000.0, f.present_room_us / 1000.0);
    };
    sink = threaded_.get();
    REXLOG_INFO("rexgpu-native: render thread on");
  }
  draws_ = std::make_unique<DrawTracker>(guest_memory_, sink);
  draws_->set_find_wvp(renderer_->game() == nullptr);
  REXGPU_INFO("rexgpu-native: NVRHI renderer ready ({}x{})", width, height);
  return true;
}

bool Backend::SetPipelineCache(const std::string& path) {
#if NR_GAME_RENDERER
  if (renderer_ && renderer_->game()) return renderer_->game()->SetPipelineCache(path);
#else
  (void)path;
#endif
  return false;
}

bool Backend::SetDriverPipelineCache(const std::string& path) {
#if NR_GAME_RENDERER
  if (renderer_ && renderer_->game() && host_ && host_->pipeline_cache()) {
    renderer_->game()->SetDriverPipelineCache(host_->pipeline_cache(), path);
    return true;
  }
#else
  (void)path;
#endif
  return false;
}

bool Backend::LoadPipelineList(const std::string& path) {
#if NR_GAME_RENDERER
  if (renderer_ && renderer_->game()) return renderer_->game()->LoadPipelineList(path);
#else
  (void)path;
#endif
  return false;
}

bool Backend::LoadResourceList(const std::string& path) {
#if NR_GAME_RENDERER
  if (renderer_ && renderer_->game()) return renderer_->game()->LoadResourceList(path);
#else
  (void)path;
#endif
  return false;
}

bool Backend::OpenShaderPack(const std::string& path) {
#if NR_GAME_RENDERER
  if (shaders_ && !shaders_->pack_open()) return shaders_->OpenPack(path);
#else
  (void)path;
#endif
  return false;
}

bool Backend::has_shader_pack() const {
#if NR_GAME_RENDERER
  return shaders_ && shaders_->pack_open();
#else
  return false;
#endif
}

void Backend::set_async_present(bool on) {
  if (on && !present_thread_) present_thread_ = std::make_unique<PresentThread>(*this);
}

void Backend::Present(rex::ui::Presenter* presenter, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
                      uint32_t display_width, uint32_t display_height) {
  if (present_thread_) {
    present_thread_->Request(presenter, frontbuffer_width, frontbuffer_height, display_width, display_height);
    return;
  }
  RunGuarded("present", [&] { PresentNow(presenter, frontbuffer_width, frontbuffer_height, display_width, display_height); });
}

void Backend::set_async_submit(bool on) {
  if (on && !submitter_ && host_) submitter_ = std::make_unique<Submitter>(*host_);
}

Backend::~Backend() { Shutdown(); }

void Backend::LogStutter(const char* when) {
#if NR_GAME_RENDERER
  if (!stutter_stats_ || !renderer_ || !renderer_->game()) return;
  const GameRenderer::StutterStats s = renderer_->game()->stutter();
  REXLOG_INFO(
      "rexgpu-native: pipeline stutter {}: {} of {} frames ({} waited for pipelines, {:.0f} ms in all; {} skipped "
      "{} draws whose pipelines were not ready); worst wait {:.1f} ms (frame {})",
      when, s.stutter_frames, s.frames, s.frames_waited, s.wait_ms, s.frames_skipped, s.draws_skipped,
      s.worst_wait_ms, s.worst_wait_frame);
#else
  (void)when;
#endif
}

void Backend::Shutdown() {
  threaded_.reset();  // runs what is queued
  LogStutter("at the end");
  {
    std::lock_guard lock(g_stutter_mutex);
    g_stutter_on = g_stutter_valid = false;
  }
  present_thread_.reset();
  submitter_.reset();  // submits what waits
#if NR_GAME_RENDERER
  if (host_) SetUpscalerIdleWait(host_->device(), nullptr);
  if (host_) SetUpscalerExclusive(host_->device(), nullptr);
#endif
  if (invalidation_handle_) {
    memory_->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle_);
    invalidation_handle_ = nullptr;
  }
  if (host_) host_->WaitForIdle();
  draws_.reset();
  sink_.reset();
  renderer_.reset();
  shaders_.reset();
  if (host_) host_->WaitForIdle();
  host_.reset();
}

void Backend::LogFrameTiming() {
  if (!renderer_) return;
  // Each value is per rendered frame of the window (sums of the window divided by its frames).
  const auto now = std::chrono::steady_clock::now();
  const Renderer::Stats& r = renderer_->stats();
  TimingMark m;
  m.at = now;
  m.frames = r.frames_submitted;
  m.render_cpu_us = r.render_cpu_us;
  m.reuse_wait_us = r.gpu_reuse_wait_us;
  m.submit_us = r.present_submit_us;
  m.refresh_us = g_swap_refresh_us.load();
  if (threaded_) {
    const ThreadedSink::Stats t = threaded_->stats();
    m.game_cpu_us = t.game_cpu_us;
    m.game_room_us = t.backpressure_us;
    m.game_presents = t.presents;
    m.render_idle_us = t.render_idle_us;
  }
#if NR_GAME_RENDERER
  GameRenderer* g = renderer_->game();
  if (g) m.pipeline_wait_ms = g->stats().pipeline_wait_ms;
#endif
  const TimingMark p = timing_mark_;
  timing_mark_ = m;
  std::vector<uint32_t> intervals = renderer_->TakeFrameIntervals();
  const AoGpuTimer::Window frame_gpu = renderer_->TakeFrameGpuWindow();
#if NR_GAME_RENDERER
  const GameRenderer::EffectGpuWindow passes = g ? g->TakeEffectGpuWindow() : GameRenderer::EffectGpuWindow{};
#endif
  const uint64_t frames = m.frames - p.frames;
  if (p.at.time_since_epoch().count() == 0 || frames == 0) return;  // the first mark starts the window
  const double seconds = std::chrono::duration<double>(now - p.at).count();
  std::sort(intervals.begin(), intervals.end());
  auto pct = [&](double q) {
    return intervals.empty() ? 0.0 : intervals[std::min(intervals.size() - 1, size_t(q * double(intervals.size())))] / 1000.0;
  };
  const double n = double(frames);
  auto per = [&](uint64_t now_us, uint64_t then_us) { return double(now_us - then_us) / 1000.0 / n; };
  const uint64_t game_frames = m.game_presents - p.game_presents;
  auto gpu = [](const AoGpuTimer::Window& w) {
    return w.frames ? fmt::format("{:.2f}", w.sum_ms / double(w.frames)) : std::string("-");
  };
  std::string groups;
#if NR_GAME_RENDERER
  groups = fmt::format("; GPU passes ms: AO {}, GI {}, upscaler {}, shadows {}, water {}, volumetrics {}, cel {}", gpu(passes.ao),
                       gpu(passes.gi), gpu(passes.upscaler), gpu(passes.shadows), gpu(passes.water),
                       gpu(passes.volumetrics), gpu(passes.cel));
#endif
  REXLOG_INFO(
      "rexgpu-native: frame timing ({} frames, {:.1f} s, {:.1f} FPS): frame p50 {:.2f} p95 {:.2f} p99 {:.2f} max {:.2f} "
      "ms; game thread CPU {} ms, waits for the render thread {} ms; render thread CPU {:.2f} ms, idle {} ms, GPU "
      "frame wait {:.2f} ms, pipeline waits {:.2f} ms, submit {:.2f} ms; presenter refresh {:.2f} ms; GPU frame {} ms "
      "(max {:.2f}){}",
      frames, seconds, n / seconds, pct(0.5), pct(0.95), pct(0.99), pct(1.0),
      threaded_ && game_frames ? fmt::format("{:.2f}", double(m.game_cpu_us - p.game_cpu_us) / 1000.0 / double(game_frames))
                               : std::string("-"),
      threaded_ && game_frames ? fmt::format("{:.2f}", double(m.game_room_us - p.game_room_us) / 1000.0 / double(game_frames))
                               : std::string("-"),
      per(m.render_cpu_us, p.render_cpu_us),
      threaded_ ? fmt::format("{:.2f}", per(m.render_idle_us, p.render_idle_us)) : std::string("-"),
      per(m.reuse_wait_us, p.reuse_wait_us), (m.pipeline_wait_ms - p.pipeline_wait_ms) / n, per(m.submit_us, p.submit_us),
      per(m.refresh_us, p.refresh_us), gpu(frame_gpu), frame_gpu.max_ms, groups);
#if NR_GAME_RENDERER
  if (g) {
    // Headroom of the per-draw volatile constant buffers (task 154).
    const GameRenderer::VolatileWindow w = g->TakeVolatileWindow();
    const GameRenderer::Stats& s = g->stats();
    REXLOG_INFO("rexgpu-native: draws per list: peak {} (motion {}) in this window, {} (motion {}) so far; budget {} of "
                "{} versions; {} lists completed early, {} draws skipped",
                w.draws, w.motion, s.volatile_peak_draws, s.volatile_peak_motion, g->volatile_budget(),
                g->options().volatile_versions, s.volatile_completions, s.volatile_skipped);
  }
#endif
}

void Backend::PresentNow(rex::ui::Presenter* presenter, uint32_t frontbuffer_width,
                         uint32_t frontbuffer_height, uint32_t display_width,
                         uint32_t display_height) {
  // The presenter's refresh can wait (for its previous paint, the swap chain): holding the
  // backend's lock through it blocked every draw of the game's thread meanwhile (6.5 ms a frame
  // in the renderer's presents and 2.3 ms in its clears at 60 frames a second). A game frame's
  // image is therefore taken under the lock and copied after it is released; the test pictures
  // before the first game frame still draw under it.
  std::unique_lock lock(mutex_);
  if (!renderer_) return;
  // Once the game has submitted a frame, keep showing the last one: the ring
  // skimmer's swaps and the hooked Presents run on different threads, so a
  // swap can come before its frame's Present, and showing the test picture
  // then made it flicker into the game's frames.
  const bool game_frame = submitted_frames_ != 0 && renderer_->presented_image();
  shown_frames_ = submitted_frames_;
  const uint32_t width = renderer_->width(), height = renderer_->height();
  const uint32_t swap = swaps_++;
  if (swap % 600 == 0) LogFrameTiming();
  if (swap % 300 == 0 && draws_ && DiagnosticsEnabled()) {
    // In the core category: the app keeps only warnings of the gpu one.
    const DrawTracker::Stats& d = draws_->stats();
    const Renderer::Stats& r = renderer_->stats();
    REXLOG_INFO("rexgpu-native: GPU frame reuse wait {} ms over {} frames", r.gpu_reuse_wait_us / 1000,
                r.frames_submitted);
    REXLOG_INFO("rexgpu-native: host CPU over {} frames: {} us submission, {} us presenter refresh",
                r.frames_submitted, host_->submit_times().cpu_us, g_swap_refresh_cpu_us.load());
    REXLOG_INFO(
        "rexgpu-native: swap {}: hooks saw {} draws ({} indexed, {} dropped), {} clears, {} "
        "resolves, {} presents; renderer drew {}, skipped {} (other target) {} (primitive) {} "
        "(range) {} (not main pass) {} (no projection matrix) {} (position format), {} clears, "
        "{} frames; presents: {} ms copy, {} ms close, {} ms submit, {} ms frame end; submissions: {} ({} ms "
        "waiting for the queue, {} ms submitting, {} ms collecting)",
        swap, d.draws, d.indexed_draws, d.dropped_draws, d.clears, d.resolves, d.presents,
        r.draws_recorded, r.draws_skipped_target, r.draws_skipped_primitive, r.draws_skipped_range,
        r.draws_skipped_overlay, r.draws_skipped_no_wvp, r.draws_skipped_format, r.clears_recorded, r.frames_submitted,
        r.present_copy_us / 1000, r.present_close_us / 1000, r.present_submit_us / 1000, r.present_end_us / 1000,
        host_->submit_times().count, host_->submit_times().wait_us / 1000, host_->submit_times().execute_us / 1000,
        host_->submit_times().garbage_us / 1000);
#if NR_GAME_RENDERER
    if (GameRenderer* g = renderer_->game()) {
      const GameRenderer::Stats& s = g->stats();
      const ShaderLibrary::Stats sh = shaders_->stats();
      REXLOG_INFO(
          "rexgpu-native: game renderer: {} draws, skipped {} (shader) {} (target) {} (primitive) {} "
          "(pipeline) {} (pipeline not ready) {} (device); {} pipelines ({} drawn after a wait, {:.0f} ms waited); {} texture uploads ({} failed), {} buffer uploads, draw constants {} uploads ({} reused), "
          "{} clears, {} resolves ({} failed), {} invalidations; shaders {} created, {} from the pack, "
          "{} compiled, {} failed, {} from objects; {:.0f} ms in pipeline creation, {:.0f} ms in texture uploads, {} aliased ({} failed); "
          "game thread in the renderer: {} ms draws, {} ms clears, {} ms resolves, {} ms presents ({} ms in {} watch calls); "
          "{} dynamic vertex buffers, {} MB of their vertices; hooks: {} calls, {} ms; swaps: {} ms waiting for "
          "the frame's submission, {} ms in the presenter's refresh",
          s.draws, s.skipped_shader, s.skipped_target, s.skipped_primitive, s.skipped_pipeline,
          s.skipped_pending, s.skipped_device, s.pipelines, s.pipeline_budget_waits + s.pipeline_waits,
          s.pipeline_wait_ms, s.texture_uploads, s.texture_failures, s.buffer_uploads,
          s.draw_constant_uploads, s.draw_constant_identical,
          s.clears, s.resolves, s.resolve_failures, s.invalidations, sh.created, sh.from_pack,
          sh.compiled, sh.failed, sh.from_object, s.pipeline_ms, s.texture_ms, s.aliases, s.alias_failures,
          g_call_us[0].load() / 1000, g_call_us[1].load() / 1000, g_call_us[2].load() / 1000,
          g_call_us[3].load() / 1000, g_call_us[4].load() / 1000, g_watch_calls.load(), s.dynamic_vertex_buffers,
          s.dynamic_vertex_bytes >> 20, ApiCallCount(), ApiCallMicroseconds() / 1000, g_swap_flush_us.load() / 1000,
          g_swap_refresh_us.load() / 1000);
    }
#endif
    if (threaded_) {
      const ThreadedSink::Stats t = threaded_->stats();
      REXLOG_INFO(
          "rexgpu-native: render thread: {} calls ({} device copies), at most {} waiting; game thread {} ms "
          "copying, {} ms waiting for room; ring skimmer waited {} times, {} ms",
          t.commands, t.device_copies, t.max_backlog, t.enqueue_us / 1000, t.backpressure_us / 1000, t.fence_waits,
          t.fence_wait_us / 1000);
      REXLOG_INFO("rexgpu-native: queue room: commands {} us, Present {} us, peak {} bytes",
                  t.command_room_us, t.present_room_us, t.max_queued_bytes);
#if NR_GAME_RENDERER
      if (const GameRenderer* g = renderer_->game()) {
        REXLOG_INFO("rexgpu-native: early uploads {} bytes in {} batches",
                    g->stats().early_upload_bytes, g->stats().early_upload_batches);
      }
#endif
    }
    {
      std::lock_guard frames_lock(g_frames_mutex);
      std::string text;
      for (const FrameRecord& f : g_worst_frames) {
        text += fmt::format(
            " [{:.1f} ms: renderer {:.1f}, present {:.1f}, pipeline waits {:.1f}, textures {:.1f}, buffers {:.1f}, "
            "targets {:.1f}, {} draws]",
            f.interval_us / 1000.0, f.renderer_us / 1000.0, f.present_us / 1000.0, f.pipeline_wait_us / 1000.0,
            f.texture_us / 1000.0, f.buffer_us / 1000.0, f.target_us / 1000.0, f.draws);
      }
      g_worst_frames.clear();
      REXLOG_INFO("rexgpu-native: slowest frames:{}", text);
    }
  }
  if (swap % 300 == 0) LogStutter("so far");
  (void)frontbuffer_width;
  (void)frontbuffer_height;
  if (game_frame && unlocked_present_) {
    // The image stays alive by its handle; a newer frame drawn into it meanwhile only shows that
    // frame (the queue orders the copy after or before the frame's own submission).
    nvrhi::TextureHandle image = renderer_->presented_image();
    lock.unlock();
    // The frame's own commands first (the copy reads what they drew).
    const auto t0 = std::chrono::steady_clock::now();
    if (submitter_) submitter_->Flush();
    const auto t1 = std::chrono::steady_clock::now();
    struct RefreshTimer {
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      uint64_t cpu_start = (REXCVAR_GET(native_debug) & 32768) ? CurrentThreadCpuMicroseconds() : 0;
      ~RefreshTimer() {
        if (cpu_start) g_swap_refresh_cpu_us += CurrentThreadCpuMicroseconds() - cpu_start;
        g_swap_refresh_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start).count());
      }
    } refresh_timer;
    g_swap_flush_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    presenter->RefreshGuestOutput(
        width, height, display_width, display_height,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          nvrhi::CommandListHandle cl = host_->device()->createCommandList(NonImmediateList());
          cl->open();
          const bool ok = host_->CopyToGuestOutput(context, cl, image, width, height);
          cl->close();
          host_->ExecuteCommandList(cl);
          return ok;
        });
    presented_game_frames_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (submitter_) submitter_->Flush();
  presenter->RefreshGuestOutput(
      width, height, display_width, display_height,
      [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        nvrhi::CommandListHandle cl = host_->device()->createCommandList(NonImmediateList());
        cl->open();
        nvrhi::ITexture* source = renderer_->presented_image();
        if (!game_frame) {
          // Test picture into the image the game is not using.
          source = renderer_->recording_image();
          renderer_->RecordClear(cl, source, Renderer::TestClearColor(swap));
          if (test_picture_ == TestPicture::kTriangle) {
            renderer_->RecordTestTriangle(cl, source);
          }
        }
        bool ok = host_->CopyToGuestOutput(context, cl, source, width, height);
        cl->close();
        host_->ExecuteCommandList(cl);
        return ok;
      });
  if (game_frame) presented_game_frames_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace nr
