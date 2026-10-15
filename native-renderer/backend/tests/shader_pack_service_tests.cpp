// Exercise the launcher's exported setup/Play job without opening a game window.
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "plugin/native_shader_pack_api.h"
#include "kkshaders/pack_builder.h"

int main(int argc, char** argv) {
  if (argc < 4) return 2;  // plugin, private cache, game-data root, optional reuse/list/audit
  rex::InitLogging((std::string(argv[2]) + ".log").c_str());
  bool reuse = false, audit = false, probe = false;
  std::string pipeline_list;
  unsigned logical = 0;
  for (int i = 4; i < argc; ++i) {
    reuse |= std::strcmp(argv[i], "reuse") == 0;
    audit |= std::strcmp(argv[i], "audit") == 0;
    probe |= std::strcmp(argv[i], "probe") == 0;
    if (std::strncmp(argv[i], "logical=", 8) == 0) logical = unsigned(std::atoi(argv[i] + 8));
  }
  if (logical) {
    DWORD_PTR allowed = 0, system = 0, mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return 2;
    unsigned found = 0;
    for (unsigned bit = 0; bit < 64 && found < logical; ++bit)
      if (allowed & (uint64_t(1) << bit)) { mask |= uint64_t(1) << bit; ++found; }
    if (found != logical || !SetProcessAffinityMask(GetCurrentProcess(), mask)) return 2;
    std::printf("process logical=%u mask=%llX\n", logical, uint64_t(mask));
  }
  const auto module = LoadLibraryA(argv[1]);
  if (!module) { std::printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
  using GetApi = const NrShaderPackApi* (*)();
  const auto get = reinterpret_cast<GetApi>(GetProcAddress(module, "nr_get_shader_pack_api"));
  if (!get) return 1;
  const auto* api = get();
  bool ok = api && api->version == NR_SHADER_PACK_API_VERSION;
  if (!ok || (!reuse && std::filesystem::exists(argv[2]))) return 1;
  for (int i = 4; i < argc; ++i) {
    if (std::strncmp(argv[i], "list=", 5) == 0) {
      pipeline_list = argv[i] + 5;
      rex::cvar::SetFlagByName("native_pipeline_list_file", pipeline_list);
    }
    if (std::strncmp(argv[i], "dxc=", 4) == 0)
      rex::cvar::SetFlagByName("native_dxc", argv[i] + 4);
  }
  bool pipeline_work = false;
  auto run = [&](const char* stage, bool want_current, bool want_ok = true) {
    auto* job = api->start(argv[2], argv[3], 6, 1, 0);
    NrShaderPackProgress p{};
    pipeline_work = false;
    const auto start = std::chrono::steady_clock::now();
    do {
      api->poll(job, &p);
      pipeline_work |= p.working && p.step_kind == 1;
      if (std::chrono::steady_clock::now() - start > std::chrono::minutes(8)) {
        api->cancel(job);
        api->release(job);
        return false;
      }
      if (!p.finished) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (!p.finished);
    std::printf("%s: ok=%d current=%d cancelled=%d pipeline_work=%d wall=%.3f seconds=%.3f %s\n",
                stage, p.ok, p.up_to_date, p.cancelled, pipeline_work,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), p.seconds, p.message);
    api->release(job);
    return bool(p.ok) == want_ok && !p.cancelled && bool(p.up_to_date) == want_current;
  };
  rex::cvar::SetFlagByName("native_render_scale", "1");
  rex::cvar::SetFlagByName("native_water_reflections", "original");
  ok = run(reuse ? "updated setup" : "fresh setup", reuse) && ok;
  if (audit || probe) {
    ok = run("warm setup", true) && ok;
    ok = !pipeline_work && ok;
    const auto driver = std::filesystem::path(argv[2]) / "native-pipelines" / "555307D3.vkcache";
    if (!std::filesystem::is_regular_file(driver)) return 1;
    const auto backup = driver.string() + ".before-095-" + std::to_string(GetCurrentProcessId());
    std::filesystem::copy_file(driver, backup, std::filesystem::copy_options::overwrite_existing);
    // Change only the private cache header's driver version, just as a driver update does.
    {
      std::fstream file(driver, std::ios::binary | std::ios::in | std::ios::out);
      file.seekg(20);
      char byte = 0;
      file.read(&byte, 1);
      byte ^= 1;
      file.seekp(20);
      file.write(&byte, 1);
    }
    ok = run("changed driver", true) && ok;
    if (!probe) ok = pipeline_work && ok;
    ok = run("warm after driver rebuild", true) && ok;
    if (!probe) ok = !pipeline_work && ok;
    if (!probe) {
      // A present but corrupt file must not be accepted on existence alone either.
      std::ofstream(driver, std::ios::binary | std::ios::trunc) << "damaged private cache";
      ok = run("damaged driver cache", true) && ok;
      ok = pipeline_work && ok;
      ok = run("warm after damaged rebuild", true) && ok;
      ok = !pipeline_work && ok;
      std::filesystem::rename(driver, driver.string() + ".missing-095-" + std::to_string(GetCurrentProcessId()));
      ok = run("missing driver cache", true) && ok;
      ok = pipeline_work && ok;
      ok = run("warm after missing rebuild", true) && ok;
      ok = !pipeline_work && ok;
    }
  }
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
  if (audit && !pipeline_list.empty()) {
    const auto marker = folder / "kkshaders-dxil.pack.pipelines-ready";
    std::ifstream input(marker, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    input.close();
    const auto driver = text.find("pipeline layouts 2\n");
    if (driver == std::string::npos || driver + std::strlen("pipeline layouts 2\n") >= text.size()) return 1;
    text[driver + std::strlen("pipeline layouts 2\n")] ^= 1;
    std::ofstream(marker, std::ios::binary | std::ios::trunc) << text;
    ok = run("DX12 driver stamp changed", true) && ok;
    ok = pipeline_work && ok;
    ok = run("warm after DX12 stamp rebuild", true) && ok;
    ok = !pipeline_work && ok;
    const auto bad_list = std::filesystem::path(argv[2]) / "missing-shader.list";
    std::filesystem::copy_file(pipeline_list, bad_list, std::filesystem::copy_options::overwrite_existing);
    {
      std::fstream file(bad_list, std::ios::binary | std::ios::in | std::ios::out);
      file.seekp(16);  // first record's vertex hash, deliberately absent from this pack
      const uint64_t absent = 0x0123456789ABCDEFull;
      file.write(reinterpret_cast<const char*>(&absent), sizeof(absent));
    }
    rex::cvar::SetFlagByName("native_pipeline_list_file", bad_list.string());
    ok = run("incomplete list refused", false, false) && ok;
    rex::cvar::SetFlagByName("native_pipeline_list_file", pipeline_list);
    ok = run("valid list still ready", true) && ok;
    ok = !pipeline_work && ok;
  }
  // The plugin's registry remains in the SDK; leave it loaded until process exit.
  std::printf("shader setup/Play service: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
