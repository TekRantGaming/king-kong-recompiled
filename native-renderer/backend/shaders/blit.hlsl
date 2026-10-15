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
  uint4 flags;     // x: 1 = map r, g, b through the gamma ramp, 2 = depth to D24S8 bytes, 4 = back,
                   // 16 = FXAA (y: edge search steps), 32 = CAS (y: level 1..3)
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<BlitConstants> c : register(b0);

Texture2D<float4> source : register(t0);
Texture2D<float4> ramp : register(t1);

#include "cas.hlsli"

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


// ---------------------------------------------------------------- FXAA
// flags.x & 16: fast approximate anti-aliasing of the source rectangle (the port's swap_post_effect
// "fxaa"; flags.y = the number of edge search steps, 12 or 24 for "fxaa_extreme"). The luma-based
// method of Timothy Lottes' FXAA 3.11 (quality preset): find the local contrast, skip flat areas,
// pick the edge's orientation, walk along it both ways to its ends, and blend the pixel with its
// neighbour across the edge by how close it is to the nearer end, plus a sub-pixel term. Everything
// is read with Load and a bilinear helper, so the blit's layout (no sampler) stays.

float FxaaLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 FxaaTexel(int2 p) {
  p = clamp(p, c.src_rect.xy, c.src_rect.xy + c.src_rect.zw - 1);
  return source.Load(int3(p, 0));
}

// Bilinear read at a position in texels (texel centres at +0.5).
float4 FxaaSample(float2 pos) {
  float2 q = pos - 0.5;
  int2 i = int2(floor(q));
  float2 f = q - float2(i);
  float4 a = lerp(FxaaTexel(i), FxaaTexel(i + int2(1, 0)), f.x);
  float4 b = lerp(FxaaTexel(i + int2(0, 1)), FxaaTexel(i + int2(1, 1)), f.x);
  return lerp(a, b, f.y);
}

float FxaaLumaAt(float2 pos) { return FxaaLuma(FxaaSample(pos).rgb); }

float4 Fxaa(int2 p, uint steps) {
  const float4 centre = FxaaTexel(p);
  const float lm = FxaaLuma(centre.rgb);
  const float ln = FxaaLuma(FxaaTexel(p + int2(0, -1)).rgb);
  const float ls = FxaaLuma(FxaaTexel(p + int2(0, 1)).rgb);
  const float le = FxaaLuma(FxaaTexel(p + int2(1, 0)).rgb);
  const float lw = FxaaLuma(FxaaTexel(p + int2(-1, 0)).rgb);
  const float hi = max(max(max(ln, ls), max(le, lw)), lm);
  const float lo = min(min(min(ln, ls), min(le, lw)), lm);
  const float range = hi - lo;
  // Flat enough: leave it (FXAA 3.11's lowest thresholds: relative 0.063, absolute 0.0312; the Xenos
  // plugin's FXAA changes about four times the pixels the default thresholds do).
  if (range < max(0.0312, hi * 0.063)) return centre;
  const float lnw = FxaaLuma(FxaaTexel(p + int2(-1, -1)).rgb);
  const float lne = FxaaLuma(FxaaTexel(p + int2(1, -1)).rgb);
  const float lsw = FxaaLuma(FxaaTexel(p + int2(-1, 1)).rgb);
  const float lse = FxaaLuma(FxaaTexel(p + int2(1, 1)).rgb);
  // Sub-pixel aliasing: how much the pixel stands out from its 3x3 average.
  const float average = (2.0 * (ln + ls + le + lw) + lnw + lne + lsw + lse) / 12.0;
  float sub = saturate(abs(average - lm) / range);
  sub = smoothstep(0.0, 1.0, sub);
  sub = sub * sub * 0.75;
  // Orientation: the larger second difference across rows or across columns.
  const float edge_h = abs(lnw + lne - 2.0 * ln) + 2.0 * abs(lw + le - 2.0 * lm) + abs(lsw + lse - 2.0 * ls);
  const float edge_v = abs(lnw + lsw - 2.0 * lw) + 2.0 * abs(ln + ls - 2.0 * lm) + abs(lne + lse - 2.0 * le);
  const bool horizontal = edge_h >= edge_v;
  // The neighbour across the edge: the side with the larger gradient.
  float l1 = horizontal ? ln : lw;
  float l2 = horizontal ? ls : le;
  const float g1 = abs(l1 - lm), g2 = abs(l2 - lm);
  const bool first = g1 >= g2;
  const float gradient = 0.25 * max(g1, g2);
  float2 across = horizontal ? float2(0.0, 1.0) : float2(1.0, 0.0);
  if (first) across = -across;
  const float l_side = first ? l1 : l2;
  const float edge_luma = 0.5 * (lm + l_side);
  // Walk along the edge, half a texel towards the neighbour, both ways.
  const float2 along = horizontal ? float2(1.0, 0.0) : float2(0.0, 1.0);
  const float2 start = float2(p) + 0.5 + across * 0.5;
  float2 pos_n = start - along, pos_p = start + along;
  float d_n = FxaaLumaAt(pos_n) - edge_luma, d_p = FxaaLumaAt(pos_p) - edge_luma;
  bool done_n = abs(d_n) >= gradient, done_p = abs(d_p) >= gradient;
  for (uint k = 1u; k < steps && !(done_n && done_p); ++k) {
    const float stride = k < 5u ? 1.0 : (k < 9u ? 2.0 : 4.0);
    if (!done_n) {
      pos_n -= along * stride;
      d_n = FxaaLumaAt(pos_n) - edge_luma;
      done_n = abs(d_n) >= gradient;
    }
    if (!done_p) {
      pos_p += along * stride;
      d_p = FxaaLumaAt(pos_p) - edge_luma;
      done_p = abs(d_p) >= gradient;
    }
  }
  const float dist_n = horizontal ? (start.x - pos_n.x) : (start.y - pos_n.y);
  const float dist_p = horizontal ? (pos_p.x - start.x) : (pos_p.y - start.y);
  const bool nearer_n = dist_n < dist_p;
  const float dist = min(dist_n, dist_p);
  const float length = dist_n + dist_p;
  // The end found must lie on the other side of the edge's luma from the pixel, else no blend.
  const bool centre_below = lm < edge_luma;
  const bool good = ((nearer_n ? d_n : d_p) < 0.0) != centre_below;
  const float edge_offset = good ? 0.5 - dist / max(length, 1e-4) : 0.0;
  const float offset = max(edge_offset, sub);
  return FxaaSample(float2(p) + 0.5 + across * offset);
}

float4 ps_main(VSOut i) : SV_Target0 {
  int2 p = c.src_rect.xy + int2(floor(i.uv * float2(c.src_rect.zw)));
  p = clamp(p, c.src_rect.xy, c.src_rect.xy + c.src_rect.zw - 1);
  if (c.flags.x & 32u) return Cas(p, c.flags.y);
  if (c.flags.x & 16u) return float4(Fxaa(p, max(c.flags.y, 1u)).rgb, 1.0);
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
