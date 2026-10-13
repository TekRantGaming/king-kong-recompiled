// The water's screen-space reflections (docs/lighting.md, "Water reflections"): what the water
// shaders' reflection hook reads, made once a frame just before the first water draw, when the
// scene holds everything up to the fog pass and the opaque objects after it:
//
//  - cs_depth: the scene's depth target over the picture into view depth (world units, the far
//    plane for the cleared sky), mip 0 of an R32F pyramid;
//  - cs_depth_down: each smaller mip, the nearest of the 2x2 (3x3 at an odd edge) below, so a ray
//    in front of a mip texel is in front of everything it covers (the hierarchical march);
//  - cs_color: the scene's colour over the picture into an RGBA16F image with mips (the copy the
//    reflections sample), cs_color_down: its smaller mips (2x2 averages), for rough water.
//
// Compute passes with their own bindings (renderer shaders: compile_shaders.ps1's usual flags).

cbuffer SsrPrepare : register(b0)
{
    int4 g_Source;      // xy: the picture's origin in the target (host pixels), zw: its size
    float4 g_Depth;     // x, y: view z = x / (y - stored depth); z: stored depth at or beyond which it is sky; w: the far plane
    int4 g_Mip;         // x: the mip written (its source is x - 1), yz: its size, w: the source's size x
    int4 g_MipSource;   // xy: the source mip's size
};

Texture2D<float> t_depth : register(t0);
Texture2D<float4> t_color : register(t0);
Texture2D<float> t_mip_depth : register(t1);
Texture2D<float4> t_mip_color : register(t1);
RWTexture2D<float> u_depth : register(u0);
RWTexture2D<float4> u_color : register(u0);

[numthreads(8, 8, 1)]
void cs_depth(uint3 id : SV_DispatchThreadID)
{
    if (any(int2(id.xy) >= g_Source.zw))
        return;
    float d = t_depth.Load(int3(g_Source.xy + int2(id.xy), 0));
    float z = d >= g_Depth.z ? g_Depth.w : g_Depth.x / (g_Depth.y - d);
    u_depth[id.xy] = z > 0.0 ? min(z, g_Depth.w) : g_Depth.w;
}

[numthreads(8, 8, 1)]
void cs_depth_down(uint3 id : SV_DispatchThreadID)
{
    if (any(int2(id.xy) >= g_Mip.yz))
        return;
    int2 s = int2(id.xy) * 2;
    int2 last = g_MipSource.xy - 1;
    float z = t_mip_depth.Load(int3(min(s, last), 0));
    z = min(z, t_mip_depth.Load(int3(min(s + int2(1, 0), last), 0)));
    z = min(z, t_mip_depth.Load(int3(min(s + int2(0, 1), last), 0)));
    z = min(z, t_mip_depth.Load(int3(min(s + int2(1, 1), last), 0)));
    // An odd source keeps its last row and column in the last texel of this mip.
    if (id.x == uint(g_Mip.y - 1) && (g_MipSource.x & 1))
    {
        z = min(z, t_mip_depth.Load(int3(min(s + int2(2, 0), last), 0)));
        z = min(z, t_mip_depth.Load(int3(min(s + int2(2, 1), last), 0)));
    }
    if (id.y == uint(g_Mip.z - 1) && (g_MipSource.y & 1))
    {
        z = min(z, t_mip_depth.Load(int3(min(s + int2(0, 2), last), 0)));
        z = min(z, t_mip_depth.Load(int3(min(s + int2(1, 2), last), 0)));
        z = min(z, t_mip_depth.Load(int3(min(s + int2(2, 2), last), 0)));
    }
    u_depth[id.xy] = z;
}

[numthreads(8, 8, 1)]
void cs_color(uint3 id : SV_DispatchThreadID)
{
    if (any(int2(id.xy) >= g_Source.zw))
        return;
    u_color[id.xy] = t_color.Load(int3(g_Source.xy + int2(id.xy), 0));
}

[numthreads(8, 8, 1)]
void cs_color_down(uint3 id : SV_DispatchThreadID)
{
    if (any(int2(id.xy) >= g_Mip.yz))
        return;
    int2 s = int2(id.xy) * 2;
    int2 last = g_MipSource.xy - 1;
    float4 c = t_mip_color.Load(int3(min(s, last), 0)) + t_mip_color.Load(int3(min(s + int2(1, 0), last), 0)) +
               t_mip_color.Load(int3(min(s + int2(0, 1), last), 0)) + t_mip_color.Load(int3(min(s + int2(1, 1), last), 0));
    u_color[id.xy] = c * 0.25;
}
