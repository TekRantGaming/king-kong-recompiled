// King Kong native renderer: the lighting options' constant buffer and the water reflection hook
// (docs/lighting.md). The C++ side is include/kkshaders/lighting.h; keep the two in step.
//
// The translator puts this text into the water shaders it translates with the reflection hook
// (kkshaders/lighting.h, ShaderInfo::waterReflection): at the top of main, where control flow is
// still uniform (derivatives), kl_WaterTrace marches the reflected ray through the scene's view
// depth pyramid; where the shader fetches its reflection (the planar reflection at fetch constant 1,
// the environment cube at 2), kl_WaterReflection puts the scene's colour at the hit in place of what
// the game's texture gave, by how sure the hit is. Everything after (the game's fresnel, intensity,
// refraction, lighting and fog) is the game's own. The text is part of the hooked shaders' input
// hash, so a change here rebuilds them.
#ifndef KK_LIGHTING_HLSLI
#define KK_LIGHTING_HLSLI

cbuffer KKLighting : register(b5, space0)
{
    float4 kl_Shadow;        // x PCF radius (guest texels), y PCSS search radius, z PCSS penumbra scale, w shadow-map scale
    float4 kl_ShadowShape;   // x receiver plane bias limit, y / z PCSS radius range (guest texels), w 1 for PCSS
    uint4 kl_SsrTextures;    // x scene colour slot, y view depth pyramid slot, z its mip count, w 1 when valid
    float4 kl_SsrRect;       // xy picture origin (host pixels), zw picture size
    uint4 kl_SsrSampler;     // x linear clamp sampler slot, y the colour's mip count
    float4 kl_SsrDepth;      // view z = x / (y - stored depth); z near, w far
    float4 kl_SsrScreen;     // view xy = (picture uv * xy + zw) * view z
    float4 kl_SsrParams;     // x strength, y longest ray (view depths), z thickness, w steps
    float4 kl_SsrMisc;       // x edge fade, y roughness from the normal map, z normal map tilt, w F0 (ocean)
};

struct KLSsr
{
    float2 pixelUv;    // the pixel in the picture (0-1)
    float2 uv;         // the hit in the picture
    float confidence;  // 0: no hit (the game's own reflection stays)
    float lod;         // mip of the colour copy (rough water)
    float fresnel;     // Schlick's, for shaders that have none of their own (the ocean)
};

float kl_ViewDepth(float stored)
{
    return kl_SsrDepth.x / (kl_SsrDepth.y - stored);
}

float3 kl_ViewFromUv(float2 uv, float z)
{
    return float3((uv * kl_SsrScreen.xy + kl_SsrScreen.zw) * z, z);
}

float2 kl_UvFromView(float3 v)
{
    return (v.xy / v.z - kl_SsrScreen.zw) / kl_SsrScreen.xy;
}

// normalMap: the water's normal map's tangent-space x y (-1..1) at the pixel, 0 without one.
KLSsr kl_WaterTrace(float4 pos, float2 normalMap)
{
    KLSsr r = (KLSsr)0;
    r.pixelUv = (pos.xy - kl_SsrRect.xy) / kl_SsrRect.zw;
    // Derivatives first, in uniform control flow (the caller is the top of main).
    float z0 = kl_ViewDepth(pos.z);
    float3 p = kl_ViewFromUv(r.pixelUv, z0);
    float3 dx = ddx(p), dy = ddy(p);
    if (kl_SsrTextures.w == 0u || !(z0 > 0.0) || z0 >= kl_SsrDepth.w)
        return r;
    // The surface's own normal from its geometry (flat water is exact), facing the eye, tilted by
    // the normal map in the screen's frame.
    float3 n = normalize(cross(dy, dx));
    if (dot(n, p) > 0.0)
        n = -n;
    float3 tangent = normalize(dx), bitangent = normalize(cross(n, tangent));
    n = normalize(n + (tangent * normalMap.x + bitangent * normalMap.y) * kl_SsrMisc.z);
    float3 v = normalize(p);
    float3 refl = reflect(v, n);
    float cosine = saturate(dot(-v, n));
    r.fresnel = kl_SsrMisc.w + (1.0 - kl_SsrMisc.w) * pow(1.0 - cosine, 5.0);
    // The ray's end: this many view depths along it, short of the near plane.
    float rayLength = kl_SsrParams.y * z0;
    float nearZ = kl_SsrDepth.z * 1.01;
    if (p.z + refl.z * rayLength < nearZ)
        rayLength = refl.z < 0.0 ? (nearZ - p.z) / refl.z : rayLength;
    if (rayLength <= 0.0)
        return r;
    float3 p1 = p + refl * rayLength;
    float2 size = kl_SsrRect.zw;
    float2 s0 = r.pixelUv * size, s1 = kl_UvFromView(p1) * size;  // mip 0 texels
    float2 d = s1 - s0;
    float span = max(abs(d.x), abs(d.y));
    if (span < 1.0)
        return r;
    // Where the ray leaves the picture.
    float tMax = 1.0;
    [unroll] for (uint a = 0; a < 2; a++)
    {
        if (d[a] > 0.0) tMax = min(tMax, (size[a] - 0.01 - s0[a]) / d[a]);
        if (d[a] < 0.0) tMax = min(tMax, (0.01 - s0[a]) / d[a]);
    }
    float k0 = 1.0 / p.z, k1 = 1.0 / p1.z;
    float2 inv = 1.0 / select(abs(d) > 1e-6, d, 1e-6);
    Texture2D<float4> pyramid = kk_Tex2D[kl_SsrTextures.y];
    int maxLevel = int(kl_SsrTextures.z) - 1;
    float eps = 0.02 / span;
    float t = 1.0 / span;  // a texel away from the start
    int level = 0;
    bool hit = false;
    uint steps = uint(kl_SsrParams.w);
    [loop] for (uint i = 0; i < steps && t < tMax; i++)
    {
        float2 s = s0 + d * t;
        float cellSize = float(1u << uint(level));
        float2 cell = floor(s / cellSize);
        float2 bound = (cell + select(d >= 0.0, 1.0, 0.0)) * cellSize;
        float2 tb = (bound - s0) * inv;
        float tExit = min(min(tb.x, tb.y), tMax);
        tExit = max(tExit, t + eps);
        uint2 mipSize;
        uint mips;
        pyramid.GetDimensions(uint(level), mipSize.x, mipSize.y, mips);
        int2 c = clamp(int2(cell), int2(0, 0), int2(mipSize) - 1);
        float zc = pyramid.Load(int3(c, level)).x;
        float zA = 1.0 / lerp(k0, k1, t), zB = 1.0 / lerp(k0, k1, tExit);
        if (max(zA, zB) < zc)
        {
            // In front of everything this texel covers: skip it, and look coarser.
            t = tExit + eps;
            level = min(level + 1, maxLevel);
            continue;
        }
        if (level > 0)
        {
            level--;
            continue;
        }
        if (min(zA, zB) <= zc * (1.0 + kl_SsrParams.z))
        {
            hit = true;
            // A ray parallel to the view plane (k1 == k0) or a zero depth has no crossing to solve for:
            // keep the texel's entry point.
            float dk = k1 - k0;
            if (abs(dk) > 1e-6 && zc != 0.0)
                t = clamp((1.0 / zc - k0) / dk, t, tExit);
            break;
        }
        t = tExit + eps;  // behind a thin object: go on behind it
    }
    if (!hit)
        return r;
    r.uv = (s0 + d * t) / size;
    float2 edge = saturate(min(r.uv, 1.0 - r.uv) / max(kl_SsrMisc.x, 1e-4));
    float fadeFar = saturate((1.0 - t) * 4.0);       // the last quarter of the ray fades
    float facing = saturate(1.0 + refl.z * 2.0);  // rays back towards the eye fade
    r.confidence = edge.x * edge.y * fadeFar * facing;
    r.lod = min(length(normalMap) * kl_SsrMisc.y, float(kl_SsrSampler.y) - 1.0);
    return r;
}

// At the reflection fetch: the scene at the hit in place of the game's texel, by the hit's confidence.
// gameUv: the coordinate the game fetched its planar reflection at (its ripples move the hit the
// same way); planar: that fetch (its alpha says where the planar reflection has nothing).
float4 kl_WaterReflection(float4 game, KLSsr s, float2 gameUv, bool planar)
{
    float a = s.confidence * kl_SsrParams.x;
    if (!(a > 0.0))
        return game;
    float2 uv = s.uv;
    if (planar)
        uv += clamp(gameUv - s.pixelUv, -0.05, 0.05);
    float3 c = kk_Tex2D[kl_SsrTextures.x].SampleLevel(kk_Sampler[kl_SsrSampler.x], saturate(uv), s.lod).rgb;
    float4 o = game;
    o.rgb = lerp(game.rgb, c, a);
    if (planar)
        o.a = lerp(game.a, 0.0, a);
    return o;
}

// The ocean (no reflection of its own in its pixel shader): the scene at the hit, by fresnel.
float4 kl_OceanReflection(float4 color, KLSsr s)
{
    float a = s.confidence * s.fresnel * kl_SsrParams.x;
    if (!(a > 0.0))
        return color;
    float3 c = kk_Tex2D[kl_SsrTextures.x].SampleLevel(kk_Sampler[kl_SsrSampler.x], saturate(s.uv), s.lod).rgb;
    return float4(lerp(color.rgb, c, a), color.a);
}

#endif
