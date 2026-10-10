// kkshaders tests (ctest runs each command):
//   kkshaders_tests assembler                 the test assembler's encodings against XenosRecomp's
//   kkshaders_tests container                 container writer / parser round trip, bad input
//   kkshaders_tests corpus --dxc <dir> [--spirv-val <exe>] [--out <dir>] [--fuzz N] [--seed S] [--filter text]
//   kkshaders_tests cache --dxc <dir> [--out <dir>]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "container_writer.h"
#include "corpus.h"
#include "kkshaders/cache.h"
#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/translator.h"
#include "xenos_asm.h"

// XenosRecomp's instruction structs, for the encoding cross-check.
#include "pch.h"
#include "shader_code.h"

namespace fs = std::filesystem;
using namespace kkshaders;

namespace {

int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                          \
        }                                                                        \
    } while (0)

std::string option(int argc, char** argv, const char* name, const char* fallback = "") {
    for (int i = 2; i + 1 < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return fallback;
}

void writeFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    f << text;
}

// --------------------------------------------------------------------------------------------
int testAssembler() {
    std::mt19937 rng(1234);
    auto rnd = [&](uint32_t n) { return uint32_t(rng() % n); };
    for (int iteration = 0; iteration < 20000; iteration++) {
        // ALU.
        xasm::Alu a;
        a.vop = rnd(30);
        a.sop = rnd(51);
        a.vdst = rnd(64);
        a.sdst = rnd(64);
        a.vmask = rnd(16);
        a.smask = rnd(16);
        a.vdstRel = rnd(2);
        a.sdstRel = rnd(2);
        a.exportData = rnd(2);
        a.vsat = rnd(2);
        a.ssat = rnd(2);
        a.absConstants = rnd(2);
        a.predicated = rnd(2);
        a.predCondition = rnd(2);
        a.constAddressA0 = rnd(2);
        a.const0Rel = rnd(2);
        a.const1Rel = rnd(2);
        for (auto& s : a.src) {
            s.temp = rnd(2);
            s.reg = rnd(s.temp ? 64 : 256);
            s.swizzle = rnd(256);
            s.negate = rnd(2);
            s.abs = s.temp && rnd(2);
            s.relative = s.temp && rnd(2);
        }
        xasm::Instr w = a.encode();
        AluInstruction d;
        std::memcpy(&d, w.data(), 12);
        CHECK(uint32_t(d.vectorOpcode) == a.vop);
        CHECK(uint32_t(d.scalarOpcode) == a.sop);
        CHECK(d.vectorDest == a.vdst && d.scalarDest == a.sdst);
        CHECK(d.vectorWriteMask == a.vmask && d.scalarWriteMask == a.smask);
        CHECK(bool(d.vectorDestRelative) == a.vdstRel && bool(d.scalarDestRelative) == a.sdstRel);
        CHECK(bool(d.exportData) == a.exportData);
        CHECK(bool(d.vectorSaturate) == a.vsat && bool(d.scalarSaturate) == a.ssat);
        CHECK(bool(d.absConstants) == a.absConstants);
        CHECK(bool(d.isPredicated) == a.predicated && bool(d.predicateCondition) == a.predCondition);
        CHECK(bool(d.constAddressRegisterRelative) == a.constAddressA0);
        CHECK(bool(d.const0Relative) == a.const0Rel && bool(d.const1Relative) == a.const1Rel);
        uint32_t regs[3] = {d.src1Register, d.src2Register, d.src3Register};
        uint32_t sels[3] = {d.src1Select, d.src2Select, d.src3Select};
        uint32_t swz[3] = {d.src1Swizzle, d.src2Swizzle, d.src3Swizzle};
        uint32_t neg[3] = {d.src1Negate, d.src2Negate, d.src3Negate};
        for (int i = 0; i < 3; i++) {
            const auto& s = a.src[i];
            uint32_t expect = s.temp ? (s.reg | (s.relative ? 0x40u : 0u) | (s.abs ? 0x80u : 0u)) : s.reg;
            CHECK(regs[i] == expect);
            CHECK(bool(sels[i]) == s.temp);
            CHECK(swz[i] == s.swizzle);
            CHECK(bool(neg[i]) == s.negate);
        }

        // Vertex fetch.
        xasm::VFetch v;
        v.dst = rnd(64);
        v.src = rnd(64);
        v.srcComponent = rnd(4);
        v.srcRel = rnd(2);
        v.dstRel = rnd(2);
        v.fetchConstant = rnd(96);
        v.format = rnd(64);
        v.dstSwizzle = rnd(4096);
        v.isSigned = rnd(2);
        v.integer = rnd(2);
        v.noZero = rnd(2);
        v.rounded = rnd(2);
        v.mini = rnd(2);
        v.expAdjust = int32_t(rnd(64)) - 32;
        v.stride = rnd(256);
        v.offset = int32_t(rnd(1u << 23)) - (1 << 22);
        v.predicated = rnd(2);
        v.predCondition = rnd(2);
        w = v.encode();
        VertexFetchInstruction vf;
        std::memcpy(&vf, w.data(), 12);
        CHECK(vf.opcode == FetchOpcode::VertexFetch);
        CHECK(vf.dstRegister == v.dst && vf.srcRegister == v.src && vf.srcSwizzle == v.srcComponent);
        CHECK(bool(vf.srcRegisterAm) == v.srcRel && bool(vf.dstRegisterAam) == v.dstRel);
        CHECK(vf.constIndex * 3 + vf.constIndexSelect == v.fetchConstant);
        CHECK(vf.format == v.format && vf.dstSwizzle == v.dstSwizzle);
        CHECK(bool(vf.formatCompAll) == v.isSigned && bool(vf.numFormatAll) == v.integer && bool(vf.signedRfModeAll) == v.noZero);
        CHECK(bool(vf.isIndexRounded) == v.rounded && bool(vf.isMiniFetch) == v.mini);
        CHECK(vf.expAdjust == v.expAdjust);
        CHECK(vf.stride == v.stride && vf.offset == v.offset);
        CHECK(bool(vf.isPredicated) == v.predicated && bool(vf.predicateCondition) == v.predCondition);

        // Texture fetch.
        xasm::TFetch t;
        uint32_t ops[] = {1, 16, 17, 18, 19, 24, 25, 26};
        t.op = xasm::Fop(ops[rnd(8)]);
        t.dst = rnd(64);
        t.src = rnd(64);
        t.srcRel = rnd(2);
        t.dstRel = rnd(2);
        t.srcSwizzle = rnd(64);
        t.dstSwizzle = rnd(4096);
        t.slot = rnd(32);
        t.dim = xasm::Dim(rnd(4));
        t.magFilter = rnd(4);
        t.minFilter = rnd(4);
        t.mipFilter = rnd(4);
        t.anisoFilter = rnd(8);
        t.volMagFilter = rnd(4);
        t.volMinFilter = rnd(4);
        t.useCompLod = rnd(2);
        t.useRegLod = rnd(2);
        t.useRegGradients = rnd(2);
        t.denorm = rnd(2);
        t.fetchValidOnly = rnd(2);
        t.lodBias = int32_t(rnd(128)) - 64;
        t.offsetX = int32_t(rnd(32)) - 16;
        t.offsetY = int32_t(rnd(32)) - 16;
        t.offsetZ = int32_t(rnd(32)) - 16;
        t.predicated = rnd(2);
        t.predCondition = rnd(2);
        w = t.encode();
        TextureFetchInstruction tf;
        std::memcpy(&tf, w.data(), 12);
        CHECK(uint32_t(tf.opcode) == uint32_t(t.op));
        CHECK(tf.dstRegister == t.dst && tf.srcRegister == t.src && tf.srcSwizzle == t.srcSwizzle && tf.dstSwizzle == t.dstSwizzle);
        CHECK(bool(tf.srcRegisterAm) == t.srcRel && bool(tf.dstRegisterAm) == t.dstRel);
        CHECK(tf.constIndex == t.slot && uint32_t(tf.dimension) == uint32_t(t.dim));
        CHECK(tf.magFilter == t.magFilter && tf.minFilter == t.minFilter && tf.mipFilter == t.mipFilter);
        CHECK(tf.anisoFilter == t.anisoFilter && tf.volMagFilter == t.volMagFilter && tf.volMinFilter == t.volMinFilter);
        CHECK(bool(tf.useCompLod) == t.useCompLod && bool(tf.useRegLod) == t.useRegLod && bool(tf.useRegGradients) == t.useRegGradients);
        CHECK(bool(tf.texCoordDenorm) == t.denorm && bool(tf.fetchValidOnly) == t.fetchValidOnly);
        CHECK(tf.lodBias == t.lodBias && tf.offsetX == t.offsetX && tf.offsetY == t.offsetY && tf.offsetZ == t.offsetZ);
        CHECK(bool(tf.isPredicated) == t.predicated && bool(tf.predCondition) == t.predCondition);
        if (failures) break;
    }

    // Control flow: build a program and decode its control flow with XenosRecomp's structs.
    xasm::Program p;
    std::vector<xasm::Op> ops(9, xasm::Op{xasm::Alu().encode(), false});
    ops[2].fetch = true;
    p.exec(ops);                                           // 0, 1 (split at 6)
    uint32_t ls = p.loopStart(17, true);                   // 2
    p.condExec(ops, 200, true, false, true);               // 3, 4
    p.loopEnd(17, ls, true, true);                         // 5
    uint32_t j = p.jump(0, false, true, 99, true);         // 6
    p.call(1, true, false, 5, false);                      // 7
    p.condExecPred({ops[0]}, true, true);                  // 8
    p.patchTarget(j, 8);
    p.ret();                                               // 9
    p.alloc(3, 5);                                         // 10
    std::vector<uint32_t> code = p.assemble();
    std::vector<ControlFlowInstruction> cf;
    for (size_t t = 0; t < 6; t++) {
        uint32_t dw[4] = {code[t * 3], code[t * 3 + 1] & 0xFFFF, (code[t * 3 + 1] >> 16) | (code[t * 3 + 2] << 16), code[t * 3 + 2] >> 16};
        ControlFlowInstruction pair[2];
        std::memcpy(pair, dw, sizeof(dw));
        cf.push_back(pair[0]);
        cf.push_back(pair[1]);
    }
    CHECK(cf[0].opcode == ControlFlowOpcode::Exec && cf[0].exec.count == 6 && cf[0].exec.address == 6 && cf[0].exec.sequence == 0x10);
    CHECK(cf[1].opcode == ControlFlowOpcode::Exec && cf[1].exec.count == 3 && cf[1].exec.address == 12);
    CHECK(cf[2].opcode == ControlFlowOpcode::LoopStart && cf[2].loopStart.loopId == 17 && cf[2].loopStart.isRepeat && cf[2].loopStart.address == 6);
    CHECK(cf[3].opcode == ControlFlowOpcode::CondExecPredClean && cf[3].condExec.boolAddress == 200 && cf[3].condExec.condition);
    CHECK(cf[5].opcode == ControlFlowOpcode::LoopEnd && cf[5].loopEnd.loopId == 17 && cf[5].loopEnd.address == 3 &&
          cf[5].loopEnd.isPredicatedBreak && cf[5].loopEnd.condition);
    CHECK(cf[6].opcode == ControlFlowOpcode::CondJmp && cf[6].condJmp.address == 8 && cf[6].condJmp.isPredicated &&
          !cf[6].condJmp.isUnconditional && cf[6].condJmp.condition && cf[6].condJmp.boolAddress == 99);
    CHECK(cf[7].opcode == ControlFlowOpcode::CondCall && cf[7].condCall.address == 1 && cf[7].condCall.isUnconditional &&
          cf[7].condCall.boolAddress == 5);
    CHECK(cf[8].opcode == ControlFlowOpcode::CondExecPredEnd && cf[8].condExecPred.condition && cf[8].condExecPred.count == 1);
    CHECK(cf[9].opcode == ControlFlowOpcode::Return);
    CHECK(cf[10].opcode == ControlFlowOpcode::Alloc && cf[10].alloc.size == 5 && cf[10].alloc.allocType == 3);
    CHECK(cf[11].opcode == ControlFlowOpcode::Nop);

    std::printf("assembler: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

// --------------------------------------------------------------------------------------------
int testContainer() {
    ctest::Spec spec;
    spec.vertex = true;
    xasm::Program p;
    xasm::VFetch f;
    f.dst = 1;
    f.format = xasm::F_32_32_32_FLOAT;
    p.exec({{f.encode(), true}, {xasm::Alu().v(xasm::ADDv, 2, "xyzw", xasm::r(1), xasm::c(3)).encode(), false}}, true);
    spec.ucode = p.assemble();
    spec.constants = {{"g_World", 2, 0, 4, 2, 3, 4, 4, 1}, {"g_Diffuse", 3, 2, 1, 4, 12, 1, 1, 1}, {"g_Skin", 0, 3, 1, 0, 1, 1, 1, 1}};
    spec.floatLiterals = {{252, {1, 2, 3, 4}}};
    spec.loopLiterals = {{2, 0x00010203u}};
    spec.boolLiterals = {{1, 0x80000001u}};
    spec.fetches = {{1, 0, 0, 1}, {2, 5, 2, 0}};
    spec.interpolators = {{5, 0, 0, 0xF}, {10, 1, 3, 0x7}};
    std::vector<uint8_t> bytes = ctest::writeContainer(spec);
    ParseResult r = parseContainer(bytes);
    CHECK(r.ok);
    if (!r.ok) std::printf("  %s\n", r.error.c_str());
    const ShaderInfo& i = r.info;
    CHECK(i.kind == ShaderKind::Vertex);
    CHECK(i.ucode == spec.ucode);
    CHECK(i.ucodeHash == hashBytes(bytes.data() + i.virtualSize, i.physicalSize));
    CHECK(i.target == "vs_3_0");
    CHECK(i.constants.size() == 3);
    if (i.constants.size() == 3) {
        CHECK(i.constants[0].name == "g_World" && i.constants[0].registerCount == 4 && i.constants[0].rows == 4);
        CHECK(i.constants[1].name == "g_Diffuse" && i.constants[1].registerSet == RegisterSet::Sampler && i.constants[1].registerIndex == 2);
        CHECK(i.constants[2].registerSet == RegisterSet::Bool && i.constants[2].registerIndex == 3);
    }
    CHECK(i.literals.size() == 1 && i.literals[0].registerIndex == 252 && i.literals[0].value[3] == 4);
    CHECK(i.loopLiterals.size() == 1 && i.loopLiterals[0].first == 2 && i.loopLiterals[0].second == 0x00010203u);
    CHECK(i.boolLiterals.size() == 1 && i.boolLiterals[0].first == 1 && i.boolLiterals[0].second == 0x80000001u);
    CHECK(i.registerWrites.size() == 3);
    CHECK(i.fetches.size() == 2 && i.fetches[1].address == 2 && i.fetches[1].usage == DeclUsage::TexCoord && i.fetches[1].usageIndex == 2);
    CHECK(i.interpolators.size() == 2 && i.interpolators[1].usage == DeclUsage::Color && i.interpolators[1].reg == 3 && i.interpolators[1].mask == 7);

    // Pixel: literals relative to c256, the pixel position after the interpolators.
    ctest::Spec ps;
    ps.vertex = false;
    ps.ucode = spec.ucode;
    ps.floatLiterals = {{255, {9, 9, 9, 9}}};
    ps.interpolators = {{5, 0, 0, 0xF}, {5, 1, 1, 0x3}};
    ps.paramGen = true;
    r = parseContainer(ctest::writeContainer(ps));
    CHECK(r.ok && r.info.kind == ShaderKind::Pixel && r.info.literals.size() == 1 && r.info.literals[0].registerIndex == 255);
    CHECK(r.ok && r.info.readsPixelPosition && r.info.pixelPositionRegister == 2 && r.info.interpolators.size() == 2);

    // The database's quirks: stripped tables with stale target bytes, no table, and words after
    // the interpolators that are not semantics (0x10F6 would read as usage 15).
    {
        ctest::Spec q = spec;
        q.strippedTable = true;
        q.extraBindingWords = {0x10F6, 0x10F7, 0xAC};
        r = parseContainer(ctest::writeContainer(q));
        CHECK(r.ok && r.info.target.empty() && r.info.constants.size() == 3 && r.info.interpolators.size() == 2);
        q.noConstantTable = true;
        r = parseContainer(ctest::writeContainer(q));
        CHECK(r.ok && r.info.constants.empty() && r.info.interpolators.size() == 2);
    }

    // Vertex fetch hoisting: fetches indexed by an untouched r0.x are done once at the top;
    // a write to r0 before a fetch keeps them in place.
    {
        r = parseContainer(bytes);
        TranslateResult t = translate(r.info);
        CHECK(t.ok && t.hlsl.find("kkIn[") != std::string::npos && t.hlsl.find("vfIndex = uint(") == std::string::npos);
        ctest::Spec w = spec;
        xasm::Program pw;
        xasm::VFetch fw;
        fw.dst = 1;
        fw.format = xasm::F_32_32_32_FLOAT;
        pw.exec({{xasm::Alu().v(xasm::ADDv, 0, "x", xasm::r(0), xasm::c(3)).encode(), false}, {fw.encode(), true},
                 {xasm::Alu().v(xasm::ADDv, 2, "xyzw", xasm::r(1), xasm::c(3)).encode(), false}}, true);
        w.ucode = pw.assemble();
        w.fetches = {{2, 0, 0, 1}};
        r = parseContainer(ctest::writeContainer(w));
        t = translate(r.info);
        CHECK(r.ok && t.ok && t.hlsl.find("kkIn[") == std::string::npos && t.hlsl.find("vfIndex = uint(") != std::string::npos);
    }

    // Bool literals: the bits of a literal dword the constant table does not name are inlined;
    // a named bool (g_Skin, b3) is read from the draw constants even inside a literal dword.
    {
        ctest::Spec b = spec;
        xasm::Program pb;
        auto op = [] { return std::vector<xasm::Op>{{xasm::Alu().v(xasm::ADDv, 2, "xyzw", xasm::r(1), xasm::c(3)).encode(), false}}; };
        pb.condExec(op(), 2, true);
        pb.condExec(op(), 3, true);
        pb.condExec(op(), 32, true);
        pb.condExec(op(), 33, false);
        pb.condExec(op(), 40, true, true);
        b.ucode = pb.assemble();
        b.boolLiterals = {{0, 0xFu}, {1, 0x80000001u}};
        r = parseContainer(ctest::writeContainer(b));
        TranslateResult t = translate(r.info);
        CHECK(r.ok && t.ok);
        for (uint32_t index : {2u, 32u, 33u, 40u})
            CHECK(t.hlsl.find("kk_BoolConst(" + std::to_string(index) + ")") == std::string::npos);
        CHECK(t.hlsl.find("kk_BoolConst(3)") != std::string::npos);
    }

    // Malformed input is refused, never read out of bounds.
    std::vector<uint8_t> bad = bytes;
    bad[2] = 0x11;
    CHECK(!parseContainer(bad).ok);
    for (size_t cut : {size_t(0), size_t(10), size_t(30), bytes.size() / 2, bytes.size() - 4}) CHECK(!parseContainer(bytes.data(), cut).ok);
    std::mt19937 rng(77);
    for (int k = 0; k < 2000; k++) {
        std::vector<uint8_t> m = bytes;
        for (int n = 0; n < 4; n++) m[rng() % m.size()] = uint8_t(rng());
        ParseResult pr = parseContainer(m);  // must not crash
        if (pr.ok) translate(pr.info);       // nor the translator on whatever parses
    }

    // The database reader.
    std::vector<uint8_t> db = ctest::writeDatabase({{"vsTest.hlsl", "float4x4 g_World;"}, {"psTest.hlsl", "sampler g_Diffuse;"}},
                                                   {{1, 0x0100000000000001ull, bytes}, {2, 0x0100000000000002ull, ctest::writeContainer(ps)}});
    fs::path dbPath = fs::temp_directory_path() / "kkshaders-test-db.bin";
    {
        std::ofstream f(dbPath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(db.data()), std::streamsize(db.size()));
    }
    Database d;
    std::string error;
    CHECK(d.load(dbPath.string(), &error));
    CHECK(d.entries().size() == 2 && d.sources().size() == 2);
    if (d.entries().size() == 2) {
        CHECK(d.container(d.entries()[0]).size() == bytes.size());
        CHECK(parseContainer(d.container(d.entries()[0])).ok);
        CHECK(parseContainer(d.secondContainer(d.entries()[0])).ok);
        CHECK(parseContainer(d.container(d.entries()[1])).ok);
        CHECK(d.familyName(ShaderKind::Vertex, 1) == "vsTest");
    }
    fs::remove(dbPath);

    // Bare microcode.
    std::vector<uint8_t> be = xasm::toBigEndian(spec.ucode);
    r = parseMicrocode(ShaderKind::Pixel, be.data(), be.size());
    CHECK(r.ok && r.info.rawMicrocode && r.info.ucode == spec.ucode && r.info.ucodeHash == hashBytes(be.data(), be.size()));
    CHECK(!parseMicrocode(ShaderKind::Pixel, be.data(), 10).ok);

    std::printf("container: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

// --------------------------------------------------------------------------------------------
struct Outcome {
    std::string stage;  // empty = passed
    std::string error;
    bool general = false, dynamic = false;
    size_t dxilBytes = 0, spirvBytes = 0;
};

Outcome runOne(const CorpusShader& s, Compiler& compiler, const std::string& spirvVal, std::string* hlslOut) {
    Outcome o;
    ParseResult p = s.raw ? parseMicrocode(s.vertex ? ShaderKind::Vertex : ShaderKind::Pixel, s.bytes.data(), s.bytes.size())
                          : parseContainer(s.bytes);
    if (!p.ok) {
        o.stage = "parse";
        o.error = p.error;
        return o;
    }
    BuildResult b = buildShader(p.info, compiler, true);
    *hlslOut = b.shader.hlsl;
    if (!b.ok) {
        o.stage = b.stage;
        o.error = b.error;
        return o;
    }
    o.general = b.shader.bindings.generalControlFlow;
    o.dynamic = b.shader.bindings.dynamicRegisters;
    o.dxilBytes = b.shader.dxil.size();
    o.spirvBytes = b.shader.spirv.size();
    if (!isDxilSigned(b.shader.dxil)) {
        o.stage = "dxil-signature";
        o.error = "DXIL not signed by the validator";
        return o;
    }
    std::string messages;
    if (!compiler.validateDxil(b.shader.dxil, &messages)) {
        o.stage = "dxil-validate";
        o.error = messages;
        return o;
    }
    if (!spirvVal.empty() && !validateSpirvExternal(spirvVal, b.shader.spirv, &messages)) {
        o.stage = "spirv-val";
        o.error = messages;
        return o;
    }
    // The cache entry round trip.
    CompiledShader back;
    std::vector<uint8_t> entry = encodeEntry(b.shader);
    if (!decodeEntry(entry.data(), entry.size(), back) || back.dxil != b.shader.dxil || back.spirv != b.shader.spirv ||
        serializeBindings(back.bindings) != serializeBindings(b.shader.bindings)) {
        o.stage = "cache-entry";
        o.error = "entry did not round-trip";
    }
    return o;
}

int testCorpus(int argc, char** argv) {
    std::string dxc = option(argc, argv, "--dxc"), spirvVal = option(argc, argv, "--spirv-val");
    fs::path out = option(argc, argv, "--out", "corpus-out");
    uint32_t fuzz = uint32_t(std::stoul(option(argc, argv, "--fuzz", "400")));
    uint32_t seed = uint32_t(std::stoul(option(argc, argv, "--seed", "20261010")));
    std::string filter = option(argc, argv, "--filter");
    std::string error;
    if (!loadDxc(dxc, &error)) {
        std::printf("corpus: %s\n", error.c_str());
        return 1;
    }
    std::printf("DXC %s; spirv-val: %s\n", dxcVersion().c_str(), spirvVal.empty() ? "not given (DXC's validator only)" : spirvVal.c_str());

    std::vector<CorpusShader> corpus = buildCorpus(fuzz, seed);
    if (!filter.empty())
        corpus.erase(std::remove_if(corpus.begin(), corpus.end(),
                                    [&](const CorpusShader& s) { return (s.group + " " + s.name).find(filter) == std::string::npos; }),
                     corpus.end());
    std::error_code ec;
    fs::remove_all(out / "failed", ec);

    std::atomic<size_t> next{0};
    std::mutex lock;
    std::map<std::string, std::pair<int, int>> groups;
    int failed = 0, general = 0, dynamic = 0;
    size_t dxilTotal = 0, spirvTotal = 0;
    auto start = std::chrono::steady_clock::now();
    auto worker = [&] {
        Compiler compiler;
        for (;;) {
            size_t i = next++;
            if (i >= corpus.size()) break;
            const CorpusShader& s = corpus[i];
            std::string hlsl;
            Outcome o = runOne(s, compiler, spirvVal, &hlsl);
            std::lock_guard<std::mutex> g(lock);
            auto& grp = groups[s.group + (s.raw ? " (bare)" : "")];
            grp.second++;
            if (o.stage.empty()) {
                grp.first++;
                general += o.general;
                dynamic += o.dynamic;
                dxilTotal += o.dxilBytes;
                spirvTotal += o.spirvBytes;
            } else {
                failed++;
                std::printf("FAILED [%s] %s: %s\n%s\n", s.group.c_str(), s.name.c_str(), o.stage.c_str(), o.error.substr(0, 600).c_str());
                std::string base = std::to_string(i);
                writeFile(out / "failed" / (base + ".hlsl"), hlsl);
                writeFile(out / "failed" / (base + ".txt"), s.group + " / " + s.name + "\n" + o.stage + "\n" + o.error);
                std::string bytes(s.bytes.begin(), s.bytes.end());
                writeFile(out / "failed" / (base + (s.raw ? ".ucode" : ".bin")), bytes);
            }
        }
    };
    unsigned n = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < n; t++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf("\ncorpus: %zu shaders, %d failed, %.1f s\n", corpus.size(), failed, seconds);
    for (const auto& [name, r] : groups) std::printf("  %-28s %4d / %4d\n", name.c_str(), r.first, r.second);
    std::printf("general control flow %d, dynamic register indexing %d; DXIL %zu KB, SPIR-V %zu KB\n", general, dynamic,
                dxilTotal / 1024, spirvTotal / 1024);

    // A sample of what the translator writes, for reading.
    for (const auto& s : corpus) {
        if (s.name == "ps cube sequence" || s.name == "nested loops 1" || s.name == "declaration elements") {
            ParseResult p = parseContainer(s.bytes);
            if (p.ok) {
                TranslateResult t = translate(p.info);
                writeFile(out / "samples" / (s.name + ".hlsl"), t.hlsl);
            }
        }
    }
    return failed ? 1 : 0;
}

// --------------------------------------------------------------------------------------------
int testCache(int argc, char** argv) {
    std::string dxc = option(argc, argv, "--dxc");
    fs::path out = option(argc, argv, "--out", "cache-out");
    std::string error;
    if (!loadDxc(dxc, &error)) {
        std::printf("cache: %s\n", error.c_str());
        return 1;
    }
    std::error_code ec;
    fs::remove_all(out, ec);
    Compiler compiler;
    std::vector<CorpusShader> corpus = buildCorpus(0, 1);
    std::vector<CompiledShader> built;
    ShaderCache cache(out / "cache");
    ShaderProvider provider(nullptr, &cache);
    size_t n = 0;
    for (const auto& s : corpus) {
        if (s.raw || n >= 60) continue;
        n++;
        ParseResult p = parseContainer(s.bytes);
        CHECK(p.ok);
        CompiledShader c;
        CHECK(provider.get(p.info, compiler, c, &error));  // builds and stores
        CompiledShader again;
        CHECK(cache.load(p.info.ucodeHash, translationInputHash(p.info), again));
        CHECK(again.dxil == c.dxil && again.spirv == c.spirv);
        built.push_back(c);
    }
    // A damaged cache file is a miss, not a crash.
    {
        fs::path f = cache.pathFor(built[0].ucodeHash, built[0].inputHash);
        std::fstream s(f, std::ios::binary | std::ios::in | std::ios::out);
        s.seekp(100);
        s.put('\x5A');
        s.close();
        CompiledShader x;
        CHECK(!cache.load(built[0].ucodeHash, built[0].inputHash, x));
    }
    // The pack: every entry found, each lookup well under a millisecond.
    fs::path packPath = out / "test.pack";
    CHECK(ShaderPack::write(packPath, built, &error));
    ShaderPack pack;
    CHECK(pack.open(packPath, &error));
    CHECK(pack.size() == built.size());
    double worst = 0;
    for (const auto& b : built) {
        CompiledShader x;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = pack.find(b.ucodeHash, b.inputHash, x);
        worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        CHECK(ok && x.dxil == b.dxil && x.spirv == b.spirv);
        CompiledShader y;
        CHECK(pack.find(b.ucodeHash, 0, y));
    }
    CompiledShader none;
    CHECK(!pack.find(0x1234, 0, none));
    ShaderProvider fromPack(&pack, nullptr);
    {
        ParseResult p = parseContainer(corpus[0].bytes);
        CompiledShader c;
        CHECK(fromPack.get(p.info, compiler, c, &error));
    }
    std::printf("cache: %zu shaders, worst pack lookup %.4f ms: %s\n", built.size(), worst, failures ? "FAILED" : "ok");
    CHECK(worst < 1.0);
    return failures ? 1 : 0;
}

// --------------------------------------------------------------------------------------------
// A synthetic xeshaders.bin and shader storage file from the corpus, for testing the tool's
// database commands without the game.
int makeFixtures(int argc, char** argv) {
    fs::path out = argc > 2 ? argv[2] : "fixtures";
    fs::create_directories(out);
    std::vector<CorpusShader> corpus = buildCorpus(40, 99);
    std::vector<ctest::DbEntry> entries;
    std::string vsSource = "// vsFixture\n", psSource = "// psFixture\n";
    for (const auto& s : corpus) {
        if (s.raw) continue;
        entries.push_back({s.vertex ? 1u : 2u, (uint64_t(1) << 56) | entries.size(), s.bytes});
        for (const auto& c : s.spec.constants) (s.vertex ? vsSource : psSource) += "uniform " + c.name + ";\n";
    }
    std::vector<uint8_t> db = ctest::writeDatabase({{"vsFixture.hlsl", vsSource}, {"psFixture.hlsl", psSource}}, entries);
    writeFile(out / "xeshaders.bin", std::string(db.begin(), db.end()));
    // Shader storage records: {u64 hash, u32 count | pixel << 31, big-endian microcode}.
    std::string xsh = "XESH";
    xsh.append(4, '\0');
    for (const auto& s : corpus) {
        if (!s.raw) continue;
        uint64_t hash = hashBytes(s.bytes.data(), s.bytes.size());
        uint32_t word = uint32_t(s.bytes.size() / 4) | (s.vertex ? 0u : 0x80000000u);
        xsh.append(reinterpret_cast<const char*>(&hash), 8);
        xsh.append(reinterpret_cast<const char*>(&word), 4);
        xsh.append(s.bytes.begin(), s.bytes.end());
    }
    writeFile(out / "fixture.xsh", xsh);
    std::printf("fixtures: %zu database entries, shader storage file written to %s\n", entries.size(), out.string().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: kkshaders_tests assembler|container|corpus|cache [options]\n");
        return 2;
    }
    std::string what = argv[1];
    if (what == "assembler") return testAssembler();
    if (what == "container") return testContainer();
    if (what == "corpus") return testCorpus(argc, argv);
    if (what == "cache") return testCache(argc, argv);
    if (what == "make-fixtures") return makeFixtures(argc, argv);
    std::printf("unknown test %s\n", what.c_str());
    return 2;
}
