#include "kkshaders/translator.h"

#include <cstring>

#include <xxh3.h>

#include "kkshaders/abi.h"

// The vendored XenosRecomp core.
#include "pch.h"
#include "shader_recompiler.h"

namespace kkshaders {

extern const char kPreludeText[];  // generated from hlsl/kk_common.hlsli

const std::string& prelude() {
    static const std::string text(kPreludeText);
    return text;
}

namespace {

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint32_t v) { bytes.push_back(uint8_t(v)); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) bytes.push_back(uint8_t(v >> (i * 8)));
    }
    void u64(uint64_t v) {
        u32(uint32_t(v));
        u32(uint32_t(v >> 32));
    }
    void str(const std::string& s) {
        u32(uint32_t(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
};

struct Reader {
    const uint8_t* p;
    size_t size;
    size_t at = 0;
    bool ok = true;
    uint32_t u8() {
        if (at + 1 > size) return ok = false, 0;
        return p[at++];
    }
    uint32_t u32() {
        if (at + 4 > size) return ok = false, 0;
        uint32_t v = uint32_t(p[at]) | (uint32_t(p[at + 1]) << 8) | (uint32_t(p[at + 2]) << 16) | (uint32_t(p[at + 3]) << 24);
        at += 4;
        return v;
    }
    std::string str() {
        uint32_t n = u32();
        if (!ok || at + n > size) return ok = false, std::string();
        std::string s(reinterpret_cast<const char*>(p + at), n);
        at += n;
        return s;
    }
};

}  // namespace

uint64_t translatorHash() {
    static const uint64_t hash = [] {
        Writer w;
        w.u32(kTranslatorVersion);
        w.u32(kAbiVersion);
        w.str(prelude());
        return XXH3_64bits(w.bytes.data(), w.bytes.size());
    }();
    return hash;
}

uint64_t translationInputHash(const ShaderInfo& info) {
    Writer w;
    w.u32(uint32_t(info.kind));
    w.u32(info.rawMicrocode ? 1 : 0);
    w.u64(info.ucodeHash);
    w.u32(uint32_t(info.ucode.size()));
    for (uint32_t v : info.ucode) w.u32(v);
    w.u32(uint32_t(info.constantTable.size()));
    w.bytes.insert(w.bytes.end(), info.constantTable.begin(), info.constantTable.end());
    w.u32(uint32_t(info.literals.size()));
    for (const auto& l : info.literals) {
        w.u32(l.registerIndex);
        for (uint32_t v : l.value) w.u32(v);
    }
    w.u32(uint32_t(info.loopLiterals.size()));
    for (const auto& [i, v] : info.loopLiterals) {
        w.u32(i);
        w.u32(v);
    }
    w.u32(uint32_t(info.boolLiterals.size()));
    for (const auto& [i, v] : info.boolLiterals) {
        w.u32(i);
        w.u32(v);
    }
    w.u32(uint32_t(info.fetches.size()));
    for (const auto& f : info.fetches) {
        w.u32(f.address);
        w.u32(uint32_t(f.usage));
        w.u32(f.usageIndex);
    }
    w.u32(uint32_t(info.interpolators.size()));
    for (const auto& i : info.interpolators) {
        w.u32(uint32_t(i.usage));
        w.u32(i.usageIndex);
        w.u32(i.reg);
    }
    w.u32(info.readsPixelPosition ? 1u + info.pixelPositionRegister : 0u);
    return XXH3_64bits(w.bytes.data(), w.bytes.size());
}

TranslateResult translate(const ShaderInfo& info) {
    TranslateResult result;

    RecompilerInput in;
    in.isPixelShader = info.kind == ShaderKind::Pixel;
    in.ucode = info.ucode;
    for (const auto& c : info.constants) {
        RecompilerConstant rc;
        rc.name = c.name;
        rc.registerSet = ::RegisterSet(uint16_t(c.registerSet));
        rc.registerIndex = c.registerIndex;
        rc.registerCount = c.registerCount;
        in.constants.push_back(rc);
    }
    for (const auto& l : info.literals) {
        std::array<uint32_t, 4> v = {l.value[0], l.value[1], l.value[2], l.value[3]};
        in.floatLiterals[l.registerIndex] = v;
    }
    for (const auto& [index, value] : info.loopLiterals) in.loopLiterals[index] = value;
    // Bool literals: the register block writes whole dwords of 32 bools, and the game sets
    // the bools its constant table names afterwards (SetVertexShaderConstantB and the pixel
    // equivalent; vertex bools are 0-127, pixel ones 128-255). So the bits of a literal dword
    // that the table does not name are the shader's own and are inlined; named ones are read
    // from the draw constants.
    {
        uint32_t named[8] = {};
        uint32_t base = info.kind == ShaderKind::Pixel ? 128 : 0;
        for (const auto& c : info.constants) {
            if (c.registerSet != RegisterSet::Bool) continue;
            for (uint32_t k = 0; k < c.registerCount; k++) {
                uint32_t index = base + c.registerIndex + k;
                if (index < 256) named[index >> 5] |= 1u << (index & 31);
            }
        }
        for (const auto& [dword, value] : info.boolLiterals) {
            if (dword >= 8) continue;
            for (uint32_t bit = 0; bit < 32; bit++)
                if (!(named[dword] & (1u << bit))) in.boolLiterals[dword * 32 + bit] = ((value >> bit) & 1) != 0;
        }
    }

    if (info.rawMicrocode) {
        in.rawVertexFetch = true;
        in.rawInterpolators = true;
    } else {
        for (const auto& f : info.fetches) {
            RecompilerVertexElement e;
            e.address = f.address;
            e.usage = ::DeclUsage(uint32_t(f.usage));
            e.usageIndex = f.usageIndex;
            in.vertexElements.push_back(e);
        }
        for (const auto& i : info.interpolators) {
            RecompilerInterpolator ri;
            ri.usage = ::DeclUsage(uint32_t(i.usage));
            ri.usageIndex = i.usageIndex;
            ri.reg = i.reg;
            in.interpolators.push_back(ri);
        }
        if (info.kind == ShaderKind::Pixel && info.readsPixelPosition) in.paramGenRegister = info.pixelPositionRegister;
    }

    ShaderRecompiler recompiler;
    std::string header = "// Translated by kkshaders (XenosRecomp core) from Xbox 360 microcode.\n";
    header += "// Microcode hash " + [&] {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(info.ucodeHash));
        return std::string(buf);
    }() + (info.rawMicrocode ? " (bare microcode)\n" : "\n");
    if (!recompiler.recompile(in, prelude())) {
        result.error = recompiler.error.empty() ? "translation failed" : recompiler.error;
        return result;
    }
    result.hlsl = header + recompiler.out;

    ShaderBindings& b = result.bindings;
    b.kind = info.kind;
    for (const auto& vb : recompiler.vertexBindings) {
        VertexBinding v;
        v.raw = vb.raw;
        if (vb.raw) {
            v.fetchConstant = uint8_t(vb.fetchConstant);
        } else {
            const auto& e = in.vertexElements[vb.element];
            v.usage = DeclUsage(uint32_t(e.usage));
            v.usageIndex = uint8_t(e.usageIndex);
        }
        b.vertexBindings.push_back(v);
    }
    for (const auto& s : recompiler.samplerBindings) {
        SamplerBinding sb;
        sb.slot = uint8_t(s.slot);
        sb.dimension = TextureDimension(uint32_t(s.dimension));
        sb.magFilter = uint8_t(s.magFilter);
        sb.minFilter = uint8_t(s.minFilter);
        sb.mipFilter = uint8_t(s.mipFilter);
        sb.anisoFilter = uint8_t(s.anisoFilter);
        sb.volMagFilter = uint8_t(s.volMagFilter);
        sb.volMinFilter = uint8_t(s.volMinFilter);
        b.samplers.push_back(sb);
    }
    for (const auto& t : recompiler.textures) b.textures.push_back({uint8_t(t.slot), TextureDimension(uint32_t(t.dimension))});
    b.pixelOutputs = recompiler.pixelOutputs;
    b.interpolators = in.rawInterpolators && !in.isPixelShader ? 0xFFFFu : recompiler.exportedInterpolators;
    b.tempRegisters = recompiler.tempRegisterCount;
    b.generalControlFlow = recompiler.generalControlFlow;
    b.dynamicRegisters = recompiler.usesRegisterArray;
    b.registerWrites = info.registerWrites;
    b.warnings = recompiler.warnings;
    result.ok = true;
    return result;
}

std::vector<uint8_t> serializeBindings(const ShaderBindings& b) {
    Writer w;
    w.u32(0x4E424B4Bu);  // 'KKBN'
    w.u32(uint32_t(b.kind));
    w.u32(uint32_t(b.vertexBindings.size()));
    for (const auto& v : b.vertexBindings) {
        w.u8(v.raw);
        w.u8(uint32_t(v.usage));
        w.u8(v.usageIndex);
        w.u8(v.fetchConstant);
    }
    w.u32(uint32_t(b.samplers.size()));
    for (const auto& s : b.samplers) {
        w.u8(s.slot);
        w.u8(uint32_t(s.dimension));
        w.u8(s.magFilter);
        w.u8(s.minFilter);
        w.u8(s.mipFilter);
        w.u8(s.anisoFilter);
        w.u8(s.volMagFilter);
        w.u8(s.volMinFilter);
    }
    w.u32(uint32_t(b.textures.size()));
    for (const auto& t : b.textures) {
        w.u8(t.slot);
        w.u8(uint32_t(t.dimension));
    }
    w.u32(b.pixelOutputs);
    w.u32(b.interpolators);
    w.u32(b.tempRegisters);
    w.u32((b.generalControlFlow ? 1u : 0u) | (b.dynamicRegisters ? 2u : 0u));
    w.u32(uint32_t(b.registerWrites.size()));
    for (const auto& r : b.registerWrites) {
        w.u32(r.offset);
        w.u32(uint32_t(r.values.size()));
        for (uint32_t v : r.values) w.u32(v);
    }
    w.u32(uint32_t(b.warnings.size()));
    for (const auto& s : b.warnings) w.str(s);
    return w.bytes;
}

bool deserializeBindings(const uint8_t* data, size_t size, ShaderBindings& b) {
    Reader r{data, size};
    if (r.u32() != 0x4E424B4Bu) return false;
    b = {};
    b.kind = ShaderKind(r.u32());
    uint32_t n = r.u32();
    for (uint32_t i = 0; r.ok && i < n && i < 64; i++) {
        VertexBinding v;
        v.raw = r.u8() != 0;
        v.usage = DeclUsage(r.u8());
        v.usageIndex = uint8_t(r.u8());
        v.fetchConstant = uint8_t(r.u8());
        b.vertexBindings.push_back(v);
    }
    n = r.u32();
    for (uint32_t i = 0; r.ok && i < n && i < 64; i++) {
        SamplerBinding s;
        s.slot = uint8_t(r.u8());
        s.dimension = TextureDimension(r.u8());
        s.magFilter = uint8_t(r.u8());
        s.minFilter = uint8_t(r.u8());
        s.mipFilter = uint8_t(r.u8());
        s.anisoFilter = uint8_t(r.u8());
        s.volMagFilter = uint8_t(r.u8());
        s.volMinFilter = uint8_t(r.u8());
        b.samplers.push_back(s);
    }
    n = r.u32();
    for (uint32_t i = 0; r.ok && i < n && i < 64; i++) {
        TextureUse t;
        t.slot = uint8_t(r.u8());
        t.dimension = TextureDimension(r.u8());
        b.textures.push_back(t);
    }
    b.pixelOutputs = r.u32();
    b.interpolators = r.u32();
    b.tempRegisters = r.u32();
    uint32_t flags = r.u32();
    b.generalControlFlow = flags & 1;
    b.dynamicRegisters = flags & 2;
    n = r.u32();
    for (uint32_t i = 0; r.ok && i < n && i < 4096; i++) {
        RegisterWrite w;
        w.offset = uint16_t(r.u32());
        uint32_t count = r.u32();
        for (uint32_t k = 0; r.ok && k < count && k < 65536; k++) w.values.push_back(r.u32());
        b.registerWrites.push_back(std::move(w));
    }
    n = r.u32();
    for (uint32_t i = 0; r.ok && i < n && i < 256; i++) b.warnings.push_back(r.str());
    return r.ok && r.at == size;
}

}  // namespace kkshaders
