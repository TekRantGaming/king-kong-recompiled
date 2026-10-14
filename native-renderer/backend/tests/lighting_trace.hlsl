// Synthetic horizontal water plane using the production reflection march.
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
float4 ps_main(float4 pos : SV_Position) : SV_Target0 {
    float2 uv = pos.xy / kl_SsrRect.zw;
    // y=-1, projection py=1: view z=1/(2v-1).
    float z = 1.0 / max(2.0 * uv.y - 1.0, 0.001);
    pos.z = kl_SsrDepth.y - kl_SsrDepth.x / z;
    KLSsr hit = kl_WaterTrace(pos, 0.0);
    float4 base = float4(0.05, 0.05, 0.05, 0.7);
    return uv.y > 0.55 ? kl_OceanReflection(base, hit) : base;
}
