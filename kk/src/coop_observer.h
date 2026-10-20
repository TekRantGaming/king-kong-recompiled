#pragma once
#include <array>
#include <cstdint>
struct PPCContext;
namespace kk::coop::observer {
// Game-thread lifecycle, camera and input entry points. UI reads copied atomics only.
void GameFrame(uint8_t* base);
void LevelLoaded();
void Checkpoint(uint32_t level,uint32_t checkpoint);
void NormalCamera(const uint8_t* base,uint32_t actor,uint32_t caller);
bool View(uint8_t* base,uint32_t view,uint32_t caller,std::array<uint8_t,68>& saved,float& saved_fov,float lens_factor);
void LateWorld(PPCContext& ctx,uint8_t* base);
void FilterPad(uint32_t user,uint8_t* state);
bool SuppressLook();
bool HideWeapon();
bool HoldWorld();
bool StatusScreen();
bool Active();
// Game-thread guard covers native movies and both local/host script cameras.
bool InCutscene(const uint8_t* base);
// UI-thread snapshot. Never reads guest memory from a drawing callback.
bool CutsceneActive();
const char* Status();
void Fallback(const char* static_reason);
// C37 integration: guest only, same loaded generation. Caller clears on revive.
// Automatically cleared by level load, lost connection, co-op off or session end.
void SetPartnerSpectating(bool enabled);
}
