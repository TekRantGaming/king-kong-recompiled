// Milestone 2 test triangle: three vertices from SV_VertexID, no buffers.
// Compiled by compile_shaders.ps1 into generated/triangle_*.h (DXIL for D3D12,
// SPIR-V for Vulkan).

struct VSOut {
  float4 pos : SV_Position;
  float3 color : COLOR0;
};

VSOut vs_main(uint id : SV_VertexID) {
  float2 positions[3] = {float2(-0.5, -0.5), float2(0.0, 0.5), float2(0.5, -0.5)};
  float3 colors[3] = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
  VSOut o;
  o.pos = float4(positions[id % 3], 0.5, 1.0);
  o.color = colors[id % 3];
  return o;
}

float4 ps_main(VSOut i) : SV_Target0 {
  return float4(i.color, 1.0);
}
