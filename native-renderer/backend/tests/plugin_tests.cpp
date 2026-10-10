// The plugin against the real SDK (Linux bundle), without the game:
// - the runtime's own loader (rex::system::LoadGpuPlugin) loads
//   librexgpu-native*.so from next to this executable and creates the
//   graphics system;
// - NVRHI goes on the SDK's own VulkanProvider device (headless, lavapipe)
//   through the plugin's vulkan_host.cpp, and the milestone pictures are
//   rendered and read back there.
//
// Run under the Khronos validation layer by the ctest wrapper
// (check_validation.py), which fails the test on any validation error.

#include <dlfcn.h>

#include <memory>
#include <sstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/vulkan/provider.h>

#include "backend/api_binding.h"
#include "backend/draw_state.h"
#include "backend/host_device.h"
#include "backend/renderer.h"
#include "backend/tests/check.h"
#include "backend/tests/fake_guest.h"
#include "backend/tests/readback.h"
#include "backend/tests/scene.h"
#include "hooks/trace_replay.h"
#include "plugin/native_api.h"

// vulkan_host.cpp reads it (plugin_main.cpp defines it in the plugin).
REXCVAR_DEFINE_BOOL(native_validation, true, "Native renderer", "NVRHI validation layer");
// backend.cpp reads it (the plugin defines it in plugin_main.cpp).
REXCVAR_DEFINE_INT32(native_dump_frame, -1, "Native renderer", "Log every call of this game frame");

using namespace nr;
using namespace nr::test;

namespace {

constexpr double kTol10 = 1.5 / 1023.0;

void CheckPixel(const Pixels& p, uint32_t w, uint32_t h, double x, double y,
                const nvrhi::Color& c, double tol, const char* what) {
  uint32_t px = uint32_t((x + 1.0) * 0.5 * w), py = uint32_t((1.0 - y) * 0.5 * h);
  const float* v = p.at(px, py);
  if (std::fabs(v[0] - c.r) > tol || std::fabs(v[1] - c.g) > tol || std::fabs(v[2] - c.b) > tol) {
    std::ostringstream s;
    s << what << ": got (" << v[0] << ", " << v[1] << ", " << v[2] << "), want (" << c.r << ", "
      << c.g << ", " << c.b << ")";
    Fail(__FILE__, __LINE__, s.str());
  }
}

}  // namespace

TEST(RuntimeLoadsThePlugin) {
  std::unique_ptr<rex::system::IGraphicsSystem> system =
      rex::system::LoadGpuPlugin("native", "vulkan");
  CHECK(system != nullptr);
  if (!system) return;
  CHECK(!system->has_presentation());
  // The hooks' entry point, found the way kk_native_hooks.cpp finds it.
  void* module = nullptr;
  for (const char* name :
       {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"}) {
    if ((module = dlopen(name, RTLD_NOW | RTLD_NOLOAD))) break;
  }
  CHECK(module != nullptr);
  if (module) {
    using GetApi = const NrApi* (*)();
    auto get_api = reinterpret_cast<GetApi>(dlsym(module, "nr_get_api"));
    CHECK(get_api != nullptr);
    if (get_api) {
      const NrApi* api = get_api();
      CHECK_EQ(api->version, uint32_t(NR_API_VERSION));
      CHECK_EQ(api->size, uint32_t(sizeof(NrApi)));
      // Before SetupGuestGpu the plugin is inactive: the hooks only run the
      // game's originals.
      CHECK_EQ(api->is_active(api->self), 0);
      api->draw_indexed(api->self, 0, 4, 0, 0, 3);  // dropped, no crash
    }
    dlclose(module);
  }
  system->Shutdown();
}

TEST(RuntimeRejectsAnUnavailableBackend) {
  // The Linux bundle has no D3D12: the factory returns null and the runtime
  // reports it (the game would then exit with the loader's message).
  CHECK(rex::system::LoadGpuPlugin("native", "d3d12") == nullptr);
}

TEST(NvrhiOnTheSdkVulkanProvider) {
  auto provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
  CHECK(provider != nullptr);
  if (!provider) return;
  std::printf("  SDK Vulkan device: %s\n", provider->vulkan_device()->properties().deviceName);
  std::unique_ptr<HostDevice> host = CreateVulkanHostDevice(*provider);
  CHECK(host != nullptr);
  if (!host) return;
  nvrhi::IDevice* device = host->device();
  const uint32_t w = 192, h = 108;
  {
    Renderer renderer(device);
    CHECK(renderer.Initialize(w, h));
    renderer.set_submit([&](nvrhi::ICommandList* cl) { host->ExecuteCommandList(cl); });

    // Milestones 1 and 2, submitted the way the plugin submits (under the
    // SDK's queue lock).
    nvrhi::ITexture* target = renderer.recording_image();
    nvrhi::CommandListHandle cl = device->createCommandList();
    cl->open();
    renderer.RecordClear(cl, target, Renderer::TestClearColor(30));
    renderer.RecordTestTriangle(cl, target);
    cl->close();
    host->ExecuteCommandList(cl);
    Pixels p = ReadBackTexture(device, target);
    nvrhi::Color bg = Renderer::TestClearColor(30);
    CheckPixel(p, w, h, -0.9, 0.9, bg, kTol10, "milestone 1 background");
    CheckPixel(p, w, h, 0.0, -1.0 / 6.0, nvrhi::Color(1 / 3.0f, 1 / 3.0f, 1 / 3.0f, 1), 0.03,
               "milestone 2 triangle centre");

    // Milestone 3: the recorded frame through the hook table.
    FakeGuestMemory memory;
    TriangleScene scene(memory, w, h);
    // The scene draws with ZFUNC always: the main-pass filter would skip it.
    renderer.options().only_depth_tested = false;
    DrawTracker state(memory, &renderer);
    ApiBinding binding;
    InitApiBinding(binding, &state, [](void* s) { return static_cast<DrawTracker*>(s); });
    std::istringstream log(scene.Log());
    hooks::ReplayTrace(log, &binding.api);
    CHECK_EQ(renderer.stats().draws_recorded, uint64_t(2));
    Pixels frame = ReadBackTexture(device, renderer.presented_image());
    CheckPixel(frame, w, h, 0.1, -0.2667,
               Renderer::DrawColor(scene.vertex_shader, scene.pixel_shader), kTol10,
               "milestone 3 draw");
    CheckPixel(frame, w, h, -0.95, -0.95,
               nvrhi::Color(0x10 / 255.0f, 0x20 / 255.0f, 0x30 / 255.0f, 1), kTol10,
               "milestone 3 clear");
    host->WaitForIdle();
  }
  host->WaitForIdle();
  host.reset();
}

NR_TEST_MAIN()
