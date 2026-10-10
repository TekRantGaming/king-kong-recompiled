// Clears a rectangle of the bound render target and depth / stencil buffer:
// the scissor is the rectangle, the triangle covers the viewport at the clear
// depth, the pixel shader writes the clear colour (the pipeline masks colour
// out for depth-only clears). Stencil goes through the stencil test's replace
// with the reference value.

struct ClearConstants {
  float4 color;
  float depth;
  float3 pad;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<ClearConstants> c : register(b0);

float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 t = float2((id << 1) & 2, id & 2);
  // Covers the viewport either way up.
  return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, c.depth, 1.0);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target0 {
  return c.color;
}
