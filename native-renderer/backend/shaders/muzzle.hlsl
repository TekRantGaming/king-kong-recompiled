// Muzzle flashes that cast light (docs/lighting.md): one full-screen pass over the scene's picture at the end of its
// opaque pass, into the scene colour target from a copy of it, only in frames with a flash. Compiled to SPIR-V and
// DXIL by compile_shaders.sh / .ps1.
//
// Each pixel's view position comes from the scene's stored depth, its normal from the working normals of a
// Gtao::Prepare (the AO's when it ran this frame). The picture holds surface colour times light, so the surface's
// colour is estimated from it: its hue, and a reflectance that does not fall to nothing in the dark, where a flash
// shows most. The texture comes back from how a pixel stands against the pixels around it, whatever the light on them,
// and the depth's normals only know the polygons, so a surface keeps its grain. Sky (stored depth at the far plane) is
// left as it is.
//
// Bindings (NVRHI's Vulkan offsets: t at 0, b at 256): b0 the constants, t0 the scene colour copy, t1 the scene depth,
// t2 the normals.

#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS 1
#define VA_SATURATE saturate
#define unorm
#include "../../thirdparty/XeGTAO/XeGTAO.h"

cbuffer MuzzleConstants : register(b0)
{
    GTAOConstants g_GTAOConsts;  // as the depth was prepared (gtao.hlsl's first part)
    int4 kk_Source;     // xy: the picture's origin in the target (host pixels), zw: its size
    uint4 kk_Working;   // xy: working size covering the picture, zw: padded size
    float4 kk_Params;   // x: step of the normals (1 or 2), y: stored depth from which a pixel is sky,
                        // z: the lights' radius (view units, metres), w: distance fall-off
    float4 kk_Colour;   // rgb: the light's colour, w: lights
    float4 kk_Surface;  // x, y: reflectance assumed in the dark / in bright light, z: wrap of the light around a
                        // surface, w: the floor that keeps the hue of a black pixel
    float4 kk_Light[4]; // xyz: view position, w: strength now
};

#include "../../thirdparty/XeGTAO/XeGTAO.hlsli"

Texture2D<float4> g_srcSceneCopy : register(t0);
Texture2D<float> g_srcSceneDepth : register(t1);
Texture2D<uint> g_srcNormals : register(t2);

// How far a pixel may stray from its surroundings: the grain of the surface, not its specks.
static const float kGrainMin = 0.6;
static const float kGrainMax = 1.5;
static const int2 kRing[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };

float Brightest(float3 c)
{
    return max(max(c.r, c.g), c.b);
}

float Grain(int2 p, float mine)
{
    int2 low = max(kk_Source.xy, int2(0, 0));
    int2 high = kk_Source.xy + kk_Source.zw - 1;
    int reach = max(kk_Source.w / 90, 2);
    float around = 0.0;
    for (int i = 0; i < 8; ++i)
        around += Brightest(g_srcSceneCopy.Load(int3(clamp(p + kRing[i] * reach, low, high), 0)).rgb);
    return clamp((mine + 0.03) / (around / 8.0 + 0.03), kGrainMin, kGrainMax);
}

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float4 ps_muzzle(float4 pos : SV_Position) : SV_Target0
{
    int2 p = int2(pos.xy);
    float4 colour = g_srcSceneCopy.Load(int3(p, 0));
    int2 q = p - kk_Source.xy;
    float stored = g_srcSceneDepth.Load(int3(p, 0)).x;
    if (any(q < 0) || any(q >= kk_Source.zw) || stored >= kk_Params.y)
        return colour;
    float z = 1.0 / max((g_GTAOConsts.DepthUnpackConsts.y - stored) / g_GTAOConsts.DepthUnpackConsts.x, 1e-9);
    float step = kk_Params.x;
    float2 uv = (float2(q) + 0.5) / step / float2(kk_Working.zw);
    float3 position = float3((g_GTAOConsts.NDCToViewMul * uv + g_GTAOConsts.NDCToViewAdd) * z, z);

    float r2 = kk_Params.z * kk_Params.z;
    float reach = 0.0;
    int count = int(kk_Colour.w);
    float3 normal = float3(0.0, 0.0, 0.0);
    bool haveNormal = false;
    for (int i = 0; i < count; ++i)
    {
        float3 toLight = kk_Light[i].xyz - position;
        float d2 = dot(toLight, toLight);
        if (d2 >= r2)
            continue;
        if (!haveNormal)
        {
            int2 w = clamp(q / int(step), int2(0, 0), int2(kk_Working.xy) - 1);
            uint packed = g_srcNormals.Load(int3(w, 0)).x;
            normal = normalize(XeGTAO_R11G11B10_UNORM_to_FLOAT3(packed) * 2.0 - 1.0);
            haveNormal = true;
        }
        float window = 1.0 - d2 / r2;
        float fall = window * window / (1.0 + d2 * kk_Params.w);
        float facing = saturate((dot(normal, toLight / sqrt(max(d2, 1e-6))) + kk_Surface.z) / (1.0 + kk_Surface.z));
        reach += fall * facing * kk_Light[i].w;
    }
    if (reach <= 0.0)
        return colour;
    // The picture is about gamma 2: the light adds in linear.
    float3 lit = colour.rgb * colour.rgb;
    float top = max(max(lit.r, lit.g), lit.b);
    float3 hue = (lit + kk_Surface.w) / (top + kk_Surface.w);
    float3 surface = hue * lerp(kk_Surface.x, kk_Surface.y, sqrt(saturate(top))) * Grain(p, sqrt(top));
    colour.rgb = sqrt(lit + surface * kk_Colour.rgb * reach);
    return colour;
}
