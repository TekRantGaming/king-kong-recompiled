#pragma once

#include "shader.h"
#include "shader_code.h"

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

// King Kong: the recompiler no longer reads a container itself. The game's 2005 containers are
// parsed by the kkshaders container adapter, which fills this in (so does the raw microcode path
// for shaders that come without a container, such as the emulator's shader cache records).
struct RecompilerConstant
{
    std::string name;
    RegisterSet registerSet = RegisterSet::Float4;
    uint32_t registerIndex = 0;
    uint32_t registerCount = 0;
};

// A vertex shader input: the vfetch instruction at `address` (in 12-byte instruction units)
// reads the declaration element with this usage. Its index in the list is its binding index.
struct RecompilerVertexElement
{
    uint32_t address = 0;
    DeclUsage usage = DeclUsage::Position;
    uint32_t usageIndex = 0;
};

// Vertex shader: export register `reg` carries this semantic. Pixel shader: register r<reg>
// starts with this semantic's interpolated value.
struct RecompilerInterpolator
{
    DeclUsage usage = DeclUsage::TexCoord;
    uint32_t usageIndex = 0;
    uint32_t reg = 0;
};

struct RecompilerInput
{
    bool isPixelShader = false;
    std::vector<uint32_t> ucode;                 // microcode dwords, host byte order
    std::vector<RecompilerConstant> constants;   // the D3DX constant table (names for comments)
    std::map<uint32_t, std::array<uint32_t, 4>> floatLiterals; // stage-relative register -> value
    std::map<uint32_t, uint32_t> loopLiterals;   // loop constant 0-31 -> packed value
    std::map<uint32_t, bool> boolLiterals;       // bool constant 0-255 -> value
    // Vertex fetch: binding mode (the vfetch instructions are unpatched templates and every one
    // has an element here) or, when rawVertexFetch is set, instruction mode (the instructions
    // carry their real format, offset and stride, as in patched microcode).
    std::vector<RecompilerVertexElement> vertexElements;
    bool rawVertexFetch = false;
    std::vector<RecompilerInterpolator> interpolators;
    // Raw linkage (no container): vertex export register k is TEXCOORDk, pixel registers r0-r15
    // start as TEXCOORD0-15.
    bool rawInterpolators = false;
    uint32_t pixelOutputs = 0;                   // outputs to declare even if never written
    int32_t paramGenRegister = -1;               // pixel: register that receives the pixel position
    // Render scale aware code (kkshaders RenderScaleAware): the pixel position and the sizes
    // read with GetDimensions are brought back to guest units through the KKScaleConstants
    // buffer (b3). Off: the output is byte for byte what it was before this existed.
    bool renderScaleAware = false;
};

// What the backend binds for a shader (see kkshaders abi.h).
struct RecompilerSampler
{
    uint32_t slot = 0;                // texture fetch constant 0-31
    TextureDimension dimension = TextureDimension::Texture2D;
    uint32_t magFilter = 3, minFilter = 3, mipFilter = 3; // 3 = from the fetch constant
    uint32_t anisoFilter = 7, volMagFilter = 3, volMinFilter = 3; // 7 / 3 = from the fetch constant
};

struct RecompilerVertexBinding
{
    bool raw = false;                 // instruction mode
    uint32_t element = 0;             // binding mode: index into vertexElements
    uint32_t fetchConstant = 0;       // instruction mode: vertex fetch constant 0-95
};

struct RecompilerTexture
{
    uint32_t slot = 0;
    TextureDimension dimension = TextureDimension::Texture2D;
};

struct ShaderRecompiler : StringBuffer
{
    uint32_t indentation = 0;
    bool isPixelShader = false;
    const RecompilerInput* input = nullptr;

    // Results besides the HLSL text.
    std::string error;                // empty when recompile() succeeded
    std::vector<std::string> warnings;
    std::vector<RecompilerSampler> samplerBindings;
    std::vector<RecompilerVertexBinding> vertexBindings;
    std::vector<RecompilerTexture> textures;
    bool generalControlFlow = false;  // emitted as a pc / switch state machine
    bool usesRegisterArray = false;   // relative temporary register addressing
    bool hoistVertexFetch = false;    // all vertex inputs fetched once at the top (kkIn)
    uint32_t tempRegisterCount = 0;
    uint32_t pixelOutputs = 0;        // pixel: PixelShaderOutputs written (plus RecompilerInput::pixelOutputs)
    uint32_t exportedInterpolators = 0; // vertex: semantic slots written (TEXCOORD0-15, COLOR0-1)

    bool recompile(const RecompilerInput& in, std::string_view include);

private:
    std::vector<ControlFlowInstruction> cf;
    uint32_t instructionCount = 0;
    std::map<uint32_t, uint32_t> elementByAddress;        // vfetch address -> vertex element index
    std::map<uint32_t, uint32_t> rawBindingByConstant;    // instruction mode: fetch constant -> binding
    std::map<uint32_t, uint32_t> vfetchBinding;           // vfetch address -> binding index
    std::map<uint32_t, uint32_t> vfetchStride;            // vfetch address -> stride (dwords) of its vfetch_full
    std::map<uint32_t, uint32_t> tfetchSampler;           // tfetch address -> sampler binding
    std::map<uint32_t, uint32_t> vsExportSlot;            // vertex export register -> output slot
    std::string epilogue;

    void fail(std::string message);
    void indent();
    std::string reg(uint32_t index, bool relative) const;
    std::string floatConstant(uint32_t index, bool addressed, bool a0Relative) const;
    std::string boolCondition(uint32_t index, bool condition) const;
    std::string loopConstant(uint32_t index) const;
    std::string aluOperand(const AluInstruction& instr, uint32_t i) const;
    std::string scalarOperandBase(const AluInstruction& instr) const;

    bool decodeControlFlow();
    bool analyze();
    bool checkStructured() const;
    void emitExec(uint32_t cfIndex);
    void emitInstructions(uint32_t address, uint32_t count, uint32_t sequence);
    void emitEnd();
    void emitFetchResult(uint32_t dst, bool dstRelative, uint32_t dstSwizzle, std::string_view value);
    void beginPredicate(bool predicated, bool condition);
    void endPredicate();
    int openPredicate = -1;           // condition of the open predicate block, -1 none
    bool predicateChanged = false;    // the last instruction wrote p0

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, uint32_t address);
    void recompile(const AluInstruction& instr);
};
