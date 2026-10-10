// Copies a rectangle of one texture into the bound render target, texel for
// texel, with a channel mapping: resolves that a plain copy cannot do (R and B
// exchanged for A8R8G8B8 destinations, 8:8:8:8 into a single channel, depth
// into an R32F texture) and the back buffer into the frame image at Present,
// through the display's gamma ramp.
//
// The viewport (and scissor) is the destination rectangle; output pixel i of
// it reads source texel src_rect.xy + i. Push constants on Vulkan, root
// constants on D3D12 (NVRHI's PushConstants at b0); the source at t0, the
// gamma ramp (256 x 1, 16-bit channels) at t1.

struct BlitConstants {
  int4 src_rect;   // x, y, width, height in source texels
  uint4 channels;  // output channel i = source channel channels[i]: 0-3 r g b a, 4 zero, 5 one
  uint4 flags;     // x: 1 = map r, g, b through the gamma ramp, 2 = depth to D24S8 bytes, 4 = back
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<BlitConstants> c : register(b0);

Texture2D<float4> source : register(t0);
Texture2D<float4> ramp : register(t1);

struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

// One triangle covering the viewport; uv 0..1 across it (y down).
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.uv = t;
  // D3D's clip space (+Y up) on both APIs: NVRHI's Vulkan viewports are
  // flipped to match.
  o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
  return o;
}

float Pick(float4 v, uint s) {
  return s < 4u ? v[s] : (s == 5u ? 1.0 : 0.0);
}

float4 ps_main(VSOut i) : SV_Target0 {
  int2 p = c.src_rect.xy + int2(floor(i.uv * float2(c.src_rect.zw)));
  p = clamp(p, c.src_rect.xy, c.src_rect.xy + c.src_rect.zw - 1);
  float4 v = source.Load(int3(p, 0));
  if (c.flags.x & 8u) {
    // A texture's X, Y, Z, W bytes (host R, G, B, A) as a D24S8 word: depth
    // from Y up.
    uint3 b = uint3(round(saturate(v.gba) * 255.0));
    v = float4(float(b.x | (b.y << 8) | (b.z << 16)) / 16777215.0, 0.0, 0.0, 1.0);
  }
  if (c.flags.x & 4u) {
    // 8:8:8:8 colour holding a D24S8 word's bytes (see below, sampled through
    // an A8R8G8B8 view) back to the depth: the resolve's word with R and B
    // exchanged, its top 24 bits.
    uint3 b = uint3(round(saturate(v.gra) * 255.0));
    v = float4(float(b.x | (b.y << 8) | (b.z << 16)) / 16777215.0, 0.0, 0.0, 1.0);
  }
  if (c.flags.x & 2u) {
    // A depth value as the bytes of the D24S8 word (depth << 8 | stencil),
    // X (the low byte, stencil) first.
    uint d = uint(round(saturate(v.r) * 16777215.0));
    v = float4(0.0, float(d & 255u), float((d >> 8) & 255u), float(d >> 16)) / 255.0;
  }
  float4 o = float4(Pick(v, c.channels.x), Pick(v, c.channels.y), Pick(v, c.channels.z),
                    Pick(v, c.channels.w));
  if (c.flags.x & 1u) {
    int3 index = int3(round(saturate(o.rgb) * 255.0));
    o.r = ramp.Load(int3(index.r, 0, 0)).r;
    o.g = ramp.Load(int3(index.g, 0, 0)).g;
    o.b = ramp.Load(int3(index.b, 0, 0)).b;
  }
  return o;
}
