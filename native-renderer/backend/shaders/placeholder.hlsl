// Milestone 3 placeholder pipeline for the game's draws. The vertex shader
// reads the position as three big-endian floats at pos_offset in each vertex
// (a raw buffer holding the guest vertex bytes unchanged, the draw's vertices
// starting at vertex_base) and transforms it by the vertex shader constants
// c0..c3, taken as the rows of the world-view-projection matrix: the guest
// shader's dp4 oPos.x, v0, c0 (and so on) is mul(wvp, pos) with wvp's rows
// c0..c3. The pixel shader outputs a flat colour chosen per draw.
//
// The constants are push constants on Vulkan and root constants on D3D12
// (NVRHI's PushConstants binding at b0).

struct DrawConstants {
  row_major float4x4 wvp;
  float4 color;
  uint stride;
  uint pos_offset;
  uint vertex_base;
  uint pad;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<DrawConstants> c : register(b0);

ByteAddressBuffer vertices : register(t0);

struct VSOut {
  float4 pos : SV_Position;
  float3 color : COLOR0;
};

uint bswap32(uint v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

VSOut vs_main(uint id : SV_VertexID) {
  uint3 raw = vertices.Load3(c.vertex_base + id * c.stride + c.pos_offset);
  float3 p = asfloat(uint3(bswap32(raw.x), bswap32(raw.y), bswap32(raw.z)));
  VSOut o;
  o.pos = mul(c.wvp, float4(p, 1.0));
  o.color = c.color.rgb;
  return o;
}

float4 ps_main(VSOut i) : SV_Target0 {
  return float4(i.color, 1.0);
}
