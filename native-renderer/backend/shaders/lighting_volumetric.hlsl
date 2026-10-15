// Sunlight scattered by the game's fog. The shadow map, rather than a bright
// material or sky centroid, decides whether sunlight reaches each sample.
cbuffer VolumetricConstants : register(b0)
{
    float4 g_Light[4];
    float4 g_Shadow;     // bias, sample count, reserved, strength
    float4 g_FogColor;
    float4 g_FogParams;  // near/distance, density, inverse distance, depth clamp
    float4 g_FogNorm;    // 1 / view z = x * stored depth - y
    float4 g_Screen;     // inverse host size, host size
    float4 g_Sun;        // view direction towards the light, phase g
    float4 g_Camera;     // projection scales, exposure, reserved
    float4 g_Rays;       // working size, reserved
};
Texture2D<float4> t_depth : register(t0);
Texture2D<float4> t_shadow : register(t1);
Texture2D<float4> t_color : register(t2);
Texture2D<float4> t_rays : register(t3);
RWTexture2D<float4> u_out : register(u0);
SamplerState s_point : register(s0);
SamplerState s_linear : register(s1);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}
float Noise(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}
float ViewDepth(float stored)
{
    float invZ = g_FogNorm.x * stored - g_FogNorm.y;
    return invZ > 1e-6 ? min(1.0 / invZ, 60000.0) : 60000.0;
}
float FogAmount(float z)
{
    return saturate(saturate(z * g_FogParams.z - g_FogParams.x) * g_FogParams.y);
}
float Phase(float cosine)
{
    float g = g_Sun.w;
    float d = 1.0 + g * g - 2.0 * g * cosine;
    return min((1.0 - g * g) / max(d * sqrt(d), 1e-4), 2.0);
}

// Same position and comparison as psshadowquad. Out of map/depth coverage is
// unknown, so it contributes no new light. A clamp sampler must not extend the
// edge of a lit map across a cave or an unrelated part of the scene.
float SunVisibility(float2 ndc, float z)
{
    float stored = (1.0 / z + g_FogNorm.y) / g_FogNorm.x;
    float4 p = float4(ndc * stored, stored, 1.0);
    float4 lp = float4(dot(g_Light[0], p), dot(g_Light[1], p),
                       dot(g_Light[2], p), dot(g_Light[3], p));
    if (!(lp.w > 1e-6)) return 0.0;
    float3 q = lp.xyz / lp.w;
    float2 uv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(uv <= 0.0) || any(uv >= 1.0) || q.z < 0.0 || q.z > 1.0) return 0.0;
    float depth = t_shadow.SampleLevel(s_point, uv, 0.0).r;
    return depth >= q.z - g_Shadow.x ? 1.0 : 0.0;
}

// Quarter-resolution view-ray integration. At each sample the map is the
// visibility query along the real light direction. No light is inferred from
// a bright surface, no shadowed fog is subtracted, and no history is used.
[numthreads(8, 8, 1)]
void cs_mask(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Rays.xy))) return;
    float2 uv = (float2(id) + 0.5) / g_Rays.xy;
    float stored = t_depth.SampleLevel(s_point, uv, 0.0).r;
    float surface = ViewDepth(stored);
    if (stored >= 1.0 - 1e-6)
    {
        u_out[id] = float4(0.0, 0.0, 0.0, surface);
        return;
    }
    float nearZ = max(g_FogParams.x / g_FogParams.z, 0.05);
    float farZ = min(surface, (g_FogParams.x + 1.0) / g_FogParams.z);
    float fog = max(FogAmount(farZ) - FogAmount(nearZ), 0.0);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 view = normalize(float3(ndc / g_Camera.xy, 1.0));
    uint steps = uint(g_Shadow.y);
    float visibility = 0.0;
    if (farZ > nearZ && fog > 0.0)
    {
        float jitter = Noise(float2(id));
        [loop] for (uint i = 0; i < steps; ++i)
        {
            float z = lerp(nearZ, farZ, (float(i) + jitter) / float(steps));
            visibility += SunVisibility(ndc, z);
        }
    }
    float energy = fog * visibility / float(steps) * Phase(dot(view, g_Sun.xyz));
    u_out[id] = float4(max(g_FogColor.rgb, 0.0) * energy * g_Camera.z, surface);
}

// Compact depth-aware noise filter. Keep the centre depth for the full-size
// upsample; distant lit fog must never spread onto a nearby silhouette.
[numthreads(8, 8, 1)]
void cs_radial(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Rays.xy))) return;
    float4 centre = t_rays.Load(int3(id, 0));
    float3 sum = 0.0;
    float total = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 p = clamp(int2(id) + int2(x, y), 0, int2(g_Rays.xy) - 1);
        float4 v = t_rays.Load(int3(p, 0));
        float w = abs(v.a - centre.a) <= max(0.15, centre.a * 0.02) ? 1.0 : 0.0;
        sum += v.rgb * w;
        total += w;
    }
    u_out[id] = float4(sum / max(total, 1.0), centre.a);
}

float4 ps_volumetric(float4 pos : SV_Position) : SV_Target0
{
    float4 colour = t_color.Load(int3(pos.xy, 0));
    float2 uv = pos.xy * g_Screen.xy;
    float stored = t_depth.SampleLevel(s_point, uv, 0.0).r;
    float surface = ViewDepth(stored);
    if (stored >= 1.0 - 1e-6 || FogAmount(surface) < 1e-3) return colour;
    float2 p = uv * g_Rays.xy - 0.5;
    int2 base = int2(floor(p));
    float2 f = frac(p);
    float3 add = 0.0;
    float total = 0.0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x)
    {
        int2 q = clamp(base + int2(x, y), 0, int2(g_Rays.xy) - 1);
        float4 v = t_rays.Load(int3(q, 0));
        float w = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
        w *= abs(v.a - surface) <= max(0.15, surface * 0.02) ? 1.0 : 0.0;
        add += v.rgb * w;
        total += w;
    }
    add /= max(total, 1e-5);
    // Bound the addition relative to the receiver as well as its headroom.
    // Intensity cannot turn an almost-black cave/character into an emitter.
    add = min(max(add, 0.0), 0.35 * max(colour.rgb, 0.03));
    return float4(colour.rgb + add * saturate(1.0 - colour.rgb), colour.a);
}
