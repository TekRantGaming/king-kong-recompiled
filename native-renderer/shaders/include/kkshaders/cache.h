// King Kong native renderer: compiled shaders on disk, keyed by the microcode hash.
//
// Two stores with the same entries:
//  - ShaderCache: a folder under the user data root, one file per shader
//    (<ucode hash>-<input hash>.kksh), written as shaders are first translated.
//  - ShaderPack: one read-only file with every shader of xeshaders.bin, built offline
//    (kkshaders db-build), memory-mapped, looked up by binary search, so first runs never compile.
// Entries carry the translator hash (translator version, ABI version, prelude): entries from
// another translator are treated as missing.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/translator.h"

namespace kkshaders {

struct CompiledShader {
    ShaderKind kind = ShaderKind::Pixel;
    uint64_t ucodeHash = 0;       // XXH3-64 of the microcode bytes as stored (big-endian)
    uint64_t inputHash = 0;       // translationInputHash()
    ShaderBindings bindings;
    std::vector<uint8_t> dxil;
    std::vector<uint8_t> spirv;
    std::string hlsl;             // optional (kept by the tool for inspection)
};

std::vector<uint8_t> encodeEntry(const CompiledShader& shader);
bool decodeEntry(const uint8_t* data, size_t size, CompiledShader& out);

class ShaderCache {
public:
    explicit ShaderCache(std::filesystem::path folder);
    bool load(uint64_t ucodeHash, uint64_t inputHash, CompiledShader& out) const;
    bool store(const CompiledShader& shader) const;
    std::filesystem::path pathFor(uint64_t ucodeHash, uint64_t inputHash) const;

private:
    std::filesystem::path folder_;
};

class ShaderPack {
public:
    ShaderPack();
    ~ShaderPack();
    ShaderPack(const ShaderPack&) = delete;
    ShaderPack& operator=(const ShaderPack&) = delete;

    static bool write(const std::filesystem::path& path, const std::vector<CompiledShader>& shaders, std::string* error);
    bool open(const std::filesystem::path& path, std::string* error = nullptr);
    void close();
    size_t size() const;
    // inputHash 0 matches any entry with this microcode hash.
    bool find(uint64_t ucodeHash, uint64_t inputHash, CompiledShader& out) const;
    struct Key {
        uint64_t ucodeHash, inputHash;
    };
    std::vector<Key> keys() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Translate and compile one shader for both backends.
struct BuildResult {
    bool ok = false;
    std::string stage;            // where it failed: "translate", "dxil", "spirv"
    std::string error;
    CompiledShader shader;
};
BuildResult buildShader(const ShaderInfo& info, Compiler& compiler, bool keepHlsl);

// What the runtime does at CreateVertexShader / CreatePixelShader: the pack, then the cache,
// then translate, compile and store in the cache.
class ShaderProvider {
public:
    ShaderProvider(const ShaderPack* pack, const ShaderCache* cache) : pack_(pack), cache_(cache) {}
    bool get(const ShaderInfo& info, Compiler& compiler, CompiledShader& out, std::string* error) const;

private:
    const ShaderPack* pack_;
    const ShaderCache* cache_;
};

}  // namespace kkshaders
