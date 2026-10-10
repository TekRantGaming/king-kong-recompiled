// The generated test corpus: Xenos shaders covering every instruction class the translator
// handles, as 2005 containers (and as bare microcode).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "container_writer.h"

struct CorpusShader {
    std::string group;
    std::string name;
    bool vertex = false;
    bool raw = false;                 // bare microcode (no container)
    std::vector<uint8_t> bytes;       // container, or big-endian microcode when raw
    ctest::Spec spec;                 // what the container was written from
};

std::vector<CorpusShader> buildCorpus(uint32_t fuzzCount, uint32_t seed);

// Instruction addresses of the vfetch instructions, in program order of the execs.
std::vector<uint32_t> findVfetchAddresses(const std::vector<uint32_t>& ucode);
