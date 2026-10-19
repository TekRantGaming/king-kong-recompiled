// Ambient occlusion on Jack's first-person hands (task 240/244, docs/ao.md): the pixel shader of a
// second draw of the hands' (and what he holds) colour pass, made after the occlusion went into the
// scene. The game puts their depth in the pre-pass (depth func ALWAYS) but their colour only after its
// fog pass, so the colour overwrote the darkened pixels. The second draw has the hands' own vertex
// shader and state, depth EQUAL without writes, and multiplies the target's colour by the occlusion
// (ao_mode 1: destination times source) or shows it (ao_mode 2). The alpha channel is left alone.
// Where the depth the occlusion was worked out from is not this surface's (the pre-pass's ALWAYS lets a
// farther part of the hands overwrite a nearer one), the occlusion belongs to a hidden surface: 1 there.
//
// Built like the game's translated shaders (the game's pipeline layout, compile_shaders.ps1): the
// constants come in the pixel constants buffer, which the renderer writes for this draw alone:
//   c0: x asuint(the occlusion's kk_Tex2D index), yz occlusion pixels per target pixel,
//       w asuint(the kk_Tex2D index of the stored depth it was worked out from)
//   c1: xy the occlusion's origin in its target (host pixels of the scene before any upscale), zw its size
//   c2: x the depth image's step (occlusion pixels per depth pixel), y the largest depth difference,
//       z the radius of the average in occlusion pixels (0: the pixel alone)
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
    // Next to a hidden surface's depth the occlusion is that step's (thin dark lines): none within the
    // average's reach of one (a cross of that radius, at least one pixel).
    if (!SameSurface(q, size, step, pos.z))
        return float4(1.0, 1.0, 1.0, 1.0);
    for (int r = 1; r <= max(radius, 1); ++r)
    {
        if (!SameSurface(q + int2(r, 0), size, step, pos.z) || !SameSurface(q - int2(r, 0), size, step, pos.z) ||
            !SameSurface(q + int2(0, r), size, step, pos.z) || !SameSurface(q - int2(0, r), size, step, pos.z))
            return float4(1.0, 1.0, 1.0, 1.0);
    }
    float sum = 0.0, count = 0.0;
    // The occlusion of the pixels around that are this surface (a small depth-aware average; wider when the
    // occlusion is left noisy for a temporal upscaler, which never sees the hands drawn after its point).
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            const int2 t = q + int2(x, y);
            if (!SameSurface(t, size, step, pos.z))
                continue;
            sum += kk_Tex2D[asuint(kk_PC[0].x)].Load(int3(t, 0)).x;
            count += 1.0;
        }
    }
    const float ao = count > 0.0 ? sum / count : 1.0;
    return float4(ao, ao, ao, 1.0);
}
