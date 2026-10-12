// The native renderer's shader pack, seen from the launcher.
//
// The native renderer (--gpu_plugin=native) draws with the game's own shaders, translated and
// compiled for the host. They come from the player's copy of the game (Shaders/xeshaders.bin),
// so the port ships none: the launcher has the native renderer's plugin build them into
// <cache>/native-shaders, once, and again whenever the port's translator, its DXC or the game
// files change (native-renderer/plugin/native_shader_pack_api.h, docs/shaders.md in
// native-renderer). With the Xenos renderer none of this runs.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace kk {

class NativeShaderPack {
 public:
  NativeShaderPack();
  ~NativeShaderPack();  // stops a build in progress (nothing is written) and waits for it
  NativeShaderPack(const NativeShaderPack&) = delete;
  NativeShaderPack& operator=(const NativeShaderPack&) = delete;

  // The game will run on the native renderer (gpu_plugin is "native") and its plugin, loaded by
  // PreloadGpuPlugin, can build the pack.
  static bool Wanted();

  // Checks the pack for the current settings and builds it if it is missing or stale, on a
  // thread of the plugin's (low priority, every hardware thread). Does nothing while one runs.
  void Start(const std::filesystem::path& cache_dir, const std::filesystem::path& game_dir);
  void Cancel();
  bool Busy();          // started and not finished
  bool Finished();      // the last one finished (Busy() is false)
  bool Ok();            // ... and the pack is current
  float Fraction();     // 0 to 1 while compiling, -1 before the shaders are counted or when idle
  uint32_t Done();
  uint32_t Total();
  std::string StatusText();  // for the launcher's status line (English, through T())

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kk
