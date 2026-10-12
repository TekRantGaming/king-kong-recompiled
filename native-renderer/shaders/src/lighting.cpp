// The lighting options' shader side: see kkshaders/lighting.h and docs/lighting.md.

#include "kkshaders/lighting.h"

#include <vector>

namespace kkshaders {

extern const char kLightingText[];  // generated from hlsl/kk_lighting.hlsli

namespace {

struct WaterEntry {
    uint64_t hash;
    WaterReflectionHook hook;
};

// The game's pswater shaders with a reflection of their own and psocean (kkshaders ao-list --family
// pswater --translate: which fetch constants each one reads). Fetch 1 is the planar reflection, 2 the
// environment cube, 3 the normal map. psocean is not listed: the game draws it with colour writes off
// (a depth pass of the sea; the census in docs/lighting.md), so its output hook would show nothing.
constexpr uint32_t kPlanar = 1u << 1, kCube = 1u << 2;
const WaterEntry kWater[] = {
    {0x2C85A92BB1A137E8ull, {kPlanar | kCube, false, true}},
    {0x360F0E42457F07F7ull, {kPlanar | kCube, false, false}},
    {0x52E2BA7C879B4217ull, {kPlanar | kCube, false, true}},
    {0x99C1A041F955C307ull, {kPlanar | kCube, false, false}},
    {0xB4FFC68C767C9D35ull, {kPlanar | kCube, false, true}},
    {0xC28D09A8A208AD93ull, {kPlanar | kCube, false, false}},
    {0x34F9B3BE4E8400FAull, {kPlanar, false, true}},
    {0x3FF7BF9C23AC717Bull, {kPlanar, false, false}},
    {0x43C90E76B4038411ull, {kPlanar, false, true}},
    {0x5D29BF8E26F803F6ull, {kPlanar, false, false}},
    {0xAEEE430797CBBE09ull, {kPlanar, false, true}},
    {0xF22A76FB8F40F38Bull, {kPlanar, false, true}},
    {0xFDC4105FB5172BD8ull, {kPlanar, false, false}},
};

}  // namespace

WaterReflectionHook waterReflectionHookFor(uint64_t ucodeHash) {
    for (const WaterEntry& e : kWater)
        if (e.hash == ucodeHash) return e.hook;
    return {};
}

const uint64_t* waterReflectionShaders(size_t* count) {
    static const auto list = [] {
        std::vector<uint64_t> v;
        for (const WaterEntry& e : kWater) v.push_back(e.hash);
        return v;
    }();
    if (count) *count = list.size();
    return list.data();
}

const std::string& lightingText() {
    static const std::string text(kLightingText);
    return text;
}

std::string formatWaterReflectionHook(const WaterReflectionHook& hook) {
    std::string s = "fetch";
    for (uint32_t i = 0; i < 32; i++)
        if (hook.fetchMask & (1u << i)) s += " " + std::to_string(i);
    if (hook.output) s += " output";
    if (hook.normalMap) s += " normal-map";
    return s;
}

}  // namespace kkshaders
