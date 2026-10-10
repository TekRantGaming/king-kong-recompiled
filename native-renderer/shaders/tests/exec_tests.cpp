// Execution tests: translated shaders run on a Vulkan device (Mesa's lavapipe here; any GPU
// works) and their results are compared with the CPU reference interpreter (xenos_ref).
//
//   kkshaders_exec_tests --dxc <dir> [--count N] [--seed S] [--out <dir>] [--only alu|random|vfetch|texture]
//
// What is checked: ALU operations and their operand / swizzle / modifier decoding, the
// previous-scalar and predicate state, constants (direct, a0 / aL relative, literals), control
// flow (execs, conditional execs, jumps, loops, calls), relative temporaries, vertex fetch
// decoding of every format, and 1D / 2D texture fetch addressing (point sampling).
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "container_writer.h"
#include "corpus.h"
#include "kkshaders/abi.h"
#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/translator.h"
#include "vk_runner.h"
#include "xenos_asm.h"
#include "xenos_ref.h"

namespace fs = std::filesystem;
using namespace xasm;

namespace {

std::string option(int argc, char** argv, const char* name, const char* fallback = "") {
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return fallback;
}

// The fixed shaders around the one under test.
const char* kPassVS = R"(
cbuffer KKVertexConstants : register(b0, space0) { float4 kk_VC[256]; };
void main(uint id : SV_VertexID, out float4 oPos : SV_Position,
    out float4 t0 : TEXCOORD0, out float4 t1 : TEXCOORD1, out float4 t2 : TEXCOORD2, out float4 t3 : TEXCOORD3,
    out float4 t4 : TEXCOORD4, out float4 t5 : TEXCOORD5, out float4 t6 : TEXCOORD6, out float4 t7 : TEXCOORD7,
    out float4 t8 : TEXCOORD8, out float4 t9 : TEXCOORD9, out float4 t10 : TEXCOORD10, out float4 t11 : TEXCOORD11,
    out float4 t12 : TEXCOORD12, out float4 t13 : TEXCOORD13, out float4 t14 : TEXCOORD14, out float4 t15 : TEXCOORD15,
    out float4 d0 : COLOR0, out float4 d1 : COLOR1)
{
    float2 p = float2((id << 1) & 2, id & 2);
    oPos = float4(p * 2.0 - 1.0, 0.5, 1.0);
    t0 = kk_VC[0]; t1 = kk_VC[1]; t2 = kk_VC[2]; t3 = kk_VC[3]; t4 = kk_VC[4]; t5 = kk_VC[5]; t6 = kk_VC[6]; t7 = kk_VC[7];
    t8 = kk_VC[8]; t9 = kk_VC[9]; t10 = kk_VC[10]; t11 = kk_VC[11]; t12 = kk_VC[12]; t13 = kk_VC[13]; t14 = kk_VC[14];
    t15 = kk_VC[15]; d0 = kk_VC[16]; d1 = kk_VC[17];
}
)";

const char* kPassPS = R"(
void main(float4 pos : SV_Position,
    nointerpolation float4 t0 : TEXCOORD0, nointerpolation float4 t1 : TEXCOORD1,
    nointerpolation float4 t2 : TEXCOORD2, nointerpolation float4 t3 : TEXCOORD3,
    out float4 c0 : SV_Target0, out float4 c1 : SV_Target1, out float4 c2 : SV_Target2, out float4 c3 : SV_Target3)
{
    c0 = t0; c1 = t1; c2 = t2; c3 = t3;
}
)";

struct Ctx {
    std::unique_ptr<kkshaders::Compiler> compiler;
    VkRunner runner;
    std::vector<uint8_t> passVS, passPS;
    std::mt19937 rng;
    fs::path out;
    int failed = 0, passed = 0;
    std::map<std::string, std::pair<int, int>> groups;

    uint32_t rnd(uint32_t n) { return uint32_t(rng() % n); }
    bool chance(uint32_t percent) { return rnd(100) < percent; }
    // Values that keep arithmetic exact: quarters in [-4, 4], with 0 and 1 common.
    float nice() {
        uint32_t k = rnd(10);
        if (k == 0) return 0.0f;
        if (k == 1) return 1.0f;
        return float(int32_t(rnd(33)) - 16) * 0.25f;
    }
    xref::Vec niceVec() { return {nice(), nice(), nice(), nice()}; }
};

// A random constant operand.
Src c_(Ctx& ctx) {
    const char* swz[] = {"xyzw", "wzyx", "xxyy", "zwzw", "y", "w", "yzxw"};
    return xasm::c(ctx.rnd(256), swz[ctx.rnd(7)], ctx.chance(30));
}

bool close(float gpu, float ref, float tolerance) {
    if (std::isnan(gpu) || std::isnan(ref)) return std::isnan(gpu) && std::isnan(ref);
    if (std::isinf(gpu) || std::isinf(ref)) return gpu == ref;
    return std::fabs(gpu - ref) <= tolerance * std::max(1.0f, std::fabs(ref));
}

std::vector<uint8_t> bytesOf(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return std::vector<uint8_t>(b, b + n);
}

std::string vecText(const std::array<float, 4>& v) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%g, %g, %g, %g)", v[0], v[1], v[2], v[3]);
    return buf;
}

struct Case {
    std::string group, name;
    bool vertex = false;          // the vertex shader is under test (else the pixel shader)
    ctest::Spec spec;             // container (ucode filled in)
    xref::Machine machine;        // reference inputs
    kkshaders::DrawConstants draw{};
    std::vector<std::vector<uint8_t>> buffers;
    std::vector<VkRunner::Draw::Texture> textures;
    float tolerance = 1e-5f;
    bool compareKill = true;
};

// Translates, compiles, runs on the device and on the reference, compares.
// Instruction addresses of the ALU instructions the execs run.
std::vector<uint32_t> findAluAddresses(const std::vector<uint32_t>& ucode) {
    std::vector<uint32_t> out;
    uint32_t cfEnd = uint32_t(ucode.size() / 3);
    for (uint32_t t = 0; t < cfEnd; t++) {
        uint64_t a = ucode[t * 3] | (uint64_t(ucode[t * 3 + 1] & 0xFFFF) << 32);
        uint64_t b = (ucode[t * 3 + 1] >> 16) | (uint64_t(ucode[t * 3 + 2]) << 16);
        for (uint64_t cf : {a, b}) {
            uint32_t op = uint32_t(cf >> 44) & 0xF;
            if (!((op >= 1 && op <= 6) || op == 13 || op == 14)) continue;
            uint32_t address = uint32_t(cf & 0xFFF), count = uint32_t(cf >> 12) & 7, sequence = uint32_t(cf >> 16) & 0xFFF;
            if (count) cfEnd = std::min(cfEnd, address);
            for (uint32_t i = 0; i < count; i++)
                if (!((sequence >> (i * 2)) & 1)) out.push_back(address + i);
        }
    }
    return out;
}

std::string disassemble(const std::vector<uint32_t>& ucode, uint32_t) {
    static const char* vops[] = {"add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad",
                                 "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push",
                                 "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne",
                                 "dst", "maxa", "?", "?"};
    std::string out;
    char line[512];
    for (uint32_t a : findAluAddresses(ucode)) {
        const uint32_t* w = &ucode[a * 3];
        if ((w[0] >> 26) == 50 && ((w[0] >> 16) & 0xFF) == 0 && !((w[0] >> 15) & 1)) continue;  // no-op
        auto src = [&](int i) {
            uint32_t reg = (w[2] >> (16 - 8 * i)) & 0xFF;
            bool temp = (w[2] >> (31 - i)) & 1;
            uint32_t swz = (w[1] >> (16 - 8 * i)) & 0xFF;
            bool neg = (w[1] >> (26 - i)) & 1;
            std::string s = neg ? "-" : "";
            if (temp) s += std::string(reg & 0x80 ? "|" : "") + "r" + std::to_string(reg & 63) + (reg & 0x40 ? "[aL]" : "");
            else s += "c" + std::to_string(reg);
            s += ".";
            for (int k = 0; k < 4; k++) s += "xyzw"[((swz >> (k * 2)) + k) & 3];
            if (temp && (reg & 0x80)) s += "|";
            return s;
        };
        std::snprintf(line, sizeof(line),
                      "  @%u %s%s %s%s%u.%X = %s, %s, %s | sop %u -> %s%u.%X%s | abs %u pred %u/%u rel c0 %u c1 %u a0 %u\n", a,
                      vops[(w[2] >> 24) & 31], (w[0] >> 24) & 1 ? "_sat" : "", (w[0] >> 15) & 1 ? "export " : "",
                      (w[0] >> 6) & 1 ? "r[aL]" : "r", w[0] & 63, (w[0] >> 16) & 15, src(0).c_str(), src(1).c_str(), src(2).c_str(),
                      w[0] >> 26, (w[0] >> 14) & 1 ? "r[aL]" : "r", (w[0] >> 8) & 63, (w[0] >> 20) & 15, (w[0] >> 25) & 1 ? " sat" : "",
                      (w[0] >> 7) & 1, (w[1] >> 28) & 1, (w[1] >> 27) & 1, w[1] >> 31, (w[1] >> 30) & 1, (w[1] >> 29) & 1);
        out += line;
    }
    return out;
}

// 1 = pass, 0 = fail, 2 = skipped (the reference does not terminate).
int evaluate(Ctx& ctx, Case& c, std::string& why, std::string& hlslOut) {
    c.spec.vertex = c.vertex;
    std::vector<uint8_t> container = ctest::writeContainer(c.spec);
    kkshaders::ParseResult p = kkshaders::parseContainer(container);
    if (!p.ok) { why = "parse: " + p.error; return 0; }
    kkshaders::TranslateResult t = kkshaders::translate(p.info);
    if (!t.ok) { why = "translate: " + t.error; return 0; }
    kkshaders::CompileOutput spv = ctx.compiler->compile(t.hlsl, c.vertex ? kkshaders::ShaderKind::Vertex : kkshaders::ShaderKind::Pixel,
                                                        kkshaders::CompileTarget::Spirv);
    if (!spv.ok) { why = "compile: " + spv.messages; hlslOut = t.hlsl; return 0; }

    // Reference.
    xref::Machine& m = c.machine;
    const std::array<xref::Vec, 64> initialRegisters = m.registers;
    m.pixel = !c.vertex;
    m.ucode = c.spec.ucode;
    for (uint32_t i = 0; i < 8; i++) m.bools[i] = c.draw.boolConstants[i];
    for (uint32_t i = 0; i < 32; i++) m.loops[i] = c.draw.loopConstants[i];
    m.buffers = c.buffers;
    m.run();
    if (m.runaway) return 2;

    // Device.
    VkRunner::Draw d;
    std::vector<float> vc(1024, 0.0f), pc(1024, 0.0f);
    if (c.vertex) {
        d.vs = spv.blob;
        d.ps = ctx.passPS;
        for (uint32_t i = 0; i < 256; i++)
            for (int k = 0; k < 4; k++) vc[i * 4 + k] = m.constants[i][k];
    } else {
        d.vs = ctx.passVS;
        d.ps = spv.blob;
        for (uint32_t i = 0; i < 256; i++)
            for (int k = 0; k < 4; k++) pc[i * 4 + k] = m.constants[i][k];
        // The pass-through vertex shader feeds the interpolators from its constants.
        for (const auto& interp : c.spec.interpolators) {
            uint32_t slot = interp.usage == 5 ? interp.usageIndex : 16 + interp.usageIndex;
            for (int k = 0; k < 4; k++) vc[slot * 4 + k] = initialRegisters[interp.reg][k];
        }
    }
    d.vertexConstants = bytesOf(vc.data(), 4096);
    d.pixelConstants = bytesOf(pc.data(), 4096);
    d.drawConstants = bytesOf(&c.draw, sizeof(c.draw));
    d.buffers = c.buffers;
    d.textures = c.textures;
    VkRunner::Result r = ctx.runner.run(d);
    if (!r.ok) { why = "device: " + r.error; hlslOut = t.hlsl; return 0; }

    std::string mismatch;
    for (uint32_t i = 0; i < 4; i++) {
        std::array<float, 4> expect;
        auto e = m.exports.find(i);
        if (!c.vertex && m.killed) expect = {VkRunner::kClear, VkRunner::kClear, VkRunner::kClear, VkRunner::kClear};
        else if (e != m.exports.end()) expect = e->second;
        else expect = c.vertex ? std::array<float, 4>{0, 0, 0, 0} : std::array<float, 4>{VkRunner::kClear, VkRunner::kClear, VkRunner::kClear, VkRunner::kClear};
        bool ok = true;
        for (int k = 0; k < 4; k++) ok &= close(r.targets[i][k], expect[k], c.tolerance);
        if (!ok) mismatch += "  target " + std::to_string(i) + ": device " + vecText(r.targets[i]) + ", reference " + vecText(expect) + "\n";
    }
    if (!mismatch.empty()) {
        why = "results differ\n" + mismatch;
        if (!c.vertex)
            for (uint32_t i = 0; i < 4; i++) why += "  input r" + std::to_string(i) + " = " + vecText(initialRegisters[i]) + "\n";
        hlslOut = t.hlsl;
        return 0;
    }
    hlslOut = t.hlsl;
    return 1;
}

std::string disassemble(const std::vector<uint32_t>& ucode, uint32_t first);

// Runs a case; on a failure, minimises it (instructions turned into no-ops while it still fails)
// and keeps the files for reading.
void runCase(Ctx& ctx, Case& c) {
    auto& group = ctx.groups[c.group];
    group.second++;
    Case work = c;
    std::string why, hlsl;
    int r = evaluate(ctx, work, why, hlsl);
    if (r != 0) {
        group.first++;
        ctx.passed += r == 1;
        return;
    }
    ctx.failed++;
    // Minimise: ALU instructions without exports become no-ops (retain_prev, no writes).
    Case small = c;
    std::vector<uint32_t> addresses = findAluAddresses(small.spec.ucode);
    for (uint32_t a : addresses) {
        Case trial = small;
        uint32_t* w = &trial.spec.ucode[a * 3];
        if ((w[0] >> 15) & 1) continue;  // keep exports
        w[0] = 50u << 26;
        w[1] = 0;
        w[2] = 0;
        Case run = trial;
        std::string why2, hlsl2;
        if (evaluate(ctx, run, why2, hlsl2) == 0) {
            small = trial;
            why = why2;
            hlsl = hlsl2;
        }
    }
    std::printf("FAILED [%s] %s: %s", c.group.c_str(), c.name.c_str(), why.c_str());
    std::string listing = disassemble(small.spec.ucode, 0);
    std::printf("  minimal program:\n%s\n", listing.c_str());
    std::string base = std::to_string(ctx.failed);
    std::error_code ec;
    fs::create_directories(ctx.out / "failed", ec);
    std::ofstream(ctx.out / "failed" / (base + ".hlsl")) << hlsl;
    std::ofstream(ctx.out / "failed" / (base + ".txt")) << c.group << " / " << c.name << "\n" << why << "\n" << listing;
}

// Pixel shader cases read TEXCOORD0-3 into r0-r3 and export r4-r7 to colours 0-3.
Case pixelCase(Ctx& ctx, const std::string& group, const std::string& name) {
    Case c;
    c.group = group;
    c.name = name;
    c.spec.interpolators = {{5, 0, 0}, {5, 1, 1}, {5, 2, 2}, {5, 3, 3}};
    for (uint32_t i = 0; i < 4; i++) c.machine.registers[i] = ctx.niceVec();
    for (auto& k : c.machine.constants) k = ctx.niceVec();
    return c;
}

void finishPixel(Program& p, uint32_t firstReg = 4) {
    p.alloc(2, 3);
    std::vector<Op> exports;
    for (uint32_t i = 0; i < 4; i++) exports.push_back({Alu().v(MAXv, i, "xyzw", r(firstReg + i), r(firstReg + i)).exp(i).encode(), false});
    p.exec(exports, true);
}

// --------------------------------------------------------------------------------------------
// Every vector and scalar operation on random operands, swizzles and modifiers.
void aluCases(Ctx& ctx, uint32_t rounds) {
    static const bool transcendental[51] = {
        false, false, false, false, false, false, false, false, false, false, false, false, false, false,
        true, true, true, true, true, true, true, true, true, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false, true, false, false, false, false, false,
        false, false, true, true, false};
    const char* swz[] = {"xyzw", "wzyx", "xxyy", "zwzw", "y", "w", "yzxw"};
    for (uint32_t round = 0; round < rounds; round++) {
        for (uint32_t vop = 0; vop <= MAXAv; vop++) {
            uint32_t sop = (vop * 7 + round * 13) % 51;
            if (sop == 41) sop = RETAIN_PREV;
            Case c = pixelCase(ctx, "alu", "vector " + std::to_string(vop) + " scalar " + std::to_string(sop) + " round " + std::to_string(round));
            Program p;
            Src a = r(ctx.rnd(4), swz[ctx.rnd(7)], ctx.chance(30), ctx.chance(30));
            Src b = ctx.chance(50) ? r(ctx.rnd(4), swz[ctx.rnd(7)], ctx.chance(30)) : c_(ctx);
            Src s3 = ctx.chance(50) ? r(ctx.rnd(4), swz[ctx.rnd(7)], ctx.chance(30), ctx.chance(30)) : c_(ctx);
            Alu alu;
            alu.v(vop, 4, ctx.chance(70) ? "xyzw" : "xz", a, b, s3);
            if (sop >= MUL_CONST_0 && sop <= SUB_CONST_1) {
                alu.sc(sop, 5, "xyzw", ctx.rnd(256), "xyzw"[ctx.rnd(4)], ctx.rnd(4), "xyzw"[ctx.rnd(4)]);
                // sc() replaced the third source: the vector operation reads it too.
            } else {
                alu.sop = sop;
                alu.sdst = 5;
                alu.smask = mask("xyzw");
            }
            if (ctx.chance(30)) alu.sat(ctx.chance(50), ctx.chance(50));
            if (ctx.chance(30)) alu.absConst();
            std::vector<Op> ops;
            // A previous scalar result for the *_prev operations.
            ops.push_back({Alu().s(ADDs, 6, "x", r(1, "xyzw")).encode(), false});
            ops.push_back({alu.encode(), false});
            // State the operations change: the predicate and the address register.
            ops.push_back({Alu().v(ADDv, 6, "xyzw", r(0), xasm::c(3)).pred(true).encode(), false});
            ops.push_back({Alu().v(ADDv, 7, "xyzw", xasm::c(100), r(1)).rel(true, false, true).encode(), false});
            p.exec(ops);
            finishPixel(p);
            c.spec.ucode = p.assemble();
            bool killOp = (vop >= KILLEQv && vop <= KILLNEv) || (sop >= KILLEs && sop <= KILLONEs);
            (void)killOp;
            if (transcendental[sop]) c.tolerance = 2e-3f;
            runCase(ctx, c);
        }
    }
}

// --------------------------------------------------------------------------------------------
// Random programs of exact operations in random control flow.
void randomCases(Ctx& ctx, uint32_t count, uint32_t features) {
    auto has = [&](uint32_t f) { return (features & f) != 0; };
    const uint32_t exactVector[] = {ADDv, MULv, MAXv, MINv, SEQv, SGTv, SGEv, SNEv, FRCv, TRUNCv, FLOORv, MADv, CNDEQv, CNDGEv,
                                    CNDGTv, DP4v, DP3v, DP2ADDv, MAX4v, SETP_EQ_PUSHv, SETP_NE_PUSHv, SETP_GT_PUSHv,
                                    SETP_GE_PUSHv, DSTv, MAXAv, CUBEv};
    const uint32_t exactScalar[] = {ADDs, ADD_PREVs, MULs, MUL_PREVs, MAXs, MINs, SEQs, SGTs, SGEs, SNEs, FRACs, TRUNCs, FLOORs,
                                    MAXAs, MAXA_FLOORs, SUBs, SUB_PREVs, PRED_SETEs, PRED_SETNEs, PRED_SETGTs, PRED_SETGTEs,
                                    PRED_SET_INVs, PRED_SET_POPs, PRED_SET_CLRs, PRED_SET_RESTOREs, MUL_CONST_0, MUL_CONST_1,
                                    ADD_CONST_0, ADD_CONST_1, SUB_CONST_0, SUB_CONST_1, RETAIN_PREV};
    const char* swz[] = {"xyzw", "wzyx", "x", "y", "z", "w", "xxyy", "zwxy", "yzwx", "wwzz"};
    const char* masks[] = {"x", "y", "z", "w", "xy", "zw", "xyz", "xyzw", "xw", "yz"};
    const uint32_t regs = 8;

    for (uint32_t n = 0; n < count; n++) {
        Case c = pixelCase(ctx, "random", "program " + std::to_string(n));
        auto src = [&]() -> Src {
            if (ctx.chance(30)) return c_(ctx);
            return r(ctx.rnd(regs), swz[ctx.rnd(10)], ctx.chance(25), ctx.chance(15), has(128) && ctx.chance(5));
        };
        auto alu = [&]() -> Op {
            Alu a;
            a.v(exactVector[ctx.rnd(sizeof(exactVector) / 4)], ctx.rnd(regs), masks[ctx.rnd(10)], src(), src(), src());
            if (ctx.chance(15)) a.vmask = 0;
            uint32_t sop = exactScalar[ctx.rnd(sizeof(exactScalar) / 4)];
            if (sop >= MUL_CONST_0 && sop <= SUB_CONST_1) {
                a.sc(sop, ctx.rnd(regs), masks[ctx.rnd(10)], ctx.rnd(256), "xyzw"[ctx.rnd(4)], ctx.rnd(regs), "xyzw"[ctx.rnd(4)]);
            } else {
                a.sop = sop;
                a.sdst = ctx.rnd(regs);
                a.smask = ctx.chance(85) ? mask(masks[ctx.rnd(10)]) : 0;
            }
            if (has(256) && ctx.chance(15)) a.sat(ctx.chance(50), ctx.chance(50));
            if (has(512) && ctx.chance(10)) a.absConst();
            if (has(32) && ctx.chance(15)) a.pred(ctx.chance(50));
            if (has(64) && ctx.chance(15)) a.rel(ctx.chance(50), ctx.chance(50), ctx.chance(50));
            if (has(128) && ctx.chance(4)) a.vrel();
            if (has(128) && ctx.chance(4)) a.srel();
            return {a.encode(), false};
        };
        auto block = [&]() {
            std::vector<Op> ops;
            uint32_t k = 1 + ctx.rnd(6);
            for (uint32_t i = 0; i < k; i++) ops.push_back(alu());
            return ops;
        };

        Program p;
        std::vector<std::pair<uint32_t, uint32_t>> openLoops;
        std::vector<uint32_t> openJumps;
        std::vector<uint32_t> calls;
        uint32_t steps = 3 + ctx.rnd(10);
        for (uint32_t s = 0; s < steps; s++) {
            uint32_t what = ctx.rnd(12);
            if (what < 4) {
                p.exec(block());
            } else if (what == 4 && has(1) && openLoops.size() < 3) {
                uint32_t id = ctx.rnd(32);
                openLoops.push_back({id, p.loopStart(id)});
            } else if (what == 5 && !openLoops.empty()) {
                p.exec(block());
                p.loopEnd(openLoops.back().first, openLoops.back().second, ctx.chance(30), ctx.chance(50));
                openLoops.pop_back();
            } else if (what == 6 && has(2)) {
                openJumps.push_back(p.jump(0, false, ctx.chance(50), ctx.rnd(256), ctx.chance(50)));
            } else if (what == 7 && !openJumps.empty()) {
                size_t which = ctx.chance(70) ? openJumps.size() - 1 : ctx.rnd(uint32_t(openJumps.size()));
                p.exec(block());
                p.patchTarget(openJumps[which], p.label());
                openJumps.erase(openJumps.begin() + std::ptrdiff_t(which));
            } else if (what == 8 && has(4)) {
                p.condExec(block(), ctx.rnd(256), ctx.chance(50));
            } else if (what == 9 && has(4)) {
                p.condExecPred(block(), ctx.chance(50));
            } else if (what == 10 && has(8) && ctx.chance(40)) {
                calls.push_back(p.call(0, ctx.chance(30), ctx.chance(30), ctx.rnd(256), ctx.chance(50)));
            } else if (what == 11 && has(16) && ctx.chance(20) && p.label() > 1) {
                // A backward jump, taken while a counter in r7.w is below a limit.
                uint32_t target = ctx.rnd(p.label());
                p.exec({{Alu().v(ADDv, 7, "w", r(7), xasm::c(254)).encode(), false},
                        {Alu().v(SGEv, 6, "w", r(7), xasm::c(255)).encode(), false},
                        {Alu().s(PRED_SETEs, 6, "z", r(6, "w")).encode(), false}});
                p.jump(target, false, true, 0, true);
            }
        }
        p.exec(block());
        while (!openLoops.empty()) {
            p.loopEnd(openLoops.back().first, openLoops.back().second);
            openLoops.pop_back();
        }
        for (uint32_t j : openJumps) p.patchTarget(j, p.label());
        finishPixel(p);
        if (!calls.empty()) {
            uint32_t fn = p.label();
            p.exec(block());
            p.ret();
            for (uint32_t cl : calls) p.patchTarget(cl, fn);
        }
        c.spec.ucode = p.assemble();
        // Loop constants: up to 3 iterations, small start and step; bools at random.
        for (auto& l : c.draw.loopConstants) l = ctx.rnd(4) | (ctx.rnd(8) << 8) | ((uint32_t(int32_t(ctx.rnd(5)) - 2) & 0xFF) << 16);
        for (auto& b : c.draw.boolConstants) b = uint32_t(ctx.rng());
        c.machine.constants[254] = {1, 1, 1, 1};
        c.machine.constants[255] = {3, 3, 3, 3};
        runCase(ctx, c);
    }
}

// --------------------------------------------------------------------------------------------
// Vertex fetch: every format and number format from a big-endian buffer.
void vertexFetchCases(Ctx& ctx, uint32_t rounds) {
    const uint32_t formats[] = {F_8_8_8_8, F_2_10_10_10, F_10_11_11, F_11_11_10, F_16_16, F_16_16_16_16, F_16_16_FLOAT,
                                F_16_16_16_16_FLOAT, F_32, F_32_32, F_32_32_32_32, F_32_FLOAT, F_32_32_FLOAT,
                                F_32_32_32_32_FLOAT, F_32_32_32_FLOAT, 0};
    const char* elementSwizzles[] = {"xyzw", "zyxw", "xy01", "x001", "wzyx", "0000"};
    const char* dstSwizzles[] = {"xyzw", "xyz_", "x_z1", "wzyx"};
    auto swz12 = [](const char* s) {
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char ch = s[k];
            uint32_t x = ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : ch == 'w' ? 3 : ch == '0' ? 4 : 5;
            v |= x << (k * 3);
        }
        return v;
    };
    for (uint32_t round = 0; round < rounds; round++) {
        for (uint32_t format : formats) {
            Case c;
            c.group = "vertex-fetch";
            c.name = "format " + std::to_string(format) + " round " + std::to_string(round);
            c.vertex = true;
            for (auto& k : c.machine.constants) k = ctx.niceVec();
            // Element 0: the position (FLOAT3 at offset 0 of a 64-byte stride in buffer 0) for a
            // full-screen triangle; elements 1-4: the format under test at offsets 12-60.
            const uint32_t stride = 64;
            std::vector<uint8_t> buffer(stride * 3);
            std::vector<float> positions = {-1, -1, 0, 3, -1, 0, -1, 3, 0};
            auto putBE = [&](size_t at, uint32_t v) {
                buffer[at] = uint8_t(v >> 24);
                buffer[at + 1] = uint8_t(v >> 16);
                buffer[at + 2] = uint8_t(v >> 8);
                buffer[at + 3] = uint8_t(v);
            };
            for (uint32_t v = 0; v < 3; v++)
                for (uint32_t k = 0; k < 3; k++) {
                    uint32_t bits;
                    std::memcpy(&bits, &positions[v * 3 + k], 4);
                    putBE(v * stride + k * 4, bits);
                }
            for (uint32_t v = 0; v < 3; v++)
                for (uint32_t at = 12; at < stride; at += 4) {
                    uint32_t word = uint32_t(ctx.rng());
                    bool isFloat32 = format >= F_32_FLOAT && format <= F_32_32_32_32_FLOAT;
                    if (isFloat32 || format == F_32_32_32_FLOAT) {
                        float f = ctx.nice() * float(1 + ctx.rnd(100));
                        std::memcpy(&word, &f, 4);
                    }
                    if (format == F_16_16_FLOAT || format == F_16_16_16_16_FLOAT) word &= 0x7BFF7BFFu;  // no inf / NaN halves
                    putBE(v * stride + at, word);
                }
            c.buffers = {buffer};

            Program p;
            VFetch pos;
            pos.dst = 1;
            pos.format = F_32_32_32_FLOAT;
            std::vector<Op> ops = {{pos.encode(), true}};
            std::vector<kkshaders::DeclUsage> usages;
            for (uint32_t e = 0; e < 4; e++) {
                VFetch f;
                f.dst = 2 + e;
                f.mini = true;
                f.dstSwizzle = fetchDstSwizzle(dstSwizzles[ctx.rnd(4)]);
                ops.push_back({f.encode(), true});
            }
            p.exec(ops);
            p.alloc(1, 0);
            p.exec({{Alu().v(MAXv, 62, "xyzw", r(1), r(1)).exp(62).encode(), false}});
            p.alloc(2, 3);
            std::vector<Op> exports;
            for (uint32_t i = 0; i < 4; i++) exports.push_back({Alu().v(MAXv, i, "xyzw", r(2 + i), r(2 + i)).exp(i).encode(), false});
            p.exec(exports, true);
            c.spec.ucode = p.assemble();
            std::vector<uint32_t> addresses = findVfetchAddresses(c.spec.ucode);
            c.spec.fetches = {{addresses[0], 0, 0, 0}};
            for (uint32_t e = 0; e < 4; e++) c.spec.fetches.push_back({addresses[1 + e], 5, e, 0});
            c.spec.interpolators = {{5, 0, 0}, {5, 1, 1}, {5, 2, 2}, {5, 3, 3}};

            // Binding table: element 0 position, 1-4 the format with random number formats.
            auto setElement = [&](uint32_t binding, uint32_t offset, uint32_t word) {
                c.draw.vertexFetch[binding][0] = 0;
                c.draw.vertexFetch[binding][1] = offset;
                c.draw.vertexFetch[binding][2] = stride;
                c.draw.vertexFetch[binding][3] = word;
                c.machine.elements[addresses[binding]] = {0, offset, stride, word};
            };
            setElement(0, 0, F_32_32_32_FLOAT | (2u << 9) | (swz12("xyz1") << 17));
            for (uint32_t e = 0; e < 4; e++) {
                uint32_t word = format | (2u << 9) | (swz12(elementSwizzles[ctx.rnd(6)]) << 17);
                if (ctx.chance(50)) word |= 0x40;   // signed
                if (ctx.chance(40)) word |= 0x80;   // integer
                if (ctx.chance(30)) word |= 0x100;  // signed without zero
                if (ctx.chance(30)) word |= (uint32_t(int32_t(ctx.rnd(9)) - 4) & 0x3F) << 11;
                setElement(1 + e, 12 + e * 12, word);
            }
            c.machine.registers[0] = {0, 0, 0, 0};  // vertex 0
            c.tolerance = 1e-5f;
            runCase(ctx, c);
        }
    }
}

// --------------------------------------------------------------------------------------------
// 1D / 2D texture fetches with point sampling: coordinate swizzles, unnormalised coordinates,
// offsets, register LOD, destination swizzles.
void textureCases(Ctx& ctx, uint32_t rounds) {
    for (uint32_t round = 0; round < rounds; round++) {
        for (int variant = 0; variant < 8; variant++) {
            Case c = pixelCase(ctx, "texture", "variant " + std::to_string(variant) + " round " + std::to_string(round));
            bool oneD = variant == 7;
            uint32_t w = 4, h = oneD ? 1 : 4;
            VkRunner::Draw::Texture tex;
            tex.width = w;
            tex.height = h;
            for (uint32_t i = 0; i < w * h * 4; i++) tex.texels.push_back(float(ctx.rnd(1000)) * 0.125f);
            c.textures = {tex};
            c.machine.textures[3] = {w, h, tex.texels};
            c.draw.textureIndex[3] = 0;
            c.draw.pixelSamplers[0] = 0;  // point
            // Coordinates at texel centres.
            bool denorm = variant == 2 || variant == 5;
            float sx = denorm ? 1.0f : 1.0f / float(w), sy = denorm ? 1.0f : 1.0f / float(h);
            c.machine.registers[0] = {(float(ctx.rnd(w)) + 0.5f) * sx, (float(ctx.rnd(h)) + 0.5f) * sy,
                                      (float(ctx.rnd(w)) + 0.5f) * sx, (float(ctx.rnd(h)) + 0.5f) * sy};
            TFetch t;
            t.slot = 3;
            t.dim = oneD ? Dim::D1 : Dim::D2;
            t.dst = 4;
            t.src = 0;
            t.srcSwizzle = fetchSrcSwizzle(variant == 1 ? "zw" : "xy");
            t.denorm = denorm;
            t.magFilter = t.minFilter = 0;
            if (variant == 3 || variant == 5) {
                t.offsetX = 2 * (int32_t(ctx.rnd(3)) - 1);
                t.offsetY = 2 * (int32_t(ctx.rnd(3)) - 1);
            }
            if (variant == 4) {
                t.useRegLod = true;
                t.useCompLod = false;
            }
            if (variant == 6) t.dstSwizzle = fetchDstSwizzle("wz01");
            Program p;
            TFetch lod;
            lod.op = Fop::SetTexLOD;
            lod.src = 1;
            p.exec({{lod.encode(), true}, {t.encode(), true}, {Alu().v(ADDv, 5, "xyzw", r(4), r(1)).encode(), false}});
            finishPixel(p);
            c.spec.ucode = p.assemble();
            runCase(ctx, c);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Ctx ctx;
    std::string dxc = option(argc, argv, "--dxc");
    uint32_t count = uint32_t(std::stoul(option(argc, argv, "--count", "300")));
    ctx.rng.seed(uint32_t(std::stoul(option(argc, argv, "--seed", "4242"))));
    ctx.out = option(argc, argv, "--out", "exec-out");
    std::string only = option(argc, argv, "--only");
    std::error_code ec;
    fs::remove_all(ctx.out / "failed", ec);

    std::string error;
    if (!kkshaders::loadDxc(dxc, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    if (!ctx.runner.init(&error)) {
        std::printf("no Vulkan device (%s): skipped\n", error.c_str());
        return 77;  // ctest SKIP_RETURN_CODE
    }
    ctx.compiler = std::make_unique<kkshaders::Compiler>();
    std::printf("device: %s; DXC %s\n", ctx.runner.deviceName().c_str(), kkshaders::dxcVersion().c_str());
    auto vs = ctx.compiler->compile(kPassVS, kkshaders::ShaderKind::Vertex, kkshaders::CompileTarget::Spirv);
    auto ps = ctx.compiler->compile(kPassPS, kkshaders::ShaderKind::Pixel, kkshaders::CompileTarget::Spirv);
    if (!vs.ok || !ps.ok) {
        std::printf("pass-through shaders: %s %s\n", vs.messages.c_str(), ps.messages.c_str());
        return 1;
    }
    ctx.passVS = vs.blob;
    ctx.passPS = ps.blob;

    if (only.empty() || only == "alu") aluCases(ctx, std::max(1u, count / 50));
    uint32_t features = uint32_t(std::stoul(option(argc, argv, "--features", "4095")));
    if (only.empty() || only == "random") randomCases(ctx, count, features);
    if (only.empty() || only == "vfetch") vertexFetchCases(ctx, std::max(1u, count / 50));
    if (only.empty() || only == "texture") textureCases(ctx, std::max(1u, count / 25));

    std::printf("\nexecution: %d passed, %d failed\n", ctx.passed, ctx.failed);
    for (const auto& [g, n] : ctx.groups) std::printf("  %-14s %4d / %4d\n", g.c_str(), n.first, n.second);
    return ctx.failed ? 1 : 0;
}
