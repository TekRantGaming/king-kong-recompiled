// King Kong native renderer: HLSL to DXIL and SPIR-V with the DirectX Shader Compiler.
//
// DXC is loaded at run time (dxcompiler.dll / libdxcompiler.so). DXIL is validated and signed
// by DXC's validator (dxil.dll / libdxil.so, found next to the compiler); SPIR-V is checked by
// DXC's built-in SPIRV-Tools validator, and can also be checked with an external spirv-val.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kkshaders/container.h"

namespace kkshaders {

enum class CompileTarget : uint8_t { Dxil = 0, Spirv = 1 };

struct CompileOutput {
    bool ok = false;
    std::string messages;      // errors (and warnings)
    std::vector<uint8_t> blob;
};

// Loads DXC once per process. `path` is the compiler library or the folder holding it; empty
// = the platform's default search. Returns false with a message if it cannot be loaded.
bool loadDxc(const std::string& path, std::string* error = nullptr);
bool dxcLoaded();
std::string dxcVersion();

// One compiler per thread (the DXC objects are not shared between threads).
class Compiler {
public:
    Compiler();
    ~Compiler();
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    bool ok() const;
    CompileOutput compile(const std::string& hlsl, ShaderKind kind, CompileTarget target);
    // Runs DXC's DXIL validator on a compiled blob (it is already validated and signed by
    // compile(); this is a second, explicit check).
    bool validateDxil(const std::vector<uint8_t>& blob, std::string* messages);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether a DXIL container carries the validator's signature (a non-zero digest).
bool isDxilSigned(const std::vector<uint8_t>& blob);

// Runs an external spirv-val on a SPIR-V blob (target Vulkan 1.2). `spirvVal` is the program.
bool validateSpirvExternal(const std::string& spirvVal, const std::vector<uint8_t>& blob, std::string* messages);

}  // namespace kkshaders
