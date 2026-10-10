#include "xenos_interp.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace xinterp {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

// ------------------------------------------------------------------------------------------
// Error-bounded arithmetic.

// Precision a conforming GPU is allowed for the operations that are not exactly rounded
// (relative unless said otherwise). Generous next to IEEE, tight next to any real bug.
constexpr float kRcpRel = 0x1p-21f;
constexpr float kRsqRel = 0x1p-20f;
constexpr float kSqrtRel = 0x1p-21f;
constexpr float kExpRel = 0x1p-18f;
constexpr float kLogAbs = 0x1p-18f;
constexpr float kSinAbs = 0x1p-17f;   // per unit of |argument| / pi beyond the first
constexpr float kFilterWeight = 0x1p-8f; // 8-bit sub-texel precision (lavapipe, most GPUs)

float ulp(float x) {
    float a = std::fabs(x);
    if (!(a < kInf)) return 0.0f;
    return std::max(a, FLT_MIN) * 0x1p-23f;
}

Num unknown() {
    Num n;
    n.unknown = true;
    return n;
}

Num exact(float v) { return Num{v, 0.0f, false}; }

bool special(float v) { return !std::isfinite(v); }

// A result r of inputs with error `e`: rounding error added unless `exactValue` matches.
Num result(float r, double exactValue, float e) {
    if (special(r)) return e > 0.0f ? unknown() : exact(r);
    float rounding = (double(r) == exactValue) ? 0.0f : ulp(r);
    float bound = e + rounding;
    // Results in the subnormal range: a GPU may flush them.
    if (std::fabs(r) < FLT_MIN && r != 0.0f) bound += FLT_MIN;
    return Num{r, bound, false};
}

Num neg(Num a) {
    a.v = -a.v;
    return a;
}

Num absn(Num a) {
    a.v = std::fabs(a.v);
    return a;
}

Num add(Num a, Num b) {
    if (a.unknown || b.unknown) return unknown();
    float r = a.v + b.v;
    return result(r, double(a.v) + double(b.v), a.e + b.e);
}

// Direct3D 9 multiplication: 0 (or a denormal, flushed) times anything is +0.
bool isZero(float v) { return v == 0.0f || std::fabs(v) < FLT_MIN; }

Num mul(Num a, Num b) {
    if (a.unknown || b.unknown) return unknown();
    // A factor that is zero only within its error, times infinity or NaN: either branch.
    if ((a.e > 0.0f && std::fabs(a.v) <= a.e && special(b.v)) || (b.e > 0.0f && std::fabs(b.v) <= b.e && special(a.v)))
        return unknown();
    // The other factor's magnitude times this one's error (terms only where there is error).
    float e = (b.e > 0.0f ? std::fabs(a.v) * b.e : 0.0f) + (a.e > 0.0f ? std::fabs(b.v) * a.e : 0.0f) + a.e * b.e;
    if (isZero(a.v) || isZero(b.v)) {
        if (special(e)) return unknown();
        return Num{0.0f, e, false};
    }
    float r = a.v * b.v;
    if (special(e)) return unknown();
    return result(r, double(a.v) * double(b.v), e);
}

// a * b + c with the product rounded or not (a fused multiply-add is allowed).
Num mad(Num a, Num b, Num c) {
    Num p = mul(a, b);
    if (p.unknown || c.unknown) return unknown();
    double exactSum = double(p.v) + double(c.v);
    if (!isZero(a.v) && !isZero(b.v)) {
        // The exact product, for a fused result.
        double exactProduct = double(a.v) * double(b.v);
        exactSum = exactProduct + double(c.v);
        if (double(p.v) != exactProduct) p.e += ulp(p.v);  // fused or not: the product's rounding may or may not happen
    }
    float r = p.v + c.v;
    return result(r, exactSum, p.e + c.e);
}

enum class Dec { False, True, Ambiguous };

// a OP b, IEEE semantics on the values; ambiguous when the errors overlap the boundary.
enum class Cmp { Eq, Ne, Gt, Ge, Lt };

Dec compare(Num a, Num b, Cmp op) {
    if (a.unknown || b.unknown) return Dec::Ambiguous;
    bool r;
    switch (op) {
        case Cmp::Eq: r = a.v == b.v; break;
        case Cmp::Ne: r = a.v != b.v; break;
        case Cmp::Gt: r = std::isgreater(a.v, b.v); break;
        case Cmp::Ge: r = std::isgreaterequal(a.v, b.v); break;
        default: r = std::isless(a.v, b.v); break;
    }
    float e = a.e + b.e;
    if (e > 0.0f) {
        if (std::isnan(a.v) || std::isnan(b.v)) return Dec::Ambiguous;
        float d = std::fabs(a.v - b.v);
        if (!(d > e)) return Dec::Ambiguous;
    }
    return r ? Dec::True : Dec::False;
}

Num fromDec(Dec d) { return d == Dec::Ambiguous ? unknown() : exact(d == Dec::True ? 1.0f : 0.0f); }

// max / min as the comparisons Xenos does (a >= b ? a : b, a < b ? a : b).
Num pick(Num a, Num b, Dec takeA) {
    if (takeA == Dec::Ambiguous) {
        if (a.unknown || b.unknown || std::isnan(a.v) || std::isnan(b.v) || special(a.v) || special(b.v)) return unknown();
        Num r = a;
        r.e = std::max(a.e, b.e) + std::fabs(a.v - b.v);
        return r;
    }
    return takeA == Dec::True ? a : b;
}

// Whether x lies within its error of an integer (floor, trunc, frac could differ).
bool nearInteger(Num x, float offset = 0.0f) {
    if (x.e == 0.0f) return false;
    float v = x.v + offset;
    float n = std::nearbyint(v);
    return std::fabs(v - n) <= x.e;
}

Num floorn(Num x) {
    if (x.unknown) return unknown();
    if (nearInteger(x)) return unknown();
    return Num{std::floor(x.v), 0.0f, false};
}

Num truncn(Num x) {
    if (x.unknown) return unknown();
    if (nearInteger(x)) return unknown();
    return Num{std::trunc(x.v), 0.0f, false};
}

Num frcn(Num x) {
    if (x.unknown) return unknown();
    if (nearInteger(x)) return unknown();
    float f = std::floor(x.v);
    return result(x.v - f, double(x.v) - double(f), x.e);
}

Num saturate(Num x) {
    if (x.unknown) return x;
    // NaN saturates to 0 (min(max(x, 0), 1) with the operand order GPUs use).
    if (std::isnan(x.v)) return x.e > 0 ? unknown() : exact(0.0f);
    x.v = std::min(std::max(x.v, 0.0f), 1.0f);
    return x;
}

// Transcendentals: f(a) with derivative bound `slope` (|f'| over the error interval) and
// the GPU's own error `own`.
Num transcendental(Num a, float r, float slope, float own) {
    if (a.unknown) return unknown();
    if (special(r) || std::isnan(a.v)) return (a.e > 0.0f) ? unknown() : exact(r);
    float e = slope * a.e + own;
    if (special(e)) return unknown();
    return Num{r, e, false};
}

// ------------------------------------------------------------------------------------------
// Decoding (layouts from the SDK's ucode.h; the same as the test assembler).

uint32_t bits(uint32_t v, uint32_t shift, uint32_t count) { return (v >> shift) & ((count == 32) ? ~0u : ((1u << count) - 1u)); }
int32_t sbits(uint32_t v, uint32_t shift, uint32_t count) {
    uint32_t u = bits(v, shift, count);
    return int32_t(u << (32 - count)) >> (32 - count);
}

struct Cf {
    uint32_t op = 0;
    uint64_t raw = 0;
    uint32_t field(uint32_t shift, uint32_t count) const { return uint32_t((raw >> shift) & ((uint64_t(1) << count) - 1)); }
};

Cf decodeCf(const std::vector<uint32_t>& ucode, uint32_t index) {
    const uint32_t* t = &ucode[size_t(index / 2) * 3];
    uint64_t raw;
    if (index & 1)
        raw = uint64_t(t[1] >> 16) | (uint64_t(t[2]) << 16);
    else
        raw = uint64_t(t[0]) | (uint64_t(t[1] & 0xFFFF) << 32);
    Cf c;
    c.raw = raw;
    c.op = uint32_t(raw >> 44) & 0xF;
    return c;
}

enum CfOp : uint32_t {
    kNop = 0, kExec, kExecEnd, kCondExec, kCondExecEnd, kCondExecPred, kCondExecPredEnd, kLoopStart, kLoopEnd, kCondCall,
    kReturn, kCondJmp, kAlloc, kCondExecPredClean, kCondExecPredCleanEnd, kMarkVsFetchDone
};

bool isExec(uint32_t op) { return (op >= kExec && op <= kCondExecPredEnd) || op == kCondExecPredClean || op == kCondExecPredCleanEnd; }
bool isEnd(uint32_t op) { return op == kExecEnd || op == kCondExecEnd || op == kCondExecPredEnd || op == kCondExecPredCleanEnd; }

// ------------------------------------------------------------------------------------------

std::string fmtNum(const Num& n) {
    char b[48];
    if (n.unknown) return "?";
    if (n.e > 0) std::snprintf(b, sizeof(b), "%.7g~%.2g", double(n.v), double(n.e));
    else std::snprintf(b, sizeof(b), "%.7g", double(n.v));
    return b;
}

std::string fmtVec(const Vec& v) { return "(" + fmtNum(v[0]) + ", " + fmtNum(v[1]) + ", " + fmtNum(v[2]) + ", " + fmtNum(v[3]) + ")"; }

class Machine {
public:
    Machine(const Inputs& in, Outputs& out) : in_(in), out_(out) {}

    void run();
    void copyRegisters() { out_.registers = r_; }

private:
    const Inputs& in_;
    Outputs& out_;
    std::array<Vec, 64> r_{};
    Num ps_{};
    bool p0_ = false;
    bool p0Fragile_ = false;
    int32_t a0_ = 0;
    bool a0Fragile_ = false;
    // Loop stack (as the sequencer: up to 4).
    uint32_t loopDepth_ = 0;
    std::array<uint32_t, 4> loopIt_{}, loopConst_{};
    std::array<uint32_t, 4> callStack_{};
    uint32_t callDepth_ = 0;
    // Texture fetch state.
    Num texLod_{};
    // Vertex fetch state.
    bool haveFullFetch_ = false;
    uint32_t lastFullW0_ = 0, lastFullW2_ = 0;
    int64_t vfIndex_ = 0;
    bool vfIndexFragile_ = false;

    void trace(const std::string& line) {
        if (in_.trace) out_.trace += line + "\n";
    }
    void fragile(const char* why) {
        if (!out_.fragile) out_.fragileWhy = why;
        out_.fragile = true;
    }
    bool predicate(bool condition) {
        if (p0Fragile_) fragile("predicate within the error");
        return p0_ == condition;
    }
    bool boolConst(uint32_t index) const { return (in_.bools[index >> 5] >> (index & 31)) & 1; }

    int32_t loopAddress() const {
        if (loopDepth_ == 0) return 0;
        uint32_t c = loopConst_[(loopDepth_ - 1) & 3];
        int32_t start = int32_t(bits(c, 8, 8)), step = sbits(c, 16, 8);
        int32_t aL = int32_t(loopIt_[(loopDepth_ - 1) & 3]) * step + start;
        // The real range of aL (IPR2015-00325 sequencer specification, as the SDK notes).
        return std::clamp(aL, -256, 256);
    }
    uint32_t tempIndex(uint32_t index, bool relative) const {
        return uint32_t(int32_t(index) + (relative ? loopAddress() : 0)) & 63u;
    }
    Vec constant(uint32_t index, bool addressed, bool a0Relative) {
        int32_t i = int32_t(index);
        if (addressed) {
            if (a0Relative) {
                if (a0Fragile_) fragile("address register within the error");
                i += a0_;
            } else {
                i += loopAddress();
            }
        }
        Vec v;
        if (i < 0 || i > 255) {
            for (auto& c : v) c = exact(0.0f);
            return v;
        }
        for (int k = 0; k < 4; k++) v[k] = exact(in_.constants[size_t(i)][size_t(k)]);
        return v;
    }

    void exec(uint32_t address, uint32_t count, uint32_t sequence);
    void alu(const uint32_t* w);
    void vfetch(const uint32_t* w, uint32_t address);
    void tfetch(const uint32_t* w);
    void storeFetch(uint32_t dst, bool relative, uint32_t dstSwizzle, const Vec& value);
    Vec sample(const Texture& t, uint32_t dim, const std::array<Num, 3>& coords, bool linear, bool& fragileOut);
};

void Machine::run() {
    // Starting state: as the translated shaders initialise it (r0.x = vertex index in vertex
    // shaders; interpolators in pixel shaders; everything else 0).
    for (size_t i = 0; i < 64; i++)
        for (size_t k = 0; k < 4; k++) r_[i][k] = exact(in_.pixel ? in_.initialRegisters[i][k] : 0.0f);
    if (!in_.pixel) r_[0][0] = exact(float(in_.vertexIndex));

    const auto& ucode = in_.ucode;
    if (ucode.empty() || ucode.size() % 3) {
        out_.error = true;
        out_.message = "microcode size";
        return;
    }
    // The control flow program ends where the first exec's instructions start.
    uint32_t cfCount = uint32_t(ucode.size() / 3) * 2;
    for (uint32_t i = 0; i < cfCount; i++) {
        Cf c = decodeCf(ucode, i);
        if (isExec(c.op) && c.field(12, 3) != 0) cfCount = std::min(cfCount, c.field(0, 12) * 2);
    }

    uint32_t pc = 0;
    while (true) {
        if (++out_.steps > in_.maxSteps) {
            out_.timeout = true;
            return;
        }
        if (pc >= cfCount) return;  // ran off the end (the translated code ends there too)
        Cf c = decodeCf(ucode, pc);
        uint32_t next = pc + 1;
        if (in_.trace) {
            char b[96];
            std::snprintf(b, sizeof(b), "cf %u op %u (p0 %d, a0 %d, aL %d)", pc, c.op, int(p0_), a0_, loopAddress());
            trace(b);
        }
        switch (c.op) {
            case kExec:
            case kExecEnd:
            case kCondExec:
            case kCondExecEnd:
            case kCondExecPred:
            case kCondExecPredEnd:
            case kCondExecPredClean:
            case kCondExecPredCleanEnd: {
                bool run = true;
                if (c.op == kCondExec || c.op == kCondExecEnd || c.op == kCondExecPredClean || c.op == kCondExecPredCleanEnd)
                    run = boolConst(c.field(34, 8)) == (c.field(42, 1) != 0);
                else if (c.op == kCondExecPred || c.op == kCondExecPredEnd)
                    run = predicate(c.field(42, 1) != 0);
                if (run) {
                    exec(c.field(0, 12), c.field(12, 3), c.field(16, 12));
                    if (out_.error) return;
                    if (isEnd(c.op)) return;
                }
                break;
            }
            case kLoopStart: {
                uint32_t id = c.field(16, 5);
                bool repeat = c.field(13, 1) != 0;
                if (loopDepth_ >= 4) {
                    out_.error = true;
                    out_.message = "loop nesting deeper than 4";
                    return;
                }
                uint32_t slot = loopDepth_;
                loopConst_[slot] = in_.loops[id];
                if (!repeat) loopIt_[slot] = 0;
                if (loopIt_[slot] >= bits(loopConst_[slot], 0, 8)) {
                    next = c.field(0, 13);
                } else {
                    loopDepth_++;
                }
                break;
            }
            case kLoopEnd: {
                if (loopDepth_ == 0) break;
                uint32_t slot = loopDepth_ - 1;
                uint32_t it = ++loopIt_[slot];
                bool breakOut = false;
                if (c.field(21, 1)) breakOut = predicate(c.field(42, 1) != 0);
                if (it < bits(loopConst_[slot], 0, 8) && !breakOut)
                    next = c.field(0, 13);
                else
                    loopDepth_--;
                break;
            }
            case kCondCall:
            case kCondJmp: {
                bool take = true;
                if (!c.field(13, 1)) {
                    if (c.field(14, 1))
                        take = predicate(c.field(42, 1) != 0);
                    else
                        take = boolConst(c.field(34, 8)) == (c.field(42, 1) != 0);
                }
                if (in_.trace) {
                    char b[96];
                    std::snprintf(b, sizeof(b), "  %s to %u: unconditional %u, predicated %u, bool %u = %d, condition %u -> %s",
                                  c.op == kCondCall ? "call" : "jump", c.field(0, 13), c.field(13, 1), c.field(14, 1),
                                  c.field(34, 8), int(boolConst(c.field(34, 8))), c.field(42, 1), take ? "taken" : "not taken");
                    trace(b);
                }
                if (take) {
                    if (c.op == kCondCall) {
                        if (callDepth_ >= 4) {
                            out_.error = true;
                            out_.message = "call nesting deeper than 4";
                            return;
                        }
                        callStack_[callDepth_++] = pc + 1;
                    }
                    next = c.field(0, 13);
                }
                break;
            }
            case kReturn:
                if (callDepth_) next = callStack_[--callDepth_];
                break;
            default:
                break;  // nop, alloc, mark_vs_fetch_done
        }
        pc = next;
    }
}

void Machine::exec(uint32_t address, uint32_t count, uint32_t sequence) {
    for (uint32_t i = 0; i < count; i++, sequence >>= 2) {
        if ((size_t(address) + i) * 3 + 3 > in_.ucode.size()) {
            out_.error = true;
            out_.message = "exec past the microcode";
            return;
        }
        const uint32_t* w = &in_.ucode[size_t(address + i) * 3];
        if (in_.trace) {
            char b[96];
            std::snprintf(b, sizeof(b), "  [%u] %s %08X %08X %08X", address + i, (sequence & 1) ? "fetch" : "alu", w[0], w[1], w[2]);
            trace(b);
        }
        if (sequence & 1) {
            bool isVertexFetch = bits(w[0], 0, 5) == 0;
            // Fetch predication: vertex fetch w1 bit 31 / w2 bit 31, texture fetch the same.
            bool predicated = bits(w[1], 31, 1) != 0;
            bool condition = bits(w[2], 31, 1) != 0;
            if (predicated && !predicate(condition)) continue;
            if (isVertexFetch) {
                if (in_.pixel) {
                    out_.error = true;
                    out_.message = "vertex fetch in a pixel shader";
                    return;
                }
                vfetch(w, address + i);
            } else {
                tfetch(w);
            }
        } else {
            bool predicated = bits(w[1], 28, 1) != 0;
            bool condition = bits(w[1], 27, 1) != 0;
            if (predicated && !predicate(condition)) continue;
            alu(w);
        }
        if (out_.error) return;
    }
}

// ------------------------------------------------------------------------------------------
// ALU.

uint32_t vectorOperandCount(uint32_t op) {
    switch (op) {
        case 8: case 9: case 10: case 18: case 19:  // frc trunc floor cube max4
            return 1;
        case 11: case 12: case 13: case 14: case 17:  // mad cndeq cndge cndgt dp2add
            return 3;
        default:
            return 2;
    }
}

// Scalar operand kinds: 0 none, 1 component a, 2 components a and b of the third source,
// 3 constant a and temporary b.
uint32_t scalarKind(uint32_t op) {
    switch (op) {
        case 0: case 2: case 4: case 5: case 6: case 23: case 24: case 25:  // adds muls muls_prev2 maxs mins maxas maxasf subs
            return 2;
        case 33: case 50:  // setp_clr retain_prev
            return 0;
        case 42: case 43: case 44: case 45: case 46: case 47:
            return 3;
        default:
            return 1;
    }
}

void Machine::alu(const uint32_t* w) {
    const uint32_t vdst = bits(w[0], 0, 6), sdst = bits(w[0], 8, 6);
    const bool vdstRel = bits(w[0], 6, 1), sdstRel = bits(w[0], 14, 1), absConst = bits(w[0], 7, 1);
    const bool isExport = bits(w[0], 15, 1);
    const uint32_t vmaskRaw = bits(w[0], 16, 4), smaskRaw = bits(w[0], 20, 4);
    const bool vsat = bits(w[0], 24, 1), ssat = bits(w[0], 25, 1);
    const uint32_t sop = bits(w[0], 26, 6);
    const uint32_t swz[4] = {0, bits(w[1], 16, 8), bits(w[1], 8, 8), bits(w[1], 0, 8)};
    const bool negs[4] = {false, bits(w[1], 26, 1) != 0, bits(w[1], 25, 1) != 0, bits(w[1], 24, 1) != 0};
    const bool a0Rel = bits(w[1], 29, 1), const1Rel = bits(w[1], 30, 1), const0Rel = bits(w[1], 31, 1);
    const uint32_t regs[4] = {0, bits(w[2], 16, 8), bits(w[2], 8, 8), bits(w[2], 0, 8)};
    const uint32_t vop = bits(w[2], 24, 5);
    const bool temps[4] = {false, bits(w[2], 31, 1) != 0, bits(w[2], 30, 1) != 0, bits(w[2], 29, 1) != 0};

    if (vop > 29 || sop > 50 || sop == 41) {
        out_.error = true;
        out_.message = "unknown ALU opcode";
        return;
    }

    auto addressed = [&](uint32_t i) {
        if (i == 1) return const0Rel;
        if (i == 2) return temps[1] ? const0Rel : const1Rel;
        return (temps[1] && temps[2]) ? const0Rel : const1Rel;
    };
    // An operand without swizzle, with absolute value; negation applied by the caller.
    auto storage = [&](uint32_t i, bool& absolute) -> Vec {
        if (temps[i]) {
            absolute = (regs[i] & 0x80) != 0;
            return r_[tempIndex(regs[i] & 0x3F, (regs[i] & 0x40) != 0)];
        }
        absolute = absConst;
        return constant(regs[i], addressed(i), a0Rel);
    };
    auto modifiers = [&](Num n, bool absolute, bool negate) {
        if (absolute) n = absn(n);
        if (negate) n = neg(n);
        return n;
    };

    const uint32_t vmask = isExport ? (vmaskRaw & ~smaskRaw) : vmaskRaw;
    const uint32_t smask = isExport ? (smaskRaw & ~vmaskRaw) : smaskRaw;
    const uint32_t const1Mask = isExport ? (vmaskRaw & smaskRaw) : 0;
    const uint32_t const0Mask = (isExport && sdstRel) ? (0xFu & ~(vmaskRaw | smaskRaw)) : 0;

    // Vector operation. State changes (predicate, address register) take effect at once, before
    // the scalar operation reads its operands (as the SDK's interpreter and SPIR-V translator do).
    Vec vr{};
    const bool changesState = (vop >= 20 && vop <= 27) || vop == 29;
    if (vmask || changesState) {
        Vec o[3];
        uint32_t n = vectorOperandCount(vop);
        for (uint32_t i = 1; i <= n; i++) {
            bool absolute = false;
            Vec s = storage(i, absolute);
            for (uint32_t k = 0; k < 4; k++) o[i - 1][k] = modifiers(s[((swz[i] >> (k * 2)) + k) & 3], absolute, negs[i]);
        }
        bool replicate = false;
        switch (vop) {
            case 0: for (int k = 0; k < 4; k++) vr[k] = add(o[0][k], o[1][k]); break;
            case 1: for (int k = 0; k < 4; k++) vr[k] = mul(o[0][k], o[1][k]); break;
            case 2: for (int k = 0; k < 4; k++) vr[k] = pick(o[0][k], o[1][k], compare(o[0][k], o[1][k], Cmp::Ge)); break;
            case 3: for (int k = 0; k < 4; k++) vr[k] = pick(o[0][k], o[1][k], compare(o[0][k], o[1][k], Cmp::Lt)); break;
            case 4: for (int k = 0; k < 4; k++) vr[k] = fromDec(compare(o[0][k], o[1][k], Cmp::Eq)); break;
            case 5: for (int k = 0; k < 4; k++) vr[k] = fromDec(compare(o[0][k], o[1][k], Cmp::Gt)); break;
            case 6: for (int k = 0; k < 4; k++) vr[k] = fromDec(compare(o[0][k], o[1][k], Cmp::Ge)); break;
            case 7: for (int k = 0; k < 4; k++) vr[k] = fromDec(compare(o[0][k], o[1][k], Cmp::Ne)); break;
            case 8: for (int k = 0; k < 4; k++) vr[k] = frcn(o[0][k]); break;
            case 9: for (int k = 0; k < 4; k++) vr[k] = truncn(o[0][k]); break;
            case 10: for (int k = 0; k < 4; k++) vr[k] = floorn(o[0][k]); break;
            case 11: for (int k = 0; k < 4; k++) vr[k] = mad(o[0][k], o[1][k], o[2][k]); break;
            case 12: case 13: case 14: {
                Cmp cmp = vop == 12 ? Cmp::Eq : (vop == 13 ? Cmp::Ge : Cmp::Gt);
                for (int k = 0; k < 4; k++) {
                    Dec d = compare(o[0][k], exact(0.0f), cmp);
                    vr[k] = d == Dec::Ambiguous ? unknown() : (d == Dec::True ? o[1][k] : o[2][k]);
                }
                break;
            }
            case 15: case 16: case 17: {
                uint32_t count = vop == 15 ? 4 : (vop == 16 ? 3 : 2);
                Num sum = exact(0.0f);
                for (uint32_t k = 0; k < count; k++) sum = add(sum, mul(o[0][k], o[1][k]));
                if (vop == 17) sum = add(sum, o[2][0]);
                // The sum may be done in another order or fused: allow one rounding per term.
                if (!sum.unknown && sum.e > 0.0f) sum.e += ulp(sum.v) * float(count);
                vr[0] = sum;
                replicate = true;
                break;
            }
            case 18: {  // cube: operand .z_xy
                Num x = o[0][2], y = o[0][3], z = o[0][0];
                if (x.unknown || y.unknown || z.unknown) {
                    for (auto& c : vr) c = unknown();
                    break;
                }
                float xa = std::fabs(x.v), ya = std::fabs(y.v), za = std::fabs(z.v);
                // Ambiguous major axis: within the errors.
                float e = x.e + y.e + z.e;
                auto close = [&](float p, float q) { return e > 0.0f && std::fabs(p - q) <= e; };
                bool ambiguous = (za >= xa && za >= ya && (close(za, xa) || close(za, ya))) ||
                                 (!(za >= xa && za >= ya) && ya >= xa && close(ya, xa)) ||
                                 (!(za >= xa && za >= ya) && !(ya >= xa) && close(xa, ya));
                if (za >= xa && za >= ya) {
                    bool negz = std::isless(z.v, 0.0f);
                    vr = {neg(y), negz ? neg(x) : x, z, exact(negz ? 5.0f : 4.0f)};
                } else if (ya >= xa) {
                    bool negy = std::isless(y.v, 0.0f);
                    vr = {negy ? neg(z) : z, x, y, exact(negy ? 3.0f : 2.0f)};
                } else {
                    bool negx = std::isless(x.v, 0.0f);
                    vr = {neg(y), negx ? z : neg(z), x, exact(negx ? 1.0f : 0.0f)};
                }
                vr[2] = mul(vr[2], exact(2.0f));
                if (ambiguous) for (auto& c : vr) c = unknown();
                break;
            }
            case 19: {  // max4
                Num a = o[0][0], b = o[0][1], c = o[0][2], d = o[0][3];
                Dec ab = compare(a, b, Cmp::Ge), ac = compare(a, c, Cmp::Ge), ad = compare(a, d, Cmp::Ge);
                Dec bc = compare(b, c, Cmp::Ge), bd = compare(b, d, Cmp::Ge), cd = compare(c, d, Cmp::Ge);
                bool amb = ab == Dec::Ambiguous || ac == Dec::Ambiguous || ad == Dec::Ambiguous || bc == Dec::Ambiguous ||
                           bd == Dec::Ambiguous || cd == Dec::Ambiguous;
                Num m;
                if (ab == Dec::True && ac == Dec::True && ad == Dec::True) m = a;
                else if (bc == Dec::True && bd == Dec::True) m = b;
                else if (cd == Dec::True) m = c;
                else m = d;
                if (amb) {
                    // Any of the close candidates: widen to cover them.
                    if (m.unknown || special(m.v)) m = unknown();
                    else {
                        float spread = 0.0f;
                        for (Num q : {a, b, c, d}) {
                            if (q.unknown || special(q.v)) { m = unknown(); break; }
                            if (std::fabs(q.v - m.v) <= q.e + m.e) spread = std::max(spread, std::fabs(q.v - m.v) + q.e);
                        }
                        if (!m.unknown) m.e += spread;
                    }
                }
                vr[0] = m;
                replicate = true;
                break;
            }
            case 20: case 21: case 22: case 23: {  // setp_*_push
                Cmp cmp = vop == 20 ? Cmp::Eq : (vop == 21 ? Cmp::Ne : (vop == 22 ? Cmp::Gt : Cmp::Ge));
                Dec w0 = compare(o[0][3], exact(0.0f), Cmp::Eq), w1 = compare(o[1][3], exact(0.0f), cmp);
                p0Fragile_ = w0 == Dec::Ambiguous || (w0 == Dec::True && w1 == Dec::Ambiguous);
                p0_ = w0 == Dec::True && w1 == Dec::True;
                Dec x0 = compare(o[0][0], exact(0.0f), Cmp::Eq), x1 = compare(o[1][0], exact(0.0f), cmp);
                bool amb = x0 == Dec::Ambiguous || (x0 == Dec::True && x1 == Dec::Ambiguous);
                if (amb) vr[0] = unknown();
                else if (x0 == Dec::True && x1 == Dec::True) vr[0] = exact(0.0f);
                else vr[0] = add(o[0][0], exact(1.0f));
                replicate = true;
                break;
            }
            case 24: case 25: case 26: case 27: {  // kill_*
                Cmp cmp = vop == 24 ? Cmp::Eq : (vop == 25 ? Cmp::Gt : (vop == 26 ? Cmp::Ge : Cmp::Ne));
                bool any = false, amb = false;
                for (int k = 0; k < 4; k++) {
                    Dec d = compare(o[0][k], o[1][k], cmp);
                    if (d == Dec::True) any = true;
                    if (d == Dec::Ambiguous) amb = true;
                }
                if (!any && amb) {
                    vr[0] = unknown();
                    if (in_.pixel) fragile("kill within the error");
                } else {
                    vr[0] = exact(any ? 1.0f : 0.0f);
                    if (any && in_.pixel) out_.killed = true;
                }
                replicate = true;
                break;
            }
            case 28:  // dst
                vr = {exact(1.0f), mul(o[0][1], o[1][1]), o[0][2], o[1][3]};
                break;
            case 29: {  // maxa
                Num wv = o[0][3];
                float c = std::clamp(wv.v, -256.0f, 255.0f);
                a0Fragile_ = wv.unknown || std::isnan(wv.v) || nearInteger(Num{c, wv.e, false}, 0.5f);
                a0_ = std::isnan(wv.v) ? 0 : int32_t(std::floor(c + 0.5f));
                for (int k = 0; k < 4; k++) vr[k] = pick(o[0][k], o[1][k], compare(o[0][k], o[1][k], Cmp::Ge));
                break;
            }
        }
        if (replicate)
            for (int k = 1; k < 4; k++) vr[k] = vr[0];
        if (vsat)
            for (auto& c : vr) c = saturate(c);
    }

    // Scalar operation.
    Num sa{}, sb{};
    uint32_t kind = scalarKind(sop);
    if (kind == 1 || kind == 2) {
        bool absolute = false;
        Vec s = storage(3, absolute);
        sa = modifiers(s[((swz[3] >> 6) + 3) & 3], absolute, negs[3]);
        sb = modifiers(s[swz[3] & 3], absolute, negs[3]);
    } else if (kind == 3) {
        Vec cv = constant(regs[3], addressed(3), a0Rel);
        uint32_t t = (sop & 1) | (uint32_t(temps[3]) << 1) | (swz[3] & 0x3C);
        sa = modifiers(cv[((swz[3] >> 6) + 3) & 3], absConst, negs[3]);
        sb = modifiers(r_[t][swz[3] & 3], absConst, negs[3]);
    }
    Num prev = ps_;
    Num s = prev;
    auto setPredicate = [&](Dec d) {
        p0Fragile_ = d == Dec::Ambiguous;
        p0_ = d == Dec::True;
    };
    switch (sop) {
        case 0: case 44: case 45: s = add(sa, sb); break;                  // adds, addsc
        case 1: s = add(sa, prev); break;                                  // adds_prev
        case 2: case 42: case 43: s = mul(sa, sb); break;                  // muls, mulsc
        case 3: s = mul(sa, prev); break;                                  // muls_prev
        case 4: {                                                          // muls_prev2
            if (prev.unknown || sb.unknown) { s = unknown(); break; }
            bool invalid = prev.v == -FLT_MAX || !std::isfinite(prev.v) || !std::isfinite(sb.v);
            Dec le = compare(sb, exact(0.0f), Cmp::Gt);
            if ((prev.e > 0 && (special(prev.v) || std::fabs(prev.v + FLT_MAX) <= prev.e)) || (!invalid && le == Dec::Ambiguous)) {
                s = unknown();
                break;
            }
            s = (invalid || le == Dec::False) ? exact(-FLT_MAX) : mul(sa, prev);
            break;
        }
        case 5: s = pick(sa, sb, compare(sa, sb, Cmp::Ge)); break;         // maxs
        case 6: s = pick(sa, sb, compare(sa, sb, Cmp::Lt)); break;         // mins
        case 7: s = fromDec(compare(sa, exact(0.0f), Cmp::Eq)); break;     // seqs
        case 8: s = fromDec(compare(sa, exact(0.0f), Cmp::Gt)); break;     // sgts
        case 9: s = fromDec(compare(sa, exact(0.0f), Cmp::Ge)); break;     // sges
        case 10: s = fromDec(compare(sa, exact(0.0f), Cmp::Ne)); break;    // snes
        case 11: s = frcn(sa); break;
        case 12: s = truncn(sa); break;
        case 13: s = floorn(sa); break;
        case 14: {                                                         // exp (exp2)
            float r = std::exp2(sa.v);
            s = transcendental(sa, r, std::fabs(r) * 0.6931472f * (1.0f + sa.e), std::fabs(r) * kExpRel + FLT_MIN);
            break;
        }
        case 15: case 16: {                                                // logc, log
            float r = std::log2(sa.v);
            if (sop == 15 && r == -kInf) r = -FLT_MAX;
            float lo = std::fabs(sa.v) - sa.e;
            float slope = (sa.e > 0.0f) ? (lo > 0.0f ? 1.4426950f / lo : kInf) : 0.0f;
            s = transcendental(sa, r, slope, kLogAbs + std::fabs(r) * 0x1p-20f);
            if (sop == 15 && !s.unknown && sa.e > 0.0f && std::fabs(sa.v) <= sa.e) s = unknown();
            break;
        }
        case 17: case 18: case 19: {                                       // rcpc, rcpf, rcp
            float r = 1.0f / sa.v;
            if (std::isinf(r)) {
                if (sop == 17) r = r < 0 ? -FLT_MAX : FLT_MAX;
                if (sop == 18) r = r < 0 ? -0.0f : 0.0f;
            }
            float lo = std::fabs(sa.v) - sa.e;
            float slope = (sa.e > 0.0f) ? (lo > 0.0f ? 1.0f / (lo * lo) : kInf) : 0.0f;
            s = transcendental(sa, r, slope, std::fabs(r) * kRcpRel);
            break;
        }
        case 20: case 21: case 22: {                                       // rsqc, rsqf, rsq
            float r = 1.0f / std::sqrt(sa.v);
            if (std::isinf(r)) {
                if (sop == 20) r = r < 0 ? -FLT_MAX : FLT_MAX;
                if (sop == 21) r = r < 0 ? -0.0f : 0.0f;
            }
            float lo = std::fabs(sa.v) - sa.e;
            float slope = (sa.e > 0.0f) ? (lo > 0.0f ? 0.5f / (lo * std::sqrt(lo)) : kInf) : 0.0f;
            s = transcendental(sa, r, slope, std::fabs(r) * kRsqRel);
            break;
        }
        case 23: case 24: {                                                // maxas, maxasf
            float c = std::clamp(sa.v, -256.0f, 255.0f);
            float off = sop == 23 ? 0.5f : 0.0f;
            a0Fragile_ = sa.unknown || std::isnan(sa.v) || nearInteger(Num{c, sa.e, false}, off);
            a0_ = std::isnan(sa.v) ? 0 : int32_t(std::floor(c + off));
            s = pick(sa, sb, compare(sa, sb, Cmp::Ge));
            break;
        }
        case 25: case 46: case 47: s = add(sa, neg(sb)); break;            // subs, subsc
        case 26: s = add(sa, neg(prev)); break;                            // subs_prev
        case 27: case 28: case 29: case 30: {                              // setp_eq ne gt ge
            Cmp cmp = sop == 27 ? Cmp::Eq : (sop == 28 ? Cmp::Ne : (sop == 29 ? Cmp::Gt : Cmp::Ge));
            Dec d = compare(sa, exact(0.0f), cmp);
            setPredicate(d);
            s = d == Dec::Ambiguous ? unknown() : exact(d == Dec::True ? 0.0f : 1.0f);
            break;
        }
        case 31: {                                                         // setp_inv
            Dec one = compare(sa, exact(1.0f), Cmp::Eq), zero = compare(sa, exact(0.0f), Cmp::Eq);
            setPredicate(one);
            if (one == Dec::Ambiguous) s = unknown();
            else if (one == Dec::True) s = exact(0.0f);
            else if (zero == Dec::Ambiguous) s = unknown();
            else s = zero == Dec::True ? exact(1.0f) : sa;
            break;
        }
        case 32: {                                                         // setp_pop
            Num t = add(sa, exact(-1.0f));
            Dec d = compare(t, exact(0.0f), Cmp::Gt);
            Dec le = d == Dec::Ambiguous ? Dec::Ambiguous : (d == Dec::True ? Dec::False : Dec::True);
            setPredicate(le);
            s = le == Dec::Ambiguous ? unknown() : (le == Dec::True ? exact(0.0f) : t);
            break;
        }
        case 33:                                                           // setp_clr
            p0_ = false;
            p0Fragile_ = false;
            s = exact(FLT_MAX);
            break;
        case 34: {                                                         // setp_rstr
            Dec d = compare(sa, exact(0.0f), Cmp::Eq);
            setPredicate(d);
            s = d == Dec::Ambiguous ? unknown() : (d == Dec::True ? exact(0.0f) : sa);
            break;
        }
        case 35: case 36: case 37: case 38: case 39: {                     // kills
            Dec d;
            if (sop == 39) d = compare(sa, exact(1.0f), Cmp::Eq);
            else d = compare(sa, exact(0.0f), sop == 35 ? Cmp::Eq : (sop == 36 ? Cmp::Gt : (sop == 37 ? Cmp::Ge : Cmp::Ne)));
            if (d == Dec::Ambiguous) {
                s = unknown();
                if (in_.pixel) fragile("kill within the error");
            } else {
                s = exact(d == Dec::True ? 1.0f : 0.0f);
                if (d == Dec::True && in_.pixel) out_.killed = true;
            }
            break;
        }
        case 40: {                                                         // sqrt
            float r = std::sqrt(sa.v);
            float lo = std::fabs(sa.v) - sa.e;
            float slope = (sa.e > 0.0f) ? (lo > 0.0f ? 0.5f / std::sqrt(lo) : kInf) : 0.0f;
            s = transcendental(sa, r, slope, std::fabs(r) * kSqrtRel);
            if (!s.unknown && sa.e > 0.0f && std::fabs(sa.v) <= sa.e) s = unknown();
            break;
        }
        case 48: case 49: {                                                // sin, cos
            float r = sop == 48 ? std::sin(sa.v) : std::cos(sa.v);
            float own = kSinAbs * std::max(1.0f, std::fabs(sa.v) / 3.14159265f);
            s = transcendental(sa, r, 1.0f, own);
            break;
        }
        case 50:                                                           // retain_prev
            break;
    }
    ps_ = s;
    Num sres = ssat ? saturate(s) : s;
    if (in_.trace) {
        char b[64];
        std::snprintf(b, sizeof(b), "    vop %u sop %u vmask %X smask %X%s: ", vop, sop, vmask, smask, isExport ? " export" : "");
        trace(std::string(b) + "v " + fmtVec(vr) + " ps " + fmtNum(s) + " -> vdst " + std::to_string(isExport ? vdst : tempIndex(vdst, vdstRel)) +
              " sdst " + std::to_string(isExport ? vdst : tempIndex(sdst, sdstRel)));
    }

    // Writes: the vector result, then the scalar result (the scalar one wins on overlap).
    if (isExport) {
        uint32_t reg = vdst;
        bool memory = reg >= 32 && reg <= 37;
        if (!memory) {
            Vec& e = out_.exports[reg];
            for (int k = 0; k < 4; k++) {
                uint32_t bit = 1u << k;
                if (vmask & bit) e[k] = vr[k];
                else if (smask & bit) e[k] = sres;
                else if (const1Mask & bit) e[k] = exact(1.0f);
                else if (const0Mask & bit) e[k] = exact(0.0f);
                else continue;
                out_.exportMask[reg] |= bit;
            }
        }
    } else {
        if (vmask) {
            Vec& d = r_[tempIndex(vdst, vdstRel)];
            for (int k = 0; k < 4; k++)
                if (vmask & (1u << k)) d[k] = vr[k];
        }
        if (smask) {
            Vec& d = r_[tempIndex(sdst, sdstRel)];
            for (int k = 0; k < 4; k++)
                if (smask & (1u << k)) d[k] = sres;
        }
    }
}

// ------------------------------------------------------------------------------------------
// Vertex fetch.

uint32_t formatComponents(uint32_t format) {
    switch (format) {
        case 6: case 7: case 26: case 32: case 35: case 38: return 4;
        case 16: case 17: case 57: return 3;
        case 25: case 31: case 34: case 37: return 2;
        case 33: case 36: return 1;
        default: return 0;
    }
}

uint32_t formatDwords(uint32_t format) {
    switch (format) {
        case 26: case 32: case 34: case 37: return 2;
        case 57: return 3;
        case 35: case 38: return 4;
        default: return 1;
    }
}

// Decodes one element: Xenos vertex formats (the SDK's SPIR-V translator's rules).
Vec decodeVertex(const uint32_t* d, uint32_t format, bool isSigned, bool integer, bool noZero, int32_t expAdjust) {
    Vec r;
    for (auto& c : r) c = exact(0.0f);
    uint32_t count = formatComponents(format);
    int widths[4] = {}, offsets[4] = {}, words[4] = {};
    bool packed = true;
    switch (format) {
        case 6: widths[0] = widths[1] = widths[2] = widths[3] = 8; offsets[1] = 8; offsets[2] = 16; offsets[3] = 24; break;
        case 7: widths[0] = widths[1] = widths[2] = 10; widths[3] = 2; offsets[1] = 10; offsets[2] = 20; offsets[3] = 30; break;
        case 16: widths[0] = widths[1] = 11; widths[2] = 10; offsets[1] = 11; offsets[2] = 22; break;
        case 17: widths[0] = 10; widths[1] = widths[2] = 11; offsets[1] = 10; offsets[2] = 21; break;
        case 25: widths[0] = widths[1] = 16; offsets[1] = 16; break;
        case 26: widths[0] = widths[1] = widths[2] = widths[3] = 16; offsets[1] = offsets[3] = 16; words[2] = words[3] = 1; break;
        default: packed = false; break;
    }
    if (packed) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t word = d[words[i]];
            double v;
            if (isSigned) v = double(sbits(word, uint32_t(offsets[i]), uint32_t(widths[i])));
            else v = double(bits(word, uint32_t(offsets[i]), uint32_t(widths[i])));
            float f = float(v);
            float e = 0.0f;
            if (!integer) {
                double scale;
                if (isSigned) scale = double((1u << (widths[i] - 1)) - 1) + (noZero ? 0.5 : 0.0);
                else scale = double((1u << widths[i]) - 1);
                double n = isSigned && noZero ? (v + 0.5) / scale : v / scale;
                if (isSigned && !noZero) n = std::max(-1.0, n);
                f = float(n);
                e = 3.0f * ulp(f);  // a multiplication by the reciprocal, then an addition
            }
            r[i] = Num{f, e, false};
        }
    } else if (format == 31 || format == 32) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t word = d[i >> 1];
            r[i] = exact(halfToFloat(uint16_t(word >> ((i & 1) * 16))));
        }
    } else if (format == 33 || format == 34 || format == 35) {
        for (uint32_t i = 0; i < count; i++) {
            double v = isSigned ? double(int32_t(d[i])) : double(d[i]);
            float f = float(v);
            float e = 0.0f;
            if (!integer) {
                double n = isSigned ? (noZero ? (v + 0.5) / 2147483647.5 : v / 2147483647.0) : v / 4294967295.0;
                f = float(n);
                // float(int) first (rounded), then the scale.
                e = 3.0f * ulp(f) + float(std::fabs(double(float(v)) - v) / (isSigned ? 2147483647.0 : 4294967295.0));
            } else {
                e = 0.0f;  // the same int-to-float rounding everywhere
            }
            r[i] = Num{f, e, false};
        }
    } else if (format == 36 || format == 37 || format == 38 || format == 57) {
        for (uint32_t i = 0; i < count; i++) r[i] = exact(std::bit_cast<float>(d[i]));
    }
    if (expAdjust) {
        float scale = std::ldexp(1.0f, expAdjust);
        for (uint32_t i = 0; i < count; i++) {
            float before = r[i].v;
            r[i].v *= scale;
            r[i].e *= scale;
            if (special(r[i].v) && !special(before)) r[i] = unknown();
        }
    }
    return r;
}

void Machine::storeFetch(uint32_t dst, bool relative, uint32_t dstSwizzle, const Vec& value) {
    if (in_.trace) trace("    fetch " + fmtVec(value) + " -> r" + std::to_string(tempIndex(dst, relative)));
    Vec& d = r_[tempIndex(dst, relative)];
    for (int k = 0; k < 4; k++) {
        uint32_t s = (dstSwizzle >> (k * 3)) & 7;
        if (s == 7) continue;
        if (s <= 3) d[k] = value[s];
        else if (s == 5) d[k] = exact(1.0f);
        else d[k] = exact(0.0f);
    }
}

void Machine::vfetch(const uint32_t* w, uint32_t address) {
    const uint32_t src = bits(w[0], 5, 6), dst = bits(w[0], 12, 6);
    const bool srcRel = bits(w[0], 11, 1), dstRel = bits(w[0], 18, 1);
    const uint32_t srcComponent = bits(w[0], 30, 2);
    const uint32_t dstSwizzle = bits(w[1], 0, 12);
    const bool mini = bits(w[1], 30, 1);
    if (!mini) {
        const bool rounded = bits(w[1], 15, 1);
        Num x = r_[tempIndex(src, srcRel)][srcComponent];
        vfIndexFragile_ = x.unknown || std::isnan(x.v) || nearInteger(x, rounded ? 0.5f : 0.0f);
        double f = std::floor(double(x.v) + (rounded ? 0.5 : 0.0));
        vfIndex_ = std::isnan(x.v) ? 0 : int64_t(std::clamp(f, -2147483648.0, 2147483647.0));
        haveFullFetch_ = true;
        lastFullW0_ = w[0];
        lastFullW2_ = w[2];
    } else if (!haveFullFetch_) {
        out_.error = true;
        out_.message = "vfetch_mini before a vfetch_full";
        return;
    }
    if (vfIndexFragile_) fragile("vertex index within the error");

    uint32_t format, endian, buffer;
    bool isSigned, integer, noZero;
    int32_t expAdjust;
    int64_t byteAddress;  // into the buffer
    uint32_t elementSwizzle = (0u | (1u << 3) | (2u << 6) | (3u << 9));
    int64_t limit = -1;
    Vec value;
    if (in_.bindingMode) {
        auto found = in_.vfetchElement.find(address);
        if (found == in_.vfetchElement.end() || found->second >= in_.elements.size()) {
            out_.error = true;
            out_.message = "vfetch without an element";
            return;
        }
        const Element& el = in_.elements[found->second];
        format = el.word & 0x3F;
        isSigned = el.word & 0x40;
        integer = el.word & 0x80;
        noZero = el.word & 0x100;
        endian = (el.word >> 9) & 3;
        expAdjust = int32_t(el.word << 15) >> 26;
        elementSwizzle = (el.word >> 17) & 0xFFF;
        buffer = el.buffer;
        if (format == 0) {
            // An element the declaration lacks: (0, 0, 0, 1).
            value = {exact(0.0f), exact(0.0f), exact(0.0f), exact(1.0f)};
            storeFetch(dst, dstRel, dstSwizzle, value);
            return;
        }
        byteAddress = int64_t(uint32_t(uint64_t(el.offset) + uint64_t(vfIndex_) * el.stride));
    } else {
        format = bits(w[1], 16, 6);
        isSigned = bits(w[1], 12, 1);
        integer = bits(w[1], 13, 1);
        noZero = bits(w[1], 14, 1);
        expAdjust = sbits(w[1], 24, 6);
        uint32_t constant = bits(lastFullW0_, 20, 5) * 3 + bits(lastFullW0_, 25, 2);
        const FetchConstant& fc = in_.fetchConstants[constant];
        endian = fc.endian;
        buffer = fc.buffer;
        uint32_t stride = bits(lastFullW2_, 0, 8);
        int32_t offset = sbits(w[2], 8, 23);
        int64_t relative = int64_t(int32_t(uint32_t(uint64_t(vfIndex_) * stride))) * 4 + int64_t(offset) * 4;
        // Outside the stream: undefined (the SDK's interpreter reads 0, its SPIR-V translator
        // reads memory): not compared.
        if (relative < 0 || relative + int64_t(formatDwords(format)) * 4 > int64_t(fc.size) || formatComponents(format) == 0) {
            for (auto& c : value) c = unknown();
            storeFetch(dst, dstRel, dstSwizzle, value);
            return;
        }
        byteAddress = int64_t(fc.offset) + relative;
        limit = int64_t(fc.offset) + int64_t(fc.size);
    }
    (void)limit;
    if (buffer >= in_.buffers.size() || formatComponents(format) == 0) {
        for (auto& c : value) c = unknown();
        storeFetch(dst, dstRel, dstSwizzle, value);
        return;
    }
    const auto& data = in_.buffers[buffer];
    uint32_t d[4] = {};
    uint32_t n = formatDwords(format);
    for (uint32_t i = 0; i < n; i++) {
        int64_t at = byteAddress + int64_t(i) * 4;
        if (at < 0 || at + 4 > int64_t(data.size())) {
            for (auto& c : value) c = unknown();
            storeFetch(dst, dstRel, dstSwizzle, value);
            return;
        }
        uint32_t raw = uint32_t(data[size_t(at)]) | (uint32_t(data[size_t(at) + 1]) << 8) | (uint32_t(data[size_t(at) + 2]) << 16) |
                       (uint32_t(data[size_t(at) + 3]) << 24);
        d[i] = endianSwap(raw, endian);
    }
    Vec decoded = decodeVertex(d, format, isSigned, integer, noZero, expAdjust);
    // The element swizzle (binding mode: D3DDECLTYPE's fill of missing components).
    for (int k = 0; k < 4; k++) {
        uint32_t s = (elementSwizzle >> (k * 3)) & 7;
        value[k] = s < 4 ? decoded[s] : exact(s == 5 ? 1.0f : 0.0f);
    }
    storeFetch(dst, dstRel, dstSwizzle, value);
}

// ------------------------------------------------------------------------------------------
// Texture fetch.

// Texel addressing: wrap or clamp to edge.
int32_t address(int64_t i, uint32_t size, bool clamp) {
    if (clamp) return int32_t(std::clamp<int64_t>(i, 0, int64_t(size) - 1));
    int64_t m = i % int64_t(size);
    return int32_t(m < 0 ? m + size : m);
}

Vec Machine::sample(const Texture& t, uint32_t dim, const std::array<Num, 3>& c, bool linear, bool& unknownOut) {
    Vec r;
    unknownOut = false;
    // Texel-space coordinates.
    uint32_t sizes[3] = {t.width, dim == 0 ? 1u : t.height, dim == 2 ? t.depth : 1u};
    uint32_t axes = dim == 0 ? 1 : (dim == 2 ? 3 : 2);
    uint32_t layer = 0;
    std::array<Num, 3> cc = c;
    if (dim == 3) {
        // Cube: direction to face and face coordinates (the Vulkan / D3D cube rules).
        float x = c[0].v, y = c[1].v, z = c[2].v;
        float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
        float e = c[0].e + c[1].e + c[2].e;
        float ma, sc, tc;
        if (ax >= ay && ax >= az) {
            layer = x >= 0 ? 0 : 1;
            ma = ax;
            sc = x >= 0 ? -z : z;
            tc = -y;
            if (e > 0 && (std::fabs(ax - ay) <= e || std::fabs(ax - az) <= e)) unknownOut = true;
        } else if (ay >= az) {
            layer = y >= 0 ? 2 : 3;
            ma = ay;
            sc = x;
            tc = y >= 0 ? z : -z;
            if (e > 0 && std::fabs(ay - az) <= e) unknownOut = true;
        } else {
            layer = z >= 0 ? 4 : 5;
            ma = az;
            sc = z >= 0 ? x : -x;
            tc = -y;
        }
        // Equal magnitudes: the face choice is the implementation's.
        if (ax == ay || ax == az || ay == az) unknownOut = true;
        if (ma == 0.0f || unknownOut) {
            unknownOut = true;
            return r;
        }
        float u = 0.5f * (sc / ma + 1.0f), v = 0.5f * (tc / ma + 1.0f);
        // Error of the face coordinates: generous (division by the major axis).
        float ce = e / ma * 2.0f + 1e-6f;
        cc[0] = Num{u, ce, false};
        cc[1] = Num{v, ce, false};
        sizes[0] = t.width;
        sizes[1] = t.height;
        axes = 2;
    }
    if (dim == 1 || dim == 3) layer = (dim == 3) ? layer : 0;
    float pos[3] = {}, posErr[3] = {};
    for (uint32_t a = 0; a < axes; a++) {
        if (cc[a].unknown || !std::isfinite(cc[a].v)) {
            unknownOut = true;
            return r;
        }
        pos[a] = cc[a].v * float(sizes[a]);
        // Coordinates are converted to fixed point with 8 sub-texel bits.
        posErr[a] = cc[a].e * float(sizes[a]) + kFilterWeight;
    }
    auto fetch = [&](int64_t x, int64_t y, int64_t z) {
        uint32_t ix = uint32_t(address(x, sizes[0], t.clamp));
        uint32_t iy = axes > 1 ? uint32_t(address(y, sizes[1], t.clamp)) : 0;
        uint32_t iz = axes > 2 ? uint32_t(address(z, sizes[2], t.clamp)) : layer;
        return t.texel(ix, iy, dim == 2 ? iz : (dim == 3 ? layer : 0));
    };
    if (!linear) {
        int64_t i[3] = {};
        for (uint32_t a = 0; a < axes; a++) {
            float f = std::floor(pos[a]);
            if (pos[a] - f <= posErr[a] || f + 1.0f - pos[a] <= posErr[a]) {
                unknownOut = true;
                return r;
            }
            i[a] = int64_t(f);
        }
        const float* p = fetch(i[0], i[1], i[2]);
        for (int k = 0; k < 4; k++) r[k] = exact(p[k]);
        return r;
    }
    // Linear: the 2, 4 or 8 texels around pos - 0.5 with their weights. Cube maps filter
    // across faces (seamless) on the host: a footprint over a face edge is not modelled.
    if (dim == 3) {
        for (uint32_t a = 0; a < 2; a++) {
            float p = pos[a] - 0.5f;
            if (p - posErr[a] < 0.0f || p + posErr[a] + 1.0f >= float(sizes[a])) {
                unknownOut = true;
                return r;
            }
        }
    }
    auto filter = [&](const float* at, double* outValue) {
        int64_t base[3] = {};
        double frac[3] = {};
        for (uint32_t a = 0; a < axes; a++) {
            double p = double(at[a]) - 0.5;
            double f = std::floor(p);
            base[a] = int64_t(f);
            frac[a] = p - f;
        }
        for (int k = 0; k < 4; k++) outValue[k] = 0.0;
        for (uint32_t cnr = 0; cnr < (1u << axes); cnr++) {
            double weight = 1.0;
            int64_t ix[3] = {};
            for (uint32_t a = 0; a < axes; a++) {
                bool hi = (cnr >> a) & 1;
                ix[a] = base[a] + (hi ? 1 : 0);
                weight *= hi ? frac[a] : 1.0 - frac[a];
            }
            const float* p = fetch(ix[0], ix[1], ix[2]);
            for (int k = 0; k < 4; k++) outValue[k] += weight * p[k];
        }
    };
    double centre[4];
    filter(pos, centre);
    // The result is piecewise linear in each coordinate, with kinks at texel centres: over the
    // coordinate's error interval its extremes are at the interval's ends or at those kinks.
    std::vector<float> candidates[3];
    for (uint32_t a = 0; a < axes; a++) {
        float lo = pos[a] - posErr[a], hi = pos[a] + posErr[a];
        candidates[a] = {lo, pos[a], hi};
        for (float k = std::floor(lo - 0.5f) + 0.5f; k <= hi; k += 1.0f)
            if (k > lo) candidates[a].push_back(k);
    }
    double spread[4] = {};
    float at[3] = {};
    for (float x : candidates[0])
        for (float y : (axes > 1 ? candidates[1] : std::vector<float>{0.0f}))
            for (float z : (axes > 2 ? candidates[2] : std::vector<float>{0.0f})) {
                at[0] = x;
                at[1] = y;
                at[2] = z;
                double v[4];
                filter(at, v);
                for (int k = 0; k < 4; k++) spread[k] = std::max(spread[k], std::fabs(v[k] - centre[k]));
            }
    for (int k = 0; k < 4; k++) {
        float v = float(centre[k]);
        r[k] = Num{v, float(spread[k]) + std::fabs(v) * 0x1p-20f + 0x1p-20f, false};
    }
    return r;
}

void Machine::tfetch(const uint32_t* w) {
    const uint32_t op = bits(w[0], 0, 5);
    const uint32_t src = bits(w[0], 5, 6), dst = bits(w[0], 12, 6);
    const bool srcRel = bits(w[0], 11, 1), dstRel = bits(w[0], 18, 1);
    const uint32_t slot = bits(w[0], 20, 5);
    const bool denorm = bits(w[0], 25, 1);
    const uint32_t srcSwizzle = bits(w[0], 26, 6);
    const uint32_t dstSwizzle = bits(w[1], 0, 12);
    const uint32_t magFilter = bits(w[1], 12, 2);
    const bool useRegLod = bits(w[1], 29, 1);
    const uint32_t dim = bits(w[2], 14, 2);
    const float offsets[3] = {float(sbits(w[2], 16, 5)) * 0.5f, float(sbits(w[2], 21, 5)) * 0.5f, float(sbits(w[2], 26, 5)) * 0.5f};
    const Vec& s = r_[tempIndex(src, srcRel)];
    auto component = [&](uint32_t i) { return s[(srcSwizzle >> (i * 2)) & 3]; };

    Vec value;
    switch (op) {
        case 24:  // setTexLOD
            texLod_ = component(0);
            return;
        case 25:  // setGradientH
        case 26:  // setGradientV
            return;  // gradients only matter with mips (the tests' textures have one level)
        case 1: {  // tfetch
            const Texture* t = in_.textures[slot];
            // No texture, or one of another dimension (a slot sampled two ways): not modelled.
            if (!t || uint32_t(t->dim) != dim) {
                for (auto& c : value) c = unknown();
                break;
            }
            uint32_t axes = dim == 0 ? 1 : (dim == 1 ? 2 : 3);
            uint32_t sizes[3] = {t->width, t->height, t->depth};
            std::array<Num, 3> c{};
            for (uint32_t i = 0; i < axes; i++) c[i] = component(i);
            bool unk = false;
            if (dim == 3) {
                // tfetchCube: (S, T, face) with S and T in [1, 2] from the cube sequence; to a
                // direction (the SDK's SPIR-V translator: 2 * c - 3, then the face's axes).
                Num faceN = c[2];
                float face = faceN.v + offsets[2];
                if (faceN.unknown || std::isnan(face)) {
                    unk = true;
                } else {
                    float clamped = std::clamp(face, 0.0f, 5.0f);
                    if (faceN.e > 0.0f && nearInteger(Num{clamped, faceN.e, false})) unk = true;
                    uint32_t f = uint32_t(clamped);
                    // Offsets on cube faces: in texels of the face (the S and T offsets), added
                    // before the division by the size with unnormalised coordinates.
                    Num st[2];
                    for (int a = 0; a < 2; a++) {
                        float size = float(sizes[a]);
                        st[a] = denorm ? mul(add(c[a], exact(offsets[a])), exact(1.0f / size)) : add(c[a], exact(offsets[a] / size));
                        if (!st[a].unknown && (denorm || offsets[a] != 0.0f)) st[a].e += 2.0f * ulp(st[a].v);
                    }
                    Num sc = add(mul(st[0], exact(2.0f)), exact(-3.0f));
                    Num tc = add(mul(st[1], exact(2.0f)), exact(-3.0f));
                    bool negative = f & 1;
                    Num sign = exact(negative ? -1.0f : 1.0f);
                    std::array<Num, 3> dir;
                    switch (f >> 1) {
                        case 0: dir = {sign, neg(tc), negative ? sc : neg(sc)}; break;
                        case 1: dir = {sc, sign, negative ? neg(tc) : tc}; break;
                        default: dir = {negative ? neg(sc) : sc, neg(tc), sign}; break;
                    }
                    c = dir;
                }
            } else {
                for (uint32_t i = 0; i < axes; i++) {
                    float size = float(i == 0 ? sizes[0] : (i == 1 ? sizes[1] : sizes[2]));
                    if (denorm) c[i] = mul(add(c[i], exact(offsets[i])), exact(1.0f / size));
                    else if (offsets[i] != 0.0f) c[i] = add(c[i], exact(offsets[i] / size));
                    // The division by the size is not exact for sizes that are not powers of two.
                    if (!c[i].unknown && (denorm || offsets[i] != 0.0f)) c[i].e += 2.0f * ulp(c[i].v);
                }
            }
            if (unk) {
                for (auto& v : value) v = unknown();
                break;
            }
            bool linear = fetchIsLinear(magFilter, t->linear);
            (void)useRegLod;
            value = sample(*t, dim, c, linear, unk);
            if (unk) for (auto& v : value) v = unknown();
            break;
        }
        default:
            // getCompTexLOD, getWeights, getBCF, getGradients: not modelled (depend on LOD,
            // derivatives and border colours): not compared.
            for (auto& c : value) c = unknown();
            break;
    }
    storeFetch(dst, dstRel, dstSwizzle, value);
}

}  // namespace

uint32_t endianSwap(uint32_t v, uint32_t endian) {
    switch (endian & 3) {
        case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
        case 2: return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
        case 3: return (v << 16) | (v >> 16);
        default: return v;
    }
}

float halfToFloat(uint16_t h) {
    uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (exponent == 0) {
        float f = std::ldexp(float(mantissa), -24);
        return sign ? -f : f;
    }
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

Outputs run(const Inputs& in) {
    Outputs out;
    Machine m(in, out);
    m.run();
    m.copyRegisters();
    return out;
}

}  // namespace xinterp
