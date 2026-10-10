#include "corpus.h"

#include <cstring>
#include <functional>
#include <random>
#include <stdexcept>

#include "xenos_asm.h"

using namespace xasm;

namespace {

// D3DDECLUSAGE values.
enum Usage : uint32_t {
    POSITION = 0, BLENDWEIGHT = 1, BLENDINDICES = 2, NORMAL = 3, PSIZE = 4, TEXCOORD = 5, TANGENT = 6, BINORMAL = 7,
    COLOR = 10, FOG = 11
};

// Export registers.
constexpr uint32_t kOPos = 62, kOMisc = 63, kODepth = 61, kExportAddress = 32, kExportData = 33;

Op A(const Alu& a) { return {a.encode(), false}; }
Op F(const VFetch& v) { return {v.encode(), true}; }
Op F(const TFetch& t) { return {t.encode(), true}; }

// Export r<src> to an export register.
Op expo(uint32_t exportReg, uint32_t src, const char* m = "xyzw") {
    return A(Alu().v(MAXv, exportReg, m, r(src), r(src)).exp(exportReg));
}

VFetch positionFetch(uint32_t dst = 1) {
    VFetch f;
    f.dst = dst;
    f.src = 0;
    f.fetchConstant = 95;
    f.format = F_32_32_32_FLOAT;
    f.stride = 3;
    return f;
}

struct Element {
    uint32_t usage, index;
};

ctest::Spec defaultSpec(bool vertex) {
    ctest::Spec s;
    s.vertex = vertex;
    s.constants.push_back({"g_Constants", 2, 0, 32, 1, 3, 1, 4, 32});
    s.constants.push_back({"g_Matrix", 2, 32, 4, 2, 3, 4, 4, 1});
    s.constants.push_back({"g_Sampler0", 3, 0, 1, 4, 12, 1, 1, 1});
    s.constants.push_back({"g_Enable", 0, 0, 1, 0, 1, 1, 1, 1});
    if (vertex) {
        s.interpolators = {{TEXCOORD, 0, 0}, {TEXCOORD, 1, 1}, {COLOR, 0, 2}, {COLOR, 1, 3}};
    } else {
        s.interpolators = {{TEXCOORD, 0, 0}, {TEXCOORD, 1, 1}, {COLOR, 0, 2}, {COLOR, 1, 3}};
    }
    return s;
}

class Gen {
public:
    explicit Gen(std::vector<CorpusShader>& out) : out_(out) {}

    void add(const std::string& group, const std::string& name, bool vertex, const Program& p, ctest::Spec spec,
             std::vector<Element> elements = {{POSITION, 0}}, bool alsoRaw = true) {
        spec.vertex = vertex;
        spec.ucode = p.assemble();
        if (vertex) {
            std::vector<uint32_t> addresses = findVfetchAddresses(spec.ucode);
            spec.fetches.clear();
            for (size_t i = 0; i < addresses.size(); i++) {
                Element e = elements.empty() ? Element{TEXCOORD, uint32_t(i)} : elements[std::min(i, elements.size() - 1)];
                if (i >= elements.size() && !elements.empty()) e.index = uint32_t(i);  // keep usages distinct
                spec.fetches.push_back({addresses[i], e.usage, e.index, 0});
            }
        }
        CorpusShader s;
        s.group = group;
        s.name = name;
        s.vertex = vertex;
        s.bytes = ctest::writeContainer(spec);
        s.spec = spec;
        out_.push_back(s);
        if (alsoRaw) {
            CorpusShader raw = s;
            raw.raw = true;
            raw.name += " (bare microcode)";
            raw.bytes = toBigEndian(spec.ucode);
            out_.push_back(raw);
        }
    }

    void addRaw(const std::string& group, const std::string& name, bool vertex, const Program& p) {
        CorpusShader s;
        s.group = group;
        s.name = name;
        s.vertex = vertex;
        s.raw = true;
        s.spec.vertex = vertex;
        s.spec.ucode = p.assemble();
        s.bytes = toBigEndian(s.spec.ucode);
        out_.push_back(s);
    }

    // A vertex shader around `body`: fetch the position into r1, run the body, export r1 as the
    // position and r2-r5 as the interpolators.
    void vs(const std::string& group, const std::string& name, const std::vector<Op>& body, ctest::Spec spec = defaultSpec(true),
            std::vector<Element> elements = {{POSITION, 0}}, bool alsoRaw = true) {
        Program p;
        std::vector<Op> ops = {F(positionFetch())};
        ops.insert(ops.end(), body.begin(), body.end());
        p.exec(ops);
        finishVs(p);
        add(group, name, true, p, spec, elements, alsoRaw);
    }

    static void finishVs(Program& p) {
        p.alloc(1, 0);
        p.exec({expo(kOPos, 1)});
        p.alloc(2, 3);
        p.exec({expo(0, 2), expo(1, 3), expo(2, 4), expo(3, 5, "xyz")}, true);
    }

    // A pixel shader around `body`, exporting r0 to colour 0.
    void ps(const std::string& group, const std::string& name, const std::vector<Op>& body, ctest::Spec spec = defaultSpec(false),
            bool alsoRaw = true) {
        Program p;
        p.exec(body);
        finishPs(p);
        add(group, name, false, p, spec, {}, alsoRaw);
    }

    static void finishPs(Program& p, uint32_t src = 0) {
        p.alloc(2, 0);
        p.exec({expo(0, src)}, true);
    }

private:
    std::vector<CorpusShader>& out_;
};

const char* const kVopNames[] = {"add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad",
                                 "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push",
                                 "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne",
                                 "dst", "maxa"};
const char* const kSopNames[] = {"adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts", "sges",
                                 "snes", "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf",
                                 "rsq", "maxas", "maxasf", "subs", "subs_prev", "setp_eq", "setp_ne", "setp_gt", "setp_ge",
                                 "setp_inv", "setp_pop", "setp_clr", "setp_rstr", "kills_eq", "kills_gt", "kills_ge",
                                 "kills_ne", "kills_one", "sqrt", "(41)", "mulsc0", "mulsc1", "addsc0", "addsc1", "subsc0",
                                 "subsc1", "sin", "cos", "retain_prev"};

void aluGroups(Gen& g) {
    // Every vector operation, in both stages, with swizzles, negation, a constant operand and
    // the result used afterwards (predicate and address register ones feed a dependent use).
    for (uint32_t op = 0; op <= MAXAv; op++) {
        for (int stage = 0; stage < 2; stage++) {
            bool vertex = stage == 0;
            uint32_t base = vertex ? 2 : 0;
            std::vector<Op> body = {
                A(Alu().v(op, 6, "xyzw", r(base, "xyzw"), r(base + 1, "yzwx", true), c(5, "wzyx"))),
                A(Alu().v(op, 7, "xz", r(base + 1, "w", false, true), c(6, "y", true), r(base))),
            };
            if (op >= SETP_EQ_PUSHv && op <= SETP_GE_PUSHv) body.push_back(A(Alu().v(ADDv, 6, "y", r(6), c(1)).pred(true)));
            if (op == MAXAv) body.push_back(A(Alu().v(ADDv, 6, "xyzw", r(6), c(10)).rel(false, true, true)));
            body.push_back(A(Alu().v(ADDv, base, "xyzw", r(6), r(7))));
            if (vertex) g.vs("alu-vector", std::string("vs ") + kVopNames[op], body);
            else g.ps("alu-vector", std::string("ps ") + kVopNames[op], body);
        }
        // Saturated, absolute constants, write to w only.
        g.ps("alu-vector", std::string("ps ") + kVopNames[op] + " sat abs", {
            A(Alu().v(op, 0, "w", c(3, "xxyy"), c(4, "zzww", true), c(7)).sat(true, false).absConst())});
    }

    // Every scalar operation (41 is not one), in both stages, on several operand forms.
    for (uint32_t op = 0; op <= RETAIN_PREV; op++) {
        if (op == 41) continue;
        for (int stage = 0; stage < 2; stage++) {
            bool vertex = stage == 0;
            uint32_t base = vertex ? 2 : 0;
            std::vector<Op> body;
            if (op >= MUL_CONST_0 && op <= SUB_CONST_1) {
                uint32_t temp = (op & 1) | 2 | 4 * (vertex ? 1 : 0);  // r3 / r6 style indices
                body.push_back(A(Alu().sc(op, base, "xw", 9, 'z', temp, 'y')));
                body.push_back(A(Alu().sc(op, base + 1, "y", 200, 'w', (op & 1), 'x').absConst()));
            } else {
                Src a = r(base + 1);
                a.swizzle = scalarSwizzle('w', 'y');
                body.push_back(A(Alu().s(op, base, "xz", a)));
                Src k = c(12);
                k.swizzle = scalarSwizzle('x', 'z');
                k.negate = true;
                body.push_back(A(Alu().s(op, base + 1, "w", k).sat(false, true)));
                Src t = r(base, "xyzw", false, true);
                t.swizzle = scalarSwizzle('z', 'z');
                body.push_back(A(Alu().v(MULv, base, "y", r(base), r(base + 1)).s(op, base, "w", t)));
            }
            if (op >= PRED_SETEs && op <= PRED_SET_RESTOREs) body.push_back(A(Alu().v(ADDv, base, "x", r(base), c(2)).pred(false)));
            if (op == MAXAs || op == MAXA_FLOORs) body.push_back(A(Alu().v(ADDv, base, "x", c(20), c(30)).rel(true, true, true)));
            if (vertex) g.vs("alu-scalar", std::string("vs ") + kSopNames[op], body);
            else g.ps("alu-scalar", std::string("ps ") + kSopNames[op], body);
        }
    }

    // Co-issue: both operations reading and writing the same register (sources are read first).
    g.ps("alu-coissue", "same register", {
        A(Alu().v(ADDv, 0, "xy", r(0), r(1)).s(ADDs, 0, "zw", r(0, "xyzw"))),
        A(Alu().v(MULv, 1, "xyzw", r(0), r(0)).s(RECIP_IEEE, 0, "x", r(1, "x")).sat(true, true)),
    });
    // Exports with both operations and the constant 0 / 1 components.
    {
        Program p;
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), r(1)))});
        p.alloc(2, 0);
        Src s = r(1);
        s.swizzle = scalarSwizzle('x', 'x');
        p.exec({A(Alu().v(MULv, 0, "xy", r(0), r(1)).s(RECIP_IEEE, 0, "yz", s).exp(0)),
                A(Alu().v(MULv, 0, "x", r(0), r(1)).s(EXP_IEEE, 0, "y", s).exp(0).srel())},
               true);
        g.add("alu-coissue", "export constants", false, p, defaultSpec(false));
    }
}

void constantGroups(Gen& g) {
    // Literal (def) constants read directly and relatively; constant table arrays.
    {
        ctest::Spec spec = defaultSpec(true);
        spec.floatLiterals = {{250, {0x3F800000u, 0, 0x40000000u, 0xBF800000u}}, {251, {0x3F000000u, 0x3F000000u, 0, 0}}};
        spec.loopLiterals = {{3, 0x00010004u}};
        spec.boolLiterals = {{0, 0x5u}};
        g.vs("constants", "literals", {
            A(Alu().v(MADv, 2, "xyzw", r(1), c(250), c(251))),
            A(Alu().s(MAXAs, 3, "x", r(1, "x"))),
            A(Alu().v(ADDv, 3, "xyzw", c(250), c(1)).rel(true, false, true)),
        }, spec);
    }
    // Three constants: the second and third share const_1_rel_abs.
    for (int mode = 0; mode < 4; mode++) {
        g.ps("constants", "three constants mode " + std::to_string(mode), {
            A(Alu().s(MAXAs, 4, "x", r(0, "x"))),
            A(Alu().v(MADv, 0, "xyzw", c(1), c(2), c(3)).rel(mode & 1, (mode >> 1) & 1, true)),
            A(Alu().v(CNDGEv, 1, "xyzw", r(1), c(4), c(5)).rel(true, false, true)),
            A(Alu().v(DP4v, 2, "x", c(6), r(0)).rel(false, true, false)),
            A(Alu().v(ADDv, 0, "xyzw", r(0), r(1))),
        });
    }
    // Constants addressed by the loop counter aL inside a loop.
    {
        Program p;
        p.exec({F(positionFetch()), A(Alu().v(MAXv, 2, "xyzw", c(0), c(0)))});
        uint32_t ls = p.loopStart(0);
        p.exec({A(Alu().v(MADv, 2, "xyzw", c(16), r(1), r(2)).rel(true, false, false)),
                A(Alu().v(DP4v, 3, "x", r(1), c(32)).rel(false, true, false)),
                A(Alu().s(ADDs, 3, "y", c(48)).rel(false, true, false))});
        p.loopEnd(0, ls);
        Gen::finishVs(p);
        g.add("constants", "aL relative", true, p, defaultSpec(true));
    }
    // Constant indices at the ends of the register file.
    g.ps("constants", "range ends", {A(Alu().v(ADDv, 0, "xyzw", c(0), c(255))), A(Alu().v(ADDv, 1, "xyzw", c(223), c(224)))});
}

void controlFlowGroups(Gen& g) {
    // Predicated instructions and execs.
    {
        Program p;
        p.exec({A(Alu().s(PRED_SETGTs, 1, "x", r(0, "x"))), A(Alu().v(ADDv, 0, "xyzw", r(0), c(1)).pred(true)),
                A(Alu().v(MULv, 0, "xyzw", r(0), c(2)).pred(false))});
        p.condExecPred({A(Alu().v(ADDv, 0, "xyz", r(0), c(3)).pred(true))}, true);
        p.condExecPred({A(Alu().v(ADDv, 0, "xyz", r(0), c(4)).pred(false))}, false);
        p.condExec({A(Alu().v(ADDv, 0, "w", r(0), c(5)))}, 7, true);
        p.condExec({A(Alu().v(ADDv, 0, "w", r(0), c(6)))}, 130, false, false, true);
        Gen::finishPs(p);
        g.add("control-flow", "predication", false, p, defaultSpec(false));
    }
    // A conditional end: cexece then a plain end.
    {
        Program p;
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(1)))});
        p.alloc(2, 0);
        p.condExec({expo(0, 0)}, 3, true, true);
        p.condExec({A(Alu().v(MULv, 0, "xyzw", r(0), c(2))), expo(0, 0)}, 3, false, true, true);
        p.exec({expo(0, 1)}, true);
        g.add("control-flow", "conditional end", false, p, defaultSpec(false));
    }
    {
        Program p;
        p.exec({A(Alu().s(PRED_SETEs, 1, "x", r(0, "x")))});
        p.alloc(2, 0);
        p.condExecPred({expo(0, 0)}, true, true);
        p.exec({expo(0, 1)}, true);
        g.add("control-flow", "predicated end", false, p, defaultSpec(false));
    }
    // Forward jumps (bool and predicate), nested: structured if blocks.
    {
        Program p;
        p.exec({A(Alu().s(PRED_SETNEs, 4, "x", r(1, "y")))});
        uint32_t j1 = p.jump(0, false, false, 5, true);
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(1)))});
        uint32_t j2 = p.jump(0, false, true, 0, false);
        p.exec({A(Alu().v(MULv, 0, "xyzw", r(0), c(2)))});
        p.patchTarget(j2, p.label());
        p.exec({A(Alu().v(ADDv, 0, "x", r(0), c(3)))});
        p.patchTarget(j1, p.label());
        p.nop();
        Gen::finishPs(p);
        g.add("control-flow", "nested forward jumps", false, p, defaultSpec(false));
    }
    // Jumps that do not nest, a backward jump, an unconditional jump: general control flow.
    {
        Program p;
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(1)))});
        uint32_t j1 = p.jump(0, false, false, 1, true);
        uint32_t j2 = p.jump(0, false, false, 2, false);
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(2)))});
        p.patchTarget(j1, p.label());
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(3)))});
        p.patchTarget(j2, p.label());
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(4)))});
        Gen::finishPs(p);
        g.add("control-flow", "overlapping jumps", false, p, defaultSpec(false));
    }
    {
        Program p;
        uint32_t top = p.label();
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(1))), A(Alu().s(PRED_SETGTs, 1, "x", r(0, "x")))});
        p.jump(top, false, true, 0, false);  // loop back while the predicate is false
        uint32_t skip = p.jump(0, true, false, 0, false);
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(9)))});
        p.patchTarget(skip, p.label());
        Gen::finishPs(p);
        g.add("control-flow", "backward and unconditional jumps", false, p, defaultSpec(false));
    }
    // Calls and returns.
    {
        Program p;
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(1)))});
        uint32_t c1 = p.call(0, false, false, 4, true);
        uint32_t c2 = p.call(0, true, false, 0, false);
        uint32_t c3 = p.call(0, false, true, 0, true);
        Gen::finishPs(p);
        uint32_t fn = p.label();
        p.exec({A(Alu().v(MULv, 0, "xyzw", r(0), c(2)))});
        p.ret();
        p.patchTarget(c1, fn);
        p.patchTarget(c2, fn);
        p.patchTarget(c3, fn);
        g.add("control-flow", "calls", false, p, defaultSpec(false));
    }
    // Loops: nested, with aL-relative temporaries and constants, a predicated break, repeat.
    for (int variant = 0; variant < 4; variant++) {
        Program p;
        p.exec({A(Alu().v(MAXv, 1, "xyzw", c(0), c(0)))});
        uint32_t outer = p.loopStart(1);
        p.exec({A(Alu().v(ADDv, 1, "xyzw", r(1), c(10)).rel(false, true, false))});
        uint32_t inner = p.loopStart(2, variant == 3);
        p.exec({A(Alu().v(MADv, 2, "xyzw", r(4, "xyzw", false, false, true), c(20), r(1)).rel(false, true, false)),
                A(Alu().v(ADDv, 4, "xyzw", r(2), r(5)).vrel()),
                A(Alu().s(PRED_SETGTEs, 3, "x", r(2, "x")))});
        p.loopEnd(2, inner, variant >= 1, variant == 2);
        if (variant == 1) {
            // A forward jump inside the outer loop.
            uint32_t j = p.jump(0, false, true, 0, true);
            p.exec({A(Alu().v(ADDv, 1, "x", r(1), c(3)))});
            p.patchTarget(j, p.label());
        }
        p.loopEnd(1, outer);
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(1), r(2)))});
        Gen::finishPs(p);
        g.add("control-flow", "nested loops " + std::to_string(variant), false, p, defaultSpec(false));
    }
    // A loop the structured writer cannot take (a jump out of it): general mode loops.
    {
        Program p;
        uint32_t ls = p.loopStart(0);
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(1))), A(Alu().s(PRED_SETGTs, 1, "x", r(0, "x")))});
        uint32_t out = p.jump(0, false, true, 0, true);
        p.loopEnd(0, ls);
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), c(2)))});
        p.patchTarget(out, p.label());
        Gen::finishPs(p);
        g.add("control-flow", "jump out of a loop", false, p, defaultSpec(false));
    }
    // Vertex shader loop with fetches inside (skinning-like).
    {
        Program p;
        p.exec({F(positionFetch()), A(Alu().v(MAXv, 2, "xyzw", c(0), c(0)))});
        uint32_t ls = p.loopStart(5);
        p.exec({A(Alu().v(DP4v, 3, "x", r(1), c(40)).rel(false, true, false)), A(Alu().v(DP4v, 3, "y", r(1), c(41)).rel(false, true, false)),
                A(Alu().v(ADDv, 2, "xyzw", r(2), r(3)))});
        p.loopEnd(5, ls);
        p.markVsFetchDone();
        Gen::finishVs(p);
        g.add("control-flow", "vertex loop", true, p, defaultSpec(true));
    }
}

void registerGroups(Gen& g) {
    // Temporaries addressed by aL (sources and destinations), a fetch into a relative register.
    {
        Program p;
        uint32_t ls = p.loopStart(0);
        TFetch t;
        t.dst = 8;
        t.dstRel = true;
        t.src = 0;
        t.srcRel = true;
        p.exec({A(Alu().v(ADDv, 10, "xyzw", r(0, "xyzw", false, false, true), c(1)).vrel()),
                A(Alu().s(ADDs, 12, "x", r(1, "xyzw", false, true, true)).srel()), F(t)});
        p.loopEnd(0, ls);
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(10), r(63)))});
        Gen::finishPs(p);
        g.add("registers", "aL relative temporaries", false, p, defaultSpec(false));
    }
    // High register numbers without relative addressing.
    g.ps("registers", "high registers", {A(Alu().v(ADDv, 40, "xyzw", r(0), c(1))), A(Alu().v(ADDv, 63, "xyzw", r(40), r(1))),
                                         A(Alu().v(ADDv, 0, "xyzw", r(63), r(40)))});
}

void vertexFetchGroups(Gen& g) {
    // Binding mode: several elements, full and mini fetches, swizzles with 0 / 1 / keep,
    // rounded and relative index, predicated.
    {
        Program p;
        VFetch pos = positionFetch();
        VFetch normal = pos;
        normal.dst = 2;
        normal.mini = true;
        normal.offset = 3;
        normal.dstSwizzle = fetchDstSwizzle("xyz0");
        VFetch uv = pos;
        uv.dst = 3;
        uv.mini = true;
        uv.offset = 6;
        uv.dstSwizzle = fetchDstSwizzle("xy01");
        VFetch color = pos;
        color.dst = 4;
        color.src = 6;
        color.srcComponent = 2;
        color.rounded = true;
        color.fetchConstant = 94;
        color.dstSwizzle = fetchDstSwizzle("zyxw");
        VFetch weights = color;
        weights.mini = true;
        weights.dst = 5;
        weights.dstSwizzle = fetchDstSwizzle("x_z_");
        weights.predicated = true;
        weights.predCondition = true;
        VFetch indices = color;
        indices.dst = 7;
        indices.srcRel = true;
        indices.src = 0;
        p.exec({F(pos), F(normal), F(uv), A(Alu().v(ADDv, 6, "z", r(0), c(1))), A(Alu().s(PRED_SETNEs, 8, "x", r(3, "x"))), F(color)});
        p.exec({F(weights), F(indices), A(Alu().v(ADDv, 2, "xyzw", r(2), r(5))), A(Alu().v(ADDv, 3, "xyzw", r(3), r(7)))});
        Gen::finishVs(p);
        g.add("vertex-fetch", "declaration elements", true, p, defaultSpec(true),
              {{POSITION, 0}, {NORMAL, 0}, {TEXCOORD, 0}, {COLOR, 0}, {BLENDWEIGHT, 0}, {BLENDINDICES, 0}});
    }
    // Every usage the D3D declaration can carry.
    {
        Program p;
        std::vector<Op> ops;
        std::vector<Element> elements;
        uint32_t usages[] = {POSITION, BLENDWEIGHT, BLENDINDICES, NORMAL, PSIZE, TEXCOORD, TANGENT, BINORMAL, COLOR, FOG};
        uint32_t dst = 1;
        for (uint32_t u : usages) {
            VFetch f = positionFetch(dst);
            f.mini = dst > 1;
            f.offset = int32_t(dst);
            ops.push_back(F(f));
            elements.push_back({u, 0});
            dst = dst == 1 ? 6 : dst + 1;
        }
        ops.push_back(A(Alu().v(ADDv, 2, "xyzw", r(6), r(7))));
        p.exec(ops);
        Gen::finishVs(p);
        g.add("vertex-fetch", "all usages", true, p, defaultSpec(true), elements);
    }
    // Instruction mode (bare microcode with patched fetches): every vertex format with every
    // number format, exponent adjustment, negative offsets, several fetch constants.
    const uint32_t formats[] = {F_8_8_8_8, F_2_10_10_10, F_10_11_11, F_11_11_10, F_16_16, F_16_16_16_16, F_16_16_FLOAT,
                                F_16_16_16_16_FLOAT, F_32, F_32_32, F_32_32_32_32, F_32_FLOAT, F_32_32_FLOAT,
                                F_32_32_32_32_FLOAT, F_32_32_32_FLOAT};
    for (uint32_t format : formats) {
        Program p;
        std::vector<Op> ops = {F(positionFetch())};
        for (int nf = 0; nf < 5; nf++) {
            VFetch f = positionFetch(uint32_t(2 + nf));
            f.format = format;
            f.mini = true;
            f.offset = nf - 2;
            f.isSigned = nf == 1 || nf == 2 || nf == 4;
            f.noZero = nf == 2;
            f.integer = nf >= 3;
            f.expAdjust = nf == 0 ? -3 : (nf == 4 ? 5 : 0);
            f.dstSwizzle = fetchDstSwizzle(nf == 1 ? "wzyx" : "xyzw");
            ops.push_back(F(f));
        }
        VFetch other = positionFetch(7);
        other.fetchConstant = 3 * 7 + 1;
        other.format = format;
        other.stride = 17;
        other.offset = 100;
        ops.push_back(F(other));
        p.exec(ops);
        Gen::finishVs(p);
        g.addRaw("vertex-fetch-raw", "format " + std::to_string(format), true, p);
    }
}

void textureFetchGroups(Gen& g) {
    struct Mode {
        const char* name;
        std::function<void(TFetch&)> apply;
    };
    std::vector<Mode> modes = {
        {"implicit", [](TFetch&) {}},
        {"bias", [](TFetch& t) { t.lodBias = -24; }},
        {"register lod", [](TFetch& t) { t.useRegLod = true; t.useCompLod = false; }},
        {"register lod plus computed", [](TFetch& t) { t.useRegLod = true; t.lodBias = 8; }},
        {"no lod", [](TFetch& t) { t.useCompLod = false; }},
        {"no lod bias", [](TFetch& t) { t.useCompLod = false; t.lodBias = 15; }},
        {"gradients", [](TFetch& t) { t.useRegGradients = true; }},
        {"gradients bias", [](TFetch& t) { t.useRegGradients = true; t.lodBias = -10; }},
        {"offsets", [](TFetch& t) { t.offsetX = 1; t.offsetY = -2; t.offsetZ = 3; }},
        {"unnormalised", [](TFetch& t) { t.denorm = true; }},
        {"unnormalised offsets", [](TFetch& t) { t.denorm = true; t.offsetX = -16; t.offsetY = 15; }},
        {"filters", [](TFetch& t) { t.magFilter = 0; t.minFilter = 1; t.mipFilter = 2; t.anisoFilter = 3; t.volMagFilter = 1; t.volMinFilter = 0; }},
        {"predicated", [](TFetch& t) { t.predicated = true; t.predCondition = true; t.dstSwizzle = fetchDstSwizzle("xy1_"); }},
        {"swizzles", [](TFetch& t) { t.srcSwizzle = fetchSrcSwizzle("wzx"); t.dstSwizzle = fetchDstSwizzle("01wz"); }},
    };
    const Dim dims[] = {Dim::D1, Dim::D2, Dim::D3, Dim::Cube};
    const char* dimNames[] = {"1D", "2D", "3D", "cube"};
    for (int d = 0; d < 4; d++) {
        for (const auto& mode : modes) {
            for (int stage = 0; stage < 2; stage++) {
                bool vertex = stage == 0;
                TFetch t;
                t.dim = dims[d];
                t.slot = uint32_t(vertex ? 16 + d : d * 3);
                t.dst = vertex ? 2 : 0;
                t.src = vertex ? 1 : 0;
                mode.apply(t);
                std::vector<Op> body;
                body.push_back(A(Alu().s(PRED_SETNEs, 9, "x", r(t.src, "x"))));
                TFetch lod;
                lod.op = Fop::SetTexLOD;
                lod.src = 3;
                lod.srcSwizzle = fetchSrcSwizzle("y");
                TFetch gh;
                gh.op = Fop::SetGradientsH;
                gh.src = 4;
                TFetch gv;
                gv.op = Fop::SetGradientsV;
                gv.src = 5;
                body.push_back(F(lod));
                body.push_back(F(gh));
                body.push_back(F(gv));
                body.push_back(F(t));
                std::string name = std::string(vertex ? "vs " : "ps ") + dimNames[d] + " " + mode.name;
                if (vertex) g.vs("texture-fetch", name, body);
                else g.ps("texture-fetch", name, body);
            }
        }
        // The other texture operations.
        for (Fop op : {Fop::GetCompTexLOD, Fop::GetWeights, Fop::GetBCF, Fop::GetGradients}) {
            for (int stage = 0; stage < 2; stage++) {
                bool vertex = stage == 0;
                TFetch t;
                t.op = op;
                t.dim = dims[d];
                t.slot = uint32_t(vertex ? 20 + d : 4 + d);
                t.dst = vertex ? 2 : 0;
                t.src = vertex ? 1 : 0;
                t.denorm = op == Fop::GetWeights && d == 1;
                const char* opName = op == Fop::GetCompTexLOD ? "getCompTexLOD" : op == Fop::GetWeights ? "getWeights"
                                     : op == Fop::GetBCF                                          ? "getBCF"
                                                                                                  : "getGradients";
                std::string name = std::string(vertex ? "vs " : "ps ") + opName + " " + dimNames[d];
                if (vertex) g.vs("texture-ops", name, {F(t)});
                else g.ps("texture-ops", name, {F(t)});
            }
        }
    }
    // The compiler's cube map sequence: cube, rcp of |2 ma|, mad to [1, 2], tfetchCube.
    for (int stage = 0; stage < 2; stage++) {
        bool vertex = stage == 0;
        uint32_t base = vertex ? 2 : 0;
        Src ma = r(base + 6, "xyzw", false, true);
        ma.swizzle = scalarSwizzle('z', 'z');
        TFetch t;
        t.dim = Dim::Cube;
        t.slot = 9;
        t.src = base + 6;
        t.srcSwizzle = fetchSrcSwizzle("yxw");
        t.dst = base;
        std::vector<Op> body = {
            A(Alu().v(CUBEv, base + 6, "xyzw", r(base, "zzxy"), r(base, "yxzz"))),
            A(Alu().s(RECIP_IEEE, base + 7, "z", ma)),
            A(Alu().v(MADv, base + 6, "xy", r(base + 6, "xyzw"), r(base + 7, "zzzz"), c(0, "wwww"))),
            F(t),
        };
        if (vertex) g.vs("texture-fetch", "vs cube sequence", body);
        else g.ps("texture-fetch", "ps cube sequence", body);
    }
    // A sample inside a loop and inside a branch (implicit derivatives in flow control).
    {
        Program p;
        uint32_t ls = p.loopStart(0);
        TFetch t;
        t.dst = 1;
        t.src = 0;
        p.exec({F(t), A(Alu().v(ADDv, 0, "xyzw", r(0), r(1)))});
        p.loopEnd(0, ls);
        uint32_t j = p.jump(0, false, false, 9, true);
        p.exec({F(t)});
        p.patchTarget(j, p.label());
        Gen::finishPs(p);
        g.add("texture-fetch", "samples in flow control", false, p, defaultSpec(false));
    }
}

void outputGroups(Gen& g) {
    // Pixel outputs: several render targets, depth, depth only, no output; the pixel position.
    for (int variant = 0; variant < 4; variant++) {
        Program p;
        p.exec({A(Alu().v(ADDv, 0, "xyzw", r(0), r(4)))});
        p.alloc(2, 0);
        std::vector<Op> exports;
        if (variant == 0) exports = {expo(0, 0), expo(1, 1), expo(2, 2), expo(3, 3)};
        if (variant == 1) exports = {expo(0, 0), expo(kODepth, 1, "x")};
        if (variant == 2) exports = {expo(kODepth, 1, "x")};
        if (variant == 3) exports = {A(Alu().v(ADDv, 1, "xyzw", r(1), r(0)))};
        p.exec(exports, true);
        ctest::Spec spec = defaultSpec(false);
        spec.paramGen = variant == 0;
        g.add("outputs", "pixel outputs " + std::to_string(variant), false, p, spec);
    }
    // Every interpolator: 16 texture coordinates and 2 colours in and out.
    {
        ctest::Spec spec = defaultSpec(false);
        spec.interpolators.clear();
        Program p;
        std::vector<Op> ops;
        for (uint32_t i = 0; i < 16; i++) {
            spec.interpolators.push_back({i < 14 ? TEXCOORD : COLOR, i < 14 ? i + 2 : i - 14, i});
            ops.push_back(A(Alu().v(ADDv, 20, "xyzw", r(20), r(i))));
        }
        spec.paramGen = true;
        ops.push_back(A(Alu().v(ADDv, 0, "xyzw", r(20), r(16))));
        p.exec(ops);
        Gen::finishPs(p);
        g.add("outputs", "sixteen interpolators", false, p, spec);
    }
    {
        ctest::Spec spec = defaultSpec(true);
        spec.interpolators.clear();
        Program p;
        p.exec({F(positionFetch())});
        p.alloc(1, 0);
        p.exec({expo(kOPos, 1), A(Alu().v(MAXv, kOMisc, "x", r(1), r(1)).exp(kOMisc))});
        p.alloc(2, 15);
        std::vector<Op> ops;
        for (uint32_t i = 0; i < 16; i++) {
            spec.interpolators.push_back({i < 15 ? TEXCOORD : COLOR, i < 15 ? i : 1, i});
            ops.push_back(expo(i, 1, i % 2 ? "xy" : "xyzw"));
        }
        p.exec(ops);
        // Memory export (not supported: dropped with a warning).
        p.alloc(3, 1);
        p.exec({expo(kExportAddress, 1), expo(kExportData, 1)}, true);
        g.add("outputs", "all vertex exports", true, p, spec);
    }
    // A vertex shader without interpolators, a pixel shader without them.
    {
        ctest::Spec spec = defaultSpec(true);
        spec.interpolators.clear();
        Program p;
        p.exec({F(positionFetch())});
        p.alloc(1, 0);
        p.exec({expo(kOPos, 1)}, true);
        g.add("outputs", "position only", true, p, spec);
        ctest::Spec ps = defaultSpec(false);
        ps.interpolators.clear();
        g.ps("outputs", "no interpolators", {A(Alu().v(MAXv, 0, "xyzw", c(0), c(0)))}, ps);
    }
}

// Random programs: random ALU and fetch instructions in random control flow.
void fuzzGroup(Gen& g, uint32_t count, uint32_t seed) {
    std::mt19937 rng(seed);
    auto rnd = [&](uint32_t n) { return uint32_t(rng() % n); };
    auto chance = [&](uint32_t percent) { return rnd(100) < percent; };
    const char* swz[] = {"xyzw", "wzyx", "x", "y", "z", "w", "xxyy", "zwxy", "yzwx", "wwzz"};
    const char* masks[] = {"x", "y", "z", "w", "xy", "zw", "xyz", "xyzw", "xw", "yz"};

    for (uint32_t n = 0; n < count; n++) {
        bool vertex = (n % 2) == 0;
        const uint32_t regs = 12;
        auto src = [&]() {
            if (chance(30)) {
                Src k = c(rnd(256), swz[rnd(10)], chance(30));
                return k;
            }
            return r(rnd(regs), swz[rnd(10)], chance(25), chance(15), chance(2));
        };
        auto alu = [&]() {
            Alu a;
            uint32_t vop = rnd(30);
            uint32_t sop = rnd(51);
            if (sop == 41) sop = RETAIN_PREV;
            if (sop >= MUL_CONST_0 && sop <= SUB_CONST_1) {
                a.sc(sop, rnd(regs), masks[rnd(10)], rnd(256), "xyzw"[rnd(4)], rnd(regs) & ~0u, "xyzw"[rnd(4)]);
                a.vop = vop;
                a.vdst = rnd(regs);
                a.vmask = chance(80) ? mask(masks[rnd(10)]) : 0;
                a.src[0] = src();
                a.src[1] = src();
            } else {
                a.v(vop, rnd(regs), masks[rnd(10)], src(), src(), src());
                if (chance(20)) a.vmask = 0;
                if (chance(70)) {
                    a.sop = sop;
                    a.sdst = rnd(regs);
                    a.smask = chance(85) ? mask(masks[rnd(10)]) : 0;
                }
            }
            if (chance(15)) a.sat(chance(50), chance(50));
            if (chance(10)) a.absConst();
            if (chance(10)) a.pred(chance(50));
            if (chance(10)) a.rel(chance(50), chance(50), chance(50));
            if (chance(2)) a.vrel();
            if (chance(2)) a.srel();
            return A(a);
        };
        auto fetch = [&]() {
            if (vertex && chance(40)) {
                VFetch f = positionFetch(rnd(regs));
                f.src = rnd(regs);
                f.srcComponent = rnd(4);
                f.dstSwizzle = fetchDstSwizzle(chance(50) ? "xyzw" : "xy01");
                f.rounded = chance(50);
                return F(f);
            }
            TFetch t;
            uint32_t ops[] = {1, 1, 1, 17, 18, 19, 24, 25, 26, 16};
            t.op = Fop(ops[rnd(10)]);
            t.dim = Dim(rnd(4));
            t.slot = rnd(32);
            t.dst = rnd(regs);
            t.src = rnd(regs);
            t.srcSwizzle = rnd(64);
            t.dstSwizzle = fetchDstSwizzle(chance(70) ? "xyzw" : "x_01");
            t.useCompLod = chance(70);
            t.useRegLod = chance(20);
            t.useRegGradients = chance(10);
            t.lodBias = int32_t(rnd(128)) - 64;
            t.offsetX = int32_t(rnd(32)) - 16;
            t.offsetY = int32_t(rnd(32)) - 16;
            t.denorm = chance(10);
            t.magFilter = rnd(4);
            t.predicated = chance(10);
            return F(t);
        };
        auto block = [&]() {
            std::vector<Op> ops;
            uint32_t k = 1 + rnd(8);
            for (uint32_t i = 0; i < k; i++) ops.push_back(chance(25) ? fetch() : alu());
            return ops;
        };

        Program p;
        if (vertex) p.exec({F(positionFetch())});
        // Random structure: execs, loops, forward jumps (maybe crossing), calls at the end.
        std::vector<std::pair<uint32_t, uint32_t>> openLoops;  // id, start
        std::vector<uint32_t> openJumps;
        uint32_t steps = 2 + rnd(8);
        std::vector<uint32_t> calls;
        for (uint32_t s = 0; s < steps; s++) {
            uint32_t what = rnd(10);
            if (what < 5) {
                p.exec(block());
            } else if (what == 5 && openLoops.size() < 3) {
                uint32_t id = rnd(32);
                openLoops.push_back({id, p.loopStart(id, chance(10))});
            } else if (what == 6 && !openLoops.empty()) {
                p.exec(block());
                p.loopEnd(openLoops.back().first, openLoops.back().second, chance(30), chance(50));
                openLoops.pop_back();
            } else if (what == 7) {
                openJumps.push_back(p.jump(0, false, chance(50), rnd(256), chance(50)));
            } else if (what == 8 && !openJumps.empty()) {
                // Close a random open jump (not always the innermost: crossing jumps).
                size_t which = chance(70) ? openJumps.size() - 1 : rnd(uint32_t(openJumps.size()));
                p.exec(block());
                p.patchTarget(openJumps[which], p.label());
                openJumps.erase(openJumps.begin() + std::ptrdiff_t(which));
            } else if (what == 9) {
                if (chance(50)) p.condExec(block(), rnd(256), chance(50));
                else p.condExecPred(block(), chance(50));
                if (chance(20)) calls.push_back(p.call(0, chance(30), chance(30), rnd(256), chance(50)));
            }
        }
        p.exec(block());
        while (!openLoops.empty()) {
            p.loopEnd(openLoops.back().first, openLoops.back().second);
            openLoops.pop_back();
        }
        for (uint32_t j : openJumps) p.patchTarget(j, p.label());
        if (vertex) Gen::finishVs(p);
        else Gen::finishPs(p, rnd(regs));
        if (!calls.empty()) {
            uint32_t fn = p.label();
            p.exec(block());
            p.ret();
            for (uint32_t c : calls) p.patchTarget(c, fn);
        }
        std::vector<Element> elements;
        for (uint32_t i = 0; i < 16; i++) elements.push_back({TEXCOORD, i});
        elements[0] = {POSITION, 0};
        g.add("fuzz", "random " + std::to_string(n), vertex, p, defaultSpec(vertex), elements);
    }
}

}  // namespace

std::vector<uint32_t> findVfetchAddresses(const std::vector<uint32_t>& ucode) {
    std::vector<uint32_t> addresses;
    uint32_t cfEnd = uint32_t(ucode.size() / 3);
    for (uint32_t t = 0; t < cfEnd; t++) {
        uint64_t a = ucode[t * 3] | (uint64_t(ucode[t * 3 + 1] & 0xFFFF) << 32);
        uint64_t b = (ucode[t * 3 + 1] >> 16) | (uint64_t(ucode[t * 3 + 2]) << 16);
        for (uint64_t cf : {a, b}) {
            uint32_t op = uint32_t(cf >> 44) & 0xF;
            bool isExec = (op >= 1 && op <= 6) || op == 13 || op == 14;
            if (!isExec) continue;
            uint32_t address = uint32_t(cf & 0xFFF), count = uint32_t(cf >> 12) & 7, sequence = uint32_t(cf >> 16) & 0xFFF;
            if (count) cfEnd = std::min(cfEnd, address);
            for (uint32_t i = 0; i < count; i++) {
                if (!((sequence >> (i * 2)) & 1)) continue;
                uint32_t w0 = ucode[(address + i) * 3];
                if ((w0 & 0x1F) == 0) addresses.push_back(address + i);
            }
        }
    }
    return addresses;
}

std::vector<CorpusShader> buildCorpus(uint32_t fuzzCount, uint32_t seed) {
    std::vector<CorpusShader> out;
    Gen g(out);
    aluGroups(g);
    constantGroups(g);
    controlFlowGroups(g);
    registerGroups(g);
    vertexFetchGroups(g);
    textureFetchGroups(g);
    outputGroups(g);
    fuzzGroup(g, fuzzCount, seed);
    return out;
}
