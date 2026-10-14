// Exercise the launcher's exported setup/Play job without opening a game window.
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <rex/cvar.h>
#include "plugin/native_shader_pack_api.h"
#include "kkshaders/pack_builder.h"

int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) return 2;  // plugin, private cache, game-data root, optional reuse
  const bool reuse = argc == 5;
  const auto module = LoadLibraryA(argv[1]);
  if (!module) { std::printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
  using GetApi = const NrShaderPackApi* (*)();
  const auto get = reinterpret_cast<GetApi>(GetProcAddress(module, "nr_get_shader_pack_api"));
  if (!get) return 1;
  const auto* api = get();
  bool ok = api && api->version == NR_SHADER_PACK_API_VERSION;
  if (!ok || (!reuse && std::filesystem::exists(argv[2]))) return 1;
  auto run = [&](const char* stage, bool want_current) {
    auto* job = api->start(argv[2], argv[3], 6, 1, 0);
    NrShaderPackProgress p{};
    const auto start = std::chrono::steady_clock::now();
    do {
      api->poll(job, &p);
      if (std::chrono::steady_clock::now() - start > std::chrono::minutes(8)) {
        api->cancel(job);
        api->release(job);
        return false;
      }
      if (!p.finished) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (!p.finished);
    std::printf("%s: ok=%d current=%d cancelled=%d seconds=%.3f %s\n",
                stage, p.ok, p.up_to_date, p.cancelled, p.seconds, p.message);
    api->release(job);
    return p.ok && !p.cancelled && bool(p.up_to_date) == want_current;
  };
  rex::cvar::SetFlagByName("native_render_scale", "1");
  rex::cvar::SetFlagByName("native_water_reflections", "off");
  ok = run(reuse ? "updated setup" : "fresh setup", reuse) && ok;
  kkshaders::PackStamp spirv, dxil;
  const auto folder = std::filesystem::path(argv[2]) / kkshaders::kPackFolderName;
  ok = kkshaders::readPackStamp(folder / kkshaders::packFileName(kkshaders::PackBackend::Spirv), spirv) && ok;
  ok = kkshaders::readPackStamp(folder / kkshaders::packFileName(kkshaders::PackBackend::Dxil), dxil) && ok;
  std::printf("setup packs: SPIR-V %u, DXIL %u, both scaled/motion/water=%d\n", spirv.shaders, dxil.shaders,
      spirv.scaled && dxil.scaled && spirv.motion && dxil.motion && spirv.lighting && dxil.lighting);
  ok = spirv.shaders == dxil.shaders && spirv.scaled && dxil.scaled && spirv.motion && dxil.motion &&
       spirv.lighting && dxil.lighting && ok;
  rex::cvar::SetFlagByName("native_render_scale", "2560x1440");
  rex::cvar::SetFlagByName("native_upscaler", "dlss");
  rex::cvar::SetFlagByName("native_upscaler_quality", "quality");
  ok = run("Play settings", true) && ok;
  rex::cvar::SetFlagByName("native_backend", "d3d12");
  ok = run("switch DX12", true) && ok;
  rex::cvar::SetFlagByName("native_backend", "vulkan");
  ok = run("switch Vulkan", true) && ok;
  // The plugin's registry remains in the SDK; leave it loaded until process exit.
  std::printf("shader setup/Play service: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
