// The game's lighting-related pixel shaders: see lighting_shaders.h.

#include "backend/lighting_shaders.h"

#include <cstddef>

namespace nr {

namespace {

struct Entry {
  uint64_t hash;
  LightFamily family;
};

// kkshaders ao-list xeshaders.bin --family <name> (the game's database).
constexpr Entry kFamilies[] = {
    {0x4681E6795797A946ull, LightFamily::kShadow},
    {0xB871D544CAB63DACull, LightFamily::kShadow},
    {0x681C90468370C52Eull, LightFamily::kShadow},
    {0x6DC2B9F3C32C2394ull, LightFamily::kShadowQuad},
    {0x9A0FBB044BB1BD85ull, LightFamily::kShadowQuad},
    {0x9F5E02F361E2A8A0ull, LightFamily::kShadowQuad},
    {0xB2328F814932E231ull, LightFamily::kCompositeShadow},
    {0x91FDCACDEB67750Eull, LightFamily::kBlurShadow},
    {0xF22A76FB8F40F38Bull, LightFamily::kWater},
    {0xAC2239A7CA9EDF1Cull, LightFamily::kWater},
    {0x43C90E76B4038411ull, LightFamily::kWater},
    {0xAEEE430797CBBE09ull, LightFamily::kWater},
    {0x805E38F150347CFAull, LightFamily::kWater},
    {0xFDC4105FB5172BD8ull, LightFamily::kWater},
    {0x3FF7BF9C23AC717Bull, LightFamily::kWater},
    {0x454157E9FE258D2Full, LightFamily::kWater},
    {0x81918D30C66A5A9Dull, LightFamily::kWater},
    {0x34F9B3BE4E8400FAull, LightFamily::kWater},
    {0xC3D3636900021FEAull, LightFamily::kWater},
    {0xC28D09A8A208AD93ull, LightFamily::kWater},
    {0x8BCD088F72D56CB0ull, LightFamily::kWater},
    {0x99C1A041F955C307ull, LightFamily::kWater},
    {0x360F0E42457F07F7ull, LightFamily::kWater},
    {0x392C35731702335Cull, LightFamily::kWater},
    {0x2C85A92BB1A137E8ull, LightFamily::kWater},
    {0xB4FFC68C767C9D35ull, LightFamily::kWater},
    {0x2C692E9464F37898ull, LightFamily::kWater},
    {0x1B76E49BEBF3D0F6ull, LightFamily::kWater},
    {0x7459E958CCB3441Full, LightFamily::kWater},
    {0x5D29BF8E26F803F6ull, LightFamily::kWater},
    {0xD3C49336D9B42E7Full, LightFamily::kWater},
    {0x52E2BA7C879B4217ull, LightFamily::kWater},
    {0xDC264F3A8121F19Full, LightFamily::kWater},
    {0x5D1062B34025F382ull, LightFamily::kWater},
    {0x4D6658690CFAE9E3ull, LightFamily::kOcean},
    {0x16C954AF5D9DEBD0ull, LightFamily::kLightShaft},
    {0xB642BE70F2A1C799ull, LightFamily::kLightShaft},
    {0x1A0E5E9F838E886Full, LightFamily::kLightShaft},
    {0x256E115710637BA5ull, LightFamily::kLightShaft},
    {0xCA12F7AA224EF5C2ull, LightFamily::kLightShaft},
    {0x906110DB7D5F83CDull, LightFamily::kLightShaft},
    {0xDC6F66FA32FCDB6Cull, LightFamily::kLightShaft},
    {0xB69C19E42E9A8675ull, LightFamily::kLightShaft},
    {0xC03F7EC62122912Cull, LightFamily::kLightShaft},
    {0x4433B15CE9563961ull, LightFamily::kLightShaft},
    {0xA2726CDECA8BAB77ull, LightFamily::kLightShaft},
    {0xC51908C450CCBC51ull, LightFamily::kLightShaft},
    {0x7FE72C9CCE1E421Bull, LightFamily::kReflection},
    {0x115AA5F3FE8AB69Aull, LightFamily::kReflection},
    {0xB6211E1727DF7872ull, LightFamily::kReflection},
    {0xF5AFB92F400260CEull, LightFamily::kReflection},
    {0x1B65AC7429B67FE9ull, LightFamily::kFog},
    {0x9F699E763A41FE07ull, LightFamily::kFog},
    {0xC4887065264F71EAull, LightFamily::kAfterEffects},
    {0x8316EDE09C2EF616ull, LightFamily::kAfterEffects},
    {0xF6ACAF1741114232ull, LightFamily::kAfterEffects},
    {0x97230F8492C76E78ull, LightFamily::kAfterEffects},
    {0x3C5A9EFF5A0F1E68ull, LightFamily::kAfterEffects},
    {0x5915C46F6EF0F08Dull, LightFamily::kAfterEffects},
    {0x52D6C130FDCFA4CCull, LightFamily::kAfterEffects},
    {0xEC85BA15F4E9DC06ull, LightFamily::kAfterEffects},
    {0x2A841607AB386FCFull, LightFamily::kAfterEffects},
    {0xB19B15ADE267E6C5ull, LightFamily::kAfterEffects},
    {0xF1C24E109CB67537ull, LightFamily::kAfterEffects},
    {0xFE14445AE8F6FCA4ull, LightFamily::kAfterEffects},
    {0xFCF2AB08E3740514ull, LightFamily::kAfterEffects},
    {0x30A0FF0EB28905F8ull, LightFamily::kAfterEffects},
    {0x9427420544F9F371ull, LightFamily::kAfterEffects},
    {0x085AB7699509AFD2ull, LightFamily::kAfterEffects},
    {0x199FFBAEA54F3AA1ull, LightFamily::kAfterEffects},
    {0x7137EA1754A79036ull, LightFamily::kAfterEffects},
    {0x82B90CB97AE23E92ull, LightFamily::kAfterEffects},
    {0x8A2FD92C1ECE4873ull, LightFamily::kRain},
    {0x19408DAF3EB827D7ull, LightFamily::kRain},
    {0x54CD0640ABC46D14ull, LightFamily::kRain},
    {0xCB0F369E95947381ull, LightFamily::kHeatShimmer},
    {0x1DDD9B4EBBB12B5Bull, LightFamily::kHeatShimmer},
    {0xB4252608EBE0D734ull, LightFamily::kHeatShimmer},
    {0x3EE1D7D47F7654C0ull, LightFamily::kHeatShimmer},
    {0xA71CAAE3F888F1DBull, LightFamily::kSprite},
    {0x5671F88122218490ull, LightFamily::kSprite},
    {0x2556ECFEAC993F0Bull, LightFamily::kFur},
};

}  // namespace

const char* LightFamilyName(LightFamily f) {
  switch (f) {
    case LightFamily::kShadow: return "psshadow";
    case LightFamily::kShadowQuad: return "psshadowquad";
    case LightFamily::kCompositeShadow: return "pscompositeshadow";
    case LightFamily::kBlurShadow: return "psblurshadow";
    case LightFamily::kWater: return "pswater";
    case LightFamily::kOcean: return "psocean";
    case LightFamily::kLightShaft: return "pslightshaft";
    case LightFamily::kReflection: return "psreflection";
    case LightFamily::kFog: return "fog";
    case LightFamily::kAfterEffects: return "psaftereffects";
    case LightFamily::kRain: return "psrain";
    case LightFamily::kHeatShimmer: return "psheatshimmer";
    case LightFamily::kSprite: return "pssprite";
    case LightFamily::kFur: return "psfur";
    default: return "-";
  }
}

LightFamily LightFamilyOf(uint64_t h) {
  for (const Entry& e : kFamilies)
    if (e.hash == h) return e.family;
  return LightFamily::kNone;
}

LightShaftMode LightShaftModeOf(uint64_t h) {
  // From each shader's fetches and constants (kkshaders ao-list --translate): one fetch and no
  // constants is the apply / resample; four fetches of slot 0 with the shaft colour the blur;
  // the rest render the cookie volumes, two of them with the shadow map at fetch 2.
  switch (h) {
    case 0x16C954AF5D9DEBD0ull: return LightShaftMode::kApply;
    case 0x256E115710637BA5ull: return LightShaftMode::kBlur;
    case 0xB69C19E42E9A8675ull:
    case 0xC03F7EC62122912Cull: return LightShaftMode::kRenderShadowed;
    default: return LightFamilyOf(h) == LightFamily::kLightShaft ? LightShaftMode::kRender : LightShaftMode::kNone;
  }
}

ShadowQuadLayout ShadowQuadLayoutOf(uint64_t h) {
  // The constant tables (kkshaders ao-list --family psshadowquad).
  switch (h) {
    case 0x9F5E02F361E2A8A0ull: return {1, 5, 4};
    case 0x6DC2B9F3C32C2394ull: return {2, 8, 10};
    case 0x9A0FBB044BB1BD85ull: return {3, 12, 15};
    default: return {};
  }
}

WaterLayout WaterLayoutOf(uint64_t h) {
  WaterLayout w;
  w.water = LightFamilyOf(h) == LightFamily::kWater;
  switch (h) {
    case 0x2C85A92BB1A137E8ull:
    case 0x360F0E42457F07F7ull:
    case 0x52E2BA7C879B4217ull:
    case 0x99C1A041F955C307ull:
    case 0xB4FFC68C767C9D35ull:
    case 0xC28D09A8A208AD93ull:
      w.dynamic_reflection = w.environment = true;
      break;
    case 0x34F9B3BE4E8400FAull:
    case 0x3FF7BF9C23AC717Bull:
    case 0x43C90E76B4038411ull:
    case 0x5D29BF8E26F803F6ull:
    case 0xAEEE430797CBBE09ull:
    case 0xF22A76FB8F40F38Bull:
    case 0xFDC4105FB5172BD8ull:
      w.dynamic_reflection = true;
      break;
    default:
      break;
  }
  return w;
}

}  // namespace nr
