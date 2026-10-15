// SMAA 1x, official v2.8, High preset. No history, jitter or temporal resolve.
struct SmaaConstants { float2 pixel_size; float2 padding; };
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<SmaaConstants> smaa_constants : register(b0);
#define SMAA_PIXEL_SIZE smaa_constants.pixel_size
#define SMAA_HLSL_4 1
#define SMAA_PRESET_HIGH 1
#include "../../thirdparty/smaa/SMAA.h"
Texture2D input0 : register(t0);
Texture2D input1 : register(t1);
Texture2D input2 : register(t2);

struct Varyings {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float2 pixel : TEXCOORD1;
  float4 offset[3] : TEXCOORD2;
};
Varyings Base(uint id) {
  Varyings o = (Varyings)0;
  o.uv = float2((id << 1) & 2, id & 2);
  o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
Varyings vs_edge(uint id : SV_VertexID) {
  Varyings o = Base(id);
  SMAAEdgeDetectionVS(o.position, o.position, o.uv, o.offset);
  return o;
}
float4 ps_edge(Varyings i) : SV_Target0 {
  return SMAALumaEdgeDetectionPS(i.uv, i.offset, input0);
}
Varyings vs_weight(uint id : SV_VertexID) {
  Varyings o = Base(id);
  SMAABlendingWeightCalculationVS(o.position, o.position, o.uv, o.pixel, o.offset);
  return o;
}
float4 ps_weight(Varyings i) : SV_Target0 {
  return SMAABlendingWeightCalculationPS(i.uv, i.pixel, i.offset, input0, input1, input2, int4(0, 0, 0, 0));
}
Varyings vs_blend(uint id : SV_VertexID) {
  Varyings o = Base(id);
  float4 offsets[2];
  SMAANeighborhoodBlendingVS(o.position, o.position, o.uv, offsets);
  o.offset[0] = offsets[0]; o.offset[1] = offsets[1];
  return o;
}
float4 ps_blend(Varyings i) : SV_Target0 {
  float4 offsets[2] = {i.offset[0], i.offset[1]};
  return SMAANeighborhoodBlendingPS(i.uv, offsets, input0, input1);
}
