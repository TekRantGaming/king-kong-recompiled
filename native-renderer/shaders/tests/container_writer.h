// Writes 2005 XDK shader containers (the layout kkshaders/container.h parses) for the tests.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ctest {

struct Constant {
    std::string name;
    uint16_t registerSet = 2;   // 0 bool, 1 int4, 2 float4, 3 sampler
    uint16_t registerIndex = 0;
    uint16_t registerCount = 1;
    uint16_t typeClass = 1;     // D3DXPC_VECTOR
    uint16_t type = 3;          // D3DXPT_FLOAT
    uint16_t rows = 1, columns = 4, elements = 1;
};

struct Fetch {
    uint32_t address = 0;       // instruction index of the vfetch
    uint32_t usage = 0, usageIndex = 0, classHint = 0;
};

struct Interp {
    uint32_t usage = 5, usageIndex = 0, reg = 0, mask = 0xF;
};

struct Spec {
    bool vertex = false;
    std::vector<uint32_t> ucode;                    // host-order dwords
    std::vector<Constant> constants;
    std::vector<std::pair<uint32_t, std::array<uint32_t, 4>>> floatLiterals;  // stage register
    std::vector<std::pair<uint32_t, uint32_t>> loopLiterals;                   // loop constant, value
    std::vector<std::pair<uint32_t, uint32_t>> boolLiterals;                   // bool dword, value
    std::vector<Fetch> fetches;
    std::vector<Interp> interpolators;
    bool paramGen = false;      // the pixel position goes in the register after the interpolators
    // Quirks of the game's database: a stripped table (creator 0, stale bytes in the target
    // field), no constant table at all, words after a vertex shader's interpolators.
    bool strippedTable = false;
    bool noConstantTable = false;
    std::vector<uint32_t> extraBindingWords;
};

std::vector<uint8_t> writeContainer(const Spec& spec);

// The database file around containers: 'SDB2', sources, entries.
struct DbEntry {
    uint32_t kind;              // 1 vertex (A container + unused B), 2 pixel
    uint64_t key;
    std::vector<uint8_t> container;
};
std::vector<uint8_t> writeDatabase(const std::vector<std::pair<std::string, std::string>>& sources, const std::vector<DbEntry>& entries);

}  // namespace ctest
