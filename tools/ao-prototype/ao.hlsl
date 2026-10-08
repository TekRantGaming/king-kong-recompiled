// Ambient occlusion prototype for the King Kong port (loaded by REX_DEV_AO).
// Reads the scene depth and writes the scene color in the plugin's EDRAM
// buffer (32bpp, 1x MSAA): tiles of tile_width x tile_height pixels (80x16
// times the resolution scale), row-major within a tile, pitch_tiles per row,
// 2048 tiles in all. Depth is D24S8 (depth in the upper 24 bits); color is
// 8:8:8:8.

cbuffer Constants : register(b0) {
  uint color_base;
  uint depth_base;
  uint pitch_tiles;
  uint width;
  uint height;
  uint tile_width;
  uint tile_height;
  uint view;  // 0 off, 1 depth, 2-4 AO only (sets A-C), 5-7 applied (sets A-C)
  float4 p0;  // x radius (near-plane units), y intensity, z bias, w fade distance (near units)
  float4 p1;  // x tan(horizontal half FOV), y power, z blur radius (pixels at 1x), w view stamp
};

RWByteAddressBuffer edram : register(u0);
RWByteAddressBuffer ao_buffer : register(u1);

uint EdramAddress(uint2 p, uint base, bool is_depth) {
  uint2 tile_size = uint2(tile_width, tile_height);
  uint2 tile = p / tile_size;
  uint2 in_tile = p - tile * tile_size;
  // Depth tiles store the left and right halves of each tile swapped.
  if (is_depth) {
    uint half_width = tile_width >> 1;
    in_tile.x = in_tile.x < half_width ? in_tile.x + half_width : in_tile.x - half_width;
  }
  uint tile_index = (base + tile.y * pitch_tiles + tile.x) & 2047u;
  return (tile_index * tile_width * tile_height + in_tile.y * tile_width + in_tile.x) * 4u;
}

float RawDepth(int2 p) {
  p = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));
  return float(edram.Load(EdramAddress(uint2(p), depth_base, true)) >> 8) * (1.0 / 16777215.0);
}

// Distance from the camera in units of the near plane (assuming the far plane
// is much further than the near one).
float LinearDepth(float z) { return 1.0 / max(1.0 - z, 1.0 / 65536.0); }

// Tuning sets compared in one run: x radius (near-plane units), y bias,
// z intensity, w power.
float4 Set() {
  uint i = view >= 5u ? view - 5u : (view >= 2u ? view - 2u : 0u);
  if (i == 0u) return float4(6.0, 0.25, 2.5, 1.0);
  if (i == 1u) return float4(8.0, 0.3, 3.5, 1.0);
  return float4(12.0, 0.3, 3.0, 1.0);
}
float Radius() { return Set().x; }
float TanX() { return p1.x > 0.0 ? p1.x : 0.684; }
float TanY() { return TanX() * float(height) / float(width); }

float3 ViewPos(int2 p) {
  float zl = LinearDepth(RawDepth(p));
  float2 ndc = (float2(p) + 0.5) / float2(width, height) * 2.0 - 1.0;
  return float3(ndc.x * TanX() * zl, -ndc.y * TanY() * zl, zl);
}

float Noise(uint2 p) { return frac(52.9829189 * frac(0.06711056 * float(p.x) + 0.00583715 * float(p.y))); }

[numthreads(8, 8, 1)]
void ComputeAO(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) {
    return;
  }
  int2 p = int2(id.xy);
  float ao = 1.0;
  float z = RawDepth(p);
  if (z < 0.99999) {
    float3 P = ViewPos(p);
    // Normal from the neighbours on the side with the smaller depth step.
    float3 pr = ViewPos(p + int2(1, 0)), pl = ViewPos(p - int2(1, 0));
    float3 pd = ViewPos(p + int2(0, 1)), pu = ViewPos(p - int2(0, 1));
    float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0.0) {
      N = -N;
    }
    float R = Radius();
    float focal = float(height) * 0.5 / TanY();
    float radius_px = clamp(R * focal / P.z, 2.0, float(height) * 0.05);
    const int kSamples = 12;
    float rotation = Noise(id.xy) * 6.2831853;
    float bias = Set().y;
    float sum = 0.0;
    [unroll]
    for (int i = 0; i < kSamples; ++i) {
      float a = rotation + float(i) * 2.3999632;  // golden angle
      float r = sqrt((float(i) + 0.5) / float(kSamples)) * radius_px;
      int2 q = p + int2(round(float2(cos(a), sin(a)) * r));
      // Samples off the screen tell nothing (clamping them to the edge would
      // darken the borders).
      if (any(q < int2(0, 0)) || any(q >= int2(int(width), int(height)))) {
        continue;
      }
      float3 v = ViewPos(q) - P;
      float vv = dot(v, v);
      float falloff = saturate(1.0 - vv / (R * R));
      // How far the sample rises above the surface (cosine), past a small
      // bias, independent of the scene's units.
      sum += max(0.0, dot(v, N) * rsqrt(vv + 1e-6) - bias) * falloff;
    }
    float intensity = Set().z;
    float power = Set().w;
    ao = pow(saturate(1.0 - intensity * sum / float(kSamples)), power);
  }
  ao_buffer.Store((id.y * width + id.x) * 4u, asuint(ao));
}

[numthreads(8, 8, 1)]
void ApplyAO(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) {
    return;
  }
  int2 p = int2(id.xy);
  float zc = LinearDepth(RawDepth(p));
  // Depth-aware blur of the AO.
  int blur = max(1, int((p1.z > 0.0 ? p1.z : 2.0) * float(width) / 1280.0 + 0.5));
  int step = max(1, blur / 2);
  float total = 0.0, weight = 0.0;
  for (int y = -blur; y <= blur; y += step) {
    for (int x = -blur; x <= blur; x += step) {
      int2 q = clamp(p + int2(x, y), int2(0, 0), int2(int(width) - 1, int(height) - 1));
      float zq = LinearDepth(RawDepth(q));
      float w = exp(-abs(zq - zc) / (zc * 0.02 + 0.0001));
      total += asfloat(ao_buffer.Load((uint(q.y) * width + uint(q.x)) * 4u)) * w;
      weight += w;
    }
  }
  float ao = weight > 0.0 ? total / weight : 1.0;
  float fade_distance = p0.w > 0.0 ? p0.w : 2000.0;
  ao = lerp(1.0, ao, saturate(1.0 - zc / fade_distance));

  uint address = EdramAddress(id.xy, color_base, false);
  uint color = edram.Load(address);
  if (view >= 2u && view <= 4u) {
    uint g = uint(saturate(ao) * 255.0 + 0.5);
    color = (color & 0xFF000000u) | (g << 16) | (g << 8) | g;
  } else if (view == 1u) {
    uint g = uint(saturate(log2(zc) / 16.0) * 255.0 + 0.5);
    color = (color & 0xFF000000u) | (g << 16) | (g << 8) | g;
    if (id.x < width / 20u && id.y < height / 20u) {
      color = (color & 0xFF000000u) | 0xFFu;  // byte 0 at full: red if byte 0 is red
    }
  } else if (view != 0u) {
    float3 c = float3(color & 0xFFu, (color >> 8) & 0xFFu, (color >> 16) & 0xFFu) * ao;
    uint3 ci = uint3(c + 0.5);
    color = (color & 0xFF000000u) | (ci.z << 16) | (ci.y << 8) | ci.x;
  }
  // View number stamp for test captures: view + 1 small squares, top right.
  uint stamp = max(height / 60u, 4u);
  if (p1.w > 0.0 && id.y < stamp && id.x >= width - (view + 1u) * stamp * 2u && ((width - 1u - id.x) / stamp) % 2u == 0u) {
    color = (color & 0xFF000000u) | 0x00FFFFFFu;
  }
  edram.Store(address, color);
}
