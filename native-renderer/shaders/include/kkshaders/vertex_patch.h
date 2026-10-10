// King Kong native renderer: the D3D library's vertex fetch patching, reproduced.
//
// The game's vertex shaders sit in the shader database as templates: every vfetch
// instruction has format 0 and fetch slot 0 (container.h). When a vertex shader is
// bound with a declaration and a set of stream strides, the library (sub_82120028,
// d3d-structs.md "Vertex shader and pixel shader") rewrites each vfetch instruction
// with the fetch slot (95 - stream), the stride, the offset and the format of the
// declaration element whose usage and usage index match the instruction's entry in
// the container's binding table, and keeps that patched copy. The Xenos plugin hashes
// the patched copy (the "vs" of REX_DEV_FRAME_LOG lines), so the native plugin has
// to rebuild it to write the same hash.
//
// The instruction layout is the hardware's (XenosRecomp shader_code.h):
//   dword 0: opcode 0-4, src 5-10, ..., const index 20-24, const index select 25-26, ...
//   dword 1: dst swizzle 0-11, comp all 12, num format all 13 (1 = integer), signed 14,
//            index rounded 15, format 16-21, exp adjust 24-29, mini fetch 30, predicated 31
//   dword 2: stride 0-7 (dwords), offset 8-30 (dwords, signed), predicate condition 31
// Fetch slot s = const index * 3 + select. A mini fetch (dword 1 bit 30) shares the fetch
// slot and stride of the full fetch before it, so only its format and offset are patched.
//
// What is reproduced from the descriptions and what is a guess is in docs/backend.md,
// "The vertex shader hash"; `kkshaders vfetch-check` compares this with the database's
// second containers (the library's own output for a dummy declaration).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kkshaders/container.h"

namespace kkshaders {

// One element of a vertex declaration, decoded.
struct DeclElement {
    uint8_t stream = 0;
    uint16_t offset = 0;        // bytes
    uint8_t format = 0;         // Xenos VertexFormat (declaration type bits 0-5)
    bool isSigned = false;      // type bit 8
    bool integer = false;       // type bit 9
    DeclUsage usage = DeclUsage::Position;
    uint8_t usageIndex = 0;
};

struct VertexPatchOptions {
    // Which instruction fields the library is taken to write. The defaults are the
    // ones the descriptions name; the rest of the instruction stays as the template.
    bool writeFormatFlags = true;   // num format all (integer) and signed
    bool clearExpAdjust = false;    // exp adjust set to 0
};

struct VertexPatchResult {
    std::vector<uint32_t> ucode;    // microcode, host byte order, as ShaderInfo::ucode
    uint32_t patched = 0;           // vfetch instructions rewritten
    uint32_t missing = 0;           // entries whose usage the declaration lacks (left as the template)
    uint32_t misaligned = 0;        // elements whose byte offset or stride is not a whole number of dwords
};

// Patches the vfetch instructions of `info` (a vertex shader parsed from a container)
// for the declaration and the stream strides in bytes (index = stream, 16 of them).
VertexPatchResult patchVertexFetches(const ShaderInfo& info, std::span<const DeclElement> declaration,
                                     std::span<const uint32_t> strides, const VertexPatchOptions& options = {});

// XXH3-64 of the microcode as the big-endian bytes it has in guest memory.
uint64_t hashMicrocode(std::span<const uint32_t> hostOrderDwords);

// The fields of vfetch instruction `address` (12-byte units), for tests and the check tool.
struct VertexFetchFields {
    bool isFetch = false;       // opcode 0 (vertex fetch)
    bool mini = false;
    uint32_t slot = 0;          // fetch slot 0-95
    uint32_t format = 0;
    bool integer = false, isSigned = false;
    uint32_t stride = 0;        // dwords
    int32_t offset = 0;         // dwords
    int32_t expAdjust = 0;
    uint32_t dstSwizzle = 0;
};
VertexFetchFields readVertexFetch(std::span<const uint32_t> ucode, uint32_t address);

}  // namespace kkshaders
