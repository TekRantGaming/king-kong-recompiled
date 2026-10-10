// What the translated shaders compute, checked on a GPU against the reference interpreter.
//
//   kkshaders_gpu_tests interp                      the interpreter's own checks (no GPU)
//   kkshaders_gpu_tests gpu --dxc <dir> [options]   the differential test:
//       --corpus 0|1        run the generated corpus (default 1; its fuzz group with --fuzz N)
//       --random N          random programs per kind (vertex / pixel, container / bare; default 200)
//       --rounds R          random environments per shader (default 3)
//       --seed S            (default 20261010)
//       --filter TEXT       only shaders whose name contains TEXT
//       --out DIR           failing shaders' HLSL and a report (default gpu-out)
//       --max-report K      mismatches printed per shader (default 4)
//       --progress 1        a line per shader with its time
//       --hlsl FILE         debugging: compile FILE instead of the translation (with --filter)
//
// Each shader is translated as the game's shaders are (kkshaders::translate), compiled to
// SPIR-V with DXC, and run on the Vulkan device (lavapipe in the container):
//   - vertex shaders inside a compute shader that calls the translated main() for 64
//     vertices and stores every output (the vertex stage itself only adds the fixed-function
//     clip and viewport steps the backend owns);
//   - pixel shaders drawn for real, as 64 one-pixel points whose interpolators come from a
//     harness vertex shader, into four RGBA32F targets.
// Each round draws a random environment: float, bool and loop constants, vertex data in every
// format with random declarations (binding mode) or fetch constants (instruction mode),
// small random textures (1D, 2D, 3D, cube) with point or linear filtering and wrap or clamp
// addressing, and random interpolators. Values are dyadic (k / 16 and the like) so most
// arithmetic is exact; the interpreter's error bounds cover the rest (see xenos_interp.h).
// Literal constants are NOT applied to the uploaded constants: the translated code must
// answer them itself.

#include <algorithm>
#include <chrono>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "corpus.h"
#include "kkshaders/abi.h"
#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/translator.h"
#include "random_program.h"
#include "vk_harness.h"
#include "xenos_asm.h"
#include "xenos_interp.h"

namespace fs = std::filesystem;
using namespace kkshaders;

namespace {

std::string option(int argc, char** argv, const char* name, const char* fallback = "") {
    for (int i = 2; i + 1 < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return fallback;
}

int interpolatorSlot(DeclUsage usage, uint32_t index) {
    if (usage == DeclUsage::TexCoord && index < 16) return int(index);
    if (usage == DeclUsage::Color && index < 2) return 16 + int(index);
    return -1;
}

constexpr uint32_t kInvocations = 64;  // per round: 64 vertices, or 8 x 8 pixels
constexpr uint32_t kGrid = 8;
constexpr float kSentinel = -12345.6875f;

// ------------------------------------------------------------------------------------------
// Random environments.

class Rng {
public:
    explicit Rng(uint32_t seed) : g_(seed) {}
    uint32_t u32() { return uint32_t(g_()); }
    uint32_t below(uint32_t n) { return n ? u32() % n : 0; }
    bool chance(uint32_t percent) { return below(100) < percent; }
    // A "nice" float: k / 16 in [-4, 4], with some special values.
    float nice() {
        uint32_t k = below(100);
        if (k < 6) return 0.0f;
        if (k < 9) return 1.0f;
        if (k < 11) return -1.0f;
        if (k < 13) return 0.5f;
        if (k < 14) return 256.0f;
        if (k < 15) return 1.0f / 1024.0f;
        if (k < 16) return 3.0f;
        return float(int32_t(below(129)) - 64) / 16.0f;
    }
    // A dword of vertex data: a nice float, two nice halves, or random bits.
    uint32_t vertexDword() {
        uint32_t k = below(100);
        if (k < 45) return std::bit_cast<uint32_t>(nice());
        if (k < 70) {
            auto half = [&]() -> uint32_t {
                // k / 16 in [-8, 8] as a binary16.
                int32_t v = int32_t(below(257)) - 128;
                if (v == 0) return 0;
                float f = float(v) / 16.0f;
                uint32_t b = std::bit_cast<uint32_t>(f);
                uint32_t sign = (b >> 16) & 0x8000u;
                int32_t e = int32_t((b >> 23) & 0xFF) - 127 + 15;
                return sign | (uint32_t(e) << 10) | ((b >> 13) & 0x3FF);
            };
            return half() | (half() << 16);
        }
        return u32();
    }

private:
    std::mt19937 g_;
};

struct Env {
    std::array<std::array<float, 4>, 256> constants{};
    std::array<uint32_t, 8> bools{};
    std::array<uint32_t, 32> loops{};
    std::map<uint32_t, xinterp::Texture> textures;  // by fetch constant slot
    std::vector<xinterp::Element> elements;          // binding mode, per binding
    std::vector<xinterp::FetchConstant> rawConstants; // instruction mode, per binding
    std::vector<std::vector<uint8_t>> buffers;       // per binding
    std::vector<std::array<std::array<float, 4>, 18>> interpolators;  // pixel, per invocation
};

uint32_t formatDwords(uint32_t f) {
    switch (f) {
        case 26: case 32: case 34: case 37: return 2;
        case 57: return 3;
        case 35: case 38: return 4;
        default: return 1;
    }
}

Env makeEnv(Rng& rng, const ShaderInfo& info, const ShaderBindings& b) {
    Env env;
    for (auto& c : env.constants)
        for (auto& v : c) v = rng.nice();
    for (auto& w : env.bools) w = rng.u32();
    for (auto& l : env.loops) {
        uint32_t count = rng.below(5), start = rng.below(7);
        int32_t step = int32_t(rng.below(5)) - 2;
        // Now and then an aL that leaves [-256, 256] within a few iterations (it is clamped).
        if (rng.below(8) == 0) {
            bool up = rng.below(2) != 0;
            start = up ? 200 + rng.below(56) : rng.below(8);
            step = up ? 64 + int32_t(rng.below(64)) : -128 + int32_t(rng.below(16));
        }
        l = count | (start << 8) | ((uint32_t(step) & 0xFF) << 16);
    }
    // Textures for the slots the shader samples.
    for (const auto& t : b.textures) {
        xinterp::Texture tex;
        tex.dim = xinterp::TexDim(uint32_t(t.dimension));
        const uint32_t sizes[] = {1, 2, 3, 4, 5, 8};
        switch (tex.dim) {
            case xinterp::TexDim::D1: tex.width = sizes[rng.below(6)]; break;
            case xinterp::TexDim::D2: tex.width = sizes[rng.below(6)]; tex.height = sizes[rng.below(6)]; break;
            case xinterp::TexDim::D3: tex.width = 1 + rng.below(4); tex.height = 1 + rng.below(4); tex.depth = 1 + rng.below(4); break;
            case xinterp::TexDim::Cube: tex.width = tex.height = 1u << rng.below(3); tex.depth = 6; break;
        }
        tex.texels.resize(size_t(tex.width) * tex.height * tex.depth * 4);
        for (auto& v : tex.texels) v = float(int32_t(rng.below(97)) - 32) / 32.0f;
        tex.linear = rng.chance(50);
        tex.clamp = rng.chance(50);
        env.textures[t.slot] = std::move(tex);
    }
    // Vertex data.
    const uint32_t formats[] = {6, 7, 16, 17, 25, 26, 31, 32, 33, 34, 35, 36, 37, 38, 57};
    for (size_t i = 0; i < b.vertexBindings.size(); i++) {
        const VertexBinding& vb = b.vertexBindings[i];
        std::vector<uint8_t> data;
        uint32_t bytes = 0;
        if (!vb.raw) {
            xinterp::Element e;
            uint32_t format = rng.chance(8) ? 0 : formats[rng.below(15)];
            uint32_t size = formatDwords(format) * 4;
            e.stride = size + 4 * rng.below(5);
            e.offset = 4 * rng.below(3);
            uint32_t endian = rng.chance(80) ? 2 : rng.below(4);
            int32_t expAdjust = rng.chance(15) ? int32_t(rng.below(9)) - 4 : 0;
            const uint32_t swizzles[] = {
                vertex_format::kIdentitySwizzle >> vertex_format::kSwizzleShift,
                0u | (1u << 3) | (4u << 6) | (5u << 9),  // x y 0 1
                2u | (1u << 3) | (0u << 6) | (3u << 9),  // z y x w
                0u | (1u << 3) | (2u << 6) | (5u << 9),  // x y z 1
            };
            uint32_t swizzle = swizzles[rng.below(4)];
            e.word = format | (rng.chance(50) ? vertex_format::kSigned : 0) | (rng.chance(30) ? vertex_format::kInteger : 0) |
                     (rng.chance(30) ? vertex_format::kSignedNoZero : 0) | (endian << vertex_format::kEndianShift) |
                     ((uint32_t(expAdjust) & 0x3F) << vertex_format::kExpAdjustShift) | (swizzle << vertex_format::kSwizzleShift);
            e.buffer = uint32_t(i);
            bytes = e.offset + e.stride * kInvocations + 64;
            env.elements.push_back(e);
        } else {
            xinterp::FetchConstant fc;
            fc.buffer = uint32_t(i);
            fc.offset = 256;
            fc.size = 8192;
            fc.endian = rng.chance(80) ? 2 : rng.below(4);
            bytes = fc.offset + fc.size + 64;
            env.rawConstants.push_back(fc);
        }
        data.resize(bytes);
        // Big-endian guest dwords (read through the endian swap).
        for (uint32_t at = 0; at + 4 <= bytes; at += 4) {
            uint32_t v = rng.vertexDword();
            data[at] = uint8_t(v >> 24);
            data[at + 1] = uint8_t(v >> 16);
            data[at + 2] = uint8_t(v >> 8);
            data[at + 3] = uint8_t(v);
        }
        env.buffers.push_back(std::move(data));
    }
    if (info.kind == ShaderKind::Pixel) {
        env.interpolators.resize(kInvocations);
        for (auto& inv : env.interpolators)
            for (auto& slot : inv)
                for (auto& v : slot) v = rng.nice();
    }
    return env;
}

// The bool constants a shader sees: the game's values, with the shader's literal dwords
// applied to the bits its constant table does not name (those the game sets itself).
std::array<uint32_t, 8> effectiveBools(const ShaderInfo& info, const std::array<uint32_t, 8>& bools) {
    std::array<uint32_t, 8> out = bools;
    std::array<uint32_t, 8> named{};
    uint32_t base = info.kind == ShaderKind::Pixel ? 128 : 0;
    for (const auto& c : info.constants) {
        if (c.registerSet != RegisterSet::Bool) continue;
        for (uint32_t k = 0; k < c.registerCount; k++) {
            uint32_t index = base + c.registerIndex + k;
            if (index < 256) named[index >> 5] |= 1u << (index & 31);
        }
    }
    for (const auto& [dword, value] : info.boolLiterals)
        if (dword < 8) out[dword] = (out[dword] & named[dword]) | (value & ~named[dword]);
    return out;
}

xinterp::Inputs interpreterInputs(const ShaderInfo& info, const ShaderBindings& b, const Env& env) {
    xinterp::Inputs in;
    in.pixel = info.kind == ShaderKind::Pixel;
    in.ucode = info.ucode;
    in.constants = env.constants;
    for (const auto& l : info.literals)
        if (l.registerIndex < 256)
            for (int k = 0; k < 4; k++) in.constants[l.registerIndex][size_t(k)] = std::bit_cast<float>(l.value[k]);
    in.bools = effectiveBools(info, env.bools);
    in.loops = env.loops;
    for (const auto& [index, value] : info.loopLiterals)
        if (index < 32) in.loops[index] = value;
    in.buffers = env.buffers;
    in.bindingMode = !info.rawMicrocode;
    if (in.bindingMode) {
        for (size_t i = 0; i < info.fetches.size(); i++) in.vfetchElement[info.fetches[i].address] = uint32_t(i);
        in.elements = env.elements;
    } else {
        for (size_t i = 0; i < b.vertexBindings.size(); i++)
            if (b.vertexBindings[i].raw) in.fetchConstants[b.vertexBindings[i].fetchConstant] = env.rawConstants[i];
    }
    for (const auto& [slot, tex] : env.textures) in.textures[slot] = &tex;
    return in;
}

void pixelRegisters(const ShaderInfo& info, const Env& env, uint32_t invocation, xinterp::Inputs& in) {
    in.initialRegisters = {};
    const auto& slots = env.interpolators[invocation];
    if (info.rawMicrocode) {
        for (uint32_t i = 0; i < 16; i++) in.initialRegisters[i] = slots[i];
    } else {
        for (const auto& interp : info.interpolators) {
            int slot = interpolatorSlot(interp.usage, interp.usageIndex);
            if (slot >= 0 && interp.reg < 64) in.initialRegisters[interp.reg] = slots[size_t(slot)];
        }
        if (info.readsPixelPosition && info.pixelPositionRegister < 64)
            in.initialRegisters[info.pixelPositionRegister] = {float(invocation % kGrid), float(invocation / kGrid), 0.0f, 0.0f};
    }
}

// ------------------------------------------------------------------------------------------
// Comparison.

struct Stats {
    uint32_t shaders = 0, translateFailures = 0, compileFailures = 0, gpuFailures = 0;
    uint64_t invocations = 0, compared = 0, components = 0, unknownComponents = 0, fragile = 0, timeouts = 0;
    uint64_t mismatches = 0;
    uint32_t mismatchedShaders = 0;
    std::map<std::string, uint32_t> mismatchByGroup;
};

bool close(float gpu, const xinterp::Num& ref) {
    if (std::isnan(ref.v)) return std::isnan(gpu);
    if (std::isinf(ref.v)) return gpu == ref.v;
    if (std::isnan(gpu) || std::isinf(gpu)) return false;
    float tol = ref.e + std::fabs(ref.v) * 0x1p-20f + 1e-30f;
    // A flushed denormal result.
    if (std::fabs(ref.v) < 1.2e-38f) tol += 1.2e-38f;
    return std::fabs(gpu - ref.v) <= tol;
}

std::string describe(const xinterp::Num& n) {
    char buf[96];
    if (n.unknown) return "unknown";
    std::snprintf(buf, sizeof(buf), "%.9g (+-%.3g)", double(n.v), double(n.e));
    return buf;
}

// ------------------------------------------------------------------------------------------
// The GPU programs.

const char* kHarnessVs = R"(
struct KKHPoint { float4 position; float4 v[18]; };
StructuredBuffer<KKHPoint> kkh_In : register(t0, space7);
struct KKHOut {
    float4 pos : SV_Position;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;
    float4 t8 : TEXCOORD8; float4 t9 : TEXCOORD9; float4 t10 : TEXCOORD10; float4 t11 : TEXCOORD11;
    float4 t12 : TEXCOORD12; float4 t13 : TEXCOORD13; float4 t14 : TEXCOORD14; float4 t15 : TEXCOORD15;
    float4 d0 : COLOR0; float4 d1 : COLOR1;
    [[vk::builtin("PointSize")]] float size : PSIZE;
};
KKHOut kkh_vs(uint id : SV_VertexID)
{
    KKHPoint p = kkh_In[id];
    KKHOut o;
    o.pos = float4((p.position.x + 0.5) / 8.0 * 2.0 - 1.0, (p.position.y + 0.5) / 8.0 * 2.0 - 1.0, 0.5, 1.0);
    o.t0 = p.v[0]; o.t1 = p.v[1]; o.t2 = p.v[2]; o.t3 = p.v[3]; o.t4 = p.v[4]; o.t5 = p.v[5]; o.t6 = p.v[6]; o.t7 = p.v[7];
    o.t8 = p.v[8]; o.t9 = p.v[9]; o.t10 = p.v[10]; o.t11 = p.v[11]; o.t12 = p.v[12]; o.t13 = p.v[13]; o.t14 = p.v[14]; o.t15 = p.v[15];
    o.d0 = p.v[16]; o.d1 = p.v[17];
    o.size = 1.0;
    return o;
}
)";

// The compute wrapper around a translated vertex shader: every output of 64 vertices.
std::string vertexWrapper(const std::string& hlsl) {
    std::string s = "#define main kk_translated_main\n" + hlsl + "\n#undef main\n";
    s += R"(
RWByteAddressBuffer kkh_Out : register(u0, space7);
[numthreads(64, 1, 1)]
void kkh_main(uint3 id : SV_DispatchThreadID)
{
    float4 o[19];
    float4 c0;
    float2 c1;
    kk_translated_main(id.x, o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10], o[11], o[12], o[13], o[14],
        o[15], o[16], o[17], o[18], c0, c1);
    for (uint i = 0; i < 19; i++)
        kkh_Out.Store4((id.x * 19 + i) * 16, asuint(o[i]));
}
)";
    return s;
}

struct GpuProgram {
    VkShaderModule module = VK_NULL_HANDLE, harness = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

class Runner {
public:
    Runner(vkh::Gpu& gpu, Compiler& compiler, const fs::path& out, uint32_t maxReport, bool trace)
        : gpu_(gpu), compiler_(compiler), out_(out), maxReport_(maxReport), trace_(trace) {}

    bool init() {
        CompileOutput vs = compiler_.compileEntry(kHarnessVs, L"vs_6_0", L"kkh_vs", CompileTarget::Spirv);
        if (!vs.ok) {
            std::printf("harness vertex shader: %s\n", vs.messages.c_str());
            return false;
        }
        harnessVs_ = gpu_.shader(vs.blob);
        return harnessVs_ != VK_NULL_HANDLE;
    }
    ~Runner() { gpu_.destroy(harnessVs_); }

    void runShader(const CorpusShader& shader, uint32_t rounds, uint32_t seed, Stats& stats);

private:
    vkh::Gpu& gpu_;
    Compiler& compiler_;
    fs::path out_;
    uint32_t maxReport_;
    bool trace_;
public:
    std::string hlslOverride;
private:
    VkShaderModule harnessVs_ = VK_NULL_HANDLE;

    bool runRound(const CorpusShader& shader, const ShaderInfo& info, const TranslateResult& tr, const GpuProgram& prog,
                  const Env& env, uint32_t round, Stats& stats, uint32_t& reported, bool& mismatched);
};

void Runner::runShader(const CorpusShader& shader, uint32_t rounds, uint32_t seed, Stats& stats) {
    stats.shaders++;
    ShaderKind kind = shader.vertex ? ShaderKind::Vertex : ShaderKind::Pixel;
    ParseResult parsed = shader.raw ? parseMicrocode(kind, shader.bytes.data(), shader.bytes.size())
                                    : parseContainer(shader.bytes.data(), shader.bytes.size());
    if (!parsed.ok) {
        stats.translateFailures++;
        std::printf("  %s: parse failed: %s\n", shader.name.c_str(), parsed.error.c_str());
        return;
    }
    const ShaderInfo& info = parsed.info;
    TranslateResult tr = translate(info);
    if (!hlslOverride.empty() && tr.ok) {
        // Debugging: an edited translation (for instance with a register routed to an export).
        std::ifstream f(hlslOverride);
        tr.hlsl.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (!tr.ok) {
        stats.translateFailures++;
        return;  // the translator refusing a random program (too many bindings and the like)
    }
    GpuProgram prog;
    std::string source = shader.vertex ? vertexWrapper(tr.hlsl) : tr.hlsl;
    CompileOutput co = shader.vertex ? compiler_.compileEntry(source, L"cs_6_0", L"kkh_main", CompileTarget::Spirv)
                                     : compiler_.compile(source, ShaderKind::Pixel, CompileTarget::Spirv);
    if (!co.ok) {
        stats.compileFailures++;
        std::printf("  %s: compile failed:\n%s\n", shader.name.c_str(), co.messages.c_str());
        std::ofstream(out_ / (shader.group + "-" + std::to_string(stats.shaders) + ".hlsl")) << source;
        return;
    }
    prog.module = gpu_.shader(co.blob);
    if (shader.vertex) {
        prog.pipeline = gpu_.computePipeline(prog.module, "kkh_main");
    } else {
        std::vector<VkColorComponentFlags> masks(4, 0);
        for (uint32_t i = 0; i < 4; i++)
            if (tr.bindings.pixelOutputs & (1u << i)) masks[i] = 0xF;
        prog.pipeline = gpu_.pointPipeline(harnessVs_, "kkh_vs", prog.module, "main", masks);
    }
    if (!prog.pipeline) {
        stats.gpuFailures++;
        std::printf("  %s: pipeline creation failed\n", shader.name.c_str());
        gpu_.destroy(prog.module);
        return;
    }
    uint32_t reported = 0;
    bool mismatched = false;
    for (uint32_t round = 0; round < rounds; round++) {
        Rng rng(seed + round * 7919u);
        Env env = makeEnv(rng, info, tr.bindings);
        if (!runRound(shader, info, tr, prog, env, round, stats, reported, mismatched)) break;
    }
    if (mismatched) {
        stats.mismatchedShaders++;
        stats.mismatchByGroup[shader.group]++;
        std::string name = shader.name;
        std::replace_if(name.begin(), name.end(), [](char c) { return !std::isalnum(uint8_t(c)); }, '_');
        std::ofstream(out_ / (shader.group + "-" + name + ".hlsl")) << tr.hlsl;
    }
    gpu_.destroy(prog.pipeline);
    gpu_.destroy(prog.module);
}

bool Runner::runRound(const CorpusShader& shader, const ShaderInfo& info, const TranslateResult& tr, const GpuProgram& prog,
                      const Env& env, uint32_t round, Stats& stats, uint32_t& reported, bool& mismatched) {
    const ShaderBindings& b = tr.bindings;
    const bool vertex = shader.vertex;

    // The reference first (a program that does not terminate is not run on the GPU).
    xinterp::Inputs base = interpreterInputs(info, b, env);
    std::vector<xinterp::Outputs> ref(kInvocations);
    for (uint32_t i = 0; i < kInvocations; i++) {
        xinterp::Inputs in = base;
        if (vertex) in.vertexIndex = i;
        else pixelRegisters(info, env, i, in);
        ref[i] = xinterp::run(in);
        if (ref[i].error) {
            std::printf("  %s: interpreter: %s\n", shader.name.c_str(), ref[i].message.c_str());
            return false;
        }
        if (ref[i].timeout) {
            stats.timeouts++;
            return true;
        }
    }

    // Constants and resources.
    std::vector<vkh::Buffer> buffers;
    std::vector<vkh::Image> images;
    auto ubo = [&](const void* data, size_t size) {
        vkh::Buffer bb = gpu_.buffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        if (data) std::memcpy(bb.mapped, data, size);
        buffers.push_back(bb);
        return bb;
    };
    vkh::Bindings bind;
    std::vector<float> stageConstants(256 * 4);
    for (size_t i = 0; i < 256; i++)
        for (size_t k = 0; k < 4; k++) stageConstants[i * 4 + k] = env.constants[i][k];
    vkh::Buffer vsc = ubo(vertex ? stageConstants.data() : nullptr, 4096);
    vkh::Buffer psc = ubo(vertex ? nullptr : stageConstants.data(), 4096);
    DrawConstants dc{};
    for (size_t i = 0; i < 8; i++) dc.boolConstants[i] = env.bools[i];
    for (size_t i = 0; i < 32; i++) dc.loopConstants[i] = env.loops[i];
    dc.ndcScale[0] = dc.ndcScale[1] = dc.ndcScale[2] = 1.0f;
    for (const auto& [slot, tex] : env.textures) {
        VkImageViewType type = tex.dim == xinterp::TexDim::D3 ? VK_IMAGE_VIEW_TYPE_3D
                               : tex.dim == xinterp::TexDim::Cube ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                  : VK_IMAGE_VIEW_TYPE_2D;
        vkh::Image im = gpu_.image(type, VK_FORMAT_R32G32B32A32_SFLOAT, tex.width, tex.height,
                                   tex.dim == xinterp::TexDim::D3 ? tex.depth : 1, tex.dim == xinterp::TexDim::Cube ? 6 : 1,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        gpu_.upload(im, tex.texels.data(), tex.texels.size() * 4);
        images.push_back(im);
        std::vector<VkImageView>* table = tex.dim == xinterp::TexDim::D3 ? &bind.tex3D
                                          : tex.dim == xinterp::TexDim::Cube ? &bind.texCube
                                                                             : &bind.tex2D;
        dc.textureIndex[slot] = uint32_t(table->size());
        table->push_back(im.view);
    }
    for (size_t i = 0; i < b.samplers.size(); i++) {
        const SamplerBinding& s = b.samplers[i];
        auto t = env.textures.find(s.slot);
        bool texLinear = t != env.textures.end() && t->second.linear;
        bool clamp = t != env.textures.end() && t->second.clamp;
        bind.samplers.push_back(gpu_.sampler(xinterp::fetchIsLinear(s.magFilter, texLinear), clamp));
        if (vertex) dc.vertexSamplers[i] = uint32_t(i);
        else dc.pixelSamplers[i] = uint32_t(i);
    }
    for (size_t i = 0; i < b.vertexBindings.size(); i++) {
        const auto& data = env.buffers[i];
        vkh::Buffer vb = gpu_.buffer((data.size() + 15) & ~size_t(15), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(vb.mapped, data.data(), data.size());
        buffers.push_back(vb);
        bind.buffers.push_back(vb.buffer);
        if (b.vertexBindings[i].raw) {
            const auto& fc = env.rawConstants[i];
            dc.vertexFetch[i][0] = uint32_t(i);
            dc.vertexFetch[i][1] = fc.offset;
            dc.vertexFetch[i][2] = fc.size;
            dc.vertexFetch[i][3] = fc.endian;
        } else {
            const auto& e = env.elements[i];
            dc.vertexFetch[i][0] = uint32_t(i);
            dc.vertexFetch[i][1] = e.offset;
            dc.vertexFetch[i][2] = e.stride;
            dc.vertexFetch[i][3] = e.word;
        }
    }
    vkh::Buffer dcb = ubo(&dc, sizeof(dc));
    bind.constants[0] = vsc.buffer;
    bind.constants[1] = psc.buffer;
    bind.constants[2] = dcb.buffer;
    bind.constantSizes[0] = 4096;
    bind.constantSizes[1] = 4096;
    bind.constantSizes[2] = sizeof(dc);

    // Run.
    std::vector<float> gpuOut;
    bool ok;
    if (vertex) {
        vkh::Buffer outBuf = gpu_.buffer(kInvocations * 19 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        buffers.push_back(outBuf);
        bind.harness = outBuf.buffer;
        bind.harnessSize = outBuf.size;
        ok = gpu_.run(bind, VK_PIPELINE_BIND_POINT_COMPUTE, [&](VkCommandBuffer cb) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, prog.pipeline);
            vkCmdDispatch(cb, kInvocations / 64, 1, 1);
        });
        if (ok) {
            gpuOut.resize(kInvocations * 19 * 4);
            std::memcpy(gpuOut.data(), outBuf.mapped, gpuOut.size() * 4);
        }
    } else {
        vkh::Buffer points = gpu_.buffer(kInvocations * 19 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        buffers.push_back(points);
        float* p = static_cast<float*>(points.mapped);
        for (uint32_t i = 0; i < kInvocations; i++) {
            p[i * 76 + 0] = float(i % kGrid);
            p[i * 76 + 1] = float(i / kGrid);
            for (uint32_t s = 0; s < 18; s++)
                for (uint32_t k = 0; k < 4; k++) p[i * 76 + 4 + s * 4 + k] = env.interpolators[i][s][k];
        }
        bind.harness = points.buffer;
        bind.harnessSize = points.size;
        vkh::Image targets[4];
        for (auto& t : targets) {
            t = gpu_.image(VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, kGrid, kGrid, 1, 1,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            images.push_back(t);
        }
        ok = gpu_.run(bind, VK_PIPELINE_BIND_POINT_GRAPHICS, [&](VkCommandBuffer cb) {
            VkImageMemoryBarrier barriers[4];
            for (int i = 0; i < 4; i++) {
                barriers[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barriers[i].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].image = targets[i].image;
                barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                                 0, nullptr, 4, barriers);
            VkRenderingAttachmentInfo att[4];
            for (int i = 0; i < 4; i++) {
                att[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                att[i].imageView = targets[i].view;
                att[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                att[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                for (int k = 0; k < 4; k++) att[i].clearValue.color.float32[k] = kSentinel;
            }
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {kGrid, kGrid}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 4;
            ri.pColorAttachments = att;
            vkCmdBeginRendering(cb, &ri);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, prog.pipeline);
            VkViewport vp{0, 0, float(kGrid), float(kGrid), 0, 1};
            VkRect2D sc{{0, 0}, {kGrid, kGrid}};
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            vkCmdDraw(cb, kInvocations, 1, 0, 0);
            vkCmdEndRendering(cb);
        });
        if (ok) {
            gpuOut.resize(4 * kInvocations * 4);
            for (int t = 0; t < 4 && ok; t++) ok = gpu_.download(targets[t], gpuOut.data() + t * kInvocations * 4, kInvocations * 16);
        }
    }
    for (auto& bb : buffers) gpu_.destroy(bb);
    for (auto& im : images) gpu_.destroy(im);
    if (!ok) {
        stats.gpuFailures++;
        std::printf("  %s: GPU run failed (device lost or timeout)\n", shader.name.c_str());
        return false;
    }

    // Compare.
    bool traced = false;
    auto mismatch = [&](uint32_t inv, const std::string& what, int comp, float gpu, const xinterp::Num& r) {
        stats.mismatches++;
        mismatched = true;
        if (reported++ < maxReport_)
            std::printf("  MISMATCH %s [%s] round %u invocation %u %s.%c: gpu %.9g, reference %s\n", shader.name.c_str(),
                        shader.group.c_str(), round, inv, what.c_str(), "xyzw"[comp], double(gpu), describe(r).c_str());
        if (trace_ && !traced) {
            traced = true;
            xinterp::Inputs in = base;
            if (vertex) in.vertexIndex = inv;
            else pixelRegisters(info, env, inv, in);
            in.trace = true;
            std::printf("%s", xinterp::run(in).trace.c_str());
        }
    };
    for (uint32_t inv = 0; inv < kInvocations; inv++) {
        const xinterp::Outputs& r = ref[inv];
        stats.invocations++;
        if (r.fragile) {
            stats.fragile++;
            continue;
        }
        stats.compared++;
        auto compareVec = [&](const std::string& what, const float* gpu, const xinterp::Vec& expect) {
            for (int k = 0; k < 4; k++) {
                stats.components++;
                if (expect[k].unknown) {
                    stats.unknownComponents++;
                    continue;
                }
                if (!close(gpu[k], expect[k])) mismatch(inv, what, k, gpu[k], expect[k]);
            }
        };
        if (vertex) {
            const float* o = &gpuOut[size_t(inv) * 19 * 4];
            // Position: the epilogue's x * 1 + 0 * w (NaN when w is not finite).
            xinterp::Vec pos = r.exports[62];
            if (!pos[3].unknown && !std::isfinite(pos[3].v))
                for (int k = 0; k < 3; k++) pos[k] = xinterp::Num{NAN, 0.0f, false};
            if (pos[3].unknown)
                for (int k = 0; k < 3; k++) pos[k].unknown = true;
            compareVec("oPos", o, pos);
            // Interpolators: by the shader's linkage.
            if (info.rawMicrocode) {
                for (uint32_t reg = 0; reg < 16; reg++) compareVec("o" + std::to_string(reg), o + (1 + reg) * 4, r.exports[reg]);
            } else {
                for (const auto& interp : info.interpolators) {
                    int slot = interpolatorSlot(interp.usage, interp.usageIndex);
                    if (slot < 0 || interp.reg >= 16) continue;
                    compareVec("o" + std::to_string(interp.reg), o + (1 + slot) * 4, r.exports[interp.reg]);
                }
            }
        } else {
            uint32_t outputs = b.pixelOutputs & 0xF;
            if (!outputs) continue;
            bool gpuKilled = true;
            for (uint32_t t = 0; t < 4; t++)
                if (outputs & (1u << t))
                    for (int k = 0; k < 4; k++)
                        if (gpuOut[(size_t(t) * kInvocations + inv) * 4 + size_t(k)] != kSentinel) gpuKilled = false;
            if (gpuKilled != r.killed) {
                stats.mismatches++;
                mismatched = true;
                if (reported++ < maxReport_)
                    std::printf("  MISMATCH %s [%s] round %u pixel %u: killed on the GPU %d, by the reference %d\n",
                                shader.name.c_str(), shader.group.c_str(), round, inv, gpuKilled, r.killed);
                continue;
            }
            if (r.killed) continue;
            for (uint32_t t = 0; t < 4; t++)
                if (outputs & (1u << t)) compareVec("oC" + std::to_string(t), &gpuOut[(size_t(t) * kInvocations + inv) * 4], r.exports[t]);
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------

int runGpu(int argc, char** argv) {
    std::string dxc = option(argc, argv, "--dxc");
    std::string error;
    if (!loadDxc(dxc, &error)) {
        std::printf("DXC: %s\n", error.c_str());
        return 1;
    }
    vkh::Gpu gpu;
    if (!gpu.init(&error)) {
        std::printf("Vulkan: %s\n", error.c_str());
        return 1;
    }
    std::printf("device: %s, validation layer %s, %s\n", gpu.deviceName().c_str(), gpu.validation() ? "on" : "off",
                dxcVersion().c_str());
    fs::path out = option(argc, argv, "--out", "gpu-out");
    fs::create_directories(out);
    uint32_t rounds = uint32_t(std::stoul(option(argc, argv, "--rounds", "3")));
    uint32_t randomCount = uint32_t(std::stoul(option(argc, argv, "--random", "200")));
    uint32_t seed = uint32_t(std::stoul(option(argc, argv, "--seed", "20261010")));
    uint32_t fuzz = uint32_t(std::stoul(option(argc, argv, "--fuzz", "100")));
    bool corpus = option(argc, argv, "--corpus", "1") != "0";
    std::string filter = option(argc, argv, "--filter");
    bool progress = option(argc, argv, "--progress", "0") != "0";
    uint32_t maxReport = uint32_t(std::stoul(option(argc, argv, "--max-report", "4")));

    std::vector<CorpusShader> shaders;
    if (corpus) shaders = buildCorpus(fuzz, seed);
    for (uint32_t i = 0; i < randomCount; i++)
        for (int k = 0; k < 4; k++) shaders.push_back(randomProgram(seed + i, (k & 1) == 0, (k & 2) != 0));

    Compiler compiler;
    Runner runner(gpu, compiler, out, maxReport, option(argc, argv, "--trace", "0") != "0");
    runner.hlslOverride = option(argc, argv, "--hlsl");
    if (!runner.init()) return 1;
    Stats stats;
    uint32_t index = 0;
    for (const auto& s : shaders) {
        index++;
        if (!filter.empty() && (s.name + " " + s.group).find(filter) == std::string::npos) continue;
        auto start = std::chrono::steady_clock::now();
        runner.runShader(s, rounds, seed ^ (index * 2654435761u), stats);
        if (progress)
            std::printf("  %s [%s] %.1f s\n", s.name.c_str(), s.group.c_str(),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        if (stats.gpuFailures) break;
    }

    std::printf(
        "\n%u shaders (%u not translated, %u not compiled), %llu invocations: %llu compared, %llu fragile (not compared), "
        "%llu rounds skipped (no termination)\n"
        "%llu components compared, %llu unknown (not compared)\n"
        "%llu mismatches in %u shaders\n",
        stats.shaders, stats.translateFailures, stats.compileFailures, (unsigned long long)stats.invocations,
        (unsigned long long)stats.compared, (unsigned long long)stats.fragile, (unsigned long long)stats.timeouts,
        (unsigned long long)(stats.components - stats.unknownComponents), (unsigned long long)stats.unknownComponents,
        (unsigned long long)stats.mismatches, stats.mismatchedShaders);
    for (const auto& [group, n] : stats.mismatchByGroup) std::printf("  %s: %u\n", group.c_str(), n);
    if (gpu.validationErrors()) std::printf("%d Vulkan validation errors\n", gpu.validationErrors());
    return stats.mismatches || stats.gpuFailures || stats.compileFailures || gpu.validationErrors() ? 1 : 0;
}

int runInterpreterChecks();

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        std::printf("usage: kkshaders_gpu_tests interp | gpu --dxc <dir> [options]\n");
        return 2;
    }
    std::string what = argv[1];
    if (what == "interp") return runInterpreterChecks();
    if (what == "gpu") return runGpu(argc, argv);
    std::printf("unknown test %s\n", what.c_str());
    return 2;
}

// ------------------------------------------------------------------------------------------
// The interpreter's own checks: hand-computed results for the semantics the comparison relies on.

namespace {

int gFailures = 0;
void dump(const char* what, const xinterp::Vec& v) {
    std::printf("    %s = (%g%s, %g%s, %g%s, %g%s)\n", what, v[0].v, v[0].unknown ? "?" : "", v[1].v, v[1].unknown ? "?" : "", v[2].v,
                v[2].unknown ? "?" : "", v[3].v, v[3].unknown ? "?" : "");
}
#define ICHECK(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++gFailures;                                                      \
        }                                                                     \
    } while (0)

using namespace xasm;

xinterp::Outputs runPixel(const Program& p, std::function<void(xinterp::Inputs&)> setup = {}) {
    xinterp::Inputs in;
    in.pixel = true;
    in.ucode = p.assemble();
    if (setup) setup(in);
    return xinterp::run(in);
}

Op A(const Alu& a) { return {a.encode(), false}; }
Op expo(uint32_t reg, uint32_t src) { return A(Alu().v(MAXv, reg, "xyzw", r(src), r(src)).exp(reg)); }

int runInterpreterChecks() {
    // Arithmetic with the Direct3D 9 zero rule, swizzles, negation, saturation.
    {
        Program p;
        p.exec({A(Alu().v(MULv, 1, "xyzw", r(0), c(0, "wzyx", true))), A(Alu().v(ADDv, 2, "xyzw", r(1), c(1)).sat(true, false))});
        p.alloc(2, 0);
        p.exec({expo(0, 1), expo(1, 2)}, true);
        auto o = runPixel(p, [](xinterp::Inputs& in) {
            in.initialRegisters[0] = {0.0f, 2.0f, -3.0f, INFINITY};
            in.constants[0] = {INFINITY, 4.0f, 5.0f, 6.0f};
            in.constants[1] = {0.25f, 0.25f, 10.0f, -10.0f};
        });
        dump("e0", o.exports[0]);
        dump("e1", o.exports[1]);
        // r1 = r0 * -c0.wzyx = (0 * -6, 2 * -5, -3 * -4, inf * -inf) = (0, -10, 12, -inf)
        ICHECK(o.exports[0][0].v == 0.0f && o.exports[0][1].v == -10.0f && o.exports[0][2].v == 12.0f && o.exports[0][3].v == -INFINITY);
        // r2 = saturate(r1 + c1) = (0.25, 0, 1, 0)
        ICHECK(o.exports[1][0].v == 0.25f && o.exports[1][1].v == 0.0f && o.exports[1][2].v == 1.0f && o.exports[1][3].v == 0.0f);
    }
    // A loop with aL-relative constants: sum of c[start + i * step].
    {
        Program p;
        p.exec({A(Alu().v(MAXv, 1, "xyzw", c(0), c(0)))});
        uint32_t ls = p.loopStart(3);
        p.exec({A(Alu().v(ADDv, 1, "xyzw", r(1), c(10)).rel(true, false, false))});
        p.loopEnd(3, ls);
        p.alloc(2, 0);
        p.exec({expo(0, 1)}, true);
        auto o = runPixel(p, [](xinterp::Inputs& in) {
            in.loops[3] = 3 | (2 << 8) | (uint32_t(int32_t(-1) & 0xFF) << 16);  // 3 iterations, aL = 2, 1, 0
            for (int i = 0; i < 20; i++) in.constants[size_t(i)] = {float(i), 0, 0, 0};
        });
        dump("loop", o.exports[0]);
        // c0 = 0, then + c12 + c11 + c10.
        ICHECK(o.exports[0][0].v == 33.0f);
    }
    // Predicates: setp_gt then a predicated add, and a predicated exec.
    {
        Program p;
        p.exec({A(Alu().s(PRED_SETGTs, 2, "x", r(0, "x"))), A(Alu().v(ADDv, 1, "xyzw", r(1), c(0)).pred(true)),
                A(Alu().v(ADDv, 1, "xyzw", r(1), c(1)).pred(false))});
        p.condExecPred({A(Alu().v(ADDv, 1, "x", r(1), c(2)))}, true);
        p.alloc(2, 0);
        p.exec({expo(0, 1), expo(1, 2)}, true);
        auto o = runPixel(p, [](xinterp::Inputs& in) {
            in.initialRegisters[0] = {5.0f, 0, 0, 0};
            in.constants[0] = {1, 1, 1, 1};
            in.constants[1] = {100, 100, 100, 100};
            in.constants[2] = {1000, 0, 0, 0};
        });
        ICHECK(o.exports[0][0].v == 1001.0f && o.exports[0][1].v == 1.0f);
        ICHECK(o.exports[1][0].v == 0.0f);  // setp result: 0 when the predicate is set
    }
    // The vector half's a0 is the one the scalar half reads (maxa then a0-relative constant).
    {
        Program p;
        Src k = c(0);
        k.swizzle = scalarSwizzle('x', 'x');
        p.exec({A(Alu().v(MAXAv, 1, "xyzw", r(0), r(0)).s(ADDs, 2, "x", k).rel(true, false, true))});
        p.alloc(2, 0);
        p.exec({expo(0, 2)}, true);
        auto o = runPixel(p, [](xinterp::Inputs& in) {
            in.initialRegisters[0] = {0, 0, 0, 3.4f};  // a0 = 3
            for (int i = 0; i < 8; i++) in.constants[size_t(i)] = {float(i), 0, 0, 0};
        });
        ICHECK(o.exports[0][0].v == 6.0f);  // c3.x + c3.x
    }
    // Vertex fetch: 2_10_10_10 signed normalised, 16_16_FLOAT, 8in32 endian.
    {
        Program p;
        VFetch f;
        f.dst = 1;
        f.format = F_2_10_10_10;
        f.isSigned = true;
        f.stride = 2;
        VFetch h = f;
        h.mini = true;
        h.dst = 2;
        h.format = F_16_16_FLOAT;
        h.isSigned = false;
        h.offset = 1;
        p.exec({{f.encode(), true}, {h.encode(), true}});
        p.alloc(2, 0);
        p.exec({A(Alu().v(MAXv, 0, "xyzw", r(1), r(1)).exp(0)), A(Alu().v(MAXv, 1, "xyzw", r(2), r(2)).exp(1))}, true);
        xinterp::Inputs in;
        in.pixel = false;
        in.ucode = p.assemble();
        in.vertexIndex = 1;
        in.fetchConstants[95] = {0, 0, 64, 2};
        // Vertex 1 at dwords 2-3: x = 511 (1.0), y = -512 (clamps to -1), z = 0, w = -1; halves 1.5, -2.
        uint32_t packed = 511u | (uint32_t(-512 & 0x3FF) << 10) | (0u << 20) | (3u << 30);
        uint32_t halves = 0x3E00u | (0xC000u << 16);
        std::vector<uint8_t> data(64);
        auto store = [&](uint32_t at, uint32_t v) {
            data[at] = uint8_t(v >> 24);
            data[at + 1] = uint8_t(v >> 16);
            data[at + 2] = uint8_t(v >> 8);
            data[at + 3] = uint8_t(v);
        };
        store(8, packed);
        store(12, halves);
        in.buffers.push_back(data);
        auto o = xinterp::run(in);
        ICHECK(o.exports[0][0].v == 1.0f && o.exports[0][1].v == -1.0f && o.exports[0][2].v == 0.0f && o.exports[0][3].v == -1.0f);
        ICHECK(o.exports[1][0].v == 1.5f && o.exports[1][1].v == -2.0f && o.exports[1][2].v == 0.0f && o.exports[1][3].v == 0.0f);
    }
    // Texture fetch: point and linear on a 2x1 texture.
    {
        xinterp::Texture t;
        t.dim = xinterp::TexDim::D2;
        t.width = 2;
        t.height = 1;
        t.texels = {0, 0, 0, 0, 1, 2, 3, 4};
        t.clamp = true;
        for (int linear = 0; linear < 2; linear++) {
            Program p;
            TFetch f;
            f.dst = 1;
            f.src = 0;
            f.magFilter = uint32_t(linear);
            p.exec({{f.encode(), true}});
            p.alloc(2, 0);
            p.exec({expo(0, 1)}, true);
            auto o = runPixel(p, [&](xinterp::Inputs& in) {
                in.initialRegisters[0] = {0.625f, 0.5f, 0, 0};  // texel space 1.25: texel 1, or 0.75 of the way
                in.textures[0] = &t;
            });
            float expect = linear ? 0.75f : 1.0f;
            dump("tex", o.exports[0]);
            ICHECK(std::fabs(o.exports[0][0].v - expect) <= o.exports[0][0].e + 1e-6f);
            ICHECK(std::fabs(o.exports[0][3].v - 4.0f * expect) <= o.exports[0][3].e + 1e-6f);
        }
    }
    // Error bounds: a comparison on a rounded value next to the boundary is not decided.
    {
        Program p;
        p.exec({A(Alu().s(RECIP_IEEE, 1, "x", r(0, "x"))), A(Alu().v(SGEv, 2, "x", r(1), c(0)))});
        p.alloc(2, 0);
        p.exec({expo(0, 2)}, true);
        auto o = runPixel(p, [](xinterp::Inputs& in) {
            in.initialRegisters[0] = {3.0f, 0, 0, 0};
            in.constants[0] = {1.0f / 3.0f, 0, 0, 0};
        });
        ICHECK(o.exports[0][0].unknown);
    }
    std::printf("interpreter checks: %d failed\n", gFailures);
    return gFailures ? 1 : 0;
}

}  // namespace
