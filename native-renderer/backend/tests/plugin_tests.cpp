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
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/vulkan/provider.h>

#include "backend/api_binding.h"
#include "backend/draw_state.h"
#include "backend/host_device.h"
#include "backend/driver_pipeline_cache.h"
#include "backend/renderer.h"
#include "backend/tests/check.h"
#include "backend/tests/fake_guest.h"
#include "backend/tests/readback.h"
#include "backend/tests/scene.h"
#include "hooks/trace_replay.h"
#include "backend/pipeline_cache.h"
#include "plugin/native_api.h"
#include "plugin/native_prewarm.h"
#include "plugin/native_stutter.h"

// vulkan_host.cpp reads it (plugin_main.cpp defines it in the plugin).
REXCVAR_DEFINE_BOOL(native_validation, true, "Native renderer", "NVRHI validation layer");
// vulkan_host.cpp reads it too: the host keeps a VkPipelineCache of its own when it is on.
REXCVAR_DEFINE_BOOL(native_vk_pipeline_cache, false, "Native renderer", "Keep the driver's pipeline cache");
// The SDK's app defines it (rex_app.cpp, not in the runtime library this test links); the plugin
// reads it by name (the launcher's prewarm runs only for the native plugin).
REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU", "GPU plugin");
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

TEST(VkPipelineCacheOnTheSdkVulkanProvider) {
  // --native_vk_pipeline_cache: the host makes the cache NVRHI's device uses, it is saved with what
  // the device made and taken by the next device on the same driver.
  const std::string path =
      (std::filesystem::temp_directory_path() / "nr_plugin_tests_pipelines" / "game.vkcache").string();
  std::filesystem::remove(path);
  REXCVAR_SET(native_vk_pipeline_cache, true);
  for (int run = 0; run < 2; ++run) {
    auto provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
    CHECK(provider != nullptr);
    if (!provider) break;
    std::unique_ptr<HostDevice> host = CreateVulkanHostDevice(*provider);
    CHECK(host != nullptr);
    if (!host) break;
    DriverPipelineCache* cache = host->pipeline_cache();
    CHECK(cache != nullptr);
    if (!cache) break;
    std::string note;
    const DriverPipelineCache::Load load = cache->LoadFile(path, &note);
    CHECK(load == (run == 0 ? DriverPipelineCache::Load::kAbsent : DriverPipelineCache::Load::kLoaded));
    {
      Renderer renderer(host->device());  // makes its pipelines through the cache
      CHECK(renderer.Initialize(64, 64));
      size_t bytes = 0;
      CHECK(cache->SaveFile(path, &bytes, &note));
      CHECK(bytes >= 32);
    }
    host->WaitForIdle();
    host.reset();
  }
  REXCVAR_SET(native_vk_pipeline_cache, false);
  // Off (the default): no cache of ours.
  auto provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
  if (provider) {
    std::unique_ptr<HostDevice> host = CreateVulkanHostDevice(*provider);
    CHECK(host != nullptr);
    if (host) CHECK(host->pipeline_cache() == nullptr);
  }
}

TEST(LauncherPrewarmEntryPoints) {
  // The launcher finds the exports in the plugin it preloaded; off unless asked for.
  std::unique_ptr<rex::system::IGraphicsSystem> system = rex::system::LoadGpuPlugin("native", "vulkan");
  CHECK(system != nullptr);
  void* module = nullptr;
  for (const char* name : {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"}) {
    if ((module = dlopen(name, RTLD_NOW | RTLD_NOLOAD))) break;
  }
  CHECK(module != nullptr);
  if (!module) return;
  using Start = uint32_t (*)(const NrPrewarmRequest*);
  using Status = uint32_t (*)(NrPrewarmStatus*);
  using Stop = void (*)();
  auto start = reinterpret_cast<Start>(dlsym(module, "nr_prewarm_start"));
  auto status = reinterpret_cast<Status>(dlsym(module, "nr_prewarm_status"));
  auto stop = reinterpret_cast<Stop>(dlsym(module, "nr_prewarm_stop"));
  CHECK(start && status && stop);
  if (!start || !status || !stop) return;

  // The player's pipeline file under the cache root the launcher passes: one description whose
  // shaders no pack has (the game would make them), so the prewarm lists it and leaves it.
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "nr_plugin_tests_prewarm";
  std::filesystem::remove_all(root);
  PipelineRecord r;
  r.vs_hash = 0xDEAD;
  r.vs_input = 1;
  r.topology = 3;
  r.color_count = 1;
  r.color_format[0] = 11;
  r.color_mask[0] = 0xF;
  CHECK(WritePipelineList((root / "native-pipelines" / "555307D3.pipelines").string(), {r}));
  const std::string root_text = root.string();
  NrPrewarmRequest request = {sizeof(NrPrewarmRequest), NR_PREWARM_VERSION, root_text.c_str(), 0x555307D3u};

  CHECK_EQ(start(&request), uint32_t(NR_PREWARM_OFF));  // --native_launcher_prewarm is off by default
  rex::cvar::SetFlagByName("native_launcher_prewarm", "true");
  const std::string plugin_before = rex::cvar::GetFlagByName("gpu_plugin");
  rex::cvar::SetFlagByName("gpu_plugin", "xenos");
  CHECK_EQ(start(&request), uint32_t(NR_PREWARM_OFF));  // another GPU plugin draws the game
  rex::cvar::SetFlagByName("gpu_plugin", "native");
  NrPrewarmRequest old = request;
  old.version = NR_PREWARM_VERSION + 1;
  CHECK_EQ(start(&old), uint32_t(NR_PREWARM_OFF));
  CHECK_EQ(start(&request), uint32_t(NR_PREWARM_RUNNING));
  NrPrewarmStatus s = {};
  s.size = sizeof(s);
  for (int i = 0; i < 600 && status(&s) == NR_PREWARM_RUNNING; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK_EQ(s.state, uint32_t(NR_PREWARM_DONE));
  CHECK_EQ(s.listed, 1u);
  CHECK_EQ(s.queued, 0u);
  CHECK_EQ(s.waiting, 1u);
  CHECK_EQ(start(&request), uint32_t(NR_PREWARM_DONE));  // once a process
  stop();  // nothing left to stop
  CHECK_EQ(status(&s), uint32_t(NR_PREWARM_DONE));
  rex::cvar::SetFlagByName("native_launcher_prewarm", "false");
  rex::cvar::SetFlagByName("gpu_plugin", plugin_before);
  // The overlay's counter: nothing without a game renderer (and --native_stutter_stats off).
  using Stutter = uint32_t (*)(NrStutterStats*);
  auto stutter = reinterpret_cast<Stutter>(dlsym(module, "nr_stutter_stats"));
  CHECK(stutter != nullptr);
  if (stutter) {
    NrStutterStats st = {};
    st.size = sizeof(st);
    CHECK_EQ(stutter(&st), 0u);
    CHECK_EQ(stutter(nullptr), 0u);
  }
  dlclose(module);
  system->Shutdown();
  std::filesystem::remove_all(root);
}

NR_TEST_MAIN()
