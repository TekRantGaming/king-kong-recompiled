#include "random_program.h"

#include <random>
#include <string>

#include "xenos_asm.h"

using namespace xasm;

namespace {

constexpr uint32_t kOPos = 62;
constexpr uint32_t kRegs = 12;   // r0-r11 (interpolators and results live in r0-r7)

class Builder {
public:
    Builder(uint32_t seed, bool vertex, bool raw) : rng_(seed), vertex_(vertex), raw_(raw) {}

    CorpusShader build(uint32_t seed);

private:
    std::mt19937 rng_;
    bool vertex_, raw_;
    bool haveA0_ = false;
    bool havePredicate_ = false;
    uint32_t loopDepth_ = 0;
    uint32_t fetchStride_ = 4;

    uint32_t rnd(uint32_t n) { return n ? uint32_t(rng_() % n) : 0; }
    bool chance(uint32_t percent) { return rnd(100) < percent; }

    uint32_t swizzle() {
        // Mostly random relative swizzles; sometimes the identity or a replicate.
        uint32_t k = rnd(10);
        if (k < 2) return aluSwizzle("xyzw");
        if (k < 4) return aluSwizzle(std::string(1, "xyzw"[rnd(4)]).c_str());
        return rnd(256);
    }
    Src operand() {
        Src s;
        if (chance(30)) {
            s.temp = false;
            s.reg = chance(80) ? rnd(64) : rnd(256);
        } else {
            s.temp = true;
            s.reg = rnd(kRegs);
            s.abs = chance(12);
            s.relative = loopDepth_ > 0 && chance(15);
        }
        s.swizzle = swizzle();
        s.negate = chance(25);
        return s;
    }
    uint32_t writeMask() { return chance(8) ? 0 : 1 + rnd(15); }

    Op alu() {
        Alu a;
        // Vector operation: kills rarely (they end the pixel), the rest evenly.
        uint32_t vop;
        do vop = rnd(30); while (vop >= KILLEQv && vop <= KILLNEv && !chance(10));
        a.vop = vop;
        a.vdst = rnd(kRegs);
        a.vmask = writeMask();
        a.src[0] = operand();
        a.src[1] = operand();
        a.src[2] = operand();
        // Scalar operation.
        uint32_t sop;
        do sop = rnd(51); while (sop == 41 || (sop >= KILLEs && sop <= KILLONEs && !chance(10)));
        if (chance(15)) sop = RETAIN_PREV;
        if (sop >= MUL_CONST_0 && sop <= SUB_CONST_1) {
            uint32_t temp = rnd(kRegs);
            a.sc(sop, rnd(kRegs), "x", rnd(64), "xyzw"[rnd(4)], temp, "xyzw"[rnd(4)]);
            a.smask = writeMask();
            if (chance(25)) a.src[2].negate = true;
        } else {
            a.sop = sop;
            a.sdst = rnd(kRegs);
            a.smask = writeMask();
        }
        if (chance(15)) a.sat(chance(50), chance(50));
        if (chance(10)) a.absConst();
        if (havePredicate_ && chance(20)) a.pred(chance(50));
        // Relative constants: by the loop counter inside loops, by a0 after a maxa.
        bool a0 = haveA0_ && chance(50);
        if ((loopDepth_ > 0 || haveA0_) && chance(20)) a.rel(chance(60), chance(60), a0 || loopDepth_ == 0);
        if (loopDepth_ > 0 && chance(8)) a.vrel();
        if (loopDepth_ > 0 && chance(8)) a.srel();
        if (vop == MAXAv || sop == MAXAs || sop == MAXA_FLOORs) haveA0_ = true;
        if ((vop >= SETP_EQ_PUSHv && vop <= SETP_GE_PUSHv) || (sop >= PRED_SETEs && sop <= PRED_SET_RESTOREs)) havePredicate_ = true;
        return {a.encode(), false};
    }

    Op tfetch() {
        TFetch t;
        if (chance(10)) {
            t.op = Fop::SetTexLOD;
            t.src = rnd(kRegs);
            t.srcSwizzle = rnd(64);
            return {t.encode(), true};
        }
        // A slot keeps one dimension (as in real shaders): slot & 3.
        t.slot = rnd(8);
        t.dim = Dim(t.slot & 3);
        t.dst = rnd(kRegs);
        t.dstRel = loopDepth_ > 0 && chance(5);
        t.src = rnd(kRegs);
        t.srcSwizzle = rnd(64);
        // Destination swizzle: components, 0, 1 and keep.
        const char* dsts[] = {"xyzw", "wzyx", "x_01", "zyx1", "y0_w", "xxyy"};
        t.dstSwizzle = fetchDstSwizzle(dsts[rnd(6)]);
        t.magFilter = rnd(4);
        t.minFilter = t.magFilter;
        t.useCompLod = chance(70);
        t.useRegLod = chance(20);
        t.lodBias = int32_t(rnd(128)) - 64;
        if (chance(40)) {
            t.offsetX = int32_t(rnd(32)) - 16;
            t.offsetY = int32_t(rnd(32)) - 16;
            t.offsetZ = int32_t(rnd(32)) - 16;
        }
        t.denorm = chance(20);
        t.predicated = havePredicate_ && chance(15);
        t.predCondition = chance(50);
        return {t.encode(), true};
    }

    // The compiler's cube sequence on a random direction register, then a tfetchCube.
    std::vector<Op> cubeSequence() {
        uint32_t dir = rnd(kRegs), tmp = (dir + 1 + rnd(kRegs - 1)) % kRegs;
        Src ma = r(tmp, "xyzw", false, true);
        ma.swizzle = scalarSwizzle('z', 'z');
        TFetch t;
        t.dim = Dim::Cube;
        t.slot = 3 + 4 * rnd(2);
        t.src = tmp;
        t.srcSwizzle = fetchSrcSwizzle("yxw");
        t.dst = rnd(kRegs);
        t.magFilter = rnd(4);
        t.minFilter = t.magFilter;
        uint32_t tmp2 = (tmp + 1) % kRegs;
        if (tmp2 == dir) tmp2 = (tmp2 + 1) % kRegs;
        return {
            {Alu().v(CUBEv, tmp, "xyzw", r(dir, "zzxy"), r(dir, "yxzz")).encode(), false},
            {Alu().s(RECIP_IEEE, tmp2, "z", ma).encode(), false},
            {Alu().v(MADv, tmp, "xy", r(tmp, "xyzw"), r(tmp2, "zzzz"), c(0, "wwww")).encode(), false},
            {t.encode(), true},
        };
    }

    Op vfetch(bool full) {
        VFetch f;
        f.dst = rnd(kRegs);
        const char* dsts[] = {"xyzw", "xyz1", "xy01", "wzyx", "x___", "z_x0"};
        f.dstSwizzle = fetchDstSwizzle(dsts[rnd(6)]);
        const uint32_t formats[] = {F_8_8_8_8, F_2_10_10_10, F_10_11_11, F_11_11_10, F_16_16, F_16_16_16_16, F_16_16_FLOAT,
                                    F_16_16_16_16_FLOAT, F_32, F_32_32, F_32_32_32_32, F_32_FLOAT, F_32_32_FLOAT,
                                    F_32_32_32_32_FLOAT, F_32_32_32_FLOAT};
        f.format = formats[rnd(15)];
        f.isSigned = chance(50);
        f.integer = chance(30);
        f.noZero = chance(30);
        f.expAdjust = chance(20) ? int32_t(rnd(9)) - 4 : 0;
        f.mini = !full;
        f.fetchConstant = chance(70) ? 95 : 94;
        if (full) {
            // The index: r0.x (the vertex index) mostly, rounded or not.
            f.src = 0;
            f.srcComponent = 0;
            f.rounded = chance(30);
            fetchStride_ = 4 + rnd(5);
        }
        f.stride = fetchStride_;
        f.offset = int32_t(rnd(fetchStride_));
        f.predicated = havePredicate_ && chance(10);
        f.predCondition = chance(50);
        return {f.encode(), true};
    }

    std::vector<Op> block() {
        std::vector<Op> ops;
        uint32_t n = 1 + rnd(6);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k = rnd(100);
            if (k < 12) {
                ops.push_back(tfetch());
            } else if (k < 14 && ops.size() + 4 <= 6) {
                auto seq = cubeSequence();
                ops.insert(ops.end(), seq.begin(), seq.end());
            } else if (k < 20 && vertex_) {
                ops.push_back(vfetch(chance(50)));
            } else {
                ops.push_back(alu());
            }
        }
        return ops;
    }
};

CorpusShader Builder::build(uint32_t seed) {
    Program p;
    // Vertex shaders start with a fetch indexed by the vertex index.
    if (vertex_) {
        std::vector<Op> first = {vfetch(true)};
        if (chance(60)) first.push_back(vfetch(false));
        p.exec(first);
    }
    std::vector<std::pair<uint32_t, uint32_t>> loops;  // id, start index
    std::vector<uint32_t> jumps;
    std::vector<uint32_t> calls;
    bool general = chance(15);  // allow crossing jumps and calls
    uint32_t steps = 3 + rnd(10);
    for (uint32_t s = 0; s < steps; s++) {
        uint32_t k = rnd(100);
        if (k < 45) {
            p.exec(block());
        } else if (k < 55 && loops.size() < 3) {
            uint32_t id = rnd(32);
            loops.push_back({id, p.loopStart(id)});
            loopDepth_++;
        } else if (k < 63 && !loops.empty()) {
            p.exec(block());
            p.loopEnd(loops.back().first, loops.back().second, havePredicate_ && chance(25), chance(50));
            loops.pop_back();
            loopDepth_--;
        } else if (k < 71) {
            p.condExec(block(), rnd(256), chance(50));
        } else if (k < 79 && havePredicate_) {
            p.condExecPred(block(), chance(50));
        } else if (k < 86 && (general || loops.empty())) {
            jumps.push_back(p.jump(0, false, havePredicate_ && chance(50), rnd(256), chance(50)));
        } else if (k < 93 && !jumps.empty()) {
            size_t which = general && chance(40) ? rnd(uint32_t(jumps.size())) : jumps.size() - 1;
            p.exec(block());
            p.patchTarget(jumps[which], p.label());
            jumps.erase(jumps.begin() + std::ptrdiff_t(which));
        } else if (general && chance(50)) {
            calls.push_back(p.call(0, chance(30), havePredicate_ && chance(30), rnd(256), chance(50)));
        } else {
            p.exec(block());
        }
    }
    p.exec(block());
    while (!loops.empty()) {
        p.loopEnd(loops.back().first, loops.back().second);
        loops.pop_back();
        loopDepth_--;
    }
    for (uint32_t j : jumps) p.patchTarget(j, p.label());

    // Exports.
    std::vector<Op> exports;
    auto exportReg = [&](uint32_t target, uint32_t src, const char* m) {
        Alu a;
        a.v(MAXv, target, m, r(src), r(src)).exp(target);
        if (chance(20)) {
            // A scalar op into the export too (and with it the constant 0 / 1 components).
            a.sop = MAXs;
            a.smask = 1 + rnd(15);
            Src s = r(rnd(kRegs));
            s.swizzle = rnd(256);
            a.src[2] = s;
            if (chance(30)) a.srel();
        }
        exports.push_back({a.encode(), false});
    };
    if (vertex_) {
        p.alloc(1, 0);
        exportReg(kOPos, rnd(kRegs), "xyzw");
        p.exec(exports);
        exports.clear();
        p.alloc(2, 7);
        for (uint32_t i = 0; i < 8; i++) exportReg(i, rnd(kRegs), chance(80) ? "xyzw" : "xy");
        p.exec(exports, true);
    } else {
        p.alloc(2, 0);
        uint32_t targets = 1 + rnd(4);
        for (uint32_t i = 0; i < targets; i++) exportReg(i, rnd(kRegs), "xyzw");
        p.exec(exports, true);
    }
    if (!calls.empty()) {
        uint32_t fn = p.label();
        p.exec(block());
        p.ret();
        for (uint32_t c : calls) p.patchTarget(c, fn);
    }

    ctest::Spec spec;
    spec.vertex = vertex_;
    spec.ucode = p.assemble();
    spec.constants.push_back({"g_Constants", 2, 0, 64, 1, 3, 1, 4, 64});
    spec.constants.push_back({"g_Bools", 0, 0, 4, 0, 1, 1, 1, 4});
    // Literal constants the program may read directly or relatively.
    if (chance(40)) {
        uint32_t n = 1 + rnd(3);
        for (uint32_t i = 0; i < n; i++) {
            std::array<uint32_t, 4> v;
            for (auto& x : v) x = std::bit_cast<uint32_t>(float(int32_t(rnd(129)) - 64) / 16.0f);
            uint32_t reg = chance(50) ? 64 + rnd(4) : rnd(64);
            bool dup = false;
            for (auto& [r2, v2] : spec.floatLiterals) dup |= r2 == reg;
            if (!dup) spec.floatLiterals.push_back({reg, v});
        }
    }
    if (chance(30)) spec.loopLiterals.push_back({rnd(32), (rnd(4)) | (rnd(6) << 8) | ((uint32_t(int32_t(rnd(5)) - 2) & 0xFF) << 16)});
    if (chance(30)) spec.boolLiterals.push_back({rnd(8), uint32_t(rng_())});
    // Interpolators: r0-r7 as TEXCOORD0-5, COLOR0-1 (vertex: export registers 0-7).
    for (uint32_t i = 0; i < 8; i++) spec.interpolators.push_back({i < 6 ? 5u : 10u, i < 6 ? i : i - 6, i});
    if (!vertex_) spec.paramGen = chance(40);
    if (vertex_) {
        std::vector<uint32_t> addresses = findVfetchAddresses(spec.ucode);
        for (size_t i = 0; i < addresses.size(); i++)
            spec.fetches.push_back({addresses[i], i == 0 ? 0u : 5u, uint32_t(i == 0 ? 0 : i - 1), 0});
    }
    CorpusShader s;
    s.group = "random";
    s.name = std::string(vertex_ ? "vs" : "ps") + (raw_ ? " raw " : " ") + std::to_string(seed);
    s.vertex = vertex_;
    s.raw = raw_;
    s.spec = spec;
    s.bytes = raw_ ? toBigEndian(spec.ucode) : ctest::writeContainer(spec);
    return s;
}

}  // namespace

CorpusShader randomProgram(uint32_t seed, bool vertex, bool raw) {
    Builder b(seed * 2654435761u + (vertex ? 1 : 0) + (raw ? 2 : 0), vertex, raw);
    return b.build(seed);
}
