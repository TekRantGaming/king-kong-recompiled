#pragma once

#include <cstdlib>

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include "backend/thread_scheduling.h"

namespace kk {

// Initialize runs on the SDK's Audio Worker, before its semaphore-driven pump.
class ScheduledAudioSystem : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;
  static std::unique_ptr<rex::audio::AudioSystem> Create(rex::runtime::FunctionDispatcher* dispatcher) {
    return std::make_unique<ScheduledAudioSystem>(dispatcher);
  }

 protected:
  void Initialize() override {
    nr::SetCpuPriorityLevel(std::atoi(rex::cvar::GetFlagByName("kk_cpu_priority").c_str()));  // task 295
    nr::ScheduleCriticalThread(true);
    SDLAudioSystem::Initialize();
  }
};

}  // namespace kk
