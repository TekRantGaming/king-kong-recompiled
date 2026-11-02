// The skin image of the red band on human skin (skin_band.h): the pixel shader of a second draw of a skin material's colour
// pass, built like the game's translated shaders (ao_hands.hlsl): rgb the base texture's colour, a how much of it is skin.
// Every skin material's pixel shader reads its base texture (slot 0, sampler 0) at TEXCOORD0 (documentary/sss/translated).
// The inputs are those of the translated shaders, which share one signature.
#define KK_PIXEL_SHADER 1
#include "../../shaders/hlsl/kk_common.hlsli"

void main(
    float4 iPos : SV_Position,
    float4 iT0 : TEXCOORD0,
    float4 iT1 : TEXCOORD1,
    float4 iT2 : TEXCOORD2,
    float4 iT3 : TEXCOORD3,
    float4 iT4 : TEXCOORD4,
    float4 iT5 : TEXCOORD5,
    float4 iT6 : TEXCOORD6,
    float4 iT7 : TEXCOORD7,
    float4 iT8 : TEXCOORD8,
    float4 iT9 : TEXCOORD9,
    float4 iT10 : TEXCOORD10,
    float4 iT11 : TEXCOORD11,
    float4 iT12 : TEXCOORD12,
    float4 iT13 : TEXCOORD13,
    float4 iT14 : TEXCOORD14,
    float4 iT15 : TEXCOORD15,
    float4 iD0 : COLOR0,
    float4 iD1 : COLOR1,
    bool iFace : SV_IsFrontFace,
    out float4 oC0 : SV_Target0)
{
    const float4 base = kk_Tex2D[kk_TexIndex(0)].Sample(kk_PSSamplerState(0), iT0.xy);
    const float top = max(base.r, 0.01);
    // Skin's red stands clear of its green and blue, and its green sits near its blue. Hair, leather and cloth, which share
    // these atlases, are grey or yellow-brown.
    const float red = (base.r - base.g) / top;
    const float warm = (base.r - base.b) / top;
    const float yellow = (base.g - base.b) / top;
    const float skin = smoothstep(0.06, 0.16, red) * smoothstep(0.15, 0.30, warm) * (1.0 - smoothstep(0.20, 0.30, yellow)) *
                       smoothstep(0.12, 0.25, base.r);
    oC0 = float4(base.rgb, skin);
}
