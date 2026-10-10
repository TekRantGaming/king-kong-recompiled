// King Kong native renderer: the 2005 XDK shader container adapter.
//
// The game's shaders are Xbox 360 "XSHADER" containers from the 2005 XDK, as stored in
// Shaders/xeshaders.bin and as passed to the D3D library's CreateVertexShader and
// CreatePixelShader. This parses one container into what the translator needs. The
// layout (big-endian words, found by reading the database and the game's own code):
//
//   word 0  0x102A0E00 pixel / 0x102A0E01 vertex
//   word 1  virtual size: the header and tables; the game copies it into the shader object
//   word 2  physical size: the microcode that follows the virtual part
//   word 3  offset of the register block (0 = none): literal constants the shader needs
//           (float ones at c249..c255), applied by SetVertexShader / SetPixelShader to the
//           device's constant shadow as {u16 offset into device+1152, u16 count, count dwords}
//           lists. Offsets 768-8959 are the float constants (device +1920), 8960-8991 the bool
//           constants (+10112, inferred: the dwords before the loop constants) and 8992-9119
//           the loop constants (+10144; XenosRecomp's own containers use the same 8992 base).
//   word 4  offset of the D3DX constant table (u32 size, then a standard CTAB; 0 = none, two
//           vertex shaders). The vertex containers' tables are stripped (creator 0, stale bytes in
//           the target field) and their string area is partly overwritten (Database::parse).
//   word 5  offset of the binding table: word 0 = this stage's SQ_PROGRAM_CNTL bits; vertex:
//           word 7 = number of vfetch entries (at word 10 + word 6) {address:12, usage:4,
//           usageIndex:4, class:4}, word 8 = number of words after them, of which the first
//           VS_EXPORT_COUNT + 1 (word 0 bits 20-23) are interpolator descriptors
//           {usageIndex:4, usage:4, register:4, mask:4, slot:4}; pixel: word 1 = (count << 8) |
//           flags, descriptors from word 8, bit 18 of word 0 = the shader reads the pixel
//           position (VPOS) in the register after the interpolators.
//
// Which is which was checked against the game image: the D3D library's own shaders at
// 0x8203E800 (0x102A0E01, CTAB target vs_3_0) go to sub_82111D90 and the one at 0x8203E908
// (0x102A0E00, ps_3_0) to sub_82111CA0, so sub_82111D90 creates vertex shaders and
// sub_82111CA0 pixel shaders; every database entry agrees (ps_3_0 tables only in kind 2).
//
// A vertex entry of the database holds two containers behind an 8-byte {sizeA, sizeB}
// prefix: A is the one the game creates shaders from (its vfetch instructions are
// unpatched templates: format 0, fetch slot 0); B is a copy bound to a dummy declaration
// and is never used.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace kkshaders {

enum class ShaderKind : uint8_t { Vertex = 0, Pixel = 1 };

// D3DDECLUSAGE
enum class DeclUsage : uint8_t {
    Position = 0, BlendWeight, BlendIndices, Normal, PointSize, TexCoord, Tangent, Binormal,
    TessFactor, PositionT, Color, Fog, Depth, Sample
};
const char* declUsageName(DeclUsage usage);  // "POSITION" etc.

// D3DXREGISTER_SET
enum class RegisterSet : uint8_t { Bool = 0, Int4 = 1, Float4 = 2, Sampler = 3 };

struct ConstantInfo {
    std::string name;
    RegisterSet registerSet = RegisterSet::Float4;
    uint16_t registerIndex = 0;
    uint16_t registerCount = 0;
    uint16_t typeClass = 0;    // D3DXPARAMETER_CLASS
    uint16_t type = 0;         // D3DXPARAMETER_TYPE
    uint16_t rows = 0, columns = 0, elements = 0;
};

// A literal float4 constant the shader expects in its constant registers (the HLSL
// compiler's "def" constants), stage-relative register index.
struct LiteralConstant {
    uint16_t registerIndex = 0;
    uint32_t value[4] = {};
};

// One write of the register block, as the XDK applies it when the shader is set: `values`
// go to the device's register image at +1152 + offset (big-endian words as stored).
struct RegisterWrite {
    uint16_t offset = 0;
    std::vector<uint32_t> values;
};

// Vertex shader: which declaration element a vfetch instruction reads.
struct FetchBinding {
    uint16_t address = 0;      // instruction address (12-byte units) of the vfetch
    DeclUsage usage = DeclUsage::Position;
    uint8_t usageIndex = 0;
    uint8_t classHint = 0;     // unknown 4-bit field (0..3), kept for reference
};

// Vertex shader output or pixel shader input: a semantic bound to a register.
struct Interpolator {
    DeclUsage usage = DeclUsage::TexCoord;
    uint8_t usageIndex = 0;
    uint8_t reg = 0;           // vertex: export register o<reg>; pixel: input register r<reg>
    uint8_t mask = 0xF;        // components the source declared
};

struct ShaderInfo {
    ShaderKind kind = ShaderKind::Pixel;
    uint32_t virtualSize = 0;
    uint32_t physicalSize = 0;
    uint32_t programControl = 0;           // binding table word 0
    std::vector<uint32_t> ucode;           // microcode, host byte order
    uint64_t ucodeHash = 0;                // XXH3-64 of the microcode bytes as stored (big-endian): today's cache key
    std::string target;                    // "vs_3_0" / "ps_3_0" (CTAB target)
    std::vector<ConstantInfo> constants;
    std::vector<LiteralConstant> literals;
    std::vector<std::pair<uint32_t, uint32_t>> loopLiterals;  // loop constant 0-31, packed value
    std::vector<std::pair<uint32_t, uint32_t>> boolLiterals;  // bool dword 0-7 (32 bools each), value
    std::vector<RegisterWrite> registerWrites;
    std::vector<FetchBinding> fetches;     // vertex only, in binding-table order
    std::vector<Interpolator> interpolators;
    bool readsPixelPosition = false;       // pixel: VPOS
    uint8_t pixelPositionRegister = 0;
    std::vector<uint32_t> bindingTable;    // raw, for reference
    std::vector<uint8_t> constantTable;    // the CTAB bytes (after its size word), for hashing
    // No container: microcode only (the emulator's shader cache records). Vertex fetches are
    // taken as patched (instruction mode) and interpolators are linked by register number.
    bool rawMicrocode = false;
    // Pixel shaders only: translate for a render scale other than 1 (see translator.h). Part of
    // the input hash only when set, so every hash of a 1:1 shader is what it always was.
    bool renderScaleAware = false;
};

struct ParseResult {
    bool ok = false;
    std::string error;
    ShaderInfo info;
};

// Parses one container. maxSize bounds the read (use SIZE_MAX for guest memory, where the
// size is only known from the header).
ParseResult parseContainer(const uint8_t* data, size_t maxSize);
inline ParseResult parseContainer(std::span<const uint8_t> bytes) { return parseContainer(bytes.data(), bytes.size()); }

// Bare microcode (big-endian bytes as in guest memory or a shader cache record).
ParseResult parseMicrocode(ShaderKind kind, const uint8_t* data, size_t size);

// XXH3-64 over bytes (the cache key convention for microcode).
uint64_t hashBytes(const void* data, size_t size);

// Shaders/xeshaders.bin: 'SDB2', the HLSL sources and a table of 36-byte entries
// {kind, vertex key, pixel key, extra, offset, size}; kind 1 = vertex, 2 = pixel, 0 = other.
struct DatabaseSource {
    std::string name;
    uint32_t hash = 0, familyMask = 0, kind = 0;
    std::string text;
};

struct DatabaseEntry {
    uint32_t kind = 0;
    uint64_t vertexKey = 0, pixelKey = 0, extra = 0;
    uint32_t offset = 0, size = 0;
};

class Database {
public:
    bool load(const std::string& path, std::string* error = nullptr);
    const std::vector<DatabaseEntry>& entries() const { return entries_; }
    const std::vector<DatabaseSource>& sources() const { return sources_; }
    // The container the game creates the shader from (vertex: A, skipping the prefix).
    std::span<const uint8_t> container(const DatabaseEntry& entry) const;
    // Vertex entries only: the unused second container.
    std::span<const uint8_t> secondContainer(const DatabaseEntry& entry) const;
    std::span<const uint8_t> raw(const DatabaseEntry& entry) const;
    // Parses the container the game uses, with constant names repaired from the second
    // container where the first one's string area is damaged (vertex shaders).
    ParseResult parse(const DatabaseEntry& entry) const;
    // Family index (key >> 56) and its name from the sources' family masks.
    static uint32_t familyIndex(uint64_t key) { return uint32_t(key >> 56); }
    std::string familyName(ShaderKind kind, uint32_t family) const;

private:
    std::vector<uint8_t> data_;
    std::vector<DatabaseEntry> entries_;
    std::vector<DatabaseSource> sources_;
};

}  // namespace kkshaders
