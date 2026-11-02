// The red band on human skin (skin_band.h, docs/lighting.md): one full-screen pass over the picture, into it from a copy.
// Compiled to SPIR-V and DXIL by compile_shaders.sh / .ps1.
//
// The picture over the skin's own colour is how much light a pixel got, from every light and shadow together. Around each
// skin pixel the darkest and brightest of that say where this pixel lies between shadow and light, and the band is fullest
// half way. Where the light is even there is no band. Only skin pixels are read from and written to.
//
// Bindings (NVRHI's Vulkan offsets: t at 0, b at 256): b0 the constants, t0 the picture copy, t1 the skin image
// (skin_band_mask.hlsl: rgb the base texture's colour, a how much of it is skin).

cbuffer SkinBandConstants : register(b0)
{
    float4 kk_Size;  // xy: the picture's size (pixels), z: how far to look (pixels)
    float4 kk_Tune;  // x: the band's strength, y: the difference of light and shadow that gives the full band
};

Texture2D<float4> g_srcColour : register(t0);
Texture2D<float4> g_srcSkin : register(t1);

// The picture is about gamma 2: red lifted, green and blue held down a little.
static const float3 kRed = float3(0.20, -0.03, -0.06);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float Brightness(float3 c)
{
    return dot(c, float3(0.3, 0.59, 0.11));
}

float Lit(float3 colour, float3 base)
{
    return min(Brightness(colour) / max(Brightness(base), 0.03), 2.0);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target0
{
    const int2 p = int2(pos.xy);
    float4 colour = g_srcColour.Load(int3(p, 0));
    const float4 mine = g_srcSkin.Load(int3(p, 0));
    if (mine.a <= 0.01)
        return colour;
    const float here = Lit(colour.rgb, mine.rgb);
    float darkest = here, brightest = here;
    const int2 last = int2(kk_Size.xy) - 1;
    const int kTaps = 12;
    [loop] for (int i = 1; i <= kTaps; ++i)
    {
        const float r = sqrt(float(i) / float(kTaps)) * kk_Size.z;
        const float angle = float(i) * 2.39996323;
        const int2 q = clamp(p + int2(round(float2(cos(angle), sin(angle)) * r)), int2(0, 0), last);
        const float4 other = g_srcSkin.Load(int3(q, 0));
        if (other.a > 0.5)
        {
            const float lit = Lit(g_srcColour.Load(int3(q, 0)).rgb, other.rgb);
            darkest = min(darkest, lit);
            brightest = max(brightest, lit);
        }
    }
    const float apart = brightest - darkest;
    if (apart <= 0.02)
        return colour;
    const float t = saturate((here - darkest) / apart);
    const float band = 4.0 * t * (1.0 - t) * saturate(apart / kk_Tune.y) * mine.a * kk_Tune.x;
    colour.rgb = saturate(colour.rgb * (1.0 + kRed * band));
    return colour;
}
