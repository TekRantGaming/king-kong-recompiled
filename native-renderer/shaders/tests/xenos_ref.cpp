#include "xenos_ref.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace xref {

namespace {

constexpr uint32_t bits(uint32_t v, uint32_t shift, uint32_t count) { return (v >> shift) & ((count == 32 ? 0u : (1u << count)) - 1u); }
int32_t sbits(uint32_t v, uint32_t shift, uint32_t count) {
    uint32_t u = bits(v, shift, count);
    return int32_t(u << (32 - count)) >> (32 - count);
}

float legacyMul(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }

float halfToFloat(uint32_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    float v;
    if (exp == 0) v = std::ldexp(float(man), -24);
    else if (exp == 31) v = man ? NAN : INFINITY;
    else v = std::ldexp(float(man | 0x400), int(exp) - 25);
    return sign ? -v : v;
}

uint32_t swap(uint32_t v, uint32_t endian) {
    switch (endian) {
        case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
        case 2: return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
        case 3: return (v << 16) | (v >> 16);
        default: return v;
    }
}

}  // namespace

void Machine::run() {
    struct Loop {
        uint32_t constant, iterator;
    };
    Loop loopStack[4] = {};
    uint32_t loopDepth = 0;
    uint32_t callStack[4] = {};
    uint32_t callDepth = 0;
    bool predicate = false;
    float previousScalar = 0.0f;
    int32_t a0 = 0;
    std::array<Vec, 64>& r = registers;
    // vfetch_full state for vfetch_mini.
    uint32_t lastIndex = 0;

    auto loopAddress = [&]() -> int32_t {
        if (!loopDepth) return 0;
        const Loop& l = loopStack[loopDepth - 1];
        int32_t start = int32_t(bits(l.constant, 8, 8)), step = sbits(l.constant, 16, 8);
        return std::clamp(int32_t(l.iterator) * step + start, -256, 256);
    };
    auto temp = [&](uint32_t index, bool relative) -> Vec& {
        return r[uint32_t(int32_t(index) + (relative ? loopAddress() : 0)) & 63];
    };
    auto boolConst = [&](uint32_t index) { return (bools[index >> 5] >> (index & 31)) & 1; };
    auto floatConst = [&](uint32_t index, bool addressed, bool useA0) -> Vec {
        int32_t i = int32_t(index) + (addressed ? (useA0 ? a0 : loopAddress()) : 0);
        if (i < 0 || i > 255) return Vec{};
        return constants[size_t(i)];
    };
    auto writeExport = [&](uint32_t reg, uint32_t mask, const Vec& v) {
        auto [it, inserted] = exports.try_emplace(reg, Vec{});
        for (int c = 0; c < 4; c++)
            if (mask & (1u << c)) it->second[size_t(c)] = v[size_t(c)];
    };

    // Control flow.
    const uint32_t* code = ucode.data();
    uint32_t instructionCount = uint32_t(ucode.size() / 3);
    auto cfAt = [&](uint32_t index) -> uint64_t {
        uint32_t t = index >> 1;
        if (t >= instructionCount) return 0;
        const uint32_t* p = code + t * 3;
        if (index & 1) return uint64_t(p[1] >> 16) | (uint64_t(p[2]) << 16);
        return uint64_t(p[0]) | (uint64_t(p[1] & 0xFFFF) << 32);
    };

    // Control flow ends where the first exec's instructions start.
    uint32_t cfEnd = instructionCount * 2;
    for (uint32_t i = 0; i < cfEnd; i++) {
        uint64_t c = cfAt(i);
        uint32_t op = uint32_t(c >> 44) & 0xF;
        bool isExec = (op >= 1 && op <= 6) || op == 13 || op == 14;
        if (isExec && ((c >> 12) & 7)) cfEnd = std::min(cfEnd, uint32_t(c & 0xFFF) * 2);
    }

    for (uint32_t cf = 0;;) {
        if (++steps > 200000) {
            runaway = true;
            return;
        }
        uint64_t c = cfAt(cf);
        uint32_t lo = uint32_t(c), hi = uint32_t(c >> 32);
        uint32_t op = bits(hi, 12, 4);
        uint32_t next = cf + 1;
        if (cf >= cfEnd) return;  // ran off the end
        switch (op) {
            case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14: {
                bool run = true;
                if (op == 3 || op == 4 || op == 13 || op == 14) run = boolConst(bits(hi, 2, 8)) == bits(hi, 10, 1);
                if (op == 5 || op == 6) run = predicate == bool(bits(hi, 10, 1));
                if (run) {
                    uint32_t address = bits(lo, 0, 12), count = bits(lo, 12, 3), sequence = bits(lo, 16, 12);
                    for (uint32_t i = 0; i < count; i++) {
                        const uint32_t* w = code + (address + i) * 3;
                        if ((sequence >> (i * 2)) & 1) {
                            // Fetch.
                            uint32_t fop = bits(w[0], 0, 5);
                            bool predicated = bits(w[1], 31, 1);
                            if (predicated && predicate != bool(bits(w[2], 31, 1))) continue;
                            uint32_t dst = bits(w[0], 12, 6);
                            bool dstRel = bits(w[0], 18, 1);
                            uint32_t dstSwizzle = bits(w[1], 0, 12);
                            Vec value{};
                            if (fop == 0) {
                                bool mini = bits(w[1], 30, 1);
                                if (!mini) {
                                    uint32_t src = bits(w[0], 5, 6);
                                    bool srcRel = bits(w[0], 11, 1);
                                    float f = temp(src, srcRel)[bits(w[0], 30, 2)];
                                    lastIndex = uint32_t(int32_t(std::floor(f + (bits(w[1], 15, 1) ? 0.5f : 0.0f))));
                                }
                                auto e = elements.find(address + i);
                                if (e != elements.end()) {
                                    const VertexElementRef& el = e->second;
                                    uint32_t word = el.formatWord;
                                    uint32_t format = word & 0x3F;
                                    bool isSigned = word & 0x40, integer = word & 0x80, noZero = word & 0x100;
                                    uint32_t endian = (word >> 9) & 3;
                                    int32_t expAdjust = sbits(word, 11, 6);
                                    uint32_t address0 = el.offset + lastIndex * el.stride;
                                    uint32_t d[4] = {};
                                    const auto& buffer = buffers[el.buffer];
                                    for (int k = 0; k < 4; k++) {
                                        size_t at = size_t(address0) + size_t(k) * 4;
                                        if (at + 4 <= buffer.size()) {
                                            uint32_t v;
                                            std::memcpy(&v, buffer.data() + at, 4);
                                            d[k] = swap(v, endian);
                                        }
                                    }
                                    Vec x{};
                                    auto packed = [&](uint32_t v, uint32_t offset, uint32_t width) {
                                        float f;
                                        if (isSigned) {
                                            f = float(sbits(v, offset, width));
                                            if (!integer) f = noZero ? (f + 0.5f) * 2.0f / float((1u << width) - 1) : std::max(-1.0f, f / float((1u << (width - 1)) - 1));
                                        } else {
                                            f = float(bits(v, offset, width));
                                            if (!integer) f /= float((1u << width) - 1);
                                        }
                                        return f;
                                    };
                                    auto full = [&](uint32_t v) {
                                        if (isSigned) {
                                            float f = float(int32_t(v));
                                            if (!integer) f = noZero ? (f + 0.5f) / 2147483647.5f : f / 2147483647.0f;
                                            return f;
                                        }
                                        float f = float(v);
                                        return integer ? f : f / 4294967295.0f;
                                    };
                                    bool missing = false;
                                    switch (format) {
                                        case 6: x = {packed(d[0], 0, 8), packed(d[0], 8, 8), packed(d[0], 16, 8), packed(d[0], 24, 8)}; break;
                                        case 7: x = {packed(d[0], 0, 10), packed(d[0], 10, 10), packed(d[0], 20, 10), packed(d[0], 30, 2)}; break;
                                        case 16: x = {packed(d[0], 0, 11), packed(d[0], 11, 11), packed(d[0], 22, 10), 0}; break;
                                        case 17: x = {packed(d[0], 0, 10), packed(d[0], 10, 11), packed(d[0], 21, 11), 0}; break;
                                        case 25: x = {packed(d[0], 0, 16), packed(d[0], 16, 16), 0, 0}; break;
                                        case 26: x = {packed(d[0], 0, 16), packed(d[0], 16, 16), packed(d[1], 0, 16), packed(d[1], 16, 16)}; break;
                                        case 31: x = {halfToFloat(d[0] & 0xFFFF), halfToFloat(d[0] >> 16), 0, 0}; break;
                                        case 32: x = {halfToFloat(d[0] & 0xFFFF), halfToFloat(d[0] >> 16), halfToFloat(d[1] & 0xFFFF), halfToFloat(d[1] >> 16)}; break;
                                        case 33: x = {full(d[0]), 0, 0, 0}; break;
                                        case 34: x = {full(d[0]), full(d[1]), 0, 0}; break;
                                        case 35: x = {full(d[0]), full(d[1]), full(d[2]), full(d[3])}; break;
                                        case 36: case 37: case 38: case 57: {
                                            uint32_t n = format == 36 ? 1 : format == 37 ? 2 : format == 57 ? 3 : 4;
                                            for (uint32_t k = 0; k < n; k++) std::memcpy(&x[k], &d[k], 4);
                                            break;
                                        }
                                        default: missing = true; break;
                                    }
                                    if (missing) {
                                        x = {0, 0, 0, 1};
                                    } else if (expAdjust) {
                                        for (auto& f : x) f = std::ldexp(f, expAdjust);
                                    }
                                    if (!missing) {
                                        Vec y{};
                                        for (int k = 0; k < 4; k++) {
                                            uint32_t s = bits(word, 17 + k * 3, 3);
                                            y[size_t(k)] = s <= 3 ? x[s] : (s == 5 ? 1.0f : 0.0f);
                                        }
                                        x = y;
                                    }
                                    value = x;
                                }
                            } else if (fop == 1) {
                                uint32_t dim = bits(w[2], 14, 2), slot = bits(w[0], 20, 5);
                                auto t = textures.find(slot);
                                value = Vec{};
                                if ((dim == 0 || dim == 1) && t != textures.end()) {
                                    const Vec& src = temp(bits(w[0], 5, 6), bits(w[0], 11, 1));
                                    uint32_t srcSwizzle = bits(w[0], 26, 6);
                                    float u = src[srcSwizzle & 3], v = dim == 1 ? src[(srcSwizzle >> 2) & 3] : 0.5f;
                                    float w_ = float(t->second.width), h_ = float(t->second.height);
                                    if (bits(w[0], 25, 1)) {
                                        u /= w_;
                                        if (dim == 1) v /= h_;
                                    }
                                    u += float(sbits(w[2], 16, 5)) * 0.5f / w_;
                                    if (dim == 1) v += float(sbits(w[2], 21, 5)) * 0.5f / h_;
                                    int32_t x = std::clamp(int32_t(std::floor(u * w_)), 0, int32_t(t->second.width) - 1);
                                    int32_t y = std::clamp(int32_t(std::floor(v * h_)), 0, int32_t(t->second.height) - 1);
                                    const float* px = &t->second.texels[(size_t(y) * t->second.width + size_t(x)) * 4];
                                    value = {px[0], px[1], px[2], px[3]};
                                }
                            } else if (fop == 16 || fop == 17 || fop == 18 || fop == 19) {
                                value = Vec{};  // not modelled
                            } else {
                                continue;  // set LOD / gradients: no result
                            }
                            Vec& d = temp(dst, dstRel);
                            for (int k = 0; k < 4; k++) {
                                uint32_t s = (dstSwizzle >> (k * 3)) & 7;
                                if (s == 7) continue;
                                d[size_t(k)] = s <= 3 ? value[s] : (s == 5 ? 1.0f : 0.0f);
                            }
                            continue;
                        }

                        // ALU.
                        if (bits(w[1], 28, 1) && predicate != bool(bits(w[1], 27, 1))) continue;
                        uint32_t vdst = bits(w[0], 0, 6), sdst = bits(w[0], 8, 6);
                        bool vdstRel = bits(w[0], 6, 1), absConstants = bits(w[0], 7, 1), sdstRel = bits(w[0], 14, 1);
                        bool exportData = bits(w[0], 15, 1);
                        uint32_t vmask = bits(w[0], 16, 4), smask = bits(w[0], 20, 4);
                        bool vsat = bits(w[0], 24, 1), ssat = bits(w[0], 25, 1);
                        uint32_t sop = bits(w[0], 26, 6);
                        uint32_t swz[3] = {bits(w[1], 16, 8), bits(w[1], 8, 8), bits(w[1], 0, 8)};
                        bool neg[3] = {bool(bits(w[1], 26, 1)), bool(bits(w[1], 25, 1)), bool(bits(w[1], 24, 1))};
                        bool useA0 = bits(w[1], 29, 1), const1Rel = bits(w[1], 30, 1), const0Rel = bits(w[1], 31, 1);
                        uint32_t reg[3] = {bits(w[2], 16, 8), bits(w[2], 8, 8), bits(w[2], 0, 8)};
                        uint32_t vop = bits(w[2], 24, 5);
                        bool sel[3] = {bool(bits(w[2], 31, 1)), bool(bits(w[2], 30, 1)), bool(bits(w[2], 29, 1))};

                        auto addressed = [&](int i) {
                            if (i == 0) return const0Rel;
                            if (i == 1) return sel[0] ? const0Rel : const1Rel;
                            return (sel[0] && sel[1]) ? const0Rel : const1Rel;
                        };
                        auto rawOperand = [&](int i) -> Vec {
                            Vec v;
                            bool abs;
                            if (sel[i]) {
                                v = temp(reg[i] & 63, (reg[i] & 0x40) != 0);
                                abs = (reg[i] & 0x80) != 0;
                            } else {
                                v = floatConst(reg[i], addressed(i), useA0);
                                abs = absConstants;
                            }
                            if (abs)
                                for (auto& f : v) f = std::fabs(f);
                            if (neg[i])
                                for (auto& f : v) f = -f;
                            return v;
                        };
                        auto vecOperand = [&](int i) -> Vec {
                            Vec raw = rawOperand(i), v;
                            for (uint32_t k = 0; k < 4; k++) v[k] = raw[((swz[i] >> (k * 2)) + k) & 3];
                            return v;
                        };

                        uint32_t vResultMask = exportData ? (vmask & ~smask) : vmask;
                        uint32_t sResultMask = exportData ? (smask & ~vmask) : smask;
                        bool vecChangesState = (vop >= 20 && vop <= 27) || vop == 29;
                        Vec vr{};
                        bool newPredicate = predicate;
                        int32_t newA0 = a0;
                        bool kill = false;
                        if (vResultMask || vecChangesState) {
                            Vec a = vecOperand(0), b = vecOperand(1), c3 = vecOperand(2);
                            bool replicate = false;
                            switch (vop) {
                                case 0: for (int k = 0; k < 4; k++) vr[k] = a[k] + b[k]; break;
                                case 1: for (int k = 0; k < 4; k++) vr[k] = legacyMul(a[k], b[k]); break;
                                case 2: for (int k = 0; k < 4; k++) vr[k] = a[k] >= b[k] ? a[k] : b[k]; break;
                                case 3: for (int k = 0; k < 4; k++) vr[k] = a[k] < b[k] ? a[k] : b[k]; break;
                                case 4: for (int k = 0; k < 4; k++) vr[k] = float(a[k] == b[k]); break;
                                case 5: for (int k = 0; k < 4; k++) vr[k] = float(a[k] > b[k]); break;
                                case 6: for (int k = 0; k < 4; k++) vr[k] = float(a[k] >= b[k]); break;
                                case 7: for (int k = 0; k < 4; k++) vr[k] = float(a[k] != b[k]); break;
                                case 8: for (int k = 0; k < 4; k++) vr[k] = a[k] - std::floor(a[k]); break;
                                case 9: for (int k = 0; k < 4; k++) vr[k] = std::trunc(a[k]); break;
                                case 10: for (int k = 0; k < 4; k++) vr[k] = std::floor(a[k]); break;
                                case 11: for (int k = 0; k < 4; k++) vr[k] = legacyMul(a[k], b[k]) + c3[k]; break;
                                case 12: for (int k = 0; k < 4; k++) vr[k] = a[k] == 0.0f ? b[k] : c3[k]; break;
                                case 13: for (int k = 0; k < 4; k++) vr[k] = a[k] >= 0.0f ? b[k] : c3[k]; break;
                                case 14: for (int k = 0; k < 4; k++) vr[k] = a[k] > 0.0f ? b[k] : c3[k]; break;
                                case 15: vr[0] = legacyMul(a[0], b[0]) + legacyMul(a[1], b[1]) + legacyMul(a[2], b[2]) + legacyMul(a[3], b[3]); replicate = true; break;
                                case 16: vr[0] = legacyMul(a[0], b[0]) + legacyMul(a[1], b[1]) + legacyMul(a[2], b[2]); replicate = true; break;
                                case 17: vr[0] = legacyMul(a[0], b[0]) + legacyMul(a[1], b[1]) + c3[0]; replicate = true; break;
                                case 18: {
                                    float x = a[2], y = a[3], z = a[0];
                                    if (std::fabs(z) >= std::fabs(x) && std::fabs(z) >= std::fabs(y)) vr = {-y, z < 0 ? -x : x, z, z < 0 ? 5.0f : 4.0f};
                                    else if (std::fabs(y) >= std::fabs(x)) vr = {y < 0 ? -z : z, x, y, y < 0 ? 3.0f : 2.0f};
                                    else vr = {-y, x < 0 ? z : -z, x, x < 0 ? 1.0f : 0.0f};
                                    vr[2] *= 2.0f;
                                    break;
                                }
                                case 19:
                                    if (a[0] >= a[1] && a[0] >= a[2] && a[0] >= a[3]) vr[0] = a[0];
                                    else if (a[1] >= a[2] && a[1] >= a[3]) vr[0] = a[1];
                                    else vr[0] = a[2] >= a[3] ? a[2] : a[3];
                                    replicate = true;
                                    break;
                                case 20: case 21: case 22: case 23: {
                                    auto cmp = [&](float x) { return vop == 20 ? x == 0 : vop == 21 ? x != 0 : vop == 22 ? x > 0 : x >= 0; };
                                    newPredicate = a[3] == 0.0f && cmp(b[3]);
                                    vr[0] = (a[0] == 0.0f && cmp(b[0])) ? 0.0f : a[0] + 1.0f;
                                    replicate = true;
                                    break;
                                }
                                case 24: case 25: case 26: case 27: {
                                    bool any = false;
                                    for (int k = 0; k < 4; k++)
                                        any |= vop == 24 ? a[k] == b[k] : vop == 25 ? a[k] > b[k] : vop == 26 ? a[k] >= b[k] : a[k] != b[k];
                                    vr[0] = any ? 1.0f : 0.0f;
                                    kill |= any;
                                    replicate = true;
                                    break;
                                }
                                case 28: vr = {1.0f, legacyMul(a[1], b[1]), a[2], b[3]}; break;
                                case 29:
                                    newA0 = int32_t(std::floor(std::clamp(a[3], -256.0f, 255.0f) + 0.5f));
                                    for (int k = 0; k < 4; k++) vr[k] = a[k] >= b[k] ? a[k] : b[k];
                                    break;
                            }
                            if (replicate) vr[1] = vr[2] = vr[3] = vr[0];
                            if (vsat)
                                for (auto& f : vr) f = std::isnan(f) ? 0.0f : std::clamp(f, 0.0f, 1.0f);
                        }

                        // Scalar.
                        float sa = 0, sb = 0;
                        bool sopIsConst = sop >= 42 && sop <= 47;
                        if (sopIsConst) {
                            Vec cv = floatConst(reg[2], addressed(2), useA0);
                            uint32_t ti = (sop & 1) | (uint32_t(sel[2]) << 1) | (swz[2] & 0x3C);
                            Vec tv = temp(ti, false);
                            sa = cv[((swz[2] >> 6) + 3) & 3];
                            sb = tv[swz[2] & 3];
                            if (absConstants) sa = std::fabs(sa), sb = std::fabs(sb);
                            if (neg[2]) sa = -sa, sb = -sb;
                        } else {
                            Vec raw = rawOperand(2);
                            sa = raw[((swz[2] >> 6) + 3) & 3];
                            sb = raw[swz[2] & 3];
                        }
                        float ps = previousScalar;
                        switch (sop) {
                            case 0: case 44: case 45: ps = sa + sb; break;
                            case 1: ps = sa + previousScalar; break;
                            case 2: case 42: case 43: ps = legacyMul(sa, sb); break;
                            case 3: ps = legacyMul(sa, previousScalar); break;
                            case 4:
                                ps = (previousScalar == -FLT_MAX || !std::isfinite(previousScalar) || !std::isfinite(sb) || sb <= 0.0f)
                                         ? -FLT_MAX : legacyMul(sa, previousScalar);
                                break;
                            case 5: ps = sa >= sb ? sa : sb; break;
                            case 6: ps = sa < sb ? sa : sb; break;
                            case 7: ps = float(sa == 0.0f); break;
                            case 8: ps = float(sa > 0.0f); break;
                            case 9: ps = float(sa >= 0.0f); break;
                            case 10: ps = float(sa != 0.0f); break;
                            case 11: ps = sa - std::floor(sa); break;
                            case 12: ps = std::trunc(sa); break;
                            case 13: ps = std::floor(sa); break;
                            case 14: ps = std::exp2(sa); break;
                            case 15: ps = std::log2(sa); if (ps == -INFINITY) ps = -FLT_MAX; break;
                            case 16: ps = std::log2(sa); break;
                            case 17: ps = 1.0f / sa; if (std::isinf(ps)) ps = ps < 0 ? -FLT_MAX : FLT_MAX; break;
                            case 18: ps = 1.0f / sa; if (std::isinf(ps)) ps = ps < 0 ? -0.0f : 0.0f; break;
                            case 19: ps = 1.0f / sa; break;
                            case 20: ps = 1.0f / std::sqrt(sa); if (std::isinf(ps)) ps = ps < 0 ? -FLT_MAX : FLT_MAX; break;
                            case 21: ps = 1.0f / std::sqrt(sa); if (std::isinf(ps)) ps = ps < 0 ? -0.0f : 0.0f; break;
                            case 22: ps = 1.0f / std::sqrt(sa); break;
                            case 23: newA0 = int32_t(std::floor(std::clamp(sa, -256.0f, 255.0f) + 0.5f)); ps = sa >= sb ? sa : sb; break;
                            case 24: newA0 = int32_t(std::floor(std::clamp(sa, -256.0f, 255.0f))); ps = sa >= sb ? sa : sb; break;
                            case 25: case 46: case 47: ps = sa - sb; break;
                            case 26: ps = sa - previousScalar; break;
                            case 27: newPredicate = sa == 0.0f; ps = newPredicate ? 0.0f : 1.0f; break;
                            case 28: newPredicate = sa != 0.0f; ps = newPredicate ? 0.0f : 1.0f; break;
                            case 29: newPredicate = sa > 0.0f; ps = newPredicate ? 0.0f : 1.0f; break;
                            case 30: newPredicate = sa >= 0.0f; ps = newPredicate ? 0.0f : 1.0f; break;
                            case 31: newPredicate = sa == 1.0f; ps = newPredicate ? 0.0f : (sa == 0.0f ? 1.0f : sa); break;
                            case 32: { float t = sa - 1.0f; newPredicate = t <= 0.0f; ps = newPredicate ? 0.0f : t; break; }
                            case 33: newPredicate = false; ps = FLT_MAX; break;
                            case 34: newPredicate = sa == 0.0f; ps = newPredicate ? 0.0f : sa; break;
                            case 35: ps = float(sa == 0.0f); kill |= ps != 0; break;
                            case 36: ps = float(sa > 0.0f); kill |= ps != 0; break;
                            case 37: ps = float(sa >= 0.0f); kill |= ps != 0; break;
                            case 38: ps = float(sa != 0.0f); kill |= ps != 0; break;
                            case 39: ps = float(sa == 1.0f); kill |= ps != 0; break;
                            case 40: ps = std::sqrt(sa); break;
                            case 48: ps = std::sin(sa); break;
                            case 49: ps = std::cos(sa); break;
                            default: break;  // retain_prev
                        }
                        previousScalar = ps;
                        float sr = ssat ? (std::isnan(ps) ? 0.0f : std::clamp(ps, 0.0f, 1.0f)) : ps;
                        predicate = newPredicate;
                        a0 = newA0;
                        if (kill && pixel) killed = true;

                        if (exportData) {
                            Vec e{};
                            uint32_t c1 = vmask & smask;
                            uint32_t c0 = sdstRel ? (0xF & ~(vmask | smask)) : 0;
                            uint32_t m = vResultMask | sResultMask | c1 | c0;
                            for (int k = 0; k < 4; k++) {
                                uint32_t bit = 1u << k;
                                e[size_t(k)] = (vResultMask & bit) ? vr[k] : (sResultMask & bit) ? sr : (c1 & bit) ? 1.0f : 0.0f;
                            }
                            writeExport(vdst, m, e);
                        } else {
                            if (vResultMask) {
                                Vec& d = temp(vdst, vdstRel);
                                for (int k = 0; k < 4; k++)
                                    if (vResultMask & (1u << k)) d[size_t(k)] = vr[k];
                            }
                            if (sResultMask) {
                                Vec& d = temp(sdst, sdstRel);
                                for (int k = 0; k < 4; k++)
                                    if (sResultMask & (1u << k)) d[size_t(k)] = sr;
                            }
                        }
                    }
                    if (op == 2 || op == 4 || op == 6 || op == 14) return;  // end (only when the exec ran)
                }
                break;
            }
            case 7: {  // loop start
                uint32_t id = bits(lo, 16, 5);
                uint32_t after = bits(lo, 0, 13);
                bool repeat = bits(lo, 13, 1);
                if (loopDepth >= 4) {
                    next = after;
                    break;
                }
                Loop& l = loopStack[loopDepth];
                l.constant = loops[id];
                if (!repeat) l.iterator = 0;
                if (l.iterator >= bits(l.constant, 0, 8)) {
                    next = after;
                    break;
                }
                loopDepth++;
                break;
            }
            case 8: {  // loop end
                if (!loopDepth) break;
                Loop& l = loopStack[loopDepth - 1];
                l.iterator++;
                bool predicatedBreak = bits(lo, 21, 1);
                bool condition = bits(hi, 10, 1);
                if (l.iterator < bits(l.constant, 0, 8) && (!predicatedBreak || condition != predicate)) next = bits(lo, 0, 13);
                else loopDepth--;
                break;
            }
            case 9: {  // call
                bool unconditional = bits(lo, 13, 1), predicated = bits(lo, 14, 1);
                bool condition = bits(hi, 10, 1);
                bool take = unconditional || (predicated ? predicate == condition : boolConst(bits(hi, 2, 8)) == condition);
                if (take && callDepth < 4) {
                    callStack[callDepth++] = cf + 1;
                    next = bits(lo, 0, 13);
                }
                break;
            }
            case 10:  // return
                if (callDepth) next = callStack[--callDepth];
                break;
            case 11: {  // jump
                bool unconditional = bits(lo, 13, 1), predicated = bits(lo, 14, 1);
                bool condition = bits(hi, 10, 1);
                bool take = unconditional || (predicated ? predicate == condition : boolConst(bits(hi, 2, 8)) == condition);
                if (take) next = bits(lo, 0, 13);
                break;
            }
            default:
                break;
        }
        cf = next;
    }
}

}  // namespace xref
