// The mask of the depth of field on Jack's first-person hands (hands_dof.h): the pixel shader of a second draw of their
// colour pass, built like the game's translated shaders (ao_hands.hlsl). 0 away from the hands; on them, 1/255 at the
// sharpest and 1 at the softest. SV_Position.w is the distance from the eye (Vulkan: -fvk-use-dx-position-w).
#define KK_PIXEL_SHADER 1
#include "../../shaders/hlsl/kk_common.hlsli"

static const float kSharpFrom = 0.65;  // metres: from here on sharp (the gun's muzzle is at about 0.7)
static const float kSoftAt = 0.25;     // and softest from here in

float4 main(float4 pos : SV_Position) : SV_Target0
{
    const float soft = saturate((kSharpFrom - pos.w) / (kSharpFrom - kSoftAt));
    return float4(1.0 / 255.0 + soft * 254.0 / 255.0, 0.0, 0.0, 1.0);
}
