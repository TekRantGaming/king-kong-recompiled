// XeGTAO (Intel, MIT, thirdparty/XeGTAO, included unchanged) for the native renderer's
// ambient occlusion: docs/ao.md. Compiled to SPIR-V and DXIL by compile_shaders.sh / .ps1.
//
// The scene's depth target is read as it is (the stored 0..1 depth); everything is sized from
// the host textures, so it follows --native_render_scale. A working image of padded_w x padded_h
// pixels (multiples of 16, so no pass writes outside its images) covers the picture: pixel i is
// host pixel origin + step * i (step 1 full resolution, 2 half). The camera's projection and the
// viewport give XeGTAO's DepthUnpackConsts and NDCToView constants (backend/ao_camera.h).
//
//   cs_prepare     the picture's stored depth into the working image (edges replicated)
//   cs_prefilter   XeGTAO_PrefilterDepths16x16: view-space depth and its four smaller mips
//   cs_normals     XeGTAO_ComputeViewspaceNormal: normals rebuilt from the depth (the game is a
//                  forward renderer, it has no normal buffer)
//   cs_main        XeGTAO_MainPass, 3 slices of 3 steps each ("high"), no temporal noise
//   cs_denoise     XeGTAO_Denoise, twice (the last one scales the term back)
//   cs_output      the occlusion per host pixel of the picture (R32F, 1 = none): the working
//                  image upsampled by depth at half resolution, the sky left alone
//   vs_main + ps_apply           the scene colour times the occlusion (multiplicative blend) or,
//                                with ao_mode 2, the occlusion itself
//   vs_main + ps_apply_resolve   a copy of the scene colour with the occlusion under the game's
//                                fog, for the resolve moment (as ambient_occlusion.hlsl does)
//
// Bindings (NVRHI's Vulkan offsets: t at 0, s at 128, b at 256, u at 384): b0 the constants
// below, t0..t2 inputs, u0..u4 outputs, s0 a point clamp sampler.

#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS 1
#define VA_SATURATE saturate
// The edges image is R32_FLOAT here (a format every Vulkan device stores to); XeGTAO declares it
// unorm float, which DXIL would bind with another return type.
#define unorm
#include "../../thirdparty/XeGTAO/XeGTAO.h"

cbuffer GTAOConstantBuffer : register(b0)
{
    GTAOConstants g_GTAOConsts;
    int4 kk_Source;     // xy: the picture's origin in the target (host pixels), zw: its size
    uint4 kk_Working;   // xy: working size covering the picture, zw: padded size
    float4 kk_Params;   // x: step (1 or 2), y: stored depth from which a pixel is sky, z: mode (1 on,
                        // 2 AO only), w: 1 when the colour target of ps_apply_resolve holds the picture
    float4 kk_FogNorm;  // the game's fog (ps_apply_resolve): xyz g_vFogNormalizing, w 1 with a fog pass
    float4 kk_FogPar;   // x near over distance, y density, z inverse distance
    float4 kk_FogCol;   // rgb fog colour
    float4 kk_Fade;     // x: view depth (world units) at which the occlusion has faded out linearly, 0: no fade
    float4 kk_Reprojection[4];
    float4 kk_Viewport;
    float4 kk_DepthScale;
    float4 kk_Temporal; // history weight, previous depth unpack mul/add, sample quality
};

#include "../../thirdparty/XeGTAO/XeGTAO.hlsli"

SamplerState g_samplerPointClamp : register(s0);

// cs_prepare
Texture2D<float> g_srcSceneDepth : register(t0);
RWTexture2D<float> g_outRawDepth : register(u0);

// cs_prefilter, cs_normals
Texture2D<float> g_srcRawDepth : register(t0);
RWTexture2D<lpfloat> g_outWorkingDepthMIP0 : register(u0);
RWTexture2D<lpfloat> g_outWorkingDepthMIP1 : register(u1);
RWTexture2D<lpfloat> g_outWorkingDepthMIP2 : register(u2);
RWTexture2D<lpfloat> g_outWorkingDepthMIP3 : register(u3);
RWTexture2D<lpfloat> g_outWorkingDepthMIP4 : register(u4);
RWTexture2D<uint> g_outNormalmap : register(u0);

// cs_main
Texture2D<lpfloat> g_srcWorkingDepth : register(t0);
Texture2D<uint> g_srcNormalmap : register(t1);
RWTexture2D<uint> g_outWorkingAOTerm : register(u0);
RWTexture2D<float> g_outWorkingEdges : register(u1);

// cs_denoise
Texture2D<uint> g_srcWorkingAOTerm : register(t0);
Texture2D<lpfloat> g_srcWorkingEdges : register(t1);
RWTexture2D<uint> g_outFinalAOTerm : register(u0);

// cs_output
Texture2D<uint> g_srcFinalAOTerm : register(t0);
Texture2D<float> g_srcOutputDepth : register(t1);   // the scene's stored depth (the picture's target)
Texture2D<float> g_srcViewDepth : register(t2);     // the working view-space depth (mip 0)
RWTexture2D<float> g_outOcclusion : register(u0);

// ps_apply, ps_apply_resolve
Texture2D<float4> g_srcColor : register(t0);
Texture2D<float> g_srcOcclusion : register(t1);
Texture2D<float> g_srcApplyDepth : register(t2);

// The sample's own normal decoding and noise (thirdparty/XeGTAO/vaGTAO.hlsl, same licence).
lpfloat3 LoadNormal(int2 pos)
{
    uint packedInput = g_srcNormalmap.Load(int3(pos, 0)).x;
    float3 unpackedOutput = XeGTAO_R11G11B10_UNORM_to_FLOAT3(packedInput);
    return (lpfloat3)normalize(unpackedOutput * 2.0.xxx - 1.0.xxx);
}

lpfloat2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)
{
    // Hilbert curve driving R2; rotate only when a local history or a temporal SDK can smooth it.
    uint index = HilbertIndex(pixCoord.x, pixCoord.y);
    index += 288 * (temporalIndex % 64);
    return lpfloat2(frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114)));
}

[numthreads(8, 8, 1)]
void cs_prepare(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= kk_Working.zw))
        return;
    // Past the picture the edge repeats: the occlusion there is the edge's surface continued.
    uint2 w = min(id, kk_Working.xy - 1);
    int2 p = kk_Source.xy + min(int2(w * uint(kk_Params.x)), kk_Source.zw - 1);
    g_outRawDepth[id] = g_srcSceneDepth.Load(int3(p, 0));
}

[numthreads(8, 8, 1)]
void cs_prefilter(uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID)
{
    XeGTAO_PrefilterDepths16x16(dispatchThreadID, groupThreadID, g_GTAOConsts, g_srcRawDepth, g_samplerPointClamp,
        g_outWorkingDepthMIP0, g_outWorkingDepthMIP1, g_outWorkingDepthMIP2, g_outWorkingDepthMIP3, g_outWorkingDepthMIP4);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void cs_normals(uint2 pixCoord : SV_DispatchThreadID)
{
    float3 viewspaceNormal = XeGTAO_ComputeViewspaceNormal(pixCoord, g_GTAOConsts, g_srcRawDepth, g_samplerPointClamp);
    g_outNormalmap[pixCoord] = XeGTAO_FLOAT3_to_R11G11B10_UNORM(saturate(viewspaceNormal * 0.5 + 0.5));
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void cs_main(uint2 pixCoord : SV_DispatchThreadID)
{
    const lpfloat2 noise = SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex);
    const lpfloat3 normal = LoadNormal(pixCoord);
    XeGTAO_MainPass(pixCoord, 3, 3, noise, normal,
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges);
}

// Separate kernels keep Low/Medium from inheriting High's register and code footprint.
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void cs_main_low(uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 2, 2, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex), LoadNormal(pixCoord),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void cs_main_high(uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 9, 3, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex), LoadNormal(pixCoord),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void cs_denoise(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    XeGTAO_Denoise(dispatchThreadID * uint2(2, 1), g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges,
        g_samplerPointClamp, g_outFinalAOTerm, false);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void cs_denoise_last(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    XeGTAO_Denoise(dispatchThreadID * uint2(2, 1), g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges,
        g_samplerPointClamp, g_outFinalAOTerm, true);
}

float ViewDepth(float stored)
{
    return XeGTAO_ScreenSpaceToViewSpaceDepth(stored, g_GTAOConsts);
}

// Local temporal denoise without an upscaler. The same camera reprojection vote used by
// the SDK motion pass maps depth into the previous picture. Reject disocclusions and
// clamp to this frame's neighbourhood to bound trails on hands and animated geometry.
Texture2D<float> g_srcSpatialAO : register(t0);
Texture2D<float> g_srcTemporalSceneDepth : register(t1);
Texture2D<float> g_srcHistoryAO : register(t2);
Texture2D<float> g_srcHistoryDepth : register(t3);
RWTexture2D<float> g_outTemporalAO : register(u0);
RWTexture2D<float> g_outTemporalDepth : register(u1);

[numthreads(8, 8, 1)]
void cs_temporal(uint2 id : SV_DispatchThreadID)
{
    if (any(int2(id) >= kk_Source.zw)) return;
    const int2 pixel = int2(id);
    const float stored = g_srcTemporalSceneDepth.Load(int3(kk_Source.xy + pixel, 0));
    const float current = g_srcSpatialAO.Load(int3(pixel, 0));
    g_outTemporalDepth[id] = stored;
    float result = current;
    if (kk_Temporal.x > 0 && stored < kk_Params.y)
    {
        const float2 guest = (float2(kk_Source.xy + pixel) + 0.5) / kk_DepthScale.zw;
        const float4 clip = float4((guest - kk_Viewport.yw) / kk_Viewport.xz,
            (stored - kk_DepthScale.y) / kk_DepthScale.x, 1);
        const float4 prev = float4(dot(kk_Reprojection[0], clip), dot(kk_Reprojection[1], clip),
            dot(kk_Reprojection[2], clip), dot(kk_Reprojection[3], clip));
        const float2 prevPixel = (prev.xy / max(prev.w, 1e-6) * kk_Viewport.xz + kk_Viewport.yw) * kk_DepthScale.zw
            - float2(kk_Source.xy) - 0.5;
        // Large camera jumps are cuts. There is no history outside the previous picture.
        if (prev.w > 1e-6 && all(prevPixel >= 0) && all(prevPixel <= float2(kk_Source.zw - 1)) &&
            all(abs(prevPixel - float2(pixel)) < float2(kk_Source.zw) * 0.25))
        {
            const float expected = prev.z / prev.w * kk_DepthScale.x + kk_DepthScale.y;
            const float expectedZ = kk_Temporal.y / (kk_Temporal.z - expected);
            float sum = 0, weight = 0;
            const int2 base = int2(floor(prevPixel));
            const float2 f = frac(prevPixel);
            [unroll] for (int j = 0; j < 4; ++j)
            {
                const int2 o = int2(j & 1, j >> 1);
                const int2 q = min(base + o, kk_Source.zw - 1);
                const float depth = g_srcHistoryDepth.Load(int3(q, 0));
                const float z = kk_Temporal.y / (kk_Temporal.z - depth);
                const float w = (o.x ? f.x : 1-f.x) * (o.y ? f.y : 1-f.y);
                if (expectedZ > 0 && z > 0 && abs(z - expectedZ) < max(0.002, expectedZ * 0.01))
                {
                    sum += g_srcHistoryAO.Load(int3(q, 0)) * w;
                    weight += w;
                }
            }
            if (weight > 0.5)
            {
                float lo = current, hi = current;
                [unroll] for (int y = -1; y <= 1; ++y)
                    [unroll] for (int x = -1; x <= 1; ++x)
                    {
                        const int2 q = clamp(pixel + int2(x,y), int2(0,0), kk_Source.zw - 1);
                        const float value = g_srcSpatialAO.Load(int3(q, 0));
                        lo = min(lo, value); hi = max(hi, value);
                    }
                result = lerp(current, clamp(sum / weight, lo, hi), kk_Temporal.x);
            }
        }
    }
    g_outTemporalAO[id] = saturate(result);
}

[numthreads(8, 8, 1)]
void cs_output(uint2 id : SV_DispatchThreadID)
{
    if (any(int2(id) >= kk_Source.zw))
        return;
    float stored = g_srcOutputDepth.Load(int3(kk_Source.xy + int2(id), 0));
    float ao = 1.0;
    if (stored < kk_Params.y)
    {
        if (kk_Params.x < 1.5)
        {
            ao = float(g_srcFinalAOTerm.Load(int3(id, 0)).x) / 255.0;
        }
        else
        {
            // Half resolution: working pixel i is host pixel 2i, so host pixel p sits at p / 2 in
            // working pixels. The four nearest working pixels, bilinear and weighted by how close
            // their depth is to this pixel's.
            float z = ViewDepth(stored);
            float2 f = float2(id) * 0.5;
            int2 base = int2(floor(f));
            float2 t = f - float2(base);
            int2 last = int2(kk_Working.xy) - 1;
            float total = 0.0, weight = 0.0;
            [unroll]
            for (int j = 0; j < 4; ++j)
            {
                int2 o = int2(j & 1, j >> 1);
                int2 q = clamp(base + o, int2(0, 0), last);
                float bilinear = (o.x ? t.x : 1.0 - t.x) * (o.y ? t.y : 1.0 - t.y);
                float dz = abs(g_srcViewDepth.Load(int3(q, 0)).x - z) / max(z, 1e-6);
                float w = (bilinear + 1e-3) / (1e-3 + dz * 50.0);
                total += float(g_srcFinalAOTerm.Load(int3(q, 0)).x) / 255.0 * w;
                weight += w;
            }
            ao = weight > 0.0 ? total / weight : 1.0;
        }
        // The legacy method's distance fade (distant terrain under the game's own per-pixel fog).
        if (kk_Fade.x > 0.0)
            ao = lerp(1.0, ao, saturate(1.0 - ViewDepth(stored) / kk_Fade.x));
    }
    g_outOcclusion[id] = saturate(ao);
}

// One triangle over the viewport (the viewport is the picture).
float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float Occlusion(int2 p)
{
    int2 q = p - kk_Source.xy;
    if (any(q < 0) || any(q >= kk_Source.zw))
        return 1.0;
    return g_srcOcclusion.Load(int3(q, 0)).x;
}

// Into the scene's colour target: blended as destination times source (ao_mode 1) or replacing
// it (ao_mode 2, the occlusion only). The alpha channel is not written.
float4 ps_apply(float4 pos : SV_Position) : SV_Target0
{
    float ao = Occlusion(int2(pos.xy));
    return float4(ao, ao, ao, 1.0);
}

// The game's fog over this pixel (ambient_occlusion.hlsl's FogAmount).
float FogAmount(float z)
{
    if (kk_FogNorm.w <= 0.0)
        return 0.0;
    float inverse_depth = kk_FogNorm.x * min(z, kk_FogNorm.z) - kk_FogNorm.y;
    if (inverse_depth <= 0.0)
        return 1.0;
    float fog = saturate((1.0 / inverse_depth) * kk_FogPar.z - kk_FogPar.x);
    return saturate(fog * kk_FogPar.y);
}

// A copy of the scene colour for the resolve: colour * ao + fog colour * fog * (1 - ao), so only
// the scene part under the fog is darkened. Pixels outside the picture pass through.
float4 ps_apply_resolve(float4 pos : SV_Position) : SV_Target0
{
    int2 p = int2(pos.xy);
    float4 color = g_srcColor.Load(int3(p, 0));
    int2 q = p - kk_Source.xy;
    if (any(q < 0) || any(q >= kk_Source.zw))
        return color;
    float ao = g_srcOcclusion.Load(int3(q, 0)).x;
    float fog = FogAmount(g_srcApplyDepth.Load(int3(p, 0)).x) * (1.0 - ao);
    if (kk_Params.z >= 1.5)
    {
        float g = saturate(ao + fog);
        return float4(g, g, g, color.a);
    }
    return float4(saturate(color.rgb * ao + saturate(kk_FogCol.rgb) * fog), color.a);
}
