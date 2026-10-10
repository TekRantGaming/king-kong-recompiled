#include "backend/backend.h"

#include <rex/logging.h>
#include <rex/system/xmemory.h>

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
  LockedSink(DrawSink& inner, std::mutex& mutex) : inner_(inner), mutex_(mutex) {}
  void OnClear(const ClearCall& c) override {
    std::lock_guard lock(mutex_);
    inner_.OnClear(c);
  }
  void OnDraw(const DrawCall& d) override {
    std::lock_guard lock(mutex_);
    inner_.OnDraw(d);
  }
  void OnResolve(const ResolveCall& r) override {
    std::lock_guard lock(mutex_);
    inner_.OnResolve(r);
  }
  void OnPresent(uint32_t frame, uint32_t rt0) override {
    std::lock_guard lock(mutex_);
    inner_.OnPresent(frame, rt0);
  }

 private:
  DrawSink& inner_;
  std::mutex& mutex_;
};

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
    : host_(std::move(host)), guest_memory_(memory) {}

Backend::~Backend() { Shutdown(); }

bool Backend::Initialize(uint32_t width, uint32_t height) {
  renderer_ = std::make_unique<Renderer>(host_->device());
  if (!renderer_->Initialize(width, height)) {
    REXGPU_ERROR("rexgpu-native: renderer initialisation failed ({}x{})", width, height);
    return false;
  }
  renderer_->set_submit([this](nvrhi::ICommandList* cl) {
    host_->ExecuteCommandList(cl);
    ++submitted_frames_;
  });
  sink_ = std::make_unique<LockedSink>(*renderer_, mutex_);
  draws_ = std::make_unique<DrawState>(guest_memory_, sink_.get());
  REXGPU_INFO("rexgpu-native: NVRHI renderer ready ({}x{})", width, height);
  return true;
}

void Backend::Shutdown() {
  if (host_) host_->WaitForIdle();
  draws_.reset();
  sink_.reset();
  renderer_.reset();
  if (host_) host_->WaitForIdle();
  host_.reset();
}

void Backend::Present(rex::ui::Presenter* presenter, uint32_t frontbuffer_width,
                      uint32_t frontbuffer_height, uint32_t display_width,
                      uint32_t display_height) {
  std::lock_guard lock(mutex_);
  if (!renderer_) return;
  const bool game_frame = submitted_frames_ != shown_frames_ && renderer_->presented_image();
  shown_frames_ = submitted_frames_;
  const uint32_t width = renderer_->width(), height = renderer_->height();
  const uint32_t swap = swaps_++;
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
