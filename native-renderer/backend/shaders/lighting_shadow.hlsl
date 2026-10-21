// The soft shadows (docs/lighting.md, "Shadows"): a replacement for the game's screen-space shadow
// pixel shader, psshadowquad, compiled once per variant of it (KL_LIGHTS lights, its constants at
// KL_PARAMS and KL_COLOR) and per quality (KL_PCSS). It computes what the game's shader computes:
// the pixel's view position from the view depth (fetch constant 0) and the quad's screen position
// (TEXCOORD0), its position in each light's shadow map (g_mCamToLightMatrix at c0, four rows per
// light), the depth less the game's bias (g_vShadowParams[i].z), and per light a channel of
// g_fShadowColor where the shadow map is nearer, 1 elsewhere. Only the comparison changes: instead
// of one shadow-map texel, a rotated disc of bilinear comparisons (PCF), and for PCSS a disc whose
// size follows the distance to the blockers found first. Shadow acne from the wider kernel is held
// back by a receiver plane bias (the depth slope across the shadow map, from the pixel's
// derivatives, limited); the game's own constant bias is kept, so nothing floats more than before.
//
// Built with the translator's prelude and bindings (the game's pipeline layout, b5 for KKLighting)
// and DXC's flags for the game's shaders (compile_shaders.ps1): the renderer uses it in place of the
// translated psshadowquad of the same hash (shader_library.h, SetReplacements).
#define KK_PIXEL_SHADER 1
#if defined(KL_SHADOW_FIXTURE)
cbuffer Fixture : register(b0) {
    float4 kl_Shadow;
    float4 kl_ShadowShape;
    float4 fixture_surface; // depth at centre, depth slope xy, reserved
};
Texture2D<float4> fixture_map : register(t0);
SamplerState fixture_sampler : register(s0);
#else
#include "../../shaders/hlsl/kk_common.hlsli"
#include "../../shaders/hlsl/kk_lighting.hlsli"
#endif

#ifndef KL_LIGHTS
#define KL_LIGHTS 1
#endif
#ifndef KL_PARAMS
#define KL_PARAMS 5
#endif
#ifndef KL_COLOR
#define KL_COLOR 4
#endif
#ifndef KL_PCSS
#define KL_PCSS 0
#endif

#define KL_PCF_TAPS 4
#define KL_SEARCH_TAPS 4
#define KL_PCSS_TAPS 8

// Interleaved gradient noise (Jimenez 2014): a per-pixel angle that turns the disc, so neighbouring
// pixels sample different offsets and the game's blur of the mask (psblurshadow) averages them.
float kl_Noise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// A Vogel disc: tap i of n at radius sqrt((i + 0.5) / n) and the golden angle times i, turned by
// the pixel's angle. The unturned taps are constants (sqrt, cos and sin of i, computed once, at
// float precision); the turn is one sine and cosine a pixel (rot = (cos, sin) of the angle)
// instead of one per tap: the same offsets up to float rounding.
static const float2 kl_Disc4[4] = {
    float2(0.353553385, 0), float2(-0.451544344, 0.413651705),
    float2(0.0691159591, -0.787542403), float2(0.569142818, 0.742345273)};
static const float2 kl_Disc8[8] = {
    float2(0.25, 0), float2(-0.319290072, 0.292495906),
    float2(0.0488723665, -0.55687654), float2(0.40244475, 0.524917364),
    float2(-0.738535166, -0.1306362), float2(0.699604869, -0.445031524),
    float2(-0.234003276, 0.870484054), float2(-0.446272045, -0.85926789)};
float2 kl_Disc(uint i, uint n, float2 rot)
{
    float2 t = n == 8 ? kl_Disc8[i] : kl_Disc4[i];
    return float2(t.x * rot.x - t.y * rot.y, t.x * rot.y + t.y * rot.x);
}

// One bilinear comparison: the four texels around uv against the reference, weighted as bilinear
// filtering would weight them (lit when the shadow map is not nearer, as the game decides).
float4 kl_GatherPlane(float2 uv, float2 size, float2 slope)
{
    float2 step = slope / size;
    float base = -dot(frac(uv * size - 0.5), step);
    // Gather order: (0,1), (1,1), (1,0), (0,0).
    return base + float4(step.y, step.x + step.y, step.x, 0.0);
}

float kl_Compare(Texture2D<float4> map, SamplerState s, float2 uv, float2 size, float reference, float2 slope)
{
    float4 z = map.GatherRed(s, uv);  // x (0,1), y (1,1), z (1,0), w (0,0)
    float4 lit = select(z >= reference + kl_GatherPlane(uv, size, slope), 1.0, 0.0);
    float2 f = frac(uv * size - 0.5);
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

struct KLReceiver
{
    float2 uv;      // in the shadow map
    float depth;    // light-space depth less the game's bias, at most 1 (the game's Z2)
    float2 slope;   // light-space depth per unit of uv (the receiver plane), limited
};

float kl_Shadow1(Texture2D<float4> map, SamplerState s, KLReceiver r, float2 rot)
{
    uint2 hostSize;
    map.GetDimensions(hostSize.x, hostSize.y);
    float2 size = float2(hostSize);
    float guest = size.x / max(kl_Shadow.w, 1e-3);  // guest texels per side (832 in the game)
#if KL_PCSS
    // Blockers: the texels nearer than the receiver within the search radius, and their mean depth.
    float searchUv = kl_Shadow.y / guest;
    float blockers = 0.0, blockerDepth = 0.0;
    [unroll] for (uint i = 0; i < KL_SEARCH_TAPS; i++)
    {
        float2 o = kl_Disc(i, KL_SEARCH_TAPS, rot) * searchUv;
        float4 z = map.GatherRed(s, r.uv + o);
        float reference = r.depth + dot(r.slope, o);
        float4 plane = kl_GatherPlane(r.uv + o, size, r.slope);
        float4 nearer = select(z < reference + plane, 1.0, 0.0);
        blockers += dot(nearer, 1.0);
        blockerDepth += dot(nearer, z - dot(r.slope, o) - plane);
    }
    if (blockers == 0.0)
        return 1.0;
    blockerDepth /= blockers;
    // The penumbra widens with the distance from the blocker to the receiver (a light of some size).
    float radius = clamp((r.depth - blockerDepth) * kl_Shadow.z, kl_ShadowShape.y, kl_ShadowShape.z);
    const uint taps = KL_PCSS_TAPS;
#else
    float radius = kl_Shadow.x;
    const uint taps = KL_PCF_TAPS;
#endif
    float radiusUv = radius / guest;
    float lit = 0.0;
    [unroll] for (uint j = 0; j < taps; j++)
    {
        float2 o = kl_Disc(j, taps, rot) * radiusUv;
        lit += kl_Compare(map, s, r.uv + o, size, r.depth + dot(r.slope, o), r.slope);
    }
    return lit / float(taps);
}

#if !defined(KL_SHADOW_FIXTURE)
KLReceiver kl_Receiver(float4 viewPos, uint light)
{
    float4 lp;
    lp.x = dot(viewPos, kk_PC[light * 4 + 0]);
    lp.y = dot(viewPos, kk_PC[light * 4 + 1]);
    lp.z = dot(viewPos, kk_PC[light * 4 + 2]);
    lp.w = dot(viewPos, kk_PC[light * 4 + 3]);
    KLReceiver r;
    float invW = 1.0 / lp.w;
    r.uv = float2(0.5 * (lp.x * invW + 1.0), 1.0 - 0.5 * (lp.y * invW + 1.0));
    r.depth = min(lp.z * invW - kk_PC[KL_PARAMS + light].z, 1.0);
    // The receiver plane (Isidoro 2006): d depth / d uv from the screen derivatives of both. Where
    // the 2x2 quad straddles two surfaces the slope is nonsense; the limit keeps it harmless.
    float2 du = ddx(r.uv), dv = ddy(r.uv);
    float det = du.x * dv.y - du.y * dv.x;
    float2 dz = float2(ddx(r.depth), ddy(r.depth));
    float2 slope = abs(det) > 1e-12 ? float2(dv.y * dz.x - du.y * dz.y, du.x * dz.y - dv.x * dz.x) / det : 0.0;
    uint2 hostSize;
    kk_Tex2D[kk_TexIndex(2 + light)].GetDimensions(hostSize.x, hostSize.y);
    float limit = kl_ShadowShape.x * float(hostSize.x) / max(kl_Shadow.w, 1e-3);  // per uv
    r.slope = clamp(slope, -limit, limit);
    return r;
}

void main(
    float4 iPos : SV_Position,
    float4 iT0 : TEXCOORD0,
    float4 iT1 : TEXCOORD1,
    float4 iT2 : TEXCOORD2,
    float4 iT3 : TEXCOORD3,
    float4 iT4 : TEXCOORD4,
    float4 iT5 : TEXCOORD5,
    float4 iT6 : TEXCOORD6,
    float4 iT7 : TEXCOORD7,
    float4 iT8 : TEXCOORD8,
    float4 iT9 : TEXCOORD9,
    float4 iT10 : TEXCOORD10,
    float4 iT11 : TEXCOORD11,
    float4 iT12 : TEXCOORD12,
    float4 iT13 : TEXCOORD13,
    float4 iT14 : TEXCOORD14,
    float4 iT15 : TEXCOORD15,
    float4 iD0 : COLOR0,
    float4 iD1 : COLOR1,
    bool iFace : SV_IsFrontFace,
    out float4 oC0 : SV_Target0)
{
    // The view depth under the pixel (the game's "Z prepass texture") and the view position.
    float2 screen = float2(iT0.x, -iT0.y) * iT0.w + 0.5;
    float z = kk_Tex2D[kk_TexIndex(0)].Sample(kk_PSSamplerState(0), screen).x;
    float4 viewPos = float4(iT0.xy * z, z, 1.0);
    float angle = kl_Noise(iPos.xy) * 6.28318531;
    float2 rot = float2(cos(angle), sin(angle));
    float3 lit = 1.0;
    [unroll] for (uint i = 0; i < KL_LIGHTS; i++)
    {
        KLReceiver r = kl_Receiver(viewPos, i);
        lit[i] = kl_Shadow1(kk_Tex2D[kk_TexIndex(2 + i)], kk_PSSamplerState(1 + i), r, rot);
    }
    float3 shadowColor = kk_PC[KL_COLOR].xyz;
    oC0 = float4(lerp(shadowColor, 1.0, lit), 0.0);
    kk_AlphaTest(oC0.w);
}
#else
// A synthetic host exercises the very same filter without game-derived shaders.
float4 vs_main(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position
{
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
float4 ps_main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    KLReceiver r;
    r.uv = uv;
    r.slope = fixture_surface.yz;
    r.depth = fixture_surface.x + dot(r.slope, uv - 0.5);
    float angle = kl_Noise(pos.xy) * 6.28318531;
    float lit = kl_Shadow1(fixture_map, fixture_sampler, r, float2(cos(angle), sin(angle)));
    return float4(lit, lit, lit, 1.0);
}
#endif
