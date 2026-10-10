// A CPU reference interpreter of Xenos microcode for the execution tests, written from the
// instruction notes in the ReXGlue SDK's graphics/format/ucode.h (the behaviour Xenia
// documents). It covers ALU, control flow and vertex fetch; texture fetches are not modelled
// (the tests that use the reference do not sample).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace xref {

using Vec = std::array<float, 4>;

struct VertexElementRef {
    uint32_t buffer = 0, offset = 0, stride = 0, formatWord = 0;  // abi.h binding mode entry
};

struct Machine {
    bool pixel = false;
    std::vector<uint32_t> ucode;
    std::array<Vec, 256> constants{};      // this stage's float constants
    std::array<uint32_t, 8> bools{};
    std::array<uint32_t, 32> loops{};
    std::array<Vec, 64> registers{};        // initial temporaries (interpolators, vertex index)
    // Vertex fetch, binding mode: the element read by the vfetch at each instruction address.
    std::map<uint32_t, VertexElementRef> elements;
    std::vector<std::vector<uint8_t>> buffers;
    // 1D / 2D textures by fetch constant, sampled with point filtering at level 0 (the tests bind
    // a point sampler); other dimensions and texture operations read 0.
    struct Texture {
        uint32_t width = 1, height = 1;
        std::vector<float> texels;  // RGBA
    };
    std::map<uint32_t, Texture> textures;

    // Results.
    std::map<uint32_t, Vec> exports;       // export register -> value (last write wins per component)
    bool killed = false;
    uint64_t steps = 0;                    // instructions run (to stop runaway programs)
    bool runaway = false;

    void run();
};

}  // namespace xref
