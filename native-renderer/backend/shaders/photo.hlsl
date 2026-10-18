// Photo mode's grade (task 208, backend/photo_grade.h): one full-screen pass over the finished picture
// (the frame image the presenter shows and the captures read), from a copy of it. Compiled to SPIR-V and
// DXIL by compile_shaders.sh / .ps1.
//
// In order: the mirror (flip), the depth of field (a gather over a disc whose size follows each pixel's
// distance from the focus, from the scene's view-space depth prepared at the end of its opaque pass),
// exposure (in linear light), white balance, contrast, saturation, the look (black and white, sepia, tints;
// backend/photo_config.cpp PhotoGradeColour is the same colour function on the CPU), the vignette, film
// grain (a fixed pattern while the picture is still) and the letterbox bars.
//
// Bindings (NVRHI's Vulkan offsets: t at 0, s at 128, b at 256, u at 384): b0 the constants, t0 the picture
// copy, t1 the view-space depth (metres; a 1 x 1 dummy when there is none).

cbuffer PhotoConstants : register(b0)
{
    float4 kk_Size;     // xy: the picture's size (pixels), zw: its aspect ratio, unused
    float4 kk_Grade;    // x: exposure gain, y: contrast, z: saturation, w: temperature (-1 cool .. 1 warm)
    float4 kk_Look;     // rgb: the look's colour over its luminance, w: its strength (0: none)
    float4 kk_Effects;  // x: vignette, y: grain, z: the bars' aspect ratio (0: none), w: 1 mirrored
    float4 kk_Dof;      // x: largest blur radius (pixels, 0: off), y: focus (m), z: sharp range (m), w: fall-off (m)
    float4 kk_Depth;    // xy: depth texels per picture pixel, zw: the depth image's last texel
    uint4 kk_Misc;      // x: 1 black and white / sepia (luminance), 2 a tint (colours kept), y: the grain's seed
};

Texture2D<float4> g_src : register(t0);
Texture2D<float> g_depth : register(t1);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

int2 Clamp(int2 p)
{
    return clamp(p, int2(0, 0), int2(kk_Size.xy) - 1);
}

float ViewDepth(float2 pixel)
{
    int2 t = int2(min(pixel * kk_Depth.xy, kk_Depth.zw));
    return g_depth.Load(int3(t, 0));
}

// 0 sharp .. 1 fully blurred.
float Coc(float z)
{
    return saturate((abs(z - kk_Dof.y) - kk_Dof.z) / kk_Dof.w);
}

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

float3 DepthOfField(int2 p)
{
    const float3 centre = g_src.Load(int3(p, 0)).rgb;
    const float c = Coc(ViewDepth(float2(p) + 0.5));
    const float radius = c * kk_Dof.x;
    if (radius < 0.5)
        return centre;
    float3 sum = centre;
    float weight = 1.0;
    // A golden-angle spiral of 40 taps over the disc; a tap counts when its own blur reaches this pixel
    // (sharp things in front are not smeared over by the background behind them).
    const int kTaps = 40;
    [loop] for (int i = 1; i <= kTaps; ++i)
    {
        const float r = sqrt((float(i) - 0.5) / float(kTaps)) * radius;
        const float a = float(i) * 2.39996323;
        const float2 o = float2(cos(a), sin(a)) * r;
        const int2 q = Clamp(p + int2(round(o)));
        const float cs = Coc(ViewDepth(float2(q) + 0.5)) * kk_Dof.x;
        const float w = saturate(cs - r + 1.0);
        sum += g_src.Load(int3(q, 0)).rgb * w;
        weight += w;
    }
    return sum / weight;
}

float4 ps_photo(float4 pos : SV_Position) : SV_Target
{
    int2 p = int2(pos.xy);
    const float2 uv = (float2(p) + 0.5) / kk_Size.xy;
    // The letterbox bars first (nothing under them is seen).
    if (kk_Effects.z > 0.0)
    {
        const float aspect = kk_Size.x / kk_Size.y;
        if (kk_Effects.z > aspect)
        {
            const float bar = 0.5 * (1.0 - aspect / kk_Effects.z);
            if (uv.y < bar || uv.y > 1.0 - bar)
                return float4(0, 0, 0, 1);
        }
        else
        {
            const float bar = 0.5 * (1.0 - kk_Effects.z / aspect);
            if (uv.x < bar || uv.x > 1.0 - bar)
                return float4(0, 0, 0, 1);
        }
    }
    if (kk_Effects.w > 0.5)
        p.x = int(kk_Size.x) - 1 - p.x;
    const float alpha = g_src.Load(int3(p, 0)).a;
    float3 c = kk_Dof.x > 0.0 ? DepthOfField(p) : g_src.Load(int3(p, 0)).rgb;

    const float t = kk_Grade.w;
    const float3 warm = float3(1.0 + 0.10 * t, 1.0 + 0.01 * t, 1.0 - 0.10 * t);
    float3 lin = pow(max(c, 0.0), 2.2) * kk_Grade.x * warm;
    c = pow(max(lin, 0.0), 1.0 / 2.2);
    c = (c - 0.5) * kk_Grade.y + 0.5;
    float l = Luma(c);
    c = l + (c - l) * kk_Grade.z;
    if (kk_Misc.x != 0)
    {
        l = Luma(c);
        const float3 target = (kk_Misc.x == 1 ? float3(l, l, l) : c) * kk_Look.rgb;
        c = lerp(c, target, kk_Look.w);
    }
    c = max(c, 0.0);

    if (kk_Effects.x > 0.0)
    {
        const float aspect = kk_Size.x / kk_Size.y;
        const float2 d = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
        const float r = length(d) / length(float2(0.5 * aspect, 0.5));
        c *= 1.0 - kk_Effects.x * 0.85 * smoothstep(0.35, 1.0, r);
    }
    if (kk_Effects.y > 0.0)
    {
        // Brighter in the midtones, as film grain; a fixed pattern for the seed (a still stays still).
        const uint h = Hash(uint(pos.x) * 1973u + Hash(uint(pos.y) * 9277u + kk_Misc.y));
        const float n = float(h & 0xFFFFu) / 65535.0 - 0.5;
        l = saturate(Luma(c));
        c = max(c + n * kk_Effects.y * 0.16 * (1.0 - 0.6 * abs(2.0 * l - 1.0)), 0.0);
    }
    return float4(c, alpha);
}
