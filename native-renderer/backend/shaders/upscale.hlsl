// The temporal upscalers' passes (docs/upscalers.md). All full-screen triangles over the viewport (the
// destination rectangle) with their parameters in one push constant block (root constants on D3D12) at
// b0, except the object motion pixel shader, which reads the motion variant's KKMotionConstants at b5
// (shaders/include/kkshaders/abi.h).
//
//   ps_camera_motion   the camera's motion of every pixel from the scene's depth (and a copy of that
//                      depth as R32F, the upscalers' depth input)
//   ps_object_motion   the motion of a draw from its two clip positions (the vertex shader's motion
//                      variant, translator.h makeMotionVertexShader)
//   ps_reactive        how much each pixel changed after the opaque pass (water, fur, smoke, rain,
//                      particles, light shafts): the reactive / responsive / bias-current-colour mask
//   ps_depth_upsample  the scene's depth at the output size (nearest), for the depth-tested draws after
//                      the upscale point (the HUD, the last heat shimmer)
//   ps_stretch         a bilinear stretch of a rectangle (targets carried to the output size, the
//                      "bilinear" upscaler, a frame without a scene)
//   ps_show_motion     debugging: the motion vectors as colours
//
// Motion vectors are in host pixels of the input picture, last frame's position minus this frame's,
// +y down: what FSR, DLSS and XeSS take (with a scale of 1).

struct UpscaleParams {
  float4 rows[4];  // camera: this frame's clip space to last frame's (prev = mul(R, cur), by rows)
  float4 p0;       // camera: x_scale, x_offset, y_scale, y_offset (guest pixel = ndc * scale + offset)
                   // reactive: x strength, y largest value
                   // depth upsample: xy source pixels per destination pixel, zw the source's size
                   // stretch, show motion: the source rectangle, normalised (x, y, width, height)
  float4 p1;       // camera: z_scale, z_offset, host_scale_x, host_scale_y
  float4 p2;       // camera: xy the jitter (input pixels), z 1 when the reprojection is valid
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<UpscaleParams> params : register(b0);

struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

// One triangle covering the viewport; uv 0..1 across it (y down). D3D's clip space on both APIs
// (NVRHI's Vulkan viewports are flipped to match).
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.uv = t;
  o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
  return o;
}

Texture2D<float4> source0 : register(t0);
Texture2D<float4> source1 : register(t1);
SamplerState linear_clamp : register(s0);

// ---------------------------------------------------------------- camera motion

struct CameraOut {
  float2 motion : SV_Target0;
  float depth : SV_Target1;
};

CameraOut ps_camera_motion(VSOut i) {
  CameraOut o;
  const int2 p = int2(i.pos.xy);
  const float d = source0.Load(int3(p, 0)).r;  // the scene's depth (D32S8's depth plane)
  o.depth = d;
  o.motion = float2(0.0, 0.0);
  if (params.p2.z <= 0.0) return o;
  // The pixel's centre less the jitter, in guest pixels, then the guest's NDC.
  const float2 g = (i.pos.xy - params.p2.xy) / params.p1.zw;
  const float4 cur = float4((g.x - params.p0.y) / params.p0.x, (g.y - params.p0.w) / params.p0.z,
                            params.p1.x != 0.0 ? (d - params.p1.y) / params.p1.x : d, 1.0);
  const float4 prev = float4(dot(params.rows[0], cur), dot(params.rows[1], cur), dot(params.rows[2], cur),
                             dot(params.rows[3], cur));
  if (prev.w <= 1e-6) return o;
  const float2 prev_px = (prev.xy / prev.w * params.p0.xz + params.p0.yw) * params.p1.zw;
  const float2 cur_px = (cur.xy * params.p0.xz + params.p0.yw) * params.p1.zw;
  o.motion = prev_px - cur_px;
  return o;
}

// ---------------------------------------------------------------- object motion

cbuffer KKMotionConstants : register(b5, space0) {
  float4 kk_VCPrev[256];
  float4 kk_MotionParams;  // x, y: half the target's host size; z: the sign of y (D3D12 -1, Vulkan +1)
};

struct ObjectIn {
  float4 pos : SV_Position;
  float4 current : KKMOTION0;
  float4 previous : KKMOTION1;
};

float2 ps_object_motion(ObjectIn i) : SV_Target0 {
  // Both positions are host clip space with this frame's jitter (the same ndcOffset): it cancels.
  const float2 c = i.current.xy / i.current.w;
  const float2 p = i.previous.xy / max(i.previous.w, 1e-6);
  return (p - c) * float2(kk_MotionParams.x, kk_MotionParams.y * kk_MotionParams.z);
}

// ---------------------------------------------------------------- reactive

float ps_reactive(VSOut i) : SV_Target0 {
  const int2 p = int2(i.pos.xy);
  const float3 a = source0.Load(int3(p, 0)).rgb;  // the scene after its opaque pass and fog
  const float3 b = source1.Load(int3(p, 0)).rgb;  // the scene at the upscale point
  const float3 d = abs(b - a);
  return min(saturate(max(d.r, max(d.g, d.b)) * params.p0.x), params.p0.y);
}

// ---------------------------------------------------------------- depth upsample

float ps_depth_upsample(VSOut i) : SV_Depth {
  const int2 p = min(int2(floor(i.pos.xy * params.p0.xy)), int2(params.p0.zw) - 1);
  return source0.Load(int3(p, 0)).r;
}

// ---------------------------------------------------------------- stretch

float4 ps_stretch(VSOut i) : SV_Target0 {
  return source0.SampleLevel(linear_clamp, params.p0.xy + i.uv * params.p0.zw, 0.0);
}

// ---------------------------------------------------------------- debugging

float4 ps_show_motion(VSOut i) : SV_Target0 {
  // The motion of the input pixel under this output pixel: red right, green down, both scaled so
  // that 8 input pixels are full intensity; grey = still.
  const float2 m = source0.SampleLevel(linear_clamp, params.p0.xy + i.uv * params.p0.zw, 0.0).xy;
  return float4(saturate(0.5 + m / 16.0), 0.5, 1.0);
}
