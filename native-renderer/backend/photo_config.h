// Photo mode in the renderer (task 208; kk/src/photo_mode.cpp, docs/photo-mode.md): the settings the game
// sends (plugin/native_photo.h), which draws photo mode leaves out, and the photo grade's colour function on
// the CPU (the GPU pass, shaders/photo.hlsl, does the same; the tests compare them).
//
// Renderer only: nothing here changes the game. With photo mode off every frame is as before.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "plugin/native_photo.h"

namespace nr {

// Draws photo mode leaves out (hide bit 1): Jack's floating first-person arms (owner, 18 Oct: only the arms;
// weapons show only while aiming and stay; Ann and Kong are never hidden), by their
// geometry (GeometryContentHash: index and vertex bytes, never addresses) and index count. Their pixel
// shaders are shared with leaves and other skinned models, so shaders are never used to pick them.
struct PhotoHiddenMesh {
  uint64_t mesh;
  uint32_t count;
  const char* what;
};
// The table (photo_config.cpp); the counts are the cheap first check before a draw is hashed.
const PhotoHiddenMesh* PhotoHiddenMeshes(size_t* count);
bool PhotoMeshCountCandidate(uint32_t index_count);
bool PhotoHidesMesh(uint64_t mesh, uint32_t index_count);

// The game's pause covers the whole picture with one black quad at the end of the frame (hide bit 2);
// the world under it is still drawn every frame (task 208 phase 0).
constexpr uint64_t kPauseCoverVs = 0x2E3B3ECDD3BC8D62ull;  // the template (patched: B9AD69E17E826C6E)
constexpr uint64_t kPauseCoverPs = 0xEC85BA15F4E9DC06ull;
inline bool PhotoIsPauseCover(uint64_t vs_hash, uint64_t ps_hash, uint32_t index_count) {
  return vs_hash == kPauseCoverVs && ps_hash == kPauseCoverPs && index_count == 4;
}

// The settings with every value in range (NaN and junk replaced by the neutral value).
NrPhotoSettings SanitizePhotoSettings(const NrPhotoSettings& in);
NrPhotoSettings NeutralPhotoSettings();
// Photo mode changes the colours at all (the grade pass runs).
bool PhotoGradeNeeded(const NrPhotoSettings& s);

// The look presets' colour (the tint a fully applied look multiplies the luminance with).
std::array<float, 3> PhotoLookColour(uint32_t look);

// The grade's colour part for one pixel (gamma-encoded 0..1 in and out), before vignette, grain and bars.
std::array<float, 3> PhotoGradeColour(const NrPhotoSettings& s, std::array<float, 3> rgb);
// Vignette factor at a picture position (0..1 each way, centre 0.5) for an aspect ratio width / height.
float PhotoVignette(float amount, float u, float v, float aspect);
// True where a pixel is under the letterbox bars (u, v 0..1 over a width x height picture).
bool PhotoUnderBars(float target_aspect, float u, float v, float width, float height);

}  // namespace nr
