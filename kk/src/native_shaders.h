// The native renderer's shader pack, seen from the launcher.
//
// The native renderer (--gpu_plugin=native) draws with the game's own shaders, translated and
// compiled for the host. They come from the player's copy of the game (Shaders/xeshaders.bin),
// so the port ships none: the launcher has the native renderer's plugin build them into
// <cache>/native-shaders, once, and again whenever the port's translator, its DXC or the game
// files change (native-renderer/plugin/native_shader_pack_api.h, docs/shaders.md in
// native-renderer). All game builds include the native renderer.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "shader_prepared.h"

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
  // thread of the plugin's (low priority, every hardware thread but one: the launcher and its
  // music keep one for themselves). Does nothing while one runs.
  // force: build it even when it is current (Home's "recompile shaders").
  void Start(const std::filesystem::path& cache_dir, const std::filesystem::path& game_dir, bool force = false);
  void Cancel();
  bool Busy();          // started and not finished
  bool Finished();      // the last one finished (Busy() is false)
  bool Ok();            // ... and the pack is current
  bool Cancelled();     // the last job finished because it was cancelled
  // How the last job ended (shader_prepared.h decides whether it was the version's one preparation).
  ShaderJobEnd End();
  // True once for each job that has finished (the launcher records the preparation then).
  bool TakeFinished();
  // The whole job as one bar (shader_progress.h): 0 to 1, never going back, once something is being
  // built; -1 while it is only checking, or when idle.
  float Fraction();
  // Something is being compiled or prepared in this job, not only checked (the launcher's music
  // waits for it).
  bool Working();
  // The bar as drawn at `now` (seconds): Fraction() eased, smooth and never backwards.
  float DrawnFraction(double now);
  // The bar's one label for the whole job (translated): no steps or graphics APIs (owner, 16 Oct).
  std::string ProgressLabel();
  // Home's SHADERS status (translated). prepared: ShadersPrepared() (shader_prepared.h), the one definition the
  // setup, Home and Play share; this job's state only adds the progress while one runs.
  ShaderHomeStatus Status(bool prepared);
  std::string StatusText(bool prepared);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kk
