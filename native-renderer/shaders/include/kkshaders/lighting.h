// King Kong native renderer: the lighting options' constants (docs/lighting.md), the C++ side of
// hlsl/kk_lighting.hlsli's KKLighting buffer.
//
//   b5 space0  LightingConstants  read by the lighting options' replacement shaders (the soft
//                                 shadows' psshadowquad) and by the water shaders translated with the
//                                 reflection hook; the binding exists only when a lighting option is on
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kkshaders {

struct alignas(16) LightingConstants {
    // Shadows (the psshadowquad replacements):
    //   x: the PCF kernel's radius in guest shadow-map texels (832 per side in the game)
    //   y: PCSS (ultra): the blocker search radius in guest texels
    //   z: PCSS: the penumbra's width per unit of light-space depth between receiver and blocker
    //   w: the shadow maps' scale (host texels per guest texel)
    float shadow[4];
    //   x: the receiver plane bias's limit (light-space depth per guest texel of offset)
    //   y: the smallest and z the largest PCSS kernel radius (guest texels); w: 1 for PCSS
    float shadowShape[4];
    // Water reflections (the reflection hook):
    //   x: the scene's colour at the fog pass (kk_Tex2D slot), y: its view depth with smaller mips
    //   (kk_Tex2D slot, R32F, each mip the nearest of the 2x2 below), z: the number of mips, w: 1 when both
    //   are this frame's
    uint32_t ssrTextures[4];
    // xy: where the picture starts in the target (host pixels), zw: its size (host pixels)
    float ssrRect[4];
    // x: a linear clamp sampler (kk_Sampler slot), y: the colour copy's mip count
    uint32_t ssrSampler[4];
    // xy: view z from the stored depth d (0-1): z = x / (y - d); z: the near plane; w: the far plane
    float ssrDepth[4];
    // From the picture's uv (0-1 over ssrRect) to view space: view xy = (uv * xy + zw) * view z
    float ssrScreen[4];
    // x: strength (0-1 of the game's reflection the hits replace), y: the longest ray (view units),
    // z: the thickness a surface is given (fraction of its view depth), w: the steps of the march
    float ssrParams[4];
    // x: the screen edge fade (fraction of the picture), y: colour mips per unit of the normal map's
    // slope (rough water), z: how far the normal map tilts the surface's normal, w: F0 for the ocean's fresnel
    float ssrMisc[4];
};
static_assert(sizeof(LightingConstants) == 9 * 16, "LightingConstants must match KKLighting");

// The water reflection hook of a pixel shader (docs/lighting.md, "Water reflections"): the screen-space
// reflection traced at the top of main (kl_WaterTrace), and where it goes.
struct WaterReflectionHook {
    uint32_t fetchMask = 0;  // texture fetch constants whose texel becomes the reflection at the hit (bit n)
    bool output = false;     // the colour output gets the reflection by fresnel (a shader without one)
    bool normalMap = false;  // the water's normal map is fetch constant 3 at TEXCOORD1.zw (pswater's)
    bool enabled() const { return fetchMask != 0 || output; }
};

// The game's water pixel shaders and their hook, by microcode hash (pswater with its planar reflection
// at fetch constant 1 and the environment cube at 2, the normal map at 3). A
// non-reflective water variant can use the output hook, leaving masks untouched.
WaterReflectionHook waterReflectionHookFor(uint64_t ucodeHash);
// Every shader of that list (the pack builder adds their hooked translations).
const uint64_t* waterReflectionShaders(size_t* count);
// hlsl/kk_lighting.hlsli: the KKLighting buffer and the hook's functions.
const std::string& lightingText();
// The hook as text (input hashes, logs).
std::string formatWaterReflectionHook(const WaterReflectionHook& hook);

}  // namespace kkshaders
