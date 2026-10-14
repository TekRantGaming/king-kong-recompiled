// The volumetric light (docs/lighting.md, "Light shafts"): sunlight scattered by the game's fog, added
// right after the game's fog pass.
//
// The game's fog pass adds its fog colour by a fog amount that grows with the distance (psaftereffects,
// g_vFogParams), as if all the air in front of a surface were lit the same. The screen-space shaft pass adds to it:
//
//  God rays from the bright sky (screen space): the sky and the fully fogged distance, where they are
//     bright, are blurred along lines towards the centre of the bright sky (the brightness-weighted
//     centroid, found on the GPU), so the light streams past the silhouettes in front of it (foliage,
//     rocks, Kong) into the air. Weaker when the shadow map's sun is behind the camera.
//
// The rays scale with the game's fog: no fog, nothing (a clear scene stays clear); denser fog, more light.
// The colour target is copied first and written whole. Light is only added.
//
//   cs_mask        the bright sky and fogged distance at a quarter of the resolution (rgb), its weight (a)
//   cs_centroid    one group: the mask's brightness-weighted centroid into a 1x1 image
//   cs_radial      the god rays at a quarter of the resolution: the mask summed along the line to the centroid
//   vs_main + ps_volumetric   a full-screen triangle into the scene's colour target: colour + scattered light
//
// Bindings (NVRHI's Vulkan offsets: t at 0, s at 128, b at 256, u at 384): b0 the constants, t0 the
// stored depth, t1 the shadow map, t2 the scene colour (a copy), t3 the mask or the rays, t4 the
// centroid, u0 the output, s0 a point clamp sampler, s1 a linear clamp sampler.

cbuffer VolumetricConstants : register(b0)
{
    float4 g_Light[4];   // psshadowquad's light 0 rows (c0..c3): light clip = dot(row, (x d, y d, d, 1))
    float4 g_Shadow;     // x: the game's depth bias, y: steps, z: share of the fog a shadowed ray loses, w: strength
    float4 g_FogColor;   // rgb: g_vFogColor
    float4 g_FogParams;  // x: g_fFogNearOverDistance, y: g_fFogDensity, z: g_fFogInvDistance, w: the depth clamp
    float4 g_FogNorm;    // xy: 1 / view z = x * stored depth - y (g_vFogNormalizing); z: 1 with a shadow map
    float4 g_Screen;     // xy: 1 / the colour target's size (host pixels), zw: its size
    float4 g_Sun;        // xyz: view-space direction to the sun (0: not known), w: the phase function's g
    float4 g_Camera;     // xy: the projection's x and y scale (view x = ndc x * z / x), z: god rays' strength, w: their samples
    float4 g_Rays;       // xy: the quarter-resolution images' size, z: the sky's brightness threshold, w: decay
};

Texture2D<float4> t_depth : register(t0);
Texture2D<float4> t_shadow : register(t1);
Texture2D<float4> t_color : register(t2);
Texture2D<float4> t_rays : register(t3);
Texture2D<float4> t_centroid : register(t4);
RWTexture2D<float4> u_out : register(u0);
SamplerState s_point : register(s0);
SamplerState s_linear : register(s1);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float Noise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// The fog amount the game gives a view depth (psaftereffects' fog pass, without its pitch attenuation).
float FogAmount(float z)
{
    return saturate(saturate(z * g_FogParams.z - g_FogParams.x) * g_FogParams.y);
}

// The view depth of a stored depth, as the fog pass reads it (clamped like it, so the sky is far fog).
float ViewDepth(float stored)
{
    float d = min(stored, g_FogParams.w);
    float invZ = g_FogNorm.x * d - g_FogNorm.y;
    return invZ > 1e-6 ? 1.0 / invZ : 1e6;
}

// Henyey-Greenstein, normalised so that its mean over the sphere is 1.
float Phase(float cosine, float g)
{
    float d = 1.0 + g * g - 2.0 * g * cosine;
    return (1.0 - g * g) / max(d * sqrt(d), 1e-4);
}

// ---------------------------------------------------------------- god rays

// The bright sky (the stored depth at the fog's clamp or beyond) and the fully fogged distance, where
// they are brighter than the threshold: what the rays stream from.
[numthreads(8, 8, 1)]
void cs_mask(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Rays.xy)))
        return;
    float3 sum = 0.0;
    [unroll] for (uint j = 0; j < 2; j++)
    {
        [unroll] for (uint i = 0; i < 2; i++)
        {
            float2 uv = (float2(id) + 0.25 + 0.5 * float2(i, j)) / g_Rays.xy;
            float stored = t_depth.SampleLevel(s_point, uv, 0.0).x;
            float3 c = t_color.SampleLevel(s_linear, uv, 0.0).rgb;
            float open = stored >= g_FogParams.w ? 1.0 : saturate((FogAmount(ViewDepth(stored)) - 0.9) * 10.0);
            float bright = smoothstep(g_Rays.z, min(g_Rays.z + 0.4, 1.0), Luma(c));
            sum += c * open * bright;
        }
    }
    sum *= 0.25;
    u_out[id] = float4(sum, Luma(sum));
}

groupshared float4 gs_sum[256];

// One group of 16 x 16 threads over the whole mask: the centroid of its brightness squared (the
// brightest part of the sky leads), and the mean weight.
[numthreads(16, 16, 1)]
void cs_centroid(uint2 tid : SV_GroupThreadID)
{
    uint index = tid.y * 16 + tid.x;
    // A coarse grid locates the source; scanning every 4K mask texel in a
    // single workgroup serializes hundreds of thousands of texture reads.
    uint2 size = min(uint2(g_Rays.xy), uint2(64, 64));
    float4 acc = 0.0;
    for (uint y = tid.y; y < size.y; y += 16)
    {
        for (uint x = tid.x; x < size.x; x += 16)
        {
            uint2 q = min(uint2((float2(x, y) + 0.5) / float2(size) * g_Rays.xy), uint2(g_Rays.xy) - 1);
            float w = t_rays.Load(int3(q, 0)).a;
            w *= w;
            acc += float4((float2(x, y) + 0.5) / float2(size) * w, w, 0.0);
        }
    }
    gs_sum[index] = acc;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1)
    {
        if (index < s)
            gs_sum[index] += gs_sum[index + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0)
    {
        float4 t = gs_sum[0];
        float2 centre = t.z > 1e-8 ? t.xy / t.z : float2(0.5, 0.0);
        u_out[uint2(0, 0)] = float4(centre, t.z / max(float(size.x * size.y), 1.0), 0.0);
    }
}

// The mask summed along the line from the texel towards the centroid, each sample weaker the further
// it is (the decay), jittered per texel (the quarter resolution's bilinear upsampling hides it).
[numthreads(8, 8, 1)]
void cs_radial(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Rays.xy)))
        return;
    float4 centreData = t_centroid.Load(int3(0, 0, 0));
    if (!(centreData.z > 1e-7))
    {
        u_out[id] = 0.0;
        return;
    }
    float2 uv = (float2(id) + 0.5) / g_Rays.xy;
    uint samples = uint(g_Camera.w);
    float2 stepUv = (centreData.xy - uv) / float(samples);
    float jitter = Noise(float2(id));
    float3 acc = 0.0;
    float weight = 1.0;
    float total = 0.0;
    [loop] for (uint i = 0; i < samples; i++)
    {
        float2 p = uv + stepUv * (float(i) + jitter);
        acc += t_rays.SampleLevel(s_linear, p, 0.0).rgb * weight;
        total += weight;
        weight *= g_Rays.w;
    }
    u_out[id] = float4(acc / max(total, 1e-4), 1.0);
}

// ------------------------------------------------------------- composite

float4 ps_volumetric(float4 pos : SV_Position) : SV_Target0
{
    float2 uv = pos.xy * g_Screen.xy;
    float4 colour = t_color.Load(int3(pos.xy, 0));
    float stored = t_depth.SampleLevel(s_point, uv, 0.0).x;
    float zSurface = ViewDepth(stored);
    float fog = FogAmount(zSurface);
    if (fog < 1e-3)
        return colour;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 view = normalize(float3(ndc.x / g_Camera.x, ndc.y / g_Camera.y, 1.0));
    bool sunKnown = dot(g_Sun.xyz, g_Sun.xyz) > 0.5;
    float3 add = 0.0;

    // The god rays from the bright sky: more in the fogged distance, less on what is near.
    float3 rays = t_rays.SampleLevel(s_linear, uv, 0.0).rgb;
    float facing = 1.0;
    if (sunKnown)
        facing = 0.25 + 0.75 * saturate(g_Sun.z + 0.3);  // the sun behind the camera: faint rays
    // On the sky itself only a little (the sky keeps its look; the rays are in the air before things).
    float onSky = stored >= g_FogParams.w ? 0.3 : 1.0;
    add += rays * g_Camera.z * facing * onSky * fog;

    // Use remaining headroom to retain bright detail; this pass never subtracts light.
    return float4(colour.rgb + max(add, 0.0) * saturate(1.0 - colour.rgb), colour.a);
}
