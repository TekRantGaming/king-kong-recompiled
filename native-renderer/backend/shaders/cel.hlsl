// Cel shading (docs/cel.md): one full-screen pass over the scene's picture at the end of its opaque
// pass, into the scene colour target from a copy of it. Compiled to SPIR-V and DXIL by
// compile_shaders.sh / .ps1.
//
//   vs_main + ps_cel   ink lines where a neighbour lies well behind this pixel's own surface (a
//                      silhouette: the line is drawn on the nearer object's side, so it also shows
//                      against the sky) or where the normals fold sharply (a crease), faded with
//                      distance; and the luminance in a few flat bands, the hue kept
//                      (backend/cel_config.cpp CelBandLuma is the same function on the CPU).
//
// Depth: the scene's stored depth at host pixels (crisp lines at any render scale). Normals: the
// working normals of a Gtao::Prepare over the same picture (the AO's, smoothed, when it ran this
// frame; else the cel shading's own). The sky (stored depth at the far plane) is left as it is.
//
// Bindings (NVRHI's Vulkan offsets: t at 0, s at 128, b at 256, u at 384): b0 the constants, t0 the
// scene colour copy, t1 the scene depth, t2 the normals.

#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS 1
#define VA_SATURATE saturate
#define unorm
#include "../../thirdparty/XeGTAO/XeGTAO.h"

cbuffer CelConstants : register(b0)
{
    GTAOConstants g_GTAOConsts;  // as the depth was prepared (gtao.hlsl's first part)
    int4 kk_Source;     // xy: the picture's origin in the target (host pixels), zw: its size
    uint4 kk_Working;   // xy: working size covering the picture, zw: padded size
    float4 kk_Params;   // x: step of the normals (1 or 2), y: stored depth from which a pixel is sky
    float4 kk_Outline;  // x: depth line radius (host pixels, >= 1), y: line opacity (thin lines), z: darkness,
                        // w: crease radius (host pixels)
    float4 kk_Lines;    // x, y: relative depth step where a line starts / is full; z, w: cos of the crease
                        // angle where a line starts / is full
    float4 kk_Fade;     // x, y: view depth where lines start to fade / are gone; z: crease weight; w: debug view
    float4 kk_Bands;    // x: bands (0: none), y: strength, z: softness (share of a band), w: log2 steps below white
    float4 kk_Levels;   // x: a band's level in it, y: the top band's
};

#include "../../thirdparty/XeGTAO/XeGTAO.hlsli"

Texture2D<float4> g_srcSceneCopy : register(t0);
Texture2D<float> g_srcSceneDepth : register(t1);
Texture2D<uint> g_srcNormals : register(t2);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// --------------------------------------------------------------------- bands

float BandLevel(float band, float n)
{
    float at = band >= n - 1.0 ? kk_Levels.y : kk_Levels.x;
    return exp2(((band + at) / n - 1.0) * kk_Bands.w);
}

// backend/cel_config.cpp CelBandLuma.
float CelBandLuma(float y, float n)
{
    float s = log2(max(y, 1e-6)) / kk_Bands.w + 1.0;
    if (s <= 0.0 || s >= 1.0)
        return y;
    float t = s * n;
    float i = floor(t), f = t - i;
    float blend = i < n - 1.0 ? smoothstep(1.0 - kk_Bands.z, 1.0, f) : 0.0;
    float level = lerp(BandLevel(i, n), BandLevel(i + 1.0, n), blend);
    float fromDark = smoothstep(0.0, kk_Bands.z / n, s);
    return lerp(y, level, fromDark);
}

// backend/cel_config.cpp CelBandColour.
float3 CelBandColour(float3 c)
{
    float y = Luma(c);
    float scale = y > 1e-5 ? CelBandLuma(y, kk_Bands.x) / y : 1.0;
    float3 b = c * scale;
    float top = max(b.r, max(b.g, b.b)), was = max(max(c.r, max(c.g, c.b)), 1.0);
    if (top > was)
        b *= was / top;
    return lerp(c, b, saturate(kk_Bands.y));
}

// --------------------------------------------------------------------- lines

// Inside the picture (taps outside it read its edge).
int2 Clamp(int2 p)
{
    return clamp(p, kk_Source.xy, kk_Source.xy + kk_Source.zw - 1);
}

float StoredDepth(int2 p)
{
    return g_srcSceneDepth.Load(int3(Clamp(p), 0)).x;
}

// 1 / view depth: linear across the screen on a flat surface (a plane's stored depth is too).
float InverseZ(float stored)
{
    return (g_GTAOConsts.DepthUnpackConsts.y - stored) / g_GTAOConsts.DepthUnpackConsts.x;
}

float3 NormalAt(int2 p)
{
    int2 w = clamp((Clamp(p) - kk_Source.xy) / int(kk_Params.x), int2(0, 0), int2(kk_Working.xy) - 1);
    uint packed = g_srcNormals.Load(int3(w, 0)).x;
    return normalize(XeGTAO_R11G11B10_UNORM_to_FLOAT3(packed) * 2.0 - 1.0);
}

static const float2 kDirections[8] = {
    float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
    float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071)};

// How much of a line this pixel (on a surface, inverse depth w) gets: the strongest of its taps.
float2 Lines(int2 p, float w)
{
    // This surface's slope in inverse depth, from the nearer-matching side in each axis (so a step
    // right next to the pixel is not taken for the slope).
    float wl = InverseZ(StoredDepth(p + int2(-1, 0))), wr = InverseZ(StoredDepth(p + int2(1, 0)));
    float wu = InverseZ(StoredDepth(p + int2(0, -1))), wd = InverseZ(StoredDepth(p + int2(0, 1)));
    float gx = abs(wr - w) < abs(w - wl) ? wr - w : w - wl;
    float gy = abs(wd - w) < abs(w - wu) ? wd - w : w - wu;
    float sky = kk_Params.y;
    float depthLine = 0.0;
    float radius = kk_Outline.x;
    [unroll]
    for (int ring = 0; ring < 2; ++ring)
    {
        float r = ring == 0 ? radius : max(1.0, floor(radius * 0.5 + 0.5));
        if (ring == 1 && radius < 2.0)
            break;
        [unroll]
        for (int i = 0; i < 8; ++i)
        {
            int2 o = int2(round(kDirections[i] * r));
            float stored = StoredDepth(p + o);
            if (stored >= sky)
            {
                depthLine = 1.0;  // the sky behind: a silhouette
                continue;
            }
            float predicted = w + gx * float(o.x) + gy * float(o.y);
            if (predicted <= 0.0)
                continue;  // this surface runs off to the horizon that way: nothing is behind it
            float behind = predicted / max(InverseZ(stored), 1e-9) - 1.0;  // view depth over predicted, minus 1
            depthLine = max(depthLine, smoothstep(kk_Lines.x, kk_Lines.y, behind));
        }
    }
    float creaseLine = 0.0;
    if (kk_Fade.z > 0.0)
    {
        // The normals around: this pixel's against each tap's, and each tap's against the opposite tap's
        // (the rebuilt normal of a pixel right on a crease is a blend of both sides; the two taps across
        // it are not).
        float3 n = NormalAt(p);
        float3 nt[8];
        bool ok[8];
        float rc = kk_Outline.w;
        float lowest = 1.0;
        [unroll]
        for (int i = 0; i < 8; ++i)
        {
            int2 o = int2(round(kDirections[i] * rc));
            float stored = StoredDepth(p + o);
            // Taps on the sky or across a real depth step (another object, nearer or farther) belong to
            // the silhouette lines. A crease bends away from this pixel's plane only a little that close.
            float predicted = w + gx * float(o.x) + gy * float(o.y);
            float step = abs(predicted / max(InverseZ(stored), 1e-9) - 1.0);
            ok[i] = stored < sky && predicted > 0.0 && step <= kk_Lines.y;
            nt[i] = NormalAt(p + o);
            if (ok[i])
                lowest = min(lowest, dot(n, nt[i]));
        }
        // Opposite taps (kDirections: 0-1, 2-3, 4-7, 5-6).
        if (ok[0] && ok[1]) lowest = min(lowest, dot(nt[0], nt[1]));
        if (ok[2] && ok[3]) lowest = min(lowest, dot(nt[2], nt[3]));
        if (ok[4] && ok[7]) lowest = min(lowest, dot(nt[4], nt[7]));
        if (ok[5] && ok[6]) lowest = min(lowest, dot(nt[5], nt[6]));
        creaseLine = 1.0 - smoothstep(kk_Lines.w, kk_Lines.z, lowest);
    }
    return float2(depthLine, creaseLine * kk_Fade.z);
}

float4 ps_cel(float4 pos : SV_Position) : SV_Target0
{
    int2 p = int2(pos.xy);
    float4 colour = g_srcSceneCopy.Load(int3(p, 0));
    int2 q = p - kk_Source.xy;
    float stored = g_srcSceneDepth.Load(int3(p, 0)).x;
    bool debugView = kk_Fade.w > 0.5;
    if (any(q < 0) || any(q >= kk_Source.zw) || stored >= kk_Params.y)
        return debugView ? float4(1.0, 1.0, 1.0, colour.a) : colour;  // outside the picture, or the sky
    float w = InverseZ(stored);
    float z = 1.0 / max(w, 1e-9);
    float ink = 0.0;
    if (kk_Outline.z > 0.0 || debugView)
    {
        float fade = 1.0 - smoothstep(kk_Fade.x, kk_Fade.y, z);
        if (fade > 0.0)
        {
            float2 both = Lines(p, w) * fade * kk_Outline.y;
            ink = max(both.x, both.y);
            if (kk_Fade.w > 1.5)
                return float4(1.0 - both.x, 1.0 - both.y, 1.0, colour.a);  // debug 2: depth lines cyan, creases magenta
        }
    }
    if (debugView)
        return float4((1.0 - ink).xxx, colour.a);
    float3 rgb = colour.rgb;
    if (kk_Bands.x >= 2.0 && kk_Bands.y > 0.0)
        rgb = CelBandColour(rgb);
    rgb *= 1.0 - ink * kk_Outline.z;
    return float4(rgb, colour.a);
}
