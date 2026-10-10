#include "backend/backend.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "backend/log.h"
#if NR_GAME_RENDERER
#include "backend/game_renderer.h"
#include "backend/shader_library.h"
#endif

REXCVAR_DECLARE(int32_t, native_dump_frame);

namespace nr {

const uint8_t* RuntimeGuestMemory::Virtual(uint32_t address) const {
  if (!address || !memory_->LookupHeap(address)) return nullptr;
  return memory_->TranslateVirtual(address);
}

const uint8_t* RuntimeGuestMemory::Physical(uint32_t address) const {
  return memory_->TranslatePhysical(address);
}

namespace {

// Records every renderer call under the backend's lock.
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
  void OnClear(const ClearCall& c) override {
    if (Dump(c.frame)) {
      REXLOG_INFO("rexgpu-native: frame {} clear flags {:X} rgba {} {} {} {} z {} rt0 {:08X} ds {:08X}",
                  c.frame, c.flags, c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3], c.z,
                  c.render_target0, c.depth_stencil);
    }
    std::lock_guard lock(mutex_);
    inner_.OnClear(c);
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
    std::lock_guard lock(mutex_);
    inner_.OnDraw(d);
  }
  void OnResolve(const ResolveCall& r) override {
    if (Dump(r.frame)) {
      REXLOG_INFO("rexgpu-native: frame {} resolve flags {:X} to {:08X} rt0 {:08X}", r.frame,
                  r.flags, r.dest_texture, r.render_target0);
    }
    std::lock_guard lock(mutex_);
    inner_.OnResolve(r);
  }
  void OnPresent(uint32_t frame, uint32_t rt0, uint32_t device) override {
    if (Dump(frame)) {
      REXLOG_INFO("rexgpu-native: frame {} present rt0 {:08X}", frame, rt0);
    }
    std::lock_guard lock(mutex_);
    inner_.OnPresent(frame, rt0, device);
  }

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
  game->InvalidateRange(start, length);
  return {start, length};
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

nvrhi::IMessageCallback* GetMessageCallback() {
  static LogCallback callback;
  return &callback;
}

Backend::Backend(std::unique_ptr<HostDevice> host, rex::memory::Memory* memory)
    : host_(std::move(host)), guest_memory_(memory), memory_(memory) {}

Backend::~Backend() { Shutdown(); }

bool Backend::Initialize(uint32_t width, uint32_t height, const GameSettings& game) {
  SetLogSink(LogToRuntime);
  renderer_ = std::make_unique<Renderer>(host_->device());
  if (!renderer_->Initialize(width, height)) {
    REXGPU_ERROR("rexgpu-native: renderer initialisation failed ({}x{})", width, height);
    return false;
  }
#if NR_GAME_RENDERER
  if (game.enabled) {
    shaders_ = std::make_unique<ShaderLibrary>(host_->device(), guest_memory_);
    shaders_->Initialize(game.shader_pack, game.dxc);
    kknr::GuestMemory physical;
    physical.base = memory_->TranslatePhysical(0);
    physical.size = 0x20000000;
    physical.origin = 0;
    auto renderer = std::make_unique<GameRenderer>(host_->device(), guest_memory_, physical, shaders_.get());
    if (renderer->Initialize()) {
      renderer->options().element_endian = game.element_endian;
      renderer->options().flip_front_face = game.flip_front_face;
      renderer->options().texture_tail_mips = game.texture_tail_mips;
      renderer->options().dump_frame = game.dump_frame;
      renderer->options().debug = game.debug;
      rex::memory::Memory* memory = memory_;
      renderer->set_watch([memory](uint32_t physical_address, uint32_t bytes) {
        memory->EnablePhysicalMemoryAccessCallbacks(physical_address, bytes, true, false);
      });
      invalidation_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, renderer.get());
      renderer_->EnableGame(std::move(renderer));
      REXLOG_INFO("rexgpu-native: game renderer ready");
    } else {
      REXGPU_ERROR("rexgpu-native: game renderer initialisation failed; placeholder pipeline instead");
      shaders_.reset();
    }
  }
#else
  (void)game;
#endif
  renderer_->set_submit([this](nvrhi::ICommandList* cl) {
    host_->ExecuteCommandList(cl);
    ++submitted_frames_;
  });
  sink_ = std::make_unique<LockedSink>(*renderer_, mutex_, guest_memory_, shaders_.get());
  draws_ = std::make_unique<DrawTracker>(guest_memory_, sink_.get());
  draws_->set_find_wvp(renderer_->game() == nullptr);
  REXGPU_INFO("rexgpu-native: NVRHI renderer ready ({}x{})", width, height);
  return true;
}

void Backend::Shutdown() {
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

void Backend::Present(rex::ui::Presenter* presenter, uint32_t frontbuffer_width,
                      uint32_t frontbuffer_height, uint32_t display_width,
                      uint32_t display_height) {
  std::lock_guard lock(mutex_);
  if (!renderer_) return;
  // Once the game has submitted a frame, keep showing the last one: the ring
  // skimmer's swaps and the hooked Presents run on different threads, so a
  // swap can come before its frame's Present, and showing the test picture
  // then made it flicker into the game's frames.
  const bool game_frame = submitted_frames_ != 0 && renderer_->presented_image();
  shown_frames_ = submitted_frames_;
  const uint32_t width = renderer_->width(), height = renderer_->height();
  const uint32_t swap = swaps_++;
  if (swap % 300 == 0 && draws_) {
    // In the core category: the app keeps only warnings of the gpu one.
    const DrawTracker::Stats& d = draws_->stats();
    const Renderer::Stats& r = renderer_->stats();
    REXLOG_INFO(
        "rexgpu-native: swap {}: hooks saw {} draws ({} indexed, {} dropped), {} clears, {} "
        "resolves, {} presents; renderer drew {}, skipped {} (other target) {} (primitive) {} "
        "(range) {} (not main pass) {} (no projection matrix) {} (position format), {} clears, "
        "{} frames",
        swap, d.draws, d.indexed_draws, d.dropped_draws, d.clears, d.resolves, d.presents,
        r.draws_recorded, r.draws_skipped_target, r.draws_skipped_primitive, r.draws_skipped_range,
        r.draws_skipped_overlay, r.draws_skipped_no_wvp, r.draws_skipped_format, r.clears_recorded, r.frames_submitted);
#if NR_GAME_RENDERER
    if (GameRenderer* g = renderer_->game()) {
      const GameRenderer::Stats& s = g->stats();
      const ShaderLibrary::Stats sh = shaders_->stats();
      REXLOG_INFO(
          "rexgpu-native: game renderer: {} draws, skipped {} (shader) {} (target) {} (primitive) {} "
          "(pipeline) {} (device); {} pipelines; {} texture uploads ({} failed), {} buffer uploads, "
          "{} clears, {} resolves ({} failed), {} invalidations; shaders {} created, {} from the pack, "
          "{} compiled, {} failed, {} from objects",
          s.draws, s.skipped_shader, s.skipped_target, s.skipped_primitive, s.skipped_pipeline,
          s.skipped_device, s.pipelines, s.texture_uploads, s.texture_failures, s.buffer_uploads,
          s.clears, s.resolves, s.resolve_failures, s.invalidations, sh.created, sh.from_pack,
          sh.compiled, sh.failed, sh.from_object);
    }
#endif
  }
  (void)frontbuffer_width;
  (void)frontbuffer_height;
  presenter->RefreshGuestOutput(
      width, height, display_width, display_height,
      [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        nvrhi::CommandListHandle cl = host_->device()->createCommandList();
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
}

}  // namespace nr
