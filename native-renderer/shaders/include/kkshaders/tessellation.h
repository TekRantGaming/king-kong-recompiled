#pragma once
#include "kkshaders/cache.h"
#include <array>

namespace kkshaders {
// Verified hair, main lit and paired depth permutations (tasks 072/077). The
// caller must additionally match Ann's material content; a shader hash is not
// an actor ID.
bool annTessellationShader(uint64_t hash);
constexpr uint64_t kTessPackTag = 0x5445535300000100ull;
constexpr uint64_t tessPackTag(unsigned stage, bool motion) {
    return kTessPackTag + stage + (motion ? 16 : 0);
}
struct TessellationSource {
    bool ok = false;
    std::string error, hlsl;
};
TessellationSource makeAnnTessellationShader(const ShaderInfo &info,
                                             bool motion);
bool buildAnnTessellationShaders(const ShaderInfo &info, Compiler &compiler,
                                 uint32_t targets,
                                 std::vector<CompiledShader> &out,
                                 std::string &error);
} // namespace kkshaders
