// Synthetic horizontal water plane using the production reflection march.
// kl_Shadow (unused by the water) carries the test's own switch: x > 0 gives the water low-poly waves of
// that height (a triangulated height grid, flat per triangle, as the game's wave meshes are).
Texture2D<float4> testDepth : register(t0);
Texture2D<float4> testColor : register(t1);
SamplerState testSampler : register(s0);
#define KL_DEPTH_TEXTURE testDepth
#define KL_COLOR_TEXTURE testColor
#define KL_LINEAR_SAMPLER testSampler
#include "../../shaders/hlsl/kk_lighting.hlsli"
float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
}
// The wave grid's height at vertex (i, j).
float waveVertex(float2 ij) {
    return sin(ij.x * 2.1 + ij.y * 1.3) * cos(ij.y * 1.7 - ij.x * 0.6);
}
// Piecewise linear over triangles of a 0.3-unit grid in the plane's x z.
float waveHeight(float2 xz) {
    float2 g = xz / 0.3, c = floor(g), f = g - c;
    float h00 = waveVertex(c), h10 = waveVertex(c + float2(1, 0)), h01 = waveVertex(c + float2(0, 1)),
          h11 = waveVertex(c + float2(1, 1));
    return f.x + f.y < 1.0 ? h00 + (h10 - h00) * f.x + (h01 - h00) * f.y
                           : h11 + (h01 - h11) * (1.0 - f.x) + (h10 - h11) * (1.0 - f.y);
}
float4 ps_main(float4 pos : SV_Position) : SV_Target0 {
    float2 uv = pos.xy / kl_SsrRect.zw;
    // y=-1, projection py=1: view z=1/(2v-1); with waves the ray meets y = -1 + height (a few
    // fixed-point steps; the slopes are small).
    float s = max(2.0 * uv.y - 1.0, 0.001);
    float z = 1.0 / s;
    [unroll] for (int i = 0; i < 6; i++)
        z = (1.0 - kl_Shadow.x * waveHeight(float2((2.0 * uv.x - 1.0) * z, z))) / s;
    pos.z = kl_SsrDepth.y - kl_SsrDepth.x / z;
    KLSsr hit = kl_WaterTrace(pos, 0.0);
    float4 base = float4(0.05, 0.05, 0.05, 0.7);
    return uv.y > 0.55 ? kl_OceanReflection(base, hit) : base;
}
