// Parallax occlusion mapping on the game's own pixel shaders (docs/lighting.md, native_parallax). The hook moves a
// shader's texture coordinates at the top of main, before its first fetch: the base texture's (fetch constant 0 at
// TEXCOORD0.xy) by a march through a height taken from that texture's brightness, and the normal map's (fetch constant 1)
// by the same move on screen when it reads TEXCOORD1.xy. Everything after is the game's own shading. The view comes
// from the KKLighting buffer's mapping (kk_lighting.hlsli: kl_SsrRect, kl_SsrDepth, kl_SsrScreen).
#pragma once

#include <string>

namespace kkshaders {

// The hook's HLSL (kp_Parallax); part of the hooked shaders' input hash.
const std::string& parallaxText();

// Puts the hook into a translated pixel shader. False, with why, when the shader does not fetch its base texture at
// TEXCOORD0.xy first: it is then not hooked.
bool applyParallax(std::string& hlsl, std::string& error);

}  // namespace kkshaders
