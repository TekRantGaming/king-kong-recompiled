// Screen-space ambient occlusion for the native renderer: the technique of the
// Xenos plugin (tools/rexglue-patches 0004-0007, ambient_occlusion.cs.hlsl) as
// full-screen pixel passes over ordinary host textures instead of compute
// passes over the EDRAM buffer.
//
// Once a frame, at half resolution (of whatever size the host targets have, so
// it follows --native_render_scale):
//   ps_copy     (at the depth resolve) the scene depth, 0..1 as the game
//               stores it, into an image of our own: the resolve may clear it
//   ps_depth    that depth to linear distance in near-plane units (R32F, half
//               size)
//   ps_ao       10 samples around a normal rebuilt from that depth
//   ps_blur     depth-aware 9-tap blur, horizontal then vertical
//   ps_upsample edge-aware 4-tap upscale to full size, with the distance fade
//               and the game's fog (x: multiplier, y: fog colour to add back)
// At each scene copy:
//   ps_apply    colour * ao + fog colour * fog * (1 - ao) into a copy of the
//               scene colour, which the resolve then reads
//
// Bindings (NVRHI's Vulkan offsets: t at 0, s at 128, b at 256): t0..t2 the
// inputs of each pass, s0 a linear clamp sampler, b0 push constants (root
// constants on D3D12).

struct AoConstants {
  uint4 size;      // x, y: full size; z, w: half size
  float4 view;     // x: tan(half horizontal FOV), y: tan(half vertical FOV), z: focal length in half-size
                   // pixels, w: fade distance (near-plane units)
  float4 tuning;   // x: radius (near-plane units), y: bias, z: intensity, w: render scale (host / guest)
  float4 fog_norm; // xyz: the game's g_vFogNormalizing, w: 1 when a fog pass was seen this frame
  float4 fog_par;  // x: near over distance, y: density, z: inverse distance, w: mode (1 on, 2 AO only)
  float4 fog_col;  // rgb: fog colour
  float4 blur;     // xy: tap direction (1,0) or (0,1)
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<AoConstants> c : register(b0);

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
SamplerState linear_clamp : register(s0);

// Fits 16-bit floats; anything this far is past the distance fade anyway.
static const float kSky = 60000.0;

// One triangle covering the viewport (the viewport is the target).
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 t = float2((id << 1) & 2, id & 2);
  return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

// Distance from the camera in units of the near plane (the far plane is much
// further than the near one); kSky for the sky.
float LinearDepth(float z) {
  return z >= 0.99999 ? kSky : min(1.0 / max(1.0 - z, 1.0 / 65536.0), 50000.0);
}

// How much of the game's own fog covers a pixel at this depth, the way its fog
// pass works it out (without its softening for looking up or down, which only
// ever lowers the fog). The fog is drawn before the scene copy: a pixel there
// is scene * (1 - fog) + fog colour * fog, and only the scene part should be
// darkened.
float FogAmount(float z) {
  if (c.fog_norm.w <= 0.0) {
    return 0.0;
  }
  float inverse_depth = c.fog_norm.x * min(z, c.fog_norm.z) - c.fog_norm.y;
  if (inverse_depth <= 0.0) {
    return 1.0;
  }
  float fog = saturate((1.0 / inverse_depth) * c.fog_par.z - c.fog_par.x);
  return saturate(fog * c.fog_par.y);
}

int2 HalfMax() { return int2(c.size.zw) - 1; }

float3 ViewPosHalf(int2 q) {
  q = clamp(q, int2(0, 0), HalfMax());
  float zl = t0.Load(int3(q, 0)).x;
  float2 ndc = (float2(q) + 0.5) / float2(c.size.zw) * 2.0 - 1.0;
  return float3(ndc.x * c.view.x * zl, -ndc.y * c.view.y * zl, zl);
}

float Noise(uint2 p) { return frac(52.9829189 * frac(0.06711056 * float(p.x) + 0.00583715 * float(p.y))); }

// t0: the depth target. Copies it (texel for texel).
float4 ps_copy(float4 pos : SV_Position) : SV_Target0 {
  return float4(t0.Load(int3(int2(pos.xy), 0)).x, 0, 0, 0);
}

// t0: the copied depth. Writes the linear depth of the half-size pixel's
// top-left full-size pixel.
float4 ps_depth(float4 pos : SV_Position) : SV_Target0 {
  int2 p = min(int2(pos.xy) * 2, int2(c.size.xy) - 1);
  return float4(LinearDepth(t0.Load(int3(p, 0)).x), 0, 0, 0);
}

// t0: linear depth (half size). Writes the occlusion, 1 = none.
float4 ps_ao(float4 pos : SV_Position) : SV_Target0 {
  int2 p = int2(pos.xy);
  float ao = 1.0;
  float3 P = ViewPosHalf(p);
  if (P.z < kSky) {
    float3 pr = ViewPosHalf(p + int2(1, 0)), pl = ViewPosHalf(p - int2(1, 0));
    float3 pd = ViewPosHalf(p + int2(0, 1)), pu = ViewPosHalf(p - int2(0, 1));
    float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0.0) {
      N = -N;
    }
    float R = c.tuning.x;
    float radius_px = clamp(R * c.view.z / P.z, 1.0, float(c.size.w) * 0.05);
    const int kSamples = 10;
    float rotation = Noise(uint2(p)) * 6.2831853;
    float sum = 0.0;
    [unroll]
    for (int i = 0; i < kSamples; ++i) {
      float a = rotation + float(i) * 2.3999632;  // golden angle
      float r = sqrt((float(i) + 0.5) / float(kSamples)) * radius_px;
      int2 q = p + int2(round(float2(cos(a), sin(a)) * r));
      if (any(q < int2(0, 0)) || any(q >= int2(c.size.zw))) {
        continue;
      }
      float3 v = ViewPosHalf(q) - P;
      float vv = dot(v, v);
      float falloff = saturate(1.0 - vv / (R * R));
      sum += max(0.0, dot(v, N) * rsqrt(vv + 1e-6) - c.tuning.y) * falloff;
    }
    ao = saturate(1.0 - c.tuning.z * sum / float(kSamples));
  }
  return float4(ao, 0, 0, 0);
}

// t0: linear depth, t1: the occlusion to blur. One direction (blur.xy); the
// taps are a pixel apart at render scale 1 and a guest pixel apart above it, so
// the blur covers the same part of the picture at any scale.
float4 ps_blur(float4 pos : SV_Position) : SV_Target0 {
  float2 p = floor(pos.xy);
  float step_px = max(c.tuning.w, 1.0);
  float zc = t0.Load(int3(int2(p), 0)).x;
  float total = 0.0, weight = 0.0;
  [unroll]
  for (int i = -4; i <= 4; ++i) {
    float2 q = p + c.blur.xy * (float(i) * step_px);
    int2 qi = clamp(int2(round(q)), int2(0, 0), HalfMax());
    float w = exp(-abs(t0.Load(int3(qi, 0)).x - zc) / (zc * 0.02 + 0.0001)) * (5.0 - abs(float(i)));
    total += t1.SampleLevel(linear_clamp, (q + 0.5) / float2(c.size.zw), 0).x * w;
    weight += w;
  }
  return float4(weight > 0.0 ? total / weight : 1.0, 0, 0, 0);
}

// t0: the copied depth (full size), t1: linear depth (half size), t2: the
// blurred occlusion. Writes x: the multiplier, y: the fog colour to add back.
float4 ps_upsample(float4 pos : SV_Position) : SV_Target0 {
  int2 p = int2(pos.xy);
  float z = t0.Load(int3(p, 0)).x;
  float zc = LinearDepth(z);
  float ao = 1.0;
  float fog = 0.0;
  if (zc < kSky) {
    float2 hp = (float2(p) + 0.5) * (float2(c.size.zw) / float2(c.size.xy)) - 0.5;
    int2 base = int2(floor(hp));
    float2 f = hp - float2(base);
    float total = 0.0, weight = 0.0;
    [unroll]
    for (int j = 0; j < 4; ++j) {
      int2 o = int2(j & 1, j >> 1);
      int2 q = clamp(base + o, int2(0, 0), HalfMax());
      float bilinear = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
      float w = (bilinear + 0.001) * exp(-abs(t1.Load(int3(q, 0)).x - zc) / (zc * 0.02 + 0.0001));
      total += t2.Load(int3(q, 0)).x * w;
      weight += w;
    }
    ao = weight > 0.0 ? total / weight : 1.0;
    float fade_distance = c.view.w > 0.0 ? c.view.w : 2000.0;
    ao = lerp(1.0, ao, saturate(1.0 - zc / fade_distance));
    fog = FogAmount(z);
  }
  // Squared: the look first tested had the AO applied twice by mistake (to the
  // scene and again to the final image), and strength 1 was liked.
  ao *= ao;
  // color * ao + fog color * fog * (1 - ao) darkens only the scene part.
  return float4(ao, fog * (1.0 - ao), 0, 0);
}

// t0: the scene colour, t1: the full-size occlusion. Pixels outside the
// occlusion's area (the part of the target below the picture) pass through.
float4 ps_apply(float4 pos : SV_Position) : SV_Target0 {
  int2 p = int2(pos.xy);
  float4 color = t0.Load(int3(p, 0));
  if (any(p >= int2(c.size.xy))) {
    return color;
  }
  float2 a = t1.Load(int3(p, 0)).xy;
  if (c.fog_par.w >= 1.5) {
    // Show the AO only: the darkening as it ends up on this pixel.
    float g = saturate(a.x + a.y);
    return float4(g, g, g, color.a);
  }
  float3 rgb = color.rgb * a.x + saturate(c.fog_col.rgb) * a.y;
  return float4(saturate(rgb), color.a);
}
