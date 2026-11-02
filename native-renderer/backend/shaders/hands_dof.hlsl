// Depth of field on Jack's first-person hands and what he holds (hands_dof.h, docs/lighting.md): one full-screen pass
// over the picture right after their colour draws, into it from a copy. Compiled to SPIR-V and DXIL by compile_shaders.sh
// / .ps1.
//
// A gather over a disc, as photo.hlsl's, but what is out of focus lies in front: a hands pixel reaches as far as its own
// softness, and where it reaches it covers what is behind it. The world never blurs. What is behind the hands was never
// drawn (their depth comes first), so under their soft edge the world pixels nearby stand in for it.
//
// Bindings (NVRHI's Vulkan offsets: t at 0, b at 256): b0 the constants, t0 the picture copy, t1 the mask (hands_dof_mask.hlsl).

cbuffer HandsDofConstants : register(b0)
{
    float4 kk_Size;  // xy: the picture's size (pixels), z: the largest blur radius (pixels)
};

Texture2D<float4> g_srcColour : register(t0);
Texture2D<float> g_srcMask : register(t1);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target0
{
    const int2 p = int2(pos.xy);
    const int2 last = int2(kk_Size.xy) - 1;
    const float largest = kk_Size.z;
    float4 colour = g_srcColour.Load(int3(p, 0));
    const float mine = g_srcMask.Load(int3(p, 0));
    float3 front = float3(0.0, 0.0, 0.0), back = float3(0.0, 0.0, 0.0);
    float fronts = 0.0, backs = 0.0, reach = 0.0;
    if (mine > 0.0)
    {
        front = colour.rgb;
        fronts = 1.0;
        reach = (mine - 1.0 / 255.0) * largest;
    }
    const int kTaps = 24;
    [loop] for (int i = 1; i <= kTaps; ++i)
    {
        const float r = sqrt((float(i) - 0.5) / float(kTaps)) * largest;
        const float angle = float(i) * 2.39996323;
        const int2 q = clamp(p + int2(round(float2(cos(angle), sin(angle)) * r)), int2(0, 0), last);
        const float m = g_srcMask.Load(int3(q, 0));
        if (m > 0.0)
        {
            const float soft = (m - 1.0 / 255.0) * largest;
            if (soft >= r)
            {
                front += g_srcColour.Load(int3(q, 0)).rgb;
                fronts += 1.0;
                reach = max(reach, soft);
            }
        }
        else if (mine > 0.0)
        {
            back += g_srcColour.Load(int3(q, 0)).rgb;
            backs += 1.0;
        }
    }
    if (fronts <= 0.0)
        return colour;
    front /= fronts;
    const float inReach = reach / largest;
    const float cover = saturate(fronts / (1.0 + float(kTaps) * inReach * inReach));
    const float3 behind = mine > 0.0 ? (backs > 0.0 ? back / backs : front) : colour.rgb;
    colour.rgb = lerp(behind, front, cover);
    return colour;
}
