// The standalone test host: NVRHI on a Vulkan device of its own (lavapipe),
// the milestone pictures rendered offscreen, read back and checked pixel by
// pixel. Milestone 3: the recorded per-draw sequence goes through the hook
// table, the NrApi binding and the tracker into the renderer, and both what
// reaches NVRHI and the resulting pixels are checked.
//
// NR_TEST_IMAGES=<dir> writes each read-back image there as a PPM.

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "backend/api_binding.h"
#include "backend/draw_state.h"
#include "backend/renderer.h"
#include "backend/tests/check.h"
#include "backend/tests/fake_guest.h"
#include "backend/tests/scene.h"
#include "backend/tests/vk_test_device.h"
#include "hooks/trace_replay.h"

using namespace nr;
using namespace nr::test;

namespace {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 144;
// One step of a 10-bit channel, plus rounding slack.
constexpr double kTol10 = 1.5 / 1023.0;

VulkanTestDevice* Device() {
  static std::unique_ptr<VulkanTestDevice> device = [] {
    auto d = VulkanTestDevice::Create(true);
    if (d) {
      std::printf("  device: %s, Khronos validation %s\n", d->device_name().c_str(),
                  d->khronos_validation() ? "on" : "not installed");
    }
    return d;
  }();
  return device.get();
}

void MaybeSave(const VulkanTestDevice::Pixels& p, const char* name) {
  if (const char* dir = std::getenv("NR_TEST_IMAGES")) {
    WritePpm(std::string(dir) + "/" + name + ".ppm", p);
  }
}

// Pixel centre for a clip-space position (D3D convention: +y is up).
std::pair<uint32_t, uint32_t> ToPixel(double x, double y) {
  return {uint32_t((x + 1.0) * 0.5 * kWidth), uint32_t((1.0 - y) * 0.5 * kHeight)};
}

void CheckPixel(const VulkanTestDevice::Pixels& p, double x, double y, const nvrhi::Color& c,
                double tol, const char* what) {
  auto [px, py] = ToPixel(x, y);
  const float* v = p.at(px, py);
  bool ok = std::fabs(v[0] - c.r) <= tol && std::fabs(v[1] - c.g) <= tol &&
            std::fabs(v[2] - c.b) <= tol;
  if (!ok) {
    std::ostringstream s;
    s << what << " at clip (" << x << ", " << y << ") = pixel (" << px << ", " << py << "): got ("
      << v[0] << ", " << v[1] << ", " << v[2] << "), want (" << c.r << ", " << c.g << ", " << c.b
      << ")";
    Fail(__FILE__, __LINE__, s.str());
  }
}

struct Fixture {
  VulkanTestDevice* gpu = Device();
  std::unique_ptr<Renderer> renderer;
  int errors_before = 0;
  bool Init() {
    if (!gpu) {
      Fail(__FILE__, __LINE__, "no Vulkan device (install mesa-vulkan-drivers for lavapipe)");
      return false;
    }
    errors_before = gpu->errors();
    renderer = std::make_unique<Renderer>(gpu->device());
    if (!renderer->Initialize(kWidth, kHeight)) {
      Fail(__FILE__, __LINE__, "Renderer::Initialize failed");
      return false;
    }
    renderer->set_submit([this](nvrhi::ICommandList* cl) { gpu->Execute(cl); });
    return true;
  }
  ~Fixture() {
    if (gpu) {
      gpu->WaitIdle();
      renderer.reset();
      gpu->WaitIdle();
      CHECK_EQ(gpu->errors(), errors_before);  // no validation errors
    }
  }
  VulkanTestDevice::Pixels Render(const std::function<void(nvrhi::ICommandList*)>& record,
                                  nvrhi::ITexture* target) {
    nvrhi::CommandListHandle cl = gpu->device()->createCommandList();
    cl->open();
    record(cl);
    cl->close();
    gpu->Execute(cl);
    return gpu->ReadBack(target);
  }
};

}  // namespace

// ----------------------------------------------------------- milestone 1 ---

TEST(Milestone1_ClearColour) {
  Fixture f;
  if (!f.Init()) return;
  nvrhi::ITexture* target = f.renderer->recording_image();
  const nvrhi::Color colour(0.2f, 0.4f, 0.6f, 1.0f);
  auto pixels = f.Render([&](nvrhi::ICommandList* cl) { f.renderer->RecordClear(cl, target, colour); },
                         target);
  MaybeSave(pixels, "m1_clear");
  CHECK_EQ(pixels.width, kWidth);
  CHECK_EQ(pixels.height, kHeight);
  int bad = 0;
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const float* v = pixels.at(x, y);
      if (std::fabs(v[0] - 0.2) > kTol10 || std::fabs(v[1] - 0.4) > kTol10 ||
          std::fabs(v[2] - 0.6) > kTol10 || v[3] != 1.0f) {
        ++bad;
      }
    }
  }
  CHECK_EQ(bad, 0);
}

TEST(Milestone1_ColourChangesEveryFrame) {
  Fixture f;
  if (!f.Init()) return;
  nvrhi::ITexture* target = f.renderer->recording_image();
  nvrhi::Color previous(-1.0f);
  for (uint32_t frame = 0; frame < 4; ++frame) {
    nvrhi::Color c = Renderer::TestClearColor(frame * 10);
    CHECK(c.r != previous.r || c.g != previous.g || c.b != previous.b);
    previous = c;
    auto pixels =
        f.Render([&](nvrhi::ICommandList* cl) { f.renderer->RecordClear(cl, target, c); }, target);
    CheckPixel(pixels, 0.0, 0.0, c, kTol10, "frame colour");
    CheckPixel(pixels, -0.99, 0.99, c, kTol10, "frame colour (corner)");
  }
  // The cycle repeats every 120 frames.
  nvrhi::Color a = Renderer::TestClearColor(5), b = Renderer::TestClearColor(125);
  CHECK(a.r == b.r && a.g == b.g && a.b == b.b);
}

// ----------------------------------------------------------- milestone 2 ---

TEST(Milestone2_Triangle) {
  Fixture f;
  if (!f.Init()) return;
  nvrhi::ITexture* target = f.renderer->recording_image();
  auto pixels = f.Render(
      [&](nvrhi::ICommandList* cl) {
        f.renderer->RecordClear(cl, target, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
        f.renderer->RecordTestTriangle(cl, target);
      },
      target);
  MaybeSave(pixels, "m2_triangle");
  // shaders/triangle.hlsl: (-0.5,-0.5) red, (0,0.5) green, (0.5,-0.5) blue.
  const double third = 1.0 / 3.0;
  CheckPixel(pixels, 0.0, -1.0 / 6.0, nvrhi::Color(third, third, third, 1), 0.03, "centroid");
  // Near each vertex its colour dominates; green at the top shows +y is up.
  auto near = [&](double x, double y, int channel, const char* what) {
    auto [px, py] = ToPixel(x, y);
    const float* v = pixels.at(px, py);
    bool ok = v[channel] > 0.8 && v[(channel + 1) % 3] < 0.2 && v[(channel + 2) % 3] < 0.2;
    if (!ok) Fail(__FILE__, __LINE__, std::string(what) + " vertex colour missing");
  };
  near(-0.47, -0.48, 0, "red (bottom left)");
  near(0.0, 0.45, 1, "green (top)");
  near(0.47, -0.48, 2, "blue (bottom right)");
  // Outside: the clear colour.
  const nvrhi::Color black(0, 0, 0, 1);
  CheckPixel(pixels, -0.9, 0.9, black, kTol10, "top-left corner");
  CheckPixel(pixels, 0.9, 0.9, black, kTol10, "top-right corner");
  CheckPixel(pixels, 0.0, -0.7, black, kTol10, "below the triangle");
  CheckPixel(pixels, -0.4, 0.3, black, kTol10, "left of the top edge");
  // Coverage: about a quarter of the clip square (base 1 x height 1 of 4).
  int covered = 0;
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const float* v = pixels.at(x, y);
      if (v[0] + v[1] + v[2] > 0.05) ++covered;
    }
  }
  CHECK_NEAR(double(covered) / (kWidth * kHeight), 0.125, 0.01);
}

// ----------------------------------------------------------- milestone 3 ---

namespace {

class NvrhiRecorder final : public SubmitObserver {
 public:
  struct Draw {
    nvrhi::GraphicsState state;
    nvrhi::DrawArguments args;
    PlaceholderConstants constants;
    std::vector<uint32_t> indices;
    uint32_t render_target0;
  };
  void OnClearRecorded(const ClearCall&, const nvrhi::Color& c) override { clears.push_back(c); }
  void OnDrawRecorded(const DrawCall& call, const nvrhi::GraphicsState& state,
                      const nvrhi::DrawArguments& args, const PlaceholderConstants& constants,
                      std::span<const uint32_t> indices) override {
    draws.push_back({state, args, constants, {indices.begin(), indices.end()},
                     call.render_targets[0]});
  }
  void OnFrameSubmitted(uint32_t frame, nvrhi::ITexture* image) override {
    frames.push_back({frame, image});
  }
  std::vector<nvrhi::Color> clears;
  std::vector<Draw> draws;
  std::vector<std::pair<uint32_t, nvrhi::ITexture*>> frames;
};

}  // namespace

TEST(Milestone3_RecordedFrameThroughNvrhi) {
  Fixture f;
  if (!f.Init()) return;
  FakeGuestMemory memory;
  TriangleScene scene(memory, kWidth, kHeight);
  NvrhiRecorder recorder;
  f.renderer->set_observer(&recorder);
  // The scene draws with ZFUNC always (an overlay for the main-pass filter,
  // which has its own test below): this test is about the plumbing.
  f.renderer->options().only_depth_tested = false;
  DrawTracker state(memory, f.renderer.get());
  ApiBinding binding;
  InitApiBinding(binding, &state, [](void* s) { return static_cast<DrawTracker*>(s); });

  // Frame 1: the main surface is not known yet, so both draws are recorded.
  std::istringstream log1(scene.Log());
  hooks::ReplayTrace(log1, &binding.api);
  CHECK_EQ(recorder.clears.size(), size_t(1));
  CHECK_EQ(recorder.draws.size(), size_t(2));
  CHECK_EQ(recorder.frames.size(), size_t(1));
  CHECK_EQ(f.renderer->stats().frames_submitted, uint64_t(1));

  // Frame 2: the render-to-texture draw is skipped.
  std::istringstream log2(scene.Log());
  hooks::ReplayTrace(log2, &binding.api);
  CHECK_EQ(recorder.draws.size(), size_t(3));
  CHECK_EQ(f.renderer->stats().draws_skipped_target, uint64_t(1));
  CHECK_EQ(recorder.frames.size(), size_t(2));
  CHECK_EQ(state.stats().dropped_draws, uint64_t(0));

  // What reached NVRHI for the scene's draw.
  if (recorder.draws.size() == 3) {
    const NvrhiRecorder::Draw& d = recorder.draws[2];
    CHECK_EQ(d.render_target0, scene.backbuffer);
    CHECK(d.state.pipeline != nullptr);
    CHECK(d.state.framebuffer != nullptr);
    if (d.state.framebuffer) {
      const auto& fbd = d.state.framebuffer->getDesc();
      CHECK_EQ(fbd.colorAttachments.size(), size_t(1));
      // Frame 2 records into the second frame image.
      CHECK(fbd.colorAttachments[0].texture == recorder.frames[1].second);
    }
    CHECK_EQ(d.state.bindings.size(), size_t(1));
    CHECK(d.state.indexBuffer.buffer != nullptr);
    CHECK(d.state.indexBuffer.format == nvrhi::Format::R32_UINT);
    CHECK_EQ(d.state.viewport.viewports.size(), size_t(1));
    if (!d.state.viewport.viewports.empty()) {
      const nvrhi::Viewport& vp = d.state.viewport.viewports[0];
      CHECK_NEAR(vp.minX, 0, 0);
      CHECK_NEAR(vp.maxX, kWidth, 0);
      CHECK_NEAR(vp.minY, 0, 0);
      CHECK_NEAR(vp.maxY, kHeight, 0);
      CHECK_NEAR(vp.minZ, 0, 0);
      CHECK_NEAR(vp.maxZ, 1, 0);
    }
    CHECK_EQ(d.args.vertexCount, 3u);
    CHECK_EQ(d.args.instanceCount, 1u);
    CHECK_EQ(d.args.startVertexLocation, 0u);
    // Indices 0,1,2 + base vertex 2 = vertices 2..4, rebased onto the
    // uploaded range.
    CHECK(d.indices == (std::vector<uint32_t>{0, 1, 2}));
    CHECK_EQ(d.constants.stride, TriangleScene::kStride);
    CHECK_EQ(d.constants.pos_offset, 4u);
    CHECK_EQ(d.constants.vertex_base % 256, 0u);
    CHECK_NEAR(d.constants.wvp[0], 1.6, 1e-6);
    CHECK_NEAR(d.constants.wvp[3], 0.1, 1e-6);
    nvrhi::Color expected = Renderer::DrawColor(scene.vertex_shader, scene.pixel_shader);
    CHECK_NEAR(d.constants.color[0], expected.r, 0);
    CHECK_NEAR(d.constants.color[1], expected.g, 0);
    CHECK_NEAR(d.constants.color[2], expected.b, 0);
  }

  // The pixels of the presented frame.
  nvrhi::ITexture* presented = f.renderer->presented_image();
  CHECK(presented == recorder.frames.back().second);
  if (!presented) return;
  auto pixels = f.gpu->ReadBack(presented);
  MaybeSave(pixels, "m3_frame");
  const nvrhi::Color clear(0x10 / 255.0f, 0x20 / 255.0f, 0x30 / 255.0f, 1);
  const nvrhi::Color draw = Renderer::DrawColor(scene.vertex_shader, scene.pixel_shader);
  // Clip-space triangle (-0.7,-0.8) (0.1,0.8) (0.9,-0.8): c0..c3 used as rows
  // (with the +0.1 x translation in c0.w), +y up.
  CheckPixel(pixels, 0.1, -0.2667, draw, kTol10, "triangle centre");
  CheckPixel(pixels, 0.1, 0.7, draw, kTol10, "near the top vertex");
  CheckPixel(pixels, -0.6, -0.75, draw, kTol10, "near the bottom-left vertex");
  CheckPixel(pixels, 0.8, -0.75, draw, kTol10, "near the bottom-right vertex");
  CheckPixel(pixels, -0.6, 0.7, clear, kTol10, "outside, top left");
  CheckPixel(pixels, 0.7, 0.7, clear, kTol10, "outside, top right");
  CheckPixel(pixels, 0.1, -0.9, clear, kTol10, "outside, below");
  CheckPixel(pixels, -0.95, -0.95, clear, kTol10, "corner");
}

TEST(Milestone3_ViewportAndTranspose) {
  Fixture f;
  if (!f.Init()) return;
  FakeGuestMemory memory;
  TriangleScene scene(memory, kWidth, kHeight);
  // The right half of the target only.
  scene.viewport = memory.NewViewport(kWidth / 2, 0, kWidth / 2, kHeight, 0.0f, 1.0f);
  f.renderer->options().only_depth_tested = false;
  DrawTracker state(memory, f.renderer.get());
  ApiBinding binding;
  InitApiBinding(binding, &state, [](void* s) { return static_cast<DrawTracker*>(s); });
  // Clear the whole target first with a full viewport, then draw.
  std::istringstream log(scene.Log());
  hooks::ReplayTrace(log, &binding.api);
  auto pixels = f.gpu->ReadBack(f.renderer->presented_image());
  MaybeSave(pixels, "m3_viewport");
  const nvrhi::Color draw = Renderer::DrawColor(scene.vertex_shader, scene.pixel_shader);
  const nvrhi::Color clear(0x10 / 255.0f, 0x20 / 255.0f, 0x30 / 255.0f, 1);
  // The triangle's centre (0.1, -0.27) in the right half's clip space is at
  // x = 0.5 + 0.1 * 0.5 = 0.55 in full-target clip space.
  CheckPixel(pixels, 0.55, -0.2667, draw, kTol10, "triangle in the right half");
  CheckPixel(pixels, -0.5, -0.2667, clear, kTol10, "left half untouched");

  // With c0..c3 read as columns the translation lands in w: the picture
  // changes (a guard that the option is wired).
  f.renderer->options().transpose_wvp = true;
  std::istringstream log2(scene.Log());
  hooks::ReplayTrace(log2, &binding.api);
  auto pixels2 = f.gpu->ReadBack(f.renderer->presented_image());
  bool differs = false;
  for (size_t i = 0; i < pixels.rgba.size() && !differs; ++i) {
    differs = std::fabs(pixels.rgba[i] - pixels2.rgba[i]) > 0.01;
  }
  CHECK(differs);
}

// The main-pass filter (Renderer::Options::only_depth_tested, on by default):
// the scene's ZFUNC always draw is an overlay and is skipped.
TEST(Milestone3_MainPassFilterSkipsOverlays) {
  Fixture f;
  if (!f.Init()) return;
  FakeGuestMemory memory;
  TriangleScene scene(memory, kWidth, kHeight);
  DrawTracker state(memory, f.renderer.get());
  ApiBinding binding;
  InitApiBinding(binding, &state, [](void* s) { return static_cast<DrawTracker*>(s); });
  std::istringstream log(scene.Log());
  hooks::ReplayTrace(log, &binding.api);
  CHECK_EQ(f.renderer->stats().draws_recorded, uint64_t(0));
  CHECK_EQ(f.renderer->stats().draws_skipped_overlay, uint64_t(2));
}

NR_TEST_MAIN()
