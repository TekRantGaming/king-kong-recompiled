// Ambient occlusion prototype for the King Kong port (loaded by REX_DEV_AO).
//
// Once a frame, at half resolution: PrepareDepth -> ComputeAO -> BlurH ->
// BlurV, then Upsample to a full-resolution AO image. ApplyAO then multiplies
// it into the scene color for each scene copy.
//
// The EDRAM buffer (32bpp, 1x MSAA) has tiles of tile_width x tile_height
// pixels (80x16 times the resolution scale), row-major within a tile,
// pitch_tiles per row, 2048 tiles in all. Depth tiles have their left and
// right halves swapped. Depth is D24S8 (depth in the upper 24 bits); color is
// 8:8:8:8 with red in the low byte.

cbuffer Constants : register(b0) {
  uint color_base;
  uint depth_base;
  uint pitch_tiles;
  uint width;  // full resolution
  uint height;
  uint tile_width;
  uint tile_height;
  uint view;  // 0 off, 1 depth, 2-4 AO only (sets 1-3), 5-7 applied (sets 1-3)
  float4 p0;  // w: fade distance (near-plane units)
  float4 p1;  // x: tan(horizontal half FOV), w: view stamp
};

RWByteAddressBuffer edram : register(u0);
RWTexture2D<float> depth_half : register(u1);  // linear depth, half resolution
RWTexture2D<float> ao_a : register(u2);        // half resolution
RWTexture2D<float> ao_b : register(u3);        // half resolution
RWTexture2D<float> ao_full : register(u4);     // full resolution

// Fits 16-bit floats; anything this far is past the distance fade anyway.
static const float kSky = 60000.0;

uint EdramAddress(uint2 p, uint base, bool is_depth) {
  uint2 tile_size = uint2(tile_width, tile_height);
  uint2 tile = p / tile_size;
  uint2 in_tile = p - tile * tile_size;
  if (is_depth) {
    uint half_width = tile_width >> 1;
    in_tile.x = in_tile.x < half_width ? in_tile.x + half_width : in_tile.x - half_width;
  }
  uint tile_index = (base + tile.y * pitch_tiles + tile.x) & 2047u;
  return (tile_index * tile_width * tile_height + in_tile.y * tile_width + in_tile.x) * 4u;
}

// Distance from the camera in units of the near plane (assuming the far plane
// is much further than the near one); kSky for the sky.
float LinearDepthAt(uint2 p) {
  float z = float(edram.Load(EdramAddress(p, depth_base, true)) >> 8) * (1.0 / 16777215.0);
  return z >= 0.99999 ? kSky : min(1.0 / max(1.0 - z, 1.0 / 65536.0), 50000.0);
}

uint2 HalfSize() { return uint2(width, height) >> 1; }
float TanX() { return p1.x > 0.0 ? p1.x : 0.684; }
float TanY() { return TanX() * float(height) / float(width); }

// Tuning sets: x radius (near-plane units), y bias, z intensity.
float3 Set() {
  uint i = view >= 5u ? view - 5u : (view >= 2u ? view - 2u : 0u);
  if (i == 0u) return float3(6.0, 0.25, 2.5);
  if (i == 1u) return float3(8.0, 0.3, 3.5);
  return float3(12.0, 0.3, 3.0);
}

float3 ViewPosHalf(int2 q) {
  uint2 size = HalfSize();
  q = clamp(q, int2(0, 0), int2(size) - 1);
  float zl = depth_half[q];
  float2 ndc = (float2(q) + 0.5) / float2(size) * 2.0 - 1.0;
  return float3(ndc.x * TanX() * zl, -ndc.y * TanY() * zl, zl);
}

float Noise(uint2 p) { return frac(52.9829189 * frac(0.06711056 * float(p.x) + 0.00583715 * float(p.y))); }

[numthreads(8, 8, 1)]
void PrepareDepth(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= HalfSize())) {
    return;
  }
  depth_half[id.xy] = LinearDepthAt(id.xy * 2u);
}

[numthreads(8, 8, 1)]
void ComputeAO(uint3 id : SV_DispatchThreadID) {
  uint2 size = HalfSize();
  if (any(id.xy >= size)) {
    return;
  }
  int2 p = int2(id.xy);
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
    float3 set = Set();
    float R = set.x;
    float focal = float(size.y) * 0.5 / TanY();
    float radius_px = clamp(R * focal / P.z, 1.0, float(size.y) * 0.05);
    const int kSamples = 10;
    float rotation = Noise(id.xy) * 6.2831853;
    float sum = 0.0;
    [unroll]
    for (int i = 0; i < kSamples; ++i) {
      float a = rotation + float(i) * 2.3999632;  // golden angle
      float r = sqrt((float(i) + 0.5) / float(kSamples)) * radius_px;
      int2 q = p + int2(round(float2(cos(a), sin(a)) * r));
      if (any(q < int2(0, 0)) || any(q >= int2(size))) {
        continue;
      }
      float3 v = ViewPosHalf(q) - P;
      float vv = dot(v, v);
      float falloff = saturate(1.0 - vv / (R * R));
      sum += max(0.0, dot(v, N) * rsqrt(vv + 1e-6) - set.y) * falloff;
    }
    ao = saturate(1.0 - set.z * sum / float(kSamples));
  }
  ao_a[id.xy] = ao;
}

// Depth-aware 9-tap blur, one direction at a time.
float BlurAt(RWTexture2D<float> source, int2 p, int2 step) {
  uint2 size = HalfSize();
  float zc = depth_half[p];
  float total = 0.0, weight = 0.0;
  [unroll]
  for (int i = -4; i <= 4; ++i) {
    int2 q = clamp(p + step * i, int2(0, 0), int2(size) - 1);
    float w = exp(-abs(depth_half[q] - zc) / (zc * 0.02 + 0.0001)) * (5.0 - abs(float(i)));
    total += source[q] * w;
    weight += w;
  }
  return weight > 0.0 ? total / weight : 1.0;
}

[numthreads(8, 8, 1)]
void BlurH(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= HalfSize())) {
    return;
  }
  ao_b[id.xy] = BlurAt(ao_a, int2(id.xy), int2(1, 0));
}

[numthreads(8, 8, 1)]
void BlurV(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= HalfSize())) {
    return;
  }
  ao_a[id.xy] = BlurAt(ao_b, int2(id.xy), int2(0, 1));
}

// Edge-aware upsampling to full resolution, with the distance fade.
[numthreads(8, 8, 1)]
void Upsample(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) {
    return;
  }
  float zc = LinearDepthAt(id.xy);
  float ao = 1.0;
  if (zc < kSky) {
    uint2 size = HalfSize();
    float2 hp = (float2(id.xy) + 0.5) * 0.5 - 0.5;
    int2 base = int2(floor(hp));
    float2 f = hp - float2(base);
    float total = 0.0, weight = 0.0;
    [unroll]
    for (int j = 0; j < 4; ++j) {
      int2 o = int2(j & 1, j >> 1);
      int2 q = clamp(base + o, int2(0, 0), int2(size) - 1);
      float bilinear = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
      float w = (bilinear + 0.001) * exp(-abs(depth_half[q] - zc) / (zc * 0.02 + 0.0001));
      total += ao_a[q] * w;
      weight += w;
    }
    ao = weight > 0.0 ? total / weight : 1.0;
    float fade_distance = p0.w > 0.0 ? p0.w : 2000.0;
    ao = lerp(1.0, ao, saturate(1.0 - zc / fade_distance));
  }
  // Squared: the look first tested had the AO applied twice by mistake (to
  // the scene and again to the final image), and strength 1 was liked.
  ao_full[id.xy] = ao * ao;
}

[numthreads(8, 8, 1)]
void ApplyAO(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) {
    return;
  }
  uint address = EdramAddress(id.xy, color_base, false);
  uint color = edram.Load(address);
  if (view >= 2u && view <= 4u) {
    uint g = uint(saturate(ao_full[id.xy]) * 255.0 + 0.5);
    color = (color & 0xFF000000u) | (g << 16) | (g << 8) | g;
  } else if (view == 1u) {
    float zc = LinearDepthAt(id.xy);
    uint g = uint(saturate(log2(zc) / 16.0) * 255.0 + 0.5);
    color = (color & 0xFF000000u) | (g << 16) | (g << 8) | g;
    if (id.x < width / 20u && id.y < height / 20u) {
      color = (color & 0xFF000000u) | 0xFFu;
    }
  } else if (view != 0u) {
    float ao = ao_full[id.xy];
    float3 c = float3(color & 0xFFu, (color >> 8) & 0xFFu, (color >> 16) & 0xFFu) * ao;
    uint3 ci = uint3(c + 0.5);
    color = (color & 0xFF000000u) | (ci.z << 16) | (ci.y << 8) | ci.x;
  }
  // View number stamp for test captures: view + 1 small squares, top right.
  uint stamp = max(height / 60u, 4u);
  if (p1.w > 0.0 && id.y < stamp && id.x >= width - (view + 1u) * stamp * 2u &&
      ((width - 1u - id.x) / stamp) % 2u == 0u) {
    color = (color & 0xFF000000u) | 0x00FFFFFFu;
  }
  edram.Store(address, color);
}
