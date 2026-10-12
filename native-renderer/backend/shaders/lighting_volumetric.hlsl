// The volumetric light (docs/lighting.md, "Light shafts"): the game's fog, lit by the sun's shadow map.
//
// The game's fog pass adds its fog colour by a fog amount that grows with the distance (psaftereffects,
// g_vFogParams), as if all the air in front of a surface were lit. Here, right after that pass, each
// pixel's view ray is marched through the part of it the fog covers, and each step is tested against the
// shadow map the game's screen-space shadow uses (psshadowquad's first light, in its own view space:
// g_mCamToLightMatrix applied to (x d, y d, d, 1), the bias of g_vShadowParams). Where the ray runs through
// shadow, part of the fog the game added is taken away again (down to an ambient share); where it runs
// through light, the game's fog stays. Shafts of fog light appear between shadowed fog, in the fog's own
// colour and only as much as the game's fog is there: a clear scene stays clear.
//
//   vs_main + ps_volumetric   a full-screen triangle into the scene's colour target, blended as
//                             destination - source (RGB only)
//
// Bindings (NVRHI's Vulkan offsets: t at 0, s at 128, b at 256): b0 the constants, t0 the game's depth
// resolve (stored depth), t1 the shadow map, s0 a point clamp sampler.

cbuffer VolumetricConstants : register(b0)
{
    float4 g_Light[4];   // psshadowquad's light 0 rows (c0..c3): light clip = dot(row, (x d, y d, d, 1))
    float4 g_Shadow;     // x: the game's depth bias, y: steps, z: ambient share of shadowed fog, w: strength
    float4 g_FogColor;   // rgb: g_vFogColor
    float4 g_FogParams;  // x: g_fFogNearOverDistance, y: g_fFogDensity, z: g_fFogInvDistance, w: the depth clamp
    float4 g_FogNorm;    // xy: 1 / view z = x * stored depth - y (g_vFogNormalizing); z: sky depth
    float4 g_Screen;     // xy: 1 / the depth texture's size (host pixels), zw: its size
};

Texture2D<float4> t_depth : register(t0);
Texture2D<float4> t_shadow : register(t1);
SamplerState s_point : register(s0);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float Noise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// The fog amount the game gives a view depth (psaftereffects' fog pass, without its pitch attenuation).
float FogAmount(float z)
{
    return saturate(saturate(z * g_FogParams.z - g_FogParams.x) * g_FogParams.y);
}

float4 ps_volumetric(float4 pos : SV_Position) : SV_Target0
{
    float2 uv = pos.xy * g_Screen.xy;
    float d = min(t_depth.SampleLevel(s_point, uv, 0.0).x, g_FogParams.w);
    float invZ = g_FogNorm.x * d - g_FogNorm.y;
    float zSurface = invZ > 1e-6 ? 1.0 / invZ : 1e6;
    float fog = FogAmount(zSurface);
    if (fog < 1e-3)
        return 0.0;
    // The fogged part of the ray: from where the fog starts to where it is whole, or the surface.
    float zStart = g_FogParams.x / max(g_FogParams.z, 1e-9);
    float zFull = (1.0 / max(g_FogParams.y, 1e-6) + g_FogParams.x) / max(g_FogParams.z, 1e-9);
    float zEnd = min(zSurface, zFull);
    if (zEnd <= zStart)
        return 0.0;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    uint steps = uint(g_Shadow.y);
    float jitter = Noise(pos.xy);
    float lit = 0.0;
    [loop] for (uint i = 0; i < steps; i++)
    {
        float z = lerp(zStart, zEnd, (float(i) + jitter) / float(steps));
        // Back to the stored depth of that view depth, and into the light's shadow map as the game does.
        float ds = (1.0 / z + g_FogNorm.y) / g_FogNorm.x;
        float4 v = float4(ndc * ds, ds, 1.0);
        float4 l = float4(dot(v, g_Light[0]), dot(v, g_Light[1]), dot(v, g_Light[2]), dot(v, g_Light[3]));
        float invW = 1.0 / l.w;
        float2 suv = float2(0.5 * (l.x * invW + 1.0), 1.0 - 0.5 * (l.y * invW + 1.0));
        float zl = min(l.z * invW - g_Shadow.x, 1.0);
        bool inside = all(suv > 0.0) && all(suv < 1.0) && l.w > 0.0;
        float blocker = t_shadow.SampleLevel(s_point, saturate(suv), 0.0).x;
        lit += (!inside || blocker >= zl) ? 1.0 : 0.0;
    }
    lit /= float(steps);
    // The fog the game added that lies in shadow, less its ambient share.
    float shadowed = fog * (1.0 - lit) * (1.0 - g_Shadow.z) * g_Shadow.w;
    return float4(g_FogColor.rgb * shadowed, 0.0);
}
