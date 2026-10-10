// A small Xenos (Xbox 360 GPU) microcode assembler for the tests.
//
// Encodings are written out bit by bit from the ReXGlue SDK's graphics/format/ucode.h (Xenia's
// layout); the "assembler" test checks them against XenosRecomp's bitfield structs, which the
// translator decodes with, so a mistake in either shows up.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace xasm {

using Instr = std::array<uint32_t, 3>;

enum Vop : uint32_t {
    ADDv = 0, MULv, MAXv, MINv, SEQv, SGTv, SGEv, SNEv, FRCv, TRUNCv, FLOORv, MADv, CNDEQv, CNDGEv, CNDGTv,
    DP4v, DP3v, DP2ADDv, CUBEv, MAX4v, SETP_EQ_PUSHv, SETP_NE_PUSHv, SETP_GT_PUSHv, SETP_GE_PUSHv,
    KILLEQv, KILLGTv, KILLGEv, KILLNEv, DSTv, MAXAv
};

enum Sop : uint32_t {
    ADDs = 0, ADD_PREVs, MULs, MUL_PREVs, MUL_PREV2s, MAXs, MINs, SEQs, SGTs, SGEs, SNEs, FRACs, TRUNCs, FLOORs,
    EXP_IEEE, LOG_CLAMP, LOG_IEEE, RECIP_CLAMP, RECIP_FF, RECIP_IEEE, RECIPSQ_CLAMP, RECIPSQ_FF, RECIPSQ_IEEE,
    MAXAs, MAXA_FLOORs, SUBs, SUB_PREVs, PRED_SETEs, PRED_SETNEs, PRED_SETGTs, PRED_SETGTEs, PRED_SET_INVs,
    PRED_SET_POPs, PRED_SET_CLRs, PRED_SET_RESTOREs, KILLEs, KILLGTs, KILLGTEs, KILLNEs, KILLONEs, SQRT_IEEE,
    MUL_CONST_0 = 42, MUL_CONST_1, ADD_CONST_0, ADD_CONST_1, SUB_CONST_0, SUB_CONST_1, SIN, COS, RETAIN_PREV
};

enum class Fop : uint32_t {
    VFetch = 0, TFetch = 1, GetBCF = 16, GetCompTexLOD = 17, GetGradients = 18, GetWeights = 19,
    SetTexLOD = 24, SetGradientsH = 25, SetGradientsV = 26
};

enum class Dim : uint32_t { D1 = 0, D2 = 1, D3 = 2, Cube = 3 };

// Vertex formats (xenos::VertexFormat).
enum VFormat : uint32_t {
    F_8_8_8_8 = 6, F_2_10_10_10 = 7, F_10_11_11 = 16, F_11_11_10 = 17, F_16_16 = 25, F_16_16_16_16 = 26,
    F_16_16_FLOAT = 31, F_16_16_16_16_FLOAT = 32, F_32 = 33, F_32_32 = 34, F_32_32_32_32 = 35, F_32_FLOAT = 36,
    F_32_32_FLOAT = 37, F_32_32_32_32_FLOAT = 38, F_32_32_32_FLOAT = 57
};

// ALU swizzle: absolute components ("wzyx", or fewer letters: the last one repeats) to the
// relative 8-bit encoding.
uint32_t aluSwizzle(const char* s);
// Scalar operand components a and b (letters) to the third source's swizzle.
uint32_t scalarSwizzle(char a, char b);
// Fetch destination swizzle: per component x y z w 0 1 or _ (keep).
uint32_t fetchDstSwizzle(const char* s);
// Write mask from letters ("xz").
uint32_t mask(const char* s);

struct Src {
    uint32_t reg = 0;
    bool temp = true;
    uint32_t swizzle = 0;  // relative encoding (aluSwizzle)
    bool negate = false;
    bool abs = false;      // temporaries only (constants use the instruction's absConstants)
    bool relative = false; // temporaries only: + aL
};
Src r(uint32_t reg, const char* swz = "xyzw", bool negate = false, bool abs = false, bool relative = false);
Src c(uint32_t reg, const char* swz = "xyzw", bool negate = false);

// One ALU instruction: a vector and a scalar operation issued together. The scalar operation
// reads the third source.
struct Alu {
    uint32_t vop = MAXv, sop = RETAIN_PREV;
    uint32_t vdst = 0, sdst = 0, vmask = 0, smask = 0;
    bool vdstRel = false, sdstRel = false, exportData = false, vsat = false, ssat = false;
    bool absConstants = false, predicated = false, predCondition = false;
    bool constAddressA0 = false, const0Rel = false, const1Rel = false;
    Src src[3];

    // Vector op writing r<dst>.<mask> (or the export register with exp()).
    Alu& v(uint32_t op, uint32_t dst, const char* m, Src a, Src b = {}, Src c3 = {});
    // Scalar op writing r<dst>.<mask> from the third source.
    Alu& s(uint32_t op, uint32_t dst, const char* m, Src a);
    // mulsc / addsc / subsc: constant c<constReg>.<a> with temporary r<tempReg>.<b>.
    Alu& sc(uint32_t baseOp, uint32_t dst, const char* m, uint32_t constReg, char a, uint32_t tempReg, char b);
    Alu& exp(uint32_t exportReg);  // export: both operations write the export register
    Alu& sat(bool vector, bool scalar);
    Alu& pred(bool condition);
    Alu& absConst();
    Alu& rel(bool c0, bool c1, bool a0);
    Alu& vrel();
    Alu& srel();
    Instr encode() const;
};

struct VFetch {
    uint32_t dst = 0, dstSwizzle = 0x688;  // xyzw
    uint32_t src = 0, srcComponent = 0;
    bool srcRel = false, dstRel = false;
    uint32_t fetchConstant = 95;           // 0-95
    uint32_t format = 0;
    bool isSigned = false, integer = false, noZero = false, rounded = false, mini = false;
    int32_t expAdjust = 0;
    uint32_t stride = 0;                   // dwords
    int32_t offset = 0;                    // dwords
    bool predicated = false, predCondition = false;
    Instr encode() const;
};

struct TFetch {
    Fop op = Fop::TFetch;
    uint32_t dst = 0, dstSwizzle = 0x688;
    uint32_t src = 0, srcSwizzle = 0x24;   // 3 components, 2 bits each (xyz)
    bool srcRel = false, dstRel = false;
    uint32_t slot = 0;
    Dim dim = Dim::D2;
    uint32_t magFilter = 3, minFilter = 3, mipFilter = 3, anisoFilter = 7, volMagFilter = 3, volMinFilter = 3;
    bool useCompLod = true, useRegLod = false, useRegGradients = false, denorm = false, fetchValidOnly = true;
    int32_t lodBias = 0;                   // 1/16 units, -64..63
    int32_t offsetX = 0, offsetY = 0, offsetZ = 0;  // half texels, -16..15
    bool predicated = false, predCondition = false;
    Instr encode() const;
};
// Fetch source swizzle from letters (up to 3).
uint32_t fetchSrcSwizzle(const char* s);

// Control flow.
enum class Cf : uint32_t {
    Nop = 0, Exec, ExecEnd, CondExec, CondExecEnd, CondExecPred, CondExecPredEnd, LoopStart, LoopEnd, CondCall,
    Return, CondJmp, Alloc, CondExecPredClean, CondExecPredCleanEnd, MarkVsFetchDone
};

struct Op {
    Instr code;
    bool fetch;
};

// A program: control flow instructions and the instructions their execs run. Instruction
// addresses are resolved when assembling (they follow the control flow).
class Program {
public:
    // Exec blocks (split into execs of at most 6 instructions; only the last one ends).
    // kind: Exec (end = ExecEnd), CondExec (bool), CondExecPred, CondExecPredClean.
    uint32_t exec(const std::vector<Op>& ops, bool end = false);
    uint32_t condExec(const std::vector<Op>& ops, uint32_t boolIndex, bool condition, bool end = false, bool clean = false);
    uint32_t condExecPred(const std::vector<Op>& ops, bool condition, bool end = false);
    uint32_t loopStart(uint32_t loopId, bool repeat = false);       // returns its index; patched by loopEnd
    uint32_t loopEnd(uint32_t loopId, uint32_t startIndex, bool predicatedBreak = false, bool condition = false);
    // Jumps and calls take a target index; use label() / patch() for forward targets.
    uint32_t jump(uint32_t target, bool unconditional, bool predicated, uint32_t boolIndex, bool condition);
    uint32_t call(uint32_t target, bool unconditional, bool predicated, uint32_t boolIndex, bool condition);
    uint32_t ret();
    uint32_t alloc(uint32_t type, uint32_t size);  // 1 position, 2 interpolators / colours, 3 memory
    uint32_t nop();
    uint32_t markVsFetchDone();
    uint32_t label() const { return uint32_t(cf_.size()); }
    void patchTarget(uint32_t cfIndex, uint32_t target);

    std::vector<uint32_t> assemble() const;  // dwords, host order

private:
    struct CfInstr {
        Cf op;
        uint64_t bits;          // without address for execs
        int32_t block = -1;     // execs: index into blocks_
    };
    std::vector<CfInstr> cf_;
    std::vector<std::vector<Op>> blocks_;
    uint32_t execPart(Cf op, const std::vector<Op>& ops, uint64_t extraBits);
};

// Big-endian bytes of dwords.
std::vector<uint8_t> toBigEndian(const std::vector<uint32_t>& dwords);

}  // namespace xasm
