// Ambient occlusion on Jack's first-person hands (task 240/244, docs/ao.md): the pixel shader of a
// second draw of the hands' (and what he holds) colour pass, made after the occlusion went into the
// scene. The game puts their depth in the pre-pass (depth func ALWAYS) but their colour only after its
// fog pass, so the colour overwrote the darkened pixels. The second draw has the hands' own vertex
// shader and state, depth EQUAL without writes, and multiplies the target's colour by the occlusion
// (ao_mode 1: destination times source) or shows it (ao_mode 2). The alpha channel is left alone.
// Where the depth the occlusion was worked out from is not this surface's (the pre-pass's ALWAYS lets a
// farther part of the hands overwrite a nearer one), the occlusion belongs to a hidden surface: 1 there,
// faded in over a small average (no sharp edges or thin dark marks between such pixels and the rest).
//
// Built like the game's translated shaders (the game's pipeline layout, compile_shaders.ps1): the
// constants come in the pixel constants buffer, which the renderer writes for this draw alone:
//   c0: x asuint(the occlusion's kk_Tex2D index), yz occlusion pixels per target pixel,
//       w asuint(the kk_Tex2D index of the stored depth it was worked out from)
//   c1: xy the occlusion's origin in its target (host pixels of the scene before any upscale), zw its size
//   c2: x the depth image's step (occlusion pixels per depth pixel), y the largest depth difference,
//       z the radius of the (tent-weighted) average in occlusion pixels (0: the pixel alone),
//       w a developer view (KK_DEV_AO_HANDS_VIEW, ao_mode 2 only): 1 the pixel's own occlusion unchecked,
//         2 the same in green with red where the stored depth is not this surface's,
//         3 the occlusion used in green, red the share of the average from pixels of another surface
#define KK_PIXEL_SHADER 1
#include "../../shaders/hlsl/kk_common.hlsli"

// Whether occlusion pixel t was worked out from this surface's depth.
bool SameSurface(int2 t, int2 size, int step, float z)
{
    if (any(t < 0) || any(t >= size))
        return false;
    return abs(kk_Tex2D[asuint(kk_PC[0].w)].Load(int3(t / step, 0)).x - z) <= kk_PC[2].y;
}

float4 main(float4 pos : SV_Position) : SV_Target0
{
    const int2 q = int2(floor(pos.xy * kk_PC[0].yz)) - int2(kk_PC[1].xy);
    const int2 size = int2(kk_PC[1].zw);
    const int step = max(int(kk_PC[2].x), 1);
    const int radius = int(kk_PC[2].z);
    if (kk_PC[2].w > 0.5 && kk_PC[2].w < 2.5)
    {
        const float raw = all(q >= 0) && all(q < size) ? kk_Tex2D[asuint(kk_PC[0].x)].Load(int3(q, 0)).x : 1.0;
        if (kk_PC[2].w < 1.5)
            return float4(raw, raw, raw, 1.0);
        return SameSurface(q, size, step, pos.z) ? float4(0.0, raw, 0.0, 1.0) : float4(1.0, 0.0, 0.0, 1.0);
    }
    // A tent-weighted average of the occlusion around (radius c2.z, at most 3) in which every pixel whose stored
    // depth is not this surface's counts as no occlusion (1): the occlusion there belongs to a hidden part of
    // the hands. So do the pixels next to one (its 3 x 3): where a hidden part meets this surface the depths
    // agree within the tolerance for a pixel or so, and their occlusion is the hidden part's (near 0, a dark
    // dotted line along every edge of a hidden patch). Leaving the hidden pixels out with a hard 1 at their own
    // place (task 244) gave sharp edges between the 1 patches and the occluded rest, and thin strips of the
    // rest between patches showed as 1-pixel dark marks (task 306, most visible without a temporal upscaler):
    // the average fades over the radius instead. Wider (and smoother) when the occlusion is left noisy for a
    // temporal upscaler, which never sees the hands drawn after its point.
    const int r = clamp(radius, 0, 3);
    uint rows[9];  // bit i of row j: pixel q + (i, j) - (r + 1) is this surface
    for (int j = 0; j < 2 * r + 3; ++j)
    {
        uint bits = 0;
        for (int i = 0; i < 2 * r + 3; ++i)
        {
            if (SameSurface(q + int2(i, j) - (r + 1), size, step, pos.z))
                bits |= 1u << i;
        }
        rows[j] = bits;
    }
    float sum = 0.0, weight = 0.0, hidden = 0.0;
    for (int y = -r; y <= r; ++y)
    {
        for (int x = -r; x <= r; ++x)
        {
            const float w = float((r + 1 - abs(x)) * (r + 1 - abs(y)));
            const uint m = 7u << (x + r);
            const int j = y + r + 1;
            float a = 1.0;
            if ((rows[j - 1] & m) == m && (rows[j] & m) == m && (rows[j + 1] & m) == m)
                a = kk_Tex2D[asuint(kk_PC[0].x)].Load(int3(q + int2(x, y), 0)).x;
            else
                hidden += w;
            sum += a * w;
            weight += w;
        }
    }
    const float ao = sum / weight;
    if (kk_PC[2].w > 2.5)
        return float4(hidden / weight, ao, 0.0, 1.0);
    return float4(ao, ao, ao, 1.0);
}
