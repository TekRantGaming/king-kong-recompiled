// Sub-pixel jitter for the temporal upscalers (docs/upscalers.md, "Jitter").
//
// Every frame the scene's draws are shifted by a fraction of an input pixel, following a Halton (2, 3)
// sequence as AMD, NVIDIA and Intel all recommend, with a sequence as long as their guidance asks:
// 8 * (output / input)^2 phases (8 at native-resolution anti-aliasing, 18 at 1.5x, 32 at 2x, 72 at 3x).
//
// Convention (the same in FSR, DLSS and XeSS): the jitter (x, y) is how far the picture moves, in input
// pixels, x to the right and y down, within [-0.5, 0.5]. The renderer applies it in clip space through
// the draw constants' ndcOffset (position.xy += offset * w, after the guest's viewport transform), so
// no vertex shader changes: JitterNdcOffset gives that offset for a target of the input size.
#pragma once

#include <cstdint>

namespace nr {

// The radical inverse of `index` in `base` (index 1 is the first point of the sequence: 0.5 in base 2).
float Halton(uint32_t index, uint32_t base);

// The phase count the SDKs' guidance asks for: ceil(8 * ratio_x * ratio_y), at least 8.
uint32_t JitterPhaseCount(float ratio_x, float ratio_y);

struct Jitter {
  float x = 0, y = 0;  // input pixels, the picture's shift (+x right, +y down), within [-0.5, 0.5]
};

// The jitter of `frame` (any counter) in a sequence of `phases` phases: Halton(i, 2) - 0.5,
// Halton(i, 3) - 0.5 with i = frame % phases + 1.
Jitter JitterForFrame(uint64_t frame, uint32_t phases);

// The clip-space offset (added to ndcOffset.xy, which the vertex shaders multiply by w) that moves the
// picture by `j` input pixels on a target `width` x `height` host pixels big. Vulkan: the translated
// vertex shaders' y is negated after the offset (-fvk-invert-y), so the y offset has the other sign.
void JitterNdcOffset(const Jitter& j, uint32_t width, uint32_t height, bool vulkan, float& ox, float& oy);

// The texture LOD bias the SDKs recommend for the scene's textures at an input / output ratio:
// log2(input / output) (negative: sharper textures to match the output's pixel size).
float UpscaleMipBias(float ratio);

}  // namespace nr
