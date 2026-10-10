#include "xenos_asm.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace xasm {

namespace {

uint32_t component(char c) {
    switch (c) {
        case 'x': case 'r': return 0;
        case 'y': case 'g': return 1;
        case 'z': case 'b': return 2;
        case 'w': case 'a': return 3;
        default: throw std::runtime_error(std::string("bad component ") + c);
    }
}

// Places `value` (masked to `bits`) at bit `shift`.
constexpr uint32_t field(uint32_t value, uint32_t shift, uint32_t bits) {
    return (value & ((bits == 32 ? 0u : (1u << bits)) - 1u)) << shift;
}
constexpr uint64_t field64(uint64_t value, uint32_t shift, uint32_t bits) {
    return (value & ((uint64_t(1) << bits) - 1)) << shift;
}

uint32_t srcRegByte(const Src& s) {
    if (!s.temp) return s.reg & 0xFF;
    return (s.reg & 0x3F) | (s.relative ? 0x40u : 0u) | (s.abs ? 0x80u : 0u);
}

}  // namespace

uint32_t aluSwizzle(const char* s) {
    size_t n = std::strlen(s);
    if (n == 0 || n > 4) throw std::runtime_error("bad swizzle");
    uint32_t r = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t comp = component(s[i < n ? i : n - 1]);
        r |= ((comp - i) & 3) << (i * 2);
    }
    return r;
}

uint32_t scalarSwizzle(char a, char b) {
    // a = (3 + swizzle[6:7]) & 3, b = (0 + swizzle[0:1]) & 3.
    return (((component(a) - 3) & 3) << 6) | (component(b) & 3);
}

uint32_t fetchDstSwizzle(const char* s) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 4; i++) {
        char ch = s[i];
        uint32_t v;
        switch (ch) {
            case '0': v = 4; break;
            case '1': v = 5; break;
            case '_': v = 7; break;
            default: v = component(ch); break;
        }
        r |= v << (i * 3);
    }
    return r;
}

uint32_t fetchSrcSwizzle(const char* s) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 3 && s[i]; i++) r |= component(s[i]) << (i * 2);
    return r;
}

uint32_t mask(const char* s) {
    uint32_t m = 0;
    for (const char* p = s; *p; p++) m |= 1u << component(*p);
    return m;
}

Src r(uint32_t reg, const char* swz, bool negate, bool abs, bool relative) {
    Src s;
    s.reg = reg;
    s.temp = true;
    s.swizzle = aluSwizzle(swz);
    s.negate = negate;
    s.abs = abs;
    s.relative = relative;
    return s;
}

Src c(uint32_t reg, const char* swz, bool negate) {
    Src s;
    s.reg = reg;
    s.temp = false;
    s.swizzle = aluSwizzle(swz);
    s.negate = negate;
    return s;
}

Alu& Alu::v(uint32_t op, uint32_t dst, const char* m, Src a, Src b, Src c3) {
    vop = op;
    vdst = dst;
    vmask = mask(m);
    src[0] = a;
    src[1] = b;
    src[2] = c3;
    return *this;
}

Alu& Alu::s(uint32_t op, uint32_t dst, const char* m, Src a) {
    sop = op;
    sdst = dst;
    smask = mask(m);
    src[2] = a;
    return *this;
}

Alu& Alu::sc(uint32_t baseOp, uint32_t dst, const char* m, uint32_t constReg, char a, uint32_t tempReg, char b) {
    // The temporary's index is built from opcode bit 0, src3_sel and swizzle bits 2-5.
    sop = (baseOp & ~1u) | (tempReg & 1);
    sdst = dst;
    smask = mask(m);
    Src s3;
    s3.reg = constReg;
    s3.temp = ((tempReg >> 1) & 1) != 0;
    s3.swizzle = (((component(a) - 3) & 3) << 6) | (tempReg & 0x3C) | (component(b) & 3);
    s3.negate = false;
    src[2] = s3;
    return *this;
}

Alu& Alu::exp(uint32_t exportReg) {
    exportData = true;
    vdst = exportReg;
    return *this;
}

Alu& Alu::sat(bool vector, bool scalar) {
    vsat = vector;
    ssat = scalar;
    return *this;
}

Alu& Alu::pred(bool condition) {
    predicated = true;
    predCondition = condition;
    return *this;
}

Alu& Alu::absConst() {
    absConstants = true;
    return *this;
}

Alu& Alu::rel(bool c0, bool c1, bool a0) {
    const0Rel = c0;
    const1Rel = c1;
    constAddressA0 = a0;
    return *this;
}

Alu& Alu::vrel() {
    vdstRel = true;
    return *this;
}

Alu& Alu::srel() {
    sdstRel = true;
    return *this;
}

Instr Alu::encode() const {
    Instr w{};
    w[0] = field(vdst, 0, 6) | field(vdstRel, 6, 1) | field(absConstants, 7, 1) | field(sdst, 8, 6) | field(sdstRel, 14, 1) |
           field(exportData, 15, 1) | field(vmask, 16, 4) | field(smask, 20, 4) | field(vsat, 24, 1) | field(ssat, 25, 1) |
           field(sop, 26, 6);
    w[1] = field(src[2].swizzle, 0, 8) | field(src[1].swizzle, 8, 8) | field(src[0].swizzle, 16, 8) | field(src[2].negate, 24, 1) |
           field(src[1].negate, 25, 1) | field(src[0].negate, 26, 1) | field(predCondition, 27, 1) | field(predicated, 28, 1) |
           field(constAddressA0, 29, 1) | field(const1Rel, 30, 1) | field(const0Rel, 31, 1);
    w[2] = field(srcRegByte(src[2]), 0, 8) | field(srcRegByte(src[1]), 8, 8) | field(srcRegByte(src[0]), 16, 8) | field(vop, 24, 5) |
           field(src[2].temp, 29, 1) | field(src[1].temp, 30, 1) | field(src[0].temp, 31, 1);
    return w;
}

Instr VFetch::encode() const {
    Instr w{};
    w[0] = field(uint32_t(Fop::VFetch), 0, 5) | field(src, 5, 6) | field(srcRel, 11, 1) | field(dst, 12, 6) | field(dstRel, 18, 1) |
           field(1, 19, 1) | field(fetchConstant / 3, 20, 5) | field(fetchConstant % 3, 25, 2) | field(0, 27, 3) |
           field(srcComponent, 30, 2);
    w[1] = field(dstSwizzle, 0, 12) | field(isSigned, 12, 1) | field(integer, 13, 1) | field(noZero, 14, 1) | field(rounded, 15, 1) |
           field(format, 16, 6) | field(uint32_t(expAdjust), 24, 6) | field(mini, 30, 1) | field(predicated, 31, 1);
    w[2] = field(stride, 0, 8) | field(uint32_t(offset), 8, 23) | field(predCondition, 31, 1);
    return w;
}

Instr TFetch::encode() const {
    Instr w{};
    w[0] = field(uint32_t(op), 0, 5) | field(src, 5, 6) | field(srcRel, 11, 1) | field(dst, 12, 6) | field(dstRel, 18, 1) |
           field(fetchValidOnly, 19, 1) | field(slot, 20, 5) | field(denorm, 25, 1) | field(srcSwizzle, 26, 6);
    w[1] = field(dstSwizzle, 0, 12) | field(magFilter, 12, 2) | field(minFilter, 14, 2) | field(mipFilter, 16, 2) |
           field(anisoFilter, 18, 3) | field(7, 21, 3) | field(volMagFilter, 24, 2) | field(volMinFilter, 26, 2) |
           field(useCompLod, 28, 1) | field(useRegLod, 29, 1) | field(predicated, 31, 1);
    w[2] = field(useRegGradients, 0, 1) | field(1, 1, 1) | field(uint32_t(lodBias), 2, 7) | field(uint32_t(dim), 14, 2) |
           field(uint32_t(offsetX), 16, 5) | field(uint32_t(offsetY), 21, 5) | field(uint32_t(offsetZ), 26, 5) |
           field(predCondition, 31, 1);
    return w;
}

// Control flow instructions are 48 bits: exec: address 0-11, count 12-14, yield 15, sequence
// 16-27, vertex cache 28-33, bool address 34-41 (cond exec) or predicate clean 41, condition 42,
// address mode 43, opcode 44-47.
uint32_t Program::execPart(Cf op, const std::vector<Op>& ops, uint64_t extraBits) {
    uint64_t sequence = 0;
    for (size_t i = 0; i < ops.size(); i++) sequence |= uint64_t(ops[i].fetch ? 1 : 0) << (i * 2);
    uint64_t bits = field64(ops.size(), 12, 3) | field64(sequence, 16, 12) | extraBits | field64(uint32_t(op), 44, 4);
    blocks_.push_back(ops);
    cf_.push_back({op, bits, int32_t(blocks_.size() - 1)});
    return uint32_t(cf_.size() - 1);
}

static std::vector<std::vector<Op>> split(const std::vector<Op>& ops) {
    std::vector<std::vector<Op>> parts;
    for (size_t i = 0; i < ops.size(); i += 6) parts.emplace_back(ops.begin() + i, ops.begin() + std::min(ops.size(), i + 6));
    if (parts.empty()) parts.emplace_back();
    return parts;
}

uint32_t Program::exec(const std::vector<Op>& ops, bool end) {
    auto parts = split(ops);
    uint32_t first = label();
    for (size_t i = 0; i < parts.size(); i++) execPart(end && i + 1 == parts.size() ? Cf::ExecEnd : Cf::Exec, parts[i], 0);
    return first;
}

uint32_t Program::condExec(const std::vector<Op>& ops, uint32_t boolIndex, bool condition, bool end, bool clean) {
    // A conditional exec with more than 6 instructions becomes several with the same condition.
    auto parts = split(ops);
    uint32_t first = label();
    for (size_t i = 0; i < parts.size(); i++) {
        bool last = i + 1 == parts.size();
        Cf op = clean ? (end && last ? Cf::CondExecPredCleanEnd : Cf::CondExecPredClean) : (end && last ? Cf::CondExecEnd : Cf::CondExec);
        execPart(op, parts[i], field64(boolIndex, 34, 8) | field64(condition, 42, 1));
    }
    return first;
}

uint32_t Program::condExecPred(const std::vector<Op>& ops, bool condition, bool end) {
    auto parts = split(ops);
    uint32_t first = label();
    for (size_t i = 0; i < parts.size(); i++) {
        bool last = i + 1 == parts.size();
        execPart(end && last ? Cf::CondExecPredEnd : Cf::CondExecPred, parts[i], field64(condition, 42, 1));
    }
    return first;
}

// loop_start: address 0-12 (after the loop end), repeat 13, loop id 16-20.
uint32_t Program::loopStart(uint32_t loopId, bool repeat) {
    cf_.push_back({Cf::LoopStart, field64(repeat, 13, 1) | field64(loopId, 16, 5) | field64(uint32_t(Cf::LoopStart), 44, 4)});
    return label() - 1;
}

// loop_end: address 0-12 (after the loop start), loop id 16-20, predicated break 21, condition 42.
uint32_t Program::loopEnd(uint32_t loopId, uint32_t startIndex, bool predicatedBreak, bool condition) {
    cf_.push_back({Cf::LoopEnd, field64(startIndex + 1, 0, 13) | field64(loopId, 16, 5) | field64(predicatedBreak, 21, 1) |
                                    field64(condition, 42, 1) | field64(uint32_t(Cf::LoopEnd), 44, 4)});
    uint32_t endIndex = label() - 1;
    cf_[startIndex].bits = (cf_[startIndex].bits & ~uint64_t(0x1FFF)) | field64(endIndex + 1, 0, 13);
    return endIndex;
}

// cond_jmp: address 0-12, unconditional 13, predicated 14, direction 33, bool address 34-41,
// condition 42. cond_call: the same without the direction.
uint32_t Program::jump(uint32_t target, bool unconditional, bool predicated, uint32_t boolIndex, bool condition) {
    cf_.push_back({Cf::CondJmp, field64(target, 0, 13) | field64(unconditional, 13, 1) | field64(predicated, 14, 1) |
                                    field64(boolIndex, 34, 8) | field64(condition, 42, 1) | field64(uint32_t(Cf::CondJmp), 44, 4)});
    return label() - 1;
}

uint32_t Program::call(uint32_t target, bool unconditional, bool predicated, uint32_t boolIndex, bool condition) {
    cf_.push_back({Cf::CondCall, field64(target, 0, 13) | field64(unconditional, 13, 1) | field64(predicated, 14, 1) |
                                     field64(boolIndex, 34, 8) | field64(condition, 42, 1) | field64(uint32_t(Cf::CondCall), 44, 4)});
    return label() - 1;
}

uint32_t Program::ret() {
    cf_.push_back({Cf::Return, field64(uint32_t(Cf::Return), 44, 4)});
    return label() - 1;
}

// alloc: size 0-2, type 41-42.
uint32_t Program::alloc(uint32_t type, uint32_t size) {
    cf_.push_back({Cf::Alloc, field64(size, 0, 3) | field64(type, 41, 2) | field64(uint32_t(Cf::Alloc), 44, 4)});
    return label() - 1;
}

uint32_t Program::nop() {
    cf_.push_back({Cf::Nop, 0});
    return label() - 1;
}

uint32_t Program::markVsFetchDone() {
    cf_.push_back({Cf::MarkVsFetchDone, field64(uint32_t(Cf::MarkVsFetchDone), 44, 4)});
    return label() - 1;
}

void Program::patchTarget(uint32_t cfIndex, uint32_t target) {
    cf_[cfIndex].bits = (cf_[cfIndex].bits & ~uint64_t(0x1FFF)) | field64(target, 0, 13);
}

std::vector<uint32_t> Program::assemble() const {
    std::vector<CfInstr> cf = cf_;
    if (cf.size() % 2) cf.push_back({Cf::Nop, 0});
    uint32_t triples = uint32_t(cf.size() / 2);
    // Instruction memory follows the control flow.
    std::vector<uint32_t> blockAddress(blocks_.size());
    uint32_t next = triples;
    for (size_t b = 0; b < blocks_.size(); b++) {
        blockAddress[b] = next;
        next += uint32_t(blocks_[b].size());
    }
    std::vector<uint32_t> out(size_t(next) * 3, 0);
    for (uint32_t t = 0; t < triples; t++) {
        uint64_t a = cf[t * 2].bits, b = cf[t * 2 + 1].bits;
        if (cf[t * 2].block >= 0) a |= field64(blockAddress[size_t(cf[t * 2].block)], 0, 12);
        if (cf[t * 2 + 1].block >= 0) b |= field64(blockAddress[size_t(cf[t * 2 + 1].block)], 0, 12);
        out[t * 3 + 0] = uint32_t(a);
        out[t * 3 + 1] = uint32_t(a >> 32) | (uint32_t(b) << 16);
        out[t * 3 + 2] = uint32_t(b >> 16);
    }
    for (size_t b = 0; b < blocks_.size(); b++)
        for (size_t i = 0; i < blocks_[b].size(); i++)
            for (int k = 0; k < 3; k++) out[(blockAddress[b] + i) * 3 + k] = blocks_[b][i].code[k];
    return out;
}

std::vector<uint8_t> toBigEndian(const std::vector<uint32_t>& dwords) {
    std::vector<uint8_t> b;
    b.reserve(dwords.size() * 4);
    for (uint32_t v : dwords) {
        b.push_back(uint8_t(v >> 24));
        b.push_back(uint8_t(v >> 16));
        b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v));
    }
    return b;
}

}  // namespace xasm
