// Ambient-only ambient occlusion (docs/ao.md): the pixel shaders whose ambient term the
// occlusion multiplies, and which term that is. The term is named in the shader's own
// microcode terms, because the translated shaders have no variable names:
//
//   output <n>                   the colour written to target n (the whole lit colour)
//   constant c<n>                every direct read of pixel constant c<n> (an ambient colour)
//   interpolator color<n>        the interpolated COLORn or TEXCOORDn input (baked lighting
//   interpolator texcoord<n>     carried by the vertices)
//   fetch <n>                    what texture fetch constant n gives (a light map, an ambient cube)
//   register r<n> after <a>      register n as the instruction at address a leaves it
//                                (`kkshaders translate --addresses` prints the addresses)
//
// followed by an optional component mask (rgb by default; r, g, b, a or x, y, z, w letters).
//
// The list: one shader per line, "<microcode hash> <term> [mask]", '#' starts a comment. Lines
// "family <name> <term> [mask]" are for `kkshaders ao-list`, which expands them into hash lines
// from the game's shader database; the renderer ignores them.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kkshaders {

struct AmbientTerm {
    enum class Kind : uint8_t { None, Output, Constant, Interpolator, Fetch, Register };
    Kind kind = Kind::None;
    uint8_t index = 0;     // Output: target 0-3; Constant: register 0-255; Interpolator: usage index;
                           // Fetch: fetch constant 0-31; Register: r0-r127
    uint8_t usage = 10;    // Interpolator: D3DDECLUSAGE (5 TEXCOORD, 10 COLOR)
    uint16_t address = 0;  // Register: the instruction's address
    uint8_t mask = 0x7;    // components (bit 0 x / red ... bit 3 w / alpha)

    bool enabled() const { return kind != Kind::None; }
    bool operator==(const AmbientTerm&) const = default;
};

// "<term> [mask]" as above. False (with the reason) when it cannot be read.
bool parseAmbientTerm(std::string_view text, AmbientTerm& out, std::string* error = nullptr);
std::string formatAmbientTerm(const AmbientTerm& term);

struct AmbientTable {
    std::unordered_map<uint64_t, AmbientTerm> byHash;
    std::vector<std::pair<std::string, AmbientTerm>> families;  // for kkshaders ao-list
    const AmbientTerm* find(uint64_t ucodeHash) const {
        auto it = byHash.find(ucodeHash);
        return it == byHash.end() ? nullptr : &it->second;
    }
    bool empty() const { return byHash.empty(); }
};

// The list's text. Bad lines are skipped and described in `errors` ("line N: why").
AmbientTable parseAmbientTable(const std::string& text, std::vector<std::string>* errors = nullptr);

}  // namespace kkshaders
