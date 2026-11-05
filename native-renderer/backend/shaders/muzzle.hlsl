// Muzzle flashes that cast light (docs/lighting.md): one pass over the part of the picture a flash can reach, at the end
// of the scene's opaque pass, only in frames with a flash. Compiled to SPIR-V and DXIL by compile_shaders.sh / .ps1.
//
// 5 Nov (Craig: it cost too much frame rate; a flash lasts a tenth of a second, so quality may give): the pass reads only
// the scene's depth, five texels a pixel. Each pixel's view position comes from its stored depth, its normal from the
// depth beside it (the nearer of the two sides, so an edge does not bend it). Nothing is copied and no normals pass runs:
// the light is blended onto the picture in two draws: first a little added, as a dark surface still shows it, then the
// picture times one plus the light, so the surface keeps its own colour and grain. A pixel the light does not reach is
// discarded. Sky (stored depth at the far plane) is left as it is.
//
// Bindings (NVRHI's Vulkan offsets: t at 0, b at 256): b0 the constants, t0 the scene depth.

cbuffer MuzzleConstants : register(b0)
{
    float4 kk_Unpack;   // x, y: view z = x / (y - stored depth); z: stored depth from which a pixel is sky; w: lights
    float4 kk_Ndc;      // xy: view xy = (xy * uv + zw) * view z, uv over the picture
    int4 kk_Source;     // xy: the picture's origin in the target (host pixels), zw: its size
    float4 kk_Params;   // x: the lights' radius (view units, metres), y: distance fall-off, z: wrap, w: gain
    float4 kk_Colour;   // rgb: the light's colour
    float4 kk_Light[4]; // xyz: view position, w: strength now
    float4 kk_Pass;     // x: 0 the light on the surface's own colour (blended times the picture), 1 the light a dark
                        // surface still shows (added), y: how much a dark surface shows
};

Texture2D<float> g_srcSceneDepth : register(t0);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float3 ViewAt(int2 q, float stored)
{
    float z = kk_Unpack.x / max(kk_Unpack.y - stored, 1e-9);
    float2 uv = (float2(q) + 0.5) / float2(kk_Source.zw);
    return float3((kk_Ndc.xy * uv + kk_Ndc.zw) * z, z);
}

float3 ViewNear(int2 p, int2 q)
{
    int2 c = clamp(p, kk_Source.xy, kk_Source.xy + kk_Source.zw - 1);
    return ViewAt(c - kk_Source.xy, g_srcSceneDepth.Load(int3(c, 0)).x);
}

float4 ps_muzzle(float4 pos : SV_Position) : SV_Target0
{
    int2 p = int2(pos.xy);
    int2 q = p - kk_Source.xy;
    float stored = g_srcSceneDepth.Load(int3(p, 0)).x;
    if (any(q < 0) || any(q >= kk_Source.zw) || stored >= kk_Unpack.z)
        discard;
    float3 position = ViewAt(q, stored);
    float r2 = kk_Params.x * kk_Params.x;
    int count = int(kk_Unpack.w);
    bool any_near = false;
    for (int i = 0; i < count; ++i)
    {
        float3 d = kk_Light[i].xyz - position;
        any_near = any_near || dot(d, d) < r2;
    }
    if (!any_near)
        discard;
    // The normal from the depth beside the pixel: on each axis the side nearer in depth.
    float3 l = ViewNear(p, p - int2(1, 0)), r = ViewNear(p, p + int2(1, 0));
    float3 u = ViewNear(p, p - int2(0, 1)), b = ViewNear(p, p + int2(0, 1));
    float3 dx = abs(r.z - position.z) < abs(position.z - l.z) ? r - position : position - l;
    float3 dy = abs(b.z - position.z) < abs(position.z - u.z) ? b - position : position - u;
    float3 normal = cross(dy, dx);
    float len = length(normal);
    normal = len > 1e-9 ? normal / len : float3(0.0, 0.0, -1.0);
    if (dot(normal, position) > 0.0)
        normal = -normal;  // toward the eye

    float reach = 0.0;
    for (int j = 0; j < count; ++j)
    {
        float3 toLight = kk_Light[j].xyz - position;
        float d2 = dot(toLight, toLight);
        if (d2 >= r2)
            continue;
        float window = 1.0 - d2 / r2;
        float fall = window * window / (1.0 + d2 * kk_Params.y);
        float facing = saturate((dot(normal, toLight / sqrt(max(d2, 1e-6))) + kk_Params.z) / (1.0 + kk_Params.z));
        reach += fall * facing * kk_Light[j].w;
    }
    if (reach <= 0.0)
        discard;
    // The picture is about gamma 2: light that adds in linear raises it by the square root.
    if (kk_Pass.x > 0.5)
        return float4(sqrt(kk_Colour.rgb * reach * kk_Pass.y), 0.0);  // a dark surface's own reflectance
    return float4(sqrt(1.0 + kk_Colour.rgb * reach * kk_Params.w) - 1.0, 0.0);
}
