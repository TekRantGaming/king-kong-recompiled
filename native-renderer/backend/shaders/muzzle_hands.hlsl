// The muzzle light on Jack's first-person hands and what he holds (muzzle_light.h, docs/lighting.md): the pixel shader
// of a second draw of their colour pass, in a frame with a flash. Their colour is drawn after the moment the light goes
// into the scene, so it would stay dark beside a lit wall. The second draw has their own vertex shader and state, depth
// EQUAL without writes, and raises the target's colour (destination times one plus source), which keeps their texture.
//
// Built like the game's translated shaders (ao_hands.hlsl); the renderer writes the constants for this draw alone:
//   c0: x lights, yz scene target pixels per target pixel, w wrap of the light around a surface
//   c1: xy the picture's origin in the scene target, zw the size its view rays are made for
//   c2: xy the depth's unpack constants, zw view ray multiply
//   c3: xy view ray add, z distance fall-off, w gain
//   c4: rgb the light's colour
//   c5 to c8: xyz view position, w strength now
#define KK_PIXEL_SHADER 1
#include "../../shaders/hlsl/kk_common.hlsli"

float4 main(float4 pos : SV_Position) : SV_Target0
{
    const float2 uv = (pos.xy * kk_PC[0].yz - kk_PC[1].xy) / kk_PC[1].zw;
    const float z = 1.0 / max((kk_PC[2].y - pos.z) / kk_PC[2].x, 1e-9);
    const float3 position = float3((kk_PC[2].zw * uv + kk_PC[3].xy) * z, z);
    float3 normal = normalize(cross(ddx(position), ddy(position)));
    if (dot(normal, position) > 0.0)
        normal = -normal;
    float reach = 0.0;
    const int count = int(kk_PC[0].x);
    for (int i = 0; i < count; ++i)
    {
        const float3 toLight = kk_PC[5 + i].xyz - position;
        const float d2 = dot(toLight, toLight);
        const float facing = saturate((dot(normal, toLight / sqrt(max(d2, 1e-6))) + kk_PC[0].w) / (1.0 + kk_PC[0].w));
        reach += facing * kk_PC[5 + i].w / (1.0 + d2 * kk_PC[3].z);
    }
    // At most about three times as bright (the picture is about gamma 2, so far more in light).
    const float gain = reach * kk_PC[3].w;
    return float4(kk_PC[4].rgb * gain / (1.0 + 0.4 * gain), 1.0);
}
