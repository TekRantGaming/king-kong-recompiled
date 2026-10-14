// Portable nearest-neighbour D32S8 migration. No optional stencil-reference
// export feature: one depth pass, then eight masked stencil replacement passes.
struct Params { float2 ratio; uint mask; uint pad; };
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<Params> c : register(b0);
Texture2D<float> old_depth : register(t0);
// NVRHI maps Vulkan's stencil aspect to the D3D stencil-in-green convention.
Texture2D<uint2> old_stencil : register(t1);
float ps_main(float4 pos : SV_Position) : SV_Depth {
  int2 p = int2(floor(pos.xy * c.ratio));
  if (c.mask != 0 && (old_stencil.Load(int3(p, 0)).g & c.mask) == 0) discard;
  return old_depth.Load(int3(p, 0));
}
// Offscreen pixel tests use this entry to read depth and every stencil value.
float ps_probe(float4 pos : SV_Position) : SV_Target0 {
  int2 p = int2(pos.xy);
  return c.mask ? float(old_stencil.Load(int3(p, 0)).g) : old_depth.Load(int3(p, 0));
}
