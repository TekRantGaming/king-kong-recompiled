// Screen-space global illumination for the native renderer (docs/gi.md): one bounce of light
// from the surfaces on the screen, gathered on the depth XeGTAO already prepares (gtao.hlsl's
// cs_prepare, cs_prefilter and cs_normals: view-space depth with four smaller mips, normals
// rebuilt from it), with the lit scene colour at the end of the game's opaque pass as the
// light the surfaces send. Compiled to SPIR-V and DXIL by compile_shaders.sh / .ps1.
//
// The gather is a visibility bitmask over GTAO's slices (Therrien, Levesque and Gilet, "Screen
// Space Indirect Lighting with Visibility Bitmask", 2023): each slice through the pixel keeps 32
// sectors of its hemisphere, a sample covers the sectors between its front and its back (the
// surface seen on the screen, taken to be `thickness` deep), and only sectors no nearer sample
// has covered yet let its light through. Sectors are spaced evenly in the slice's own measure of
// light (the cosine to the normal times the sine of the angle from the view vector, GTAO's
// integrand), so each one is the same share of the slice's light; slices are weighted by that
// measure's total times the projected normal's length. A full hemisphere of light L gives exactly L. The result is the light the surface gets from what hides its sky, in the
// units of the scene colour (the game's ambient constant is in the same units), and the share
// of the hemisphere that nothing hides (the visibility).
//
//   cs_radiance        the scene colour over the picture into the working image (a block's mean)
//   cs_radiance_down   its smaller mips (2x2 means), for samples far away
//   cs_gather          the bounce light (rgb) and the visibility (a) per working pixel
//   cs_denoise         an edge-aware blur (5x5 a-trous, depth and normal weights), run with steps
//                      1, 2, 4 ...
//   vs_main + ps_composite   into the scene colour target: the light added to the surface's
//                            colour (and the flat ambient light taken away where it is hidden)
//
// The working image is the one XeGTAO's passes prepared: pixel i is host pixel origin + step * i
// (step 1 full resolution, 2 half). Bindings (NVRHI's Vulkan offsets: t at 0, s at 128, b at 256,
// u at 384): b0 the constants below, t0..t4 inputs, u0 the output, s0 point clamp, s1 linear clamp.

#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS 1
#define VA_SATURATE saturate
#define unorm
#include "../../thirdparty/XeGTAO/XeGTAO.h"

cbuffer SsgiConstants : register(b0)
{
    GTAOConstants g_GTAOConsts;  // XeGTAO's, as the depth was prepared (ViewportSize is the padded size)
    int4 kk_Source;     // xy: the picture's origin in the target (host pixels), zw: its size
    uint4 kk_Working;   // xy: working size covering the picture, zw: padded size
    float4 kk_Params;   // x: step (1 or 2), y: stored depth from which a pixel is sky, z: radius, w: thickness (world units)
    float4 kk_Gather;   // x: slices, y: steps per side, z: largest radius on the screen (working pixels), w: intensity
    float4 kk_Ambient;  // rgb: the frame's flat ambient light (g_vAmbientColor), w: the largest albedo assumed
    float4 kk_Mode;     // x: occlusion of the ambient (0 to 1), y: debug view, z: radiance mips, w: denoise step (working pixels)
    float4 kk_Extra;    // x: 1 when the occlusion of the AO (t2) is to be multiplied in, yzw: unused
};

#include "../../thirdparty/XeGTAO/XeGTAO.hlsli"

SamplerState g_samplerPointClamp : register(s0);
SamplerState g_samplerLinearClamp : register(s1);

// cs_radiance, cs_radiance_down
Texture2D<float4> g_srcColor : register(t0);      // the scene colour target (radiance) or the mip above (down)
RWTexture2D<float4> g_outRadiance : register(u0);

// cs_gather
Texture2D<float> g_srcViewDepth : register(t0);   // view-space depth, XE_GTAO_DEPTH_MIP_LEVELS mips
Texture2D<uint> g_srcNormalmap : register(t1);
Texture2D<float4> g_srcRadiance : register(t2);   // the scene colour, working size, with mips
Texture2D<float> g_srcRawDepth : register(t3);    // the stored depth (sky test)
RWTexture2D<float4> g_outGi : register(u0);

// cs_denoise
Texture2D<float4> g_srcGi : register(t0);
Texture2D<float> g_srcDenoiseDepth : register(t1);
Texture2D<uint> g_srcDenoiseNormals : register(t2);

// ps_composite
Texture2D<float4> g_srcSceneCopy : register(t0);  // the scene colour as it was (a copy of the target)
Texture2D<float4> g_srcGiResult : register(t1);   // the gathered light, working size
Texture2D<float> g_srcOcclusion : register(t2);   // the AO's occlusion over the picture (kk_Extra.x)
Texture2D<float> g_srcWorkDepth : register(t3);   // working view-space depth (mip 0)
Texture2D<float> g_srcSceneDepth : register(t4);  // the scene's stored depth (host pixels)

static const float kPi = 3.14159265;
static const float kHalfPi = 1.5707963;

float3 LoadNormalAt(Texture2D<uint> map, int2 pos)
{
    uint packedInput = map.Load(int3(pos, 0)).x;
    float3 n = XeGTAO_R11G11B10_UNORM_to_FLOAT3(packedInput) * 2.0 - 1.0;
    float l = length(n);
    return l > 1e-4 ? n / l : float3(0.0, 0.0, -1.0);
}

float2 Noise(uint2 pixCoord)
{
    // XeGTAO's Hilbert curve driving R2 (vaGTAO.hlsl), with a different start so the GI's pattern
    // is not the AO's. The same every frame: without temporal accumulation it must not crawl.
    uint index = HilbertIndex(pixCoord.x, pixCoord.y) + 17u;
    return frac(0.5 + float(index) * float2(0.75487766624669276005, 0.5698402909980532659114));
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// ------------------------------------------------------------------ radiance

[numthreads(8, 8, 1)]
void cs_radiance(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= kk_Working.zw))
        return;
    uint2 w = min(id, kk_Working.xy - 1);
    int step = int(kk_Params.x);
    int2 base = kk_Source.xy + int2(w) * step;
    int2 last = kk_Source.xy + kk_Source.zw - 1;
    float3 sum = 0.0;
    for (int j = 0; j < step; ++j)
        for (int i = 0; i < step; ++i)
            sum += g_srcColor.Load(int3(min(base + int2(i, j), last), 0)).rgb;
    g_outRadiance[id] = float4(max(sum / float(step * step), 0.0), 1.0);
}

[numthreads(8, 8, 1)]
void cs_radiance_down(uint2 id : SV_DispatchThreadID)
{
    uint w, h;
    g_srcColor.GetDimensions(w, h);
    int2 last = int2(w, h) - 1;
    int2 p = int2(id) * 2;
    float3 sum = g_srcColor.Load(int3(min(p, last), 0)).rgb + g_srcColor.Load(int3(min(p + int2(1, 0), last), 0)).rgb +
                 g_srcColor.Load(int3(min(p + int2(0, 1), last), 0)).rgb + g_srcColor.Load(int3(min(p + int2(1, 1), last), 0)).rgb;
    g_outRadiance[id] = float4(sum * 0.25, 1.0);
}

// -------------------------------------------------------------------- gather

// The slice's measure of light from its lower edge (the normal's horizon, n - pi/2) up to angle t
// (both from the view vector, signed): the integral of cos(t - n) |sin t| (GTAO's integrand).
// sinN, cosN: sin(n), cos(n) (the same for the whole slice).
float SliceMeasure(float t, float n, float sinN, float cosN)
{
    float c = cos(2.0 * t - n) * 0.25, k = t * sinN * 0.5, z = cosN * 0.25;
    return t >= 0.0 ? (-c + k + z) : (c - k - z);
}

// The sectors between two heights u0 <= u1 (0 to 1, the sine space of the slice's hemisphere),
// each edge rounded to the nearest sector boundary: a sliver of a sector covers nothing (so a flat
// surface, whose samples graze its own horizon, does not hide its own sky).
uint SectorBits(float u0, float u1)
{
    uint a = uint(clamp(round(u0 * 32.0), 0.0, 32.0));
    uint b = uint(clamp(round(u1 * 32.0), 0.0, 32.0));
    if (b <= a)
        return 0u;
    uint count = b - a;
    uint run = count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u);
    return a >= 32u ? 0u : (run << a);
}

[numthreads(8, 8, 1)]
void cs_gather(uint2 pix : SV_DispatchThreadID)
{
    if (any(pix >= kk_Working.zw))
        return;
    if (any(pix >= kk_Working.xy) || g_srcRawDepth.Load(int3(pix, 0)).x >= kk_Params.y)
    {
        g_outGi[pix] = float4(0.0, 0.0, 0.0, 1.0);  // the sky, or the padding
        return;
    }
    const GTAOConstants consts = g_GTAOConsts;
    float2 uv = (float2(pix) + 0.5) * consts.ViewportPixelSize;
    float z = g_srcViewDepth.Load(int3(pix, 0)).x * 0.99999;
    float3 P = XeGTAO_ComputeViewspacePosition(uv, z, consts);
    float3 V = normalize(-P);
    float3 N = LoadNormalAt(g_srcNormalmap, int2(pix));

    const float radius = kk_Params.z;
    const float thickness = kk_Params.w;
    // View-space size of a working pixel at this depth, and the radius on the screen.
    float pixelSize = z * consts.NDCToViewMul_x_PixelSize.x;
    float radiusPx = min(radius / max(pixelSize, 1e-6), kk_Gather.z);
    if (radiusPx < 2.0)
    {
        g_outGi[pix] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const uint sliceCount = uint(kk_Gather.x);
    const uint stepCount = uint(kk_Gather.y);
    const float minS = 1.3 / radiusPx;  // not the pixel itself
    const float maxMip = kk_Mode.z - 1.0;
    float2 noise = Noise(pix);

    float3 light = 0.0;
    float hidden = 0.0;
    float weightSum = 0.0;
    for (uint slice = 0; slice < sliceCount; ++slice)
    {
        float phi = (float(slice) + noise.x) / float(sliceCount) * kPi;
        float cosPhi = cos(phi), sinPhi = sin(phi);
        float2 omega = float2(cosPhi, -sinPhi) * radiusPx;  // working pixels (y down)
        float3 dirV = float3(cosPhi, sinPhi, 0.0);           // view space (y up)
        float3 ortho = dirV - dot(dirV, V) * V;
        float3 axis = normalize(cross(ortho, V));
        float3 projN = N - axis * dot(N, axis);
        float projLen = length(projN);
        float cosN = saturate(dot(projN, V) / max(projLen, 1e-4));
        float n = (dot(ortho, projN) >= 0.0 ? 1.0 : -1.0) * XeGTAO_FastACos(cosN);
        // The slice's measure from the normal's one horizon to the other (cos n + n sin n).
        float sinN = sin(n), cosNs = cos(n);
        float low = SliceMeasure(n - kHalfPi, n, sinN, cosNs);
        float total = max(SliceMeasure(n + kHalfPi, n, sinN, cosNs) - low, 1e-4);
        float invTotal = 1.0 / total;

        uint mask = 0u;
        float3 sliceLight = 0.0;
        [loop]
        for (uint side = 0; side < 2; ++side)
        {
            float sideSign = side == 0 ? 1.0 : -1.0;
            [loop]
            for (uint s = 0; s < stepCount; ++s)
            {
                float stepNoise = frac(noise.y + float(slice + s * sliceCount + side * 7u) * 0.6180339887);
                float t = (float(s) + stepNoise) / float(stepCount);
                t = t * t + minS;  // denser near the pixel
                float2 offset = round(t * omega) * sideSign;
                float2 sp = float2(pix) + 0.5 + offset;
                if (any(sp < 0.0) || any(sp >= float2(kk_Working.xy)))
                    break;
                float offsetLength = length(offset);
                float2 suv = sp * consts.ViewportPixelSize;
                float mip = clamp(log2(offsetLength) - consts.DepthMIPSamplingOffset, 0.0, float(XE_GTAO_DEPTH_MIP_LEVELS - 1));
                float sz = g_srcViewDepth.SampleLevel(g_samplerPointClamp, suv, mip).x;
                float3 S = XeGTAO_ComputeViewspacePosition(suv, sz, consts);
                float3 d = S - P;
                float dist = length(d);
                if (dist > radius || dist < 1e-5)
                    continue;  // too far to hide anything or send light here
                float3 back = d - V * thickness;
                float af = XeGTAO_FastACos(dot(d / dist, V));
                float ab = XeGTAO_FastACos(dot(normalize(back), V));
                // Angles in the slice from the view vector, positive towards +omega, within the
                // normal's hemisphere; into the sector space (the share of the slice's light below them).
                float2 h = side == 0 ? float2(af, ab) : float2(-ab, -af);
                h = clamp(h, n - kHalfPi, n + kHalfPi);
                float2 u = saturate((float2(SliceMeasure(h.x, n, sinN, cosNs), SliceMeasure(h.y, n, sinN, cosNs)) - low) * invTotal);
                uint bits = SectorBits(u.x, u.y);
                uint fresh = bits & ~mask;
                mask |= bits;
                if (fresh == 0u)
                    continue;
                // The surface there must face this pixel to light it (its seen side may face away).
                float3 Ns = LoadNormalAt(g_srcNormalmap, int2(sp));
                float facing = saturate(dot(Ns, -d / dist) * 4.0);
                float fade = saturate((radius - dist) / (0.3 * radius));
                float rmip = clamp(log2(offsetLength) - 2.0, 0.0, maxMip);
                float3 L = g_srcRadiance.SampleLevel(g_samplerLinearClamp, suv, rmip).rgb;
                sliceLight += L * (float(countbits(fresh)) / 32.0) * facing * fade;
            }
        }
        float sliceWeight = projLen * total;
        light += sliceLight * sliceWeight;
        hidden += float(countbits(mask)) / 32.0 * sliceWeight;
        weightSum += sliceWeight;
    }
    float inv = weightSum > 1e-4 ? 1.0 / weightSum : 0.0;
    // Fade in over small radii on the screen (a few pixels give no reliable slices).
    float fadeIn = saturate((radiusPx - 2.0) / 6.0);
    g_outGi[pix] = float4(light * inv * fadeIn, 1.0 - saturate(hidden * inv) * fadeIn);
}

// ------------------------------------------------------------------- denoise

[numthreads(8, 8, 1)]
void cs_denoise(uint2 pix : SV_DispatchThreadID)
{
    if (any(pix >= kk_Working.zw))
        return;
    float4 centre = g_srcGi.Load(int3(pix, 0));
    if (any(pix >= kk_Working.xy) || g_srcRawDepth.Load(int3(pix, 0)).x >= kk_Params.y)
    {
        g_outGi[pix] = centre;
        return;
    }
    float z = g_srcDenoiseDepth.Load(int3(pix, 0)).x;
    float3 N = LoadNormalAt(g_srcDenoiseNormals, int2(pix));
    int stepPx = int(kk_Mode.w);
    int2 last = int2(kk_Working.xy) - 1;
    static const float kKernel[3] = {0.375, 0.25, 0.0625};
    float4 sum = 0.0;
    float weight = 0.0;
    [unroll]
    for (int j = -2; j <= 2; ++j)
    {
        [unroll]
        for (int i = -2; i <= 2; ++i)
        {
            int2 q = int2(pix) + int2(i, j) * stepPx;
            if (any(q < 0) || any(q > last))
                continue;
            float zq = g_srcDenoiseDepth.Load(int3(q, 0)).x;
            float3 Nq = LoadNormalAt(g_srcDenoiseNormals, q);
            float w = kKernel[abs(i)] * kKernel[abs(j)];
            w *= exp(-abs(zq - z) / max(z * 0.03 * float(stepPx), 1e-5));
            float nd = saturate(dot(N, Nq));
            nd *= nd;
            nd *= nd;
            w *= nd * nd;
            if (g_srcRawDepth.Load(int3(q, 0)).x >= kk_Params.y)
                w = 0.0;  // the sky has no light to share
            sum += g_srcGi.Load(int3(q, 0)) * w;
            weight += w;
        }
    }
    g_outGi[pix] = weight > 1e-6 ? sum / weight : centre;
}

// ----------------------------------------------------------------- composite

// One triangle over the viewport (the viewport is the picture).
float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

// The gathered light at a host pixel of the picture: the working pixels around it, bilinear and
// weighted by how close their depth is to this pixel's (as gtao.hlsl's cs_output).
float4 GiAt(int2 q, float z)
{
    float step = kk_Params.x;
    if (step < 1.5)
        return g_srcGiResult.Load(int3(q, 0));
    float2 f = float2(q) / step;
    int2 base = int2(floor(f));
    float2 t = f - float2(base);
    int2 last = int2(kk_Working.xy) - 1;
    float4 total = 0.0;
    float weight = 0.0;
    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        int2 o = int2(j & 1, j >> 1);
        int2 c = clamp(base + o, int2(0, 0), last);
        float bilinear = (o.x ? t.x : 1.0 - t.x) * (o.y ? t.y : 1.0 - t.y);
        float dz = abs(g_srcWorkDepth.Load(int3(c, 0)).x - z) / max(z, 1e-6);
        float w = (bilinear + 1e-3) / (1e-3 + dz * 50.0);
        total += g_srcGiResult.Load(int3(c, 0)) * w;
        weight += w;
    }
    return weight > 0.0 ? total / weight : float4(0.0, 0.0, 0.0, 1.0);
}

// Into the scene colour target (alpha not written). The game's lighting shaders give a surface
// colour = albedo * (ambient + direct light). Where the surface is shaded its colour is about
// albedo * ambient, so colour / ambient is its albedo; where it is lit its colour is brighter than
// albedo-limit * ambient, and the light is taken to be its brightness over that limit. So the
// albedo estimate never exceeds the limit and is exact in the shade, which is where indirect light
// matters. Then: colour + albedo * (bounce light * intensity - ambient * hidden share * occlusion).
float4 ps_composite(float4 pos : SV_Position) : SV_Target0
{
    int2 p = int2(pos.xy);
    float4 colour = g_srcSceneCopy.Load(int3(p, 0));
    int2 q = p - kk_Source.xy;
    float stored = g_srcSceneDepth.Load(int3(p, 0)).x;
    if (any(q < 0) || any(q >= kk_Source.zw) || stored >= kk_Params.y)
        return colour;  // outside the picture, or the sky
    float z = XeGTAO_ScreenSpaceToViewSpaceDepth(stored, g_GTAOConsts);
    float4 gi = GiAt(q, z);
    float3 bounce = gi.rgb * kk_Gather.w;
    float hiddenShare = 1.0 - gi.a;
    float3 A = max(kk_Ambient.rgb, 1e-3);
    float lumaA = Luma(A);
    float scale = max(1.0, Luma(colour.rgb) / max(kk_Ambient.w * lumaA, 1e-4));
    float3 albedo = min(colour.rgb / (A * scale), kk_Ambient.w);
    float3 delta = albedo * (bounce - A * hiddenShare * kk_Mode.x);
    float ao = kk_Extra.x > 0.5 ? g_srcOcclusion.Load(int3(q, 0)).x : 1.0;
    int debugView = int(kk_Mode.y + 0.5);
    if (debugView == 1)
        return float4(bounce, colour.a);
    if (debugView == 2)
        return float4(saturate(0.5 + delta * 2.0), colour.a);
    if (debugView == 3)
        return float4(gi.aaa, colour.a);
    return float4(max(colour.rgb * ao + delta, 0.0), colour.a);
}
