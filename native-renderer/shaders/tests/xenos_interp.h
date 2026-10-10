// A reference interpreter for Xenos (Xbox 360 GPU) shader microcode, on the CPU, for checking
// what the translated shaders compute.
//
// Semantics follow the ReXGlue SDK's shader code (Xenia's): include/rex/graphics/format/ucode.h
// for the encodings and operation notes, src/graphics/pipeline/shader/interpreter.cpp for the ALU
// and control flow, spirv_translator_fetch.cpp for vertex formats and texture coordinates
// (the SDK's own CPU interpreter skips the component offsets of unsigned packed formats, so the
// SPIR-V translator is taken there). The microcode is decoded here bit by bit, independently of
// XenosRecomp's structures, so a decoding mistake in the translator shows up as a mismatch.
//
// Every value carries an error bound: the largest difference a conforming GPU (IEEE float32
// with any rounding of transcendental functions within a stated precision, fused or separate
// multiply-add, 8-bit sub-texel precision in filtering) may show against the value computed
// here. Inputs are exact. A value whose bound cannot be stated (a comparison, floor or point
// sample that could go either way within the error) becomes "unknown" and is not compared; a
// control decision of that kind (predicate, kill, address register, branch) makes the whole
// invocation "fragile" and it is not compared either. With the dyadic inputs the tests use,
// most arithmetic is exact and both cases are rare; the tests count them.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xinterp {

struct Num {
    float v = 0.0f;
    float e = 0.0f;        // absolute error bound
    bool unknown = false;  // not comparable
};
using Vec = std::array<Num, 4>;

enum class TexDim : uint8_t { D1 = 0, D2 = 1, D3 = 2, Cube = 3 };

// A synthetic texture: RGBA float texels, layer-major ([layer][y][x]); cube maps have 6 layers
// (+X, -X, +Y, -Y, +Z, -Z), 1D textures height 1, 2D depth 1. One mip level.
struct Texture {
    TexDim dim = TexDim::D2;
    uint32_t width = 1, height = 1, depth = 1;
    std::vector<float> texels;
    // The fetch constant's filter (used when the instruction's filter field says "fetch
    // constant", 3) and address mode (clamp to edge, else wrap).
    bool linear = false;
    bool clamp = false;
    const float* texel(uint32_t x, uint32_t y, uint32_t layer) const {
        return &texels[((size_t(layer) * height + y) * width + x) * 4];
    }
};

// The filter a texture fetch uses: the instruction's magnification filter field (0 point,
// 1 linear, 2 base map = point, 3 = the fetch constant's). The harness builds its samplers with
// the same rule (one filter for magnification and minification).
inline bool fetchIsLinear(uint32_t instructionMagFilter, bool fetchConstantLinear) {
    return instructionMagFilter == 3 ? fetchConstantLinear : instructionMagFilter == 1;
}

// A vertex declaration element as the backend binds it in binding mode (see abi.h): the
// element's guest data in `buffer` from byte `offset`, `stride` bytes per vertex, and the
// format word (format, number format, endian, exponent adjust, element swizzle).
struct Element {
    uint32_t buffer = 0, offset = 0, stride = 0, word = 0;
};

// A vertex fetch constant in instruction mode: the stream's guest data in `buffer` from byte
// `offset`, `size` bytes long, with an endian mode.
struct FetchConstant {
    uint32_t buffer = 0, offset = 0, size = 0, endian = 2;
};

struct Inputs {
    bool pixel = false;
    std::vector<uint32_t> ucode;                        // host-order dwords
    std::array<std::array<float, 4>, 256> constants{};  // the stage's float constants (literals applied)
    std::array<uint32_t, 8> bools{};                    // 256 bool constants
    std::array<uint32_t, 32> loops{};                   // loop constants
    // Vertex shaders.
    uint32_t vertexIndex = 0;
    bool bindingMode = false;
    std::map<uint32_t, uint32_t> vfetchElement;         // binding mode: vfetch address -> elements index
    std::vector<Element> elements;
    std::array<FetchConstant, 96> fetchConstants{};     // instruction mode
    std::vector<std::vector<uint8_t>> buffers;          // guest bytes (big-endian data)
    // Pixel shaders: the registers' starting values (interpolators, pixel position).
    std::array<std::array<float, 4>, 64> initialRegisters{};
    std::array<const Texture*, 32> textures{};          // per texture fetch constant
    uint32_t maxSteps = 200000;                          // control flow steps before giving up
    bool trace = false;                                  // fill Outputs::trace
};

struct Outputs {
    std::array<Vec, 64> exports{};      // by export register: VS 0-15 interpolators, 62 position,
                                        // 63 point size / misc; PS 0-3 colours, 61 depth
    std::array<uint32_t, 64> exportMask{};
    std::array<Vec, 64> registers{};    // the temporaries at the end
    bool killed = false;                // pixel shader: a kill fired
    bool fragile = false;               // a control decision within the error: do not compare
    std::string fragileWhy;
    bool timeout = false;               // more than maxSteps (the GPU would hang: do not run)
    bool error = false;                 // malformed program
    std::string message;
    uint32_t steps = 0;
    std::string trace;                  // with Inputs::trace: every step and what it wrote
};

Outputs run(const Inputs& in);

// Helpers shared with the tests.
uint32_t endianSwap(uint32_t v, uint32_t endian);
float halfToFloat(uint16_t h);  // IEEE binary16 (as the SDK's SPIR-V translator decodes vertex halves)

}  // namespace xinterp
