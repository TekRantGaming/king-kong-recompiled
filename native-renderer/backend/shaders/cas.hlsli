// Sharpen-only FP32 adaptation of AMD FidelityFX CAS (SDK 1.1.4, gpu/cas/ffx_cas.h).
// Uses the standard five-tap filter and green-channel coefficient. No scaling.
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

float3 CasLinear(float3 v) {
  return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}
float3 CasGamma(float3 v) {
  return lerp(v * 12.92, 1.055 * pow(v, 1.0 / 2.4) - 0.055, step(0.0031308, v));
}
float3 CasLoad(int2 p) {
  return CasLinear(saturate(source.Load(int3(clamp(p, c.src_rect.xy, c.src_rect.xy + c.src_rect.zw - 1), 0)).rgb));
}
float4 Cas(int2 p, uint level) {
  float3 b = CasLoad(p + int2(0, -1));
  float3 d = CasLoad(p + int2(-1, 0));
  float3 e = CasLoad(p);
  float3 f = CasLoad(p + int2(1, 0));
  float3 h = CasLoad(p + int2(0, 1));
  float3 mn = min(min(min(d, e), f), min(b, h));
  float3 mx = max(max(max(d, e), f), max(b, h));
  // Guard constant black against 0/0; retain the standard CAS signal-limit weight.
  float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-8)));
  float sharpness = (clamp(level, 1u, 3u) - 1u) * 0.5;
  float weight = amp.g * (-1.0 / lerp(8.0, 5.0, sharpness));
  float3 color = saturate(((b + d + f + h) * weight + e) / (1.0 + 4.0 * weight));
  return float4(CasGamma(color), source.Load(int3(p, 0)).a);
}
