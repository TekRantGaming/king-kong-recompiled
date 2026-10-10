// Milestone 3 placeholder pipeline for the game's draws. The vertex shader
// reads the position as three big-endian floats at pos_offset in each vertex of
// stream 0 (a raw buffer holding the guest vertex buffer bytes) and transforms
// it by vertex shader constants c0..c3 (assumed to be the world-view-projection
// matrix, rows in c0..c3, so mul(pos, m) matches dp4 oPos, v0, c0..c3). The
// pixel shader outputs a flat colour that differs per draw.

struct DrawConstants {
  row_major float4x4 wvp;
  float4 color;
  uint stride;
  uint pos_offset;
  uint2 pad;
};
cbuffer DrawConstantsBuffer : register(b0) { DrawConstants c; }

ByteAddressBuffer vertices : register(t0);

struct VSOut {
  float4 pos : SV_Position;
  float3 color : COLOR0;
};

uint bswap32(uint v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

VSOut vs_main(uint id : SV_VertexID) {
  uint3 raw = vertices.Load3(id * c.stride + c.pos_offset);
  float3 p = asfloat(uint3(bswap32(raw.x), bswap32(raw.y), bswap32(raw.z)));
  VSOut o;
  o.pos = mul(float4(p, 1.0), c.wvp);
  o.color = c.color.rgb;
  return o;
}

float4 ps_main(VSOut i) : SV_Target0 {
  return float4(i.color, 1.0);
}
