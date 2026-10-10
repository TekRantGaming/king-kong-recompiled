// King Kong native renderer: Xbox 360 shader microcode to HLSL (XenosRecomp core).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kkshaders/container.h"

namespace kkshaders {

// The translator's own version: part of every cache key together with the prelude text and
// abi.h's kAbiVersion. Bump it when the generated code changes.
constexpr uint32_t kTranslatorVersion = 4;  // 4: differential testing fixes (docs/shaders.md), bool literals inlined, DXC -Gis
// 3: pixel interpolator count from the mask (word 6)
// 2: Windows validation pass (literal relative reads, select forms, predicate runs)

enum class TextureDimension : uint8_t { Tex1D = 0, Tex2D = 1, Tex3D = 2, Cube = 3 };

// A vertex binding (DrawConstants::vertexFetch[index]; see abi.h).
struct VertexBinding {
    bool raw = false;                       // instruction mode
    DeclUsage usage = DeclUsage::Position;  // binding mode: the declaration element to find
    uint8_t usageIndex = 0;
    uint8_t fetchConstant = 0;              // instruction mode: vertex fetch constant 0-95
};

// A sampler binding (DrawConstants::vertexSamplers / pixelSamplers[index]): the sampler for
// texture fetch constant `slot`, with the fetch instruction's filter overrides applied (3 =
// TextureFilter use the fetch constant; anisotropy 7 = use the fetch constant).
struct SamplerBinding {
    uint8_t slot = 0;
    TextureDimension dimension = TextureDimension::Tex2D;
    uint8_t magFilter = 3, minFilter = 3, mipFilter = 3, anisoFilter = 7, volMagFilter = 3, volMinFilter = 3;
};

struct TextureUse {
    uint8_t slot = 0;
    TextureDimension dimension = TextureDimension::Tex2D;
};

struct ShaderBindings {
    ShaderKind kind = ShaderKind::Pixel;
    std::vector<VertexBinding> vertexBindings;
    std::vector<SamplerBinding> samplers;
    std::vector<TextureUse> textures;
    uint32_t pixelOutputs = 0;          // bits 0-3 colour targets, bit 4 depth
    uint32_t interpolators = 0;         // vertex: semantic slots written (bits 0-15 TEXCOORD, 16-17 COLOR)
    uint32_t tempRegisters = 0;
    bool generalControlFlow = false;
    bool dynamicRegisters = false;
    std::vector<RegisterWrite> registerWrites;  // apply when the shader is set (see abi.h)
    std::vector<std::string> warnings;
};

struct TranslateResult {
    bool ok = false;
    std::string error;
    std::string hlsl;
    ShaderBindings bindings;
};

// The prelude every shader starts with (hlsl/kk_common.hlsli).
const std::string& prelude();

// Hash of everything translation depends on besides the microcode (constant table, bindings,
// literals) together with the microcode: the second half of the cache key.
uint64_t translationInputHash(const ShaderInfo& info);

// Hash of the translator itself (version, ABI version, prelude text).
uint64_t translatorHash();

TranslateResult translate(const ShaderInfo& info);

// Bindings to / from bytes (cache entries).
std::vector<uint8_t> serializeBindings(const ShaderBindings& bindings);
bool deserializeBindings(const uint8_t* data, size_t size, ShaderBindings& out);

}  // namespace kkshaders
