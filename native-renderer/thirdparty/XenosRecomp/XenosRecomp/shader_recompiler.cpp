// XenosRecomp's shader recompiler, reworked for King Kong (see native-renderer/thirdparty/NOTES.md).
// Instruction semantics follow the ReXGlue SDK's ucode.h notes (from Xenia).

#include "shader_recompiler.h"

static constexpr char SWIZZLES[] = { 'x', 'y', 'z', 'w', '0', '1', '_', '_' };

static constexpr const char* USAGE_SEMANTICS[] =
{
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
    "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"
};

// The interpolator signature, identical in every vertex and pixel shader so any pair links:
// slots 0-15 TEXCOORD0-15, 16-17 COLOR0-1.
static constexpr uint32_t INTERPOLATOR_SLOTS = 18;

static int interpolatorSlot(DeclUsage usage, uint32_t usageIndex)
{
    if (usage == DeclUsage::TexCoord && usageIndex < 16)
        return int(usageIndex);
    if (usage == DeclUsage::Color && usageIndex < 2)
        return 16 + int(usageIndex);
    return -1;
}

static std::string slotName(bool output, uint32_t slot)
{
    if (slot < 16)
        return fmt::format("{}T{}", output ? 'o' : 'i', slot);
    return fmt::format("{}D{}", output ? 'o' : 'i', slot - 16);
}

static std::string slotSemantic(uint32_t slot)
{
    if (slot < 16)
        return fmt::format("TEXCOORD{}", slot);
    return fmt::format("COLOR{}", slot - 16);
}

static const char* usageName(DeclUsage usage)
{
    uint32_t i = uint32_t(usage);
    return i < std::size(USAGE_SEMANTICS) ? USAGE_SEMANTICS[i] : "UNKNOWN";
}

static std::string hexFloat(uint32_t bits)
{
    return fmt::format("asfloat(0x{:08X}u)", bits);
}

static bool isExecOpcode(ControlFlowOpcode op)
{
    switch (op)
    {
    case ControlFlowOpcode::Exec:
    case ControlFlowOpcode::ExecEnd:
    case ControlFlowOpcode::CondExec:
    case ControlFlowOpcode::CondExecEnd:
    case ControlFlowOpcode::CondExecPred:
    case ControlFlowOpcode::CondExecPredEnd:
    case ControlFlowOpcode::CondExecPredClean:
    case ControlFlowOpcode::CondExecPredCleanEnd:
        return true;
    default:
        return false;
    }
}

static bool isEndOpcode(ControlFlowOpcode op)
{
    return op == ControlFlowOpcode::ExecEnd || op == ControlFlowOpcode::CondExecEnd ||
        op == ControlFlowOpcode::CondExecPredEnd || op == ControlFlowOpcode::CondExecPredCleanEnd;
}

static bool isBoolCondExec(ControlFlowOpcode op)
{
    return op == ControlFlowOpcode::CondExec || op == ControlFlowOpcode::CondExecEnd ||
        op == ControlFlowOpcode::CondExecPredClean || op == ControlFlowOpcode::CondExecPredCleanEnd;
}

static bool isPredCondExec(ControlFlowOpcode op)
{
    return op == ControlFlowOpcode::CondExecPred || op == ControlFlowOpcode::CondExecPredEnd;
}

// Operand counts of the vector operations (sources 1-3 actually read).
static uint32_t vectorOperandCount(AluVectorOpcode op)
{
    switch (op)
    {
    case AluVectorOpcode::Frc:
    case AluVectorOpcode::Trunc:
    case AluVectorOpcode::Floor:
    case AluVectorOpcode::Max4:
    case AluVectorOpcode::Cube: // only the first operand (.z_xy) is used
        return 1;
    case AluVectorOpcode::Mad:
    case AluVectorOpcode::CndEq:
    case AluVectorOpcode::CndGe:
    case AluVectorOpcode::CndGt:
    case AluVectorOpcode::Dp2Add:
        return 3;
    default:
        return 2;
    }
}

static bool vectorChangesState(AluVectorOpcode op)
{
    return (op >= AluVectorOpcode::SetpEqPush && op <= AluVectorOpcode::KillNe) || op == AluVectorOpcode::MaxA;
}

// 0 = no operand, 1 = `a` only, 2 = `a` and `b`, 3 = constant `a` and temporary `b`.
static uint32_t scalarOperandKind(AluScalarOpcode op)
{
    switch (op)
    {
    case AluScalarOpcode::Adds:
    case AluScalarOpcode::Muls:
    case AluScalarOpcode::MulsPrev2:
    case AluScalarOpcode::Maxs:
    case AluScalarOpcode::Mins:
    case AluScalarOpcode::MaxAs:
    case AluScalarOpcode::MaxAsf:
    case AluScalarOpcode::Subs:
        return 2;
    case AluScalarOpcode::SetpClr:
    case AluScalarOpcode::RetainPrev:
        return 0;
    case AluScalarOpcode::Mulsc0:
    case AluScalarOpcode::Mulsc1:
    case AluScalarOpcode::Addsc0:
    case AluScalarOpcode::Addsc1:
    case AluScalarOpcode::Subsc0:
    case AluScalarOpcode::Subsc1:
        return 3;
    default:
        return 1;
    }
}

static bool isValidScalarOpcode(uint32_t op)
{
    return op <= 50 && op != 41;
}

void ShaderRecompiler::fail(std::string message)
{
    if (error.empty())
        error = std::move(message);
}

void ShaderRecompiler::indent()
{
    for (uint32_t i = 0; i < indentation; i++)
        out += '\t';
}

std::string ShaderRecompiler::reg(uint32_t index, bool relative) const
{
    if (usesRegisterArray)
    {
        if (relative)
            return fmt::format("r[({} + aL) & 63]", index);
        return fmt::format("r[{}]", index);
    }
    return fmt::format("r{}", index);
}

std::string ShaderRecompiler::floatConstant(uint32_t index, bool addressed, bool a0Relative) const
{
    const char* stage = isPixelShader ? "PS" : "VS";
    if (addressed)
    {
        // With literal constants, relative reads go through kkConstRel, which serves the literal
        // registers from the shader itself (the game's skinning shaders index c250-c254 by the
        // loop counter), so the result never depends on the backend having applied them.
        if (!input->floatLiterals.empty())
            return fmt::format("kkConstRel({} + {})", index, a0Relative ? "a0" : "aL");
        return fmt::format("kk_{}Const({} + {})", stage, index, a0Relative ? "a0" : "aL");
    }
    if (input->floatLiterals.count(index))
        return fmt::format("kkLiteral{}", index);
    return fmt::format("kk_{}[{}]", isPixelShader ? "PC" : "VC", index);
}

std::string ShaderRecompiler::boolCondition(uint32_t index, bool condition) const
{
    auto literal = input->boolLiterals.find(index);
    if (literal != input->boolLiterals.end())
        return literal->second == condition ? "true" : "false";
    return fmt::format("{}kk_BoolConst({})", condition ? "" : "!", index);
}

std::string ShaderRecompiler::loopConstant(uint32_t index) const
{
    auto literal = input->loopLiterals.find(index);
    if (literal != input->loopLiterals.end())
        return fmt::format("0x{:X}u", literal->second);
    return fmt::format("kk_LoopConst({})", index);
}

// A vector source operand as a float4 expression, with its swizzle, absolute value and negation.
std::string ShaderRecompiler::aluOperand(const AluInstruction& instr, uint32_t i) const
{
    uint32_t regValue = i == 1 ? instr.src1Register : (i == 2 ? instr.src2Register : instr.src3Register);
    bool isTemp = (i == 1 ? instr.src1Select : (i == 2 ? instr.src2Select : instr.src3Select)) != 0;
    uint32_t swizzle = i == 1 ? instr.src1Swizzle : (i == 2 ? instr.src2Swizzle : instr.src3Swizzle);
    bool negate = (i == 1 ? instr.src1Negate : (i == 2 ? instr.src2Negate : instr.src3Negate)) != 0;

    std::string base;
    bool abs;
    if (isTemp)
    {
        base = reg(regValue & 0x3F, (regValue & 0x40) != 0);
        abs = (regValue & 0x80) != 0;
    }
    else
    {
        // The addressing mode of a constant operand comes from const_0_rel_abs for the first
        // constant of the instruction and const_1_rel_abs for the later ones.
        bool addressed;
        if (i == 1)
            addressed = instr.const0Relative;
        else if (i == 2)
            addressed = instr.src1Select ? instr.const0Relative : instr.const1Relative;
        else
            addressed = (instr.src1Select && instr.src2Select) ? instr.const0Relative : instr.const1Relative;
        base = floatConstant(regValue, addressed, instr.constAddressRegisterRelative);
        abs = instr.absConstants;
    }

    std::string result = base;
    result += '.';
    for (uint32_t j = 0; j < 4; j++)
        result += SWIZZLES[((swizzle >> (j * 2)) + j) & 0x3];
    if (abs)
        result = fmt::format("abs({})", result);
    if (negate)
        result = fmt::format("(-{})", result);
    return result;
}

// The third source as a float4 with absolute value and negation but no swizzle, for the scalar
// operation (`a` is component (3 + swizzle[6:7]) & 3, `b` is component swizzle[0:1]).
std::string ShaderRecompiler::scalarOperandBase(const AluInstruction& instr) const
{
    std::string base;
    bool abs;
    if (instr.src3Select)
    {
        base = reg(instr.src3Register & 0x3F, (instr.src3Register & 0x40) != 0);
        abs = (instr.src3Register & 0x80) != 0;
    }
    else
    {
        bool addressed = (instr.src1Select && instr.src2Select) ? instr.const0Relative : instr.const1Relative;
        base = floatConstant(instr.src3Register, addressed, instr.constAddressRegisterRelative);
        abs = instr.absConstants;
    }
    if (abs)
        base = fmt::format("abs({})", base);
    if (instr.src3Negate)
        base = fmt::format("(-{})", base);
    return base;
}

void ShaderRecompiler::emitFetchResult(uint32_t dst, bool dstRelative, uint32_t dstSwizzle, std::string_view value)
{
    for (uint32_t i = 0; i < 4; i++)
    {
        uint32_t s = (dstSwizzle >> (i * 3)) & 0x7;
        if (s == 7)
            continue;
        indent();
        print("{}.{} = ", reg(dst, dstRelative), SWIZZLES[i]);
        if (s <= 3)
            println("{}.{};", value, SWIZZLES[s]);
        else if (s == 5)
            println("1.0;");
        else
            println("0.0;");
    }
}

void ShaderRecompiler::recompile(const VertexFetchInstruction& instr, uint32_t address)
{
    if (isPixelShader)
    {
        fail(fmt::format("vertex fetch at {} in a pixel shader", address));
        return;
    }

    beginPredicate(instr.isPredicated, instr.predicateCondition);

    if (!instr.isMiniFetch)
    {
        indent();
        println("vfIndex = uint(int(floor({}.{}{})));", reg(instr.srcRegister, instr.srcRegisterAm),
            SWIZZLES[instr.srcSwizzle & 3], instr.isIndexRounded ? " + 0.5" : "");
    }

    uint32_t binding = vfetchBinding.at(address);
    indent();
    out += "{\n";
    ++indentation;
    indent();
    if (input->rawVertexFetch)
    {
        uint32_t word = uint32_t(instr.format) | (instr.formatCompAll ? 0x40u : 0u) | (instr.numFormatAll ? 0x80u : 0u) |
            (instr.signedRfModeAll ? 0x100u : 0u) | ((uint32_t(instr.expAdjust) & 0x3Fu) << 11) |
            ((0u | (1u << 3) | (2u << 6) | (3u << 9)) << 17);
        println("float4 kf = kk_FetchRaw({}, vfIndex, {}u, {}, 0x{:X}u);", binding, vfetchStride.at(address) * 4,
            int32_t(instr.offset) * 4, word);
    }
    else
    {
        println("float4 kf = kk_FetchElement({}, vfIndex);", binding);
    }
    emitFetchResult(instr.dstRegister, instr.dstRegisterAam, instr.dstSwizzle, "kf");
    --indentation;
    indent();
    out += "}\n";

}

void ShaderRecompiler::recompile(const TextureFetchInstruction& instr, uint32_t address)
{
    beginPredicate(instr.isPredicated, instr.predCondition);

    auto srcComponents = [&](uint32_t count)
        {
            std::string s = reg(instr.srcRegister, instr.srcRegisterAm) + ".";
            for (uint32_t i = 0; i < count; i++)
                s += SWIZZLES[(instr.srcSwizzle >> (i * 2)) & 0x3];
            return s;
        };

    switch (instr.opcode)
    {
    case FetchOpcode::SetTextureLod:
        indent();
        println("kkTexLod = {};", srcComponents(1));
        break;
    case FetchOpcode::SetTextureGradientsHorz:
        indent();
        println("kkGradH = {};", srcComponents(3));
        break;
    case FetchOpcode::SetTextureGradientsVert:
        indent();
        println("kkGradV = {};", srcComponents(3));
        break;
    case FetchOpcode::GetTextureGradients:
        indent();
        out += "{\n";
        ++indentation;
        indent();
        println("float2 kc = {};", srcComponents(2));
        indent();
        if (isPixelShader)
            out += "float4 kt = float4(ddx(kc.x), ddy(kc.x), ddx(kc.y), ddy(kc.y));\n";
        else
            out += "float4 kt = float4(0.0, 0.0, 0.0, 0.0);\n";
        emitFetchResult(instr.dstRegister, instr.dstRegisterAm, instr.dstSwizzle, "kt");
        --indentation;
        indent();
        out += "}\n";
        break;
    case FetchOpcode::GetTextureBorderColorFrac:
        // The fraction of border colour in the sample: not modelled, the backend's samplers
        // handle borders.
        emitFetchResult(instr.dstRegister, instr.dstRegisterAm, instr.dstSwizzle, "float4(0.0, 0.0, 0.0, 0.0)");
        break;
    case FetchOpcode::TextureFetch:
    case FetchOpcode::GetTextureComputedLod:
    case FetchOpcode::GetTextureWeights:
    {
        uint32_t samplerBinding = tfetchSampler.at(address);
        const char* stage = isPixelShader ? "PS" : "VS";
        TextureDimension dim = instr.dimension;
        uint32_t coordCount = (dim == TextureDimension::Texture1D) ? 1 : (dim == TextureDimension::Texture2D ? 2 : 3);
        float lodBias = float(instr.lodBias) / 16.0f;
        float offset[3] = { float(instr.offsetX) * 0.5f, float(instr.offsetY) * 0.5f, float(instr.offsetZ) * 0.5f };
        bool hasOffset = dim != TextureDimension::TextureCube &&
            (offset[0] != 0.0f || (coordCount > 1 && offset[1] != 0.0f) || (coordCount > 2 && offset[2] != 0.0f));
        bool implicitLod = isPixelShader && instr.useCompLod;

        indent();
        out += "{\n";
        ++indentation;

        indent();
        println("uint kti = kk_TexIndex({});", instr.constIndex);
        indent();
        println("SamplerState kts = kk_{}SamplerState({});", stage, samplerBinding);
        indent();
        out += "float4 kt = float4(0.0, 0.0, 0.0, 0.0);\n";

        // The sample call for one texture object and coordinate type.
        auto sampleCall = [&](std::string_view object, std::string_view coord, uint32_t gradComponents)
            {
                std::string extra;
                if (instr.useRegGradients)
                {
                    std::string sw = gradComponents == 2 ? "xy" : "xyz";
                    std::string scale = lodBias != 0.0f ? fmt::format(" * {}", std::exp2(lodBias)) : "";
                    return fmt::format("{}.SampleGrad(kts, {}, kkGradH.{}{}, kkGradV.{}{})", object, coord, sw, scale, sw, scale);
                }
                std::string lod;
                if (instr.useRegLod)
                    lod = "kkTexLod";
                if (lodBias != 0.0f)
                    lod += lod.empty() ? fmt::format("{}", lodBias) : fmt::format(" + {}", lodBias);
                if (!implicitLod)
                    return fmt::format("{}.SampleLevel(kts, {}, {})", object, coord, lod.empty() ? "0.0" : lod);
                if (lod.empty())
                    return fmt::format("{}.Sample(kts, {})", object, coord);
                return fmt::format("{}.SampleBias(kts, {}, clamp({}, -16.0, 15.99))", object, coord, lod);
            };

        // Coordinates with unnormalised addressing and offsets applied, given the size.
        auto adjustCoords = [&](std::string_view sizeExpr)
            {
                if (instr.texCoordDenorm)
                {
                    indent();
                    println("kc /= {};", sizeExpr);
                }
                if (hasOffset)
                {
                    indent();
                    if (coordCount == 1)
                        println("kc += {} / {};", offset[0], sizeExpr);
                    else if (coordCount == 2)
                        println("kc += float2({}, {}) / {};", offset[0], offset[1], sizeExpr);
                    else
                        println("kc += float3({}, {}, {}) / {};", offset[0], offset[1], offset[2], sizeExpr);
                }
            };

        auto body = [&](std::string_view object, std::string_view coord, uint32_t components, std::string_view sizeExpr)
            {
                switch (instr.opcode)
                {
                case FetchOpcode::TextureFetch:
                    indent();
                    println("kt = {};", sampleCall(object, coord, components));
                    break;
                case FetchOpcode::GetTextureComputedLod:
                    indent();
                    if (isPixelShader)
                        println("kt.x = {}.CalculateLevelOfDetailUnclamped(kts, {});", object, coord);
                    else
                        out += "kt.x = 0.0;\n";
                    break;
                case FetchOpcode::GetTextureWeights:
                    if (!sizeExpr.empty())
                    {
                        indent();
                        if (components == 2)
                            println("kt.xy = frac(kc * {} - 0.5);", sizeExpr);
                        else
                            println("kt.xyz = frac(kc * {} - 0.5);", sizeExpr);
                    }
                    if (isPixelShader)
                    {
                        indent();
                        println("kt.w = frac({}.CalculateLevelOfDetailUnclamped(kts, {}));", object, coord);
                    }
                    break;
                default:
                    break;
                }
            };

        switch (dim)
        {
        case TextureDimension::Texture1D:
        case TextureDimension::Texture2D:
        {
            indent();
            if (dim == TextureDimension::Texture1D)
                println("float kc = {};", srcComponents(1));
            else
                println("float2 kc = {};", srcComponents(2));
            indent();
            out += "Texture2D<float4> kto = kk_Tex2D[kti];\n";
            bool needSize = instr.texCoordDenorm || hasOffset || instr.opcode == FetchOpcode::GetTextureWeights;
            if (needSize)
            {
                indent();
                out += "uint2 ktd;\n";
                indent();
                out += "kto.GetDimensions(ktd.x, ktd.y);\n";
            }
            adjustCoords(dim == TextureDimension::Texture1D ? "float(ktd.x)" : "float2(ktd)");
            if (dim == TextureDimension::Texture1D)
            {
                indent();
                out += "float2 kc2 = float2(kc, 0.5);\n";
                body("kto", "kc2", 2, "");
                if (instr.opcode == FetchOpcode::GetTextureWeights)
                {
                    indent();
                    out += "kt.x = frac(kc * float(ktd.x) - 0.5);\n";
                }
            }
            else
            {
                body("kto", "kc", 2, "float2(ktd)");
            }
            break;
        }
        case TextureDimension::Texture3D:
        {
            // 3D or stacked: the texture index says which (bit 31 = a 2D array).
            indent();
            println("float3 kc = {};", srcComponents(3));
            indent();
            out += "[branch] if ((kti & 0x80000000u) != 0)\n";
            indent();
            out += "{\n";
            ++indentation;
            indent();
            out += "Texture2DArray<float4> kto = kk_Tex2DArray[kti & 0x7FFFFFFFu];\n";
            indent();
            out += "uint3 ktd;\n";
            indent();
            out += "kto.GetDimensions(ktd.x, ktd.y, ktd.z);\n";
            adjustCoords("float3(ktd)");
            indent();
            out += "float3 kcl = float3(kc.xy, kc.z * float(ktd.z) - 0.5);\n";
            if (instr.opcode == FetchOpcode::TextureFetch)
            {
                indent();
                std::string call = sampleCall("kto", "kcl", 2);
                println("kt = {};", call);
            }
            else if (instr.opcode == FetchOpcode::GetTextureComputedLod)
            {
                indent();
                if (isPixelShader)
                    out += "kt.x = kto.CalculateLevelOfDetailUnclamped(kts, kc.xy);\n";
                else
                    out += "kt.x = 0.0;\n";
            }
            else
            {
                indent();
                out += "kt.xyz = frac(kc * float3(ktd) - 0.5);\n";
                if (isPixelShader)
                {
                    indent();
                    out += "kt.w = frac(kto.CalculateLevelOfDetailUnclamped(kts, kc.xy));\n";
                }
            }
            --indentation;
            indent();
            out += "}\n";
            indent();
            out += "else\n";
            indent();
            out += "{\n";
            ++indentation;
            indent();
            out += "Texture3D<float4> kto = kk_Tex3D[kti];\n";
            indent();
            out += "uint3 ktd;\n";
            indent();
            out += "kto.GetDimensions(ktd.x, ktd.y, ktd.z);\n";
            adjustCoords("float3(ktd)");
            body("kto", "kc", 3, "float3(ktd)");
            --indentation;
            indent();
            out += "}\n";
            break;
        }
        case TextureDimension::TextureCube:
        {
            indent();
            println("float3 kc = kk_CubeDirection({});", srcComponents(3));
            indent();
            out += "TextureCube<float4> kto = kk_TexCube[kti];\n";
            body("kto", "kc", 3, "");
            break;
        }
        }

        emitFetchResult(instr.dstRegister, instr.dstRegisterAm, instr.dstSwizzle, "kt");
        --indentation;
        indent();
        out += "}\n";
        break;
    }
    default:
        fail(fmt::format("unknown fetch opcode {} at {}", uint32_t(instr.opcode), address));
        break;
    }

}

void ShaderRecompiler::recompile(const AluInstruction& instr)
{
    beginPredicate(instr.isPredicated, instr.predicateCondition);

    indent();
    out += "{\n";
    ++indentation;

    const AluVectorOpcode vop = instr.vectorOpcode;
    const AluScalarOpcode sop = instr.scalarOpcode;

    uint32_t vectorMask = instr.vectorWriteMask;
    uint32_t scalarMask = instr.scalarWriteMask;
    uint32_t constant0Mask = 0, constant1Mask = 0;
    if (instr.exportData)
    {
        vectorMask = instr.vectorWriteMask & ~instr.scalarWriteMask;
        scalarMask = instr.scalarWriteMask & ~instr.vectorWriteMask;
        constant1Mask = instr.vectorWriteMask & instr.scalarWriteMask;
        constant0Mask = instr.scalarDestRelative ? (0xF & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
    }

    bool newP0 = false, newA0 = false;

    // Vector operation (sources are read before anything this instruction writes).
    bool doVector = vectorMask != 0 || vectorChangesState(vop);
    if (doVector)
    {
        uint32_t n = vectorOperandCount(vop);
        for (uint32_t i = 1; i <= n; i++)
        {
            indent();
            println("float4 kv{} = {};", i, aluOperand(instr, i));
        }

        std::string value;
        switch (vop)
        {
        case AluVectorOpcode::Add: value = "kv1 + kv2"; break;
        case AluVectorOpcode::Mul: value = "kk_Mul(kv1, kv2)"; break;
        case AluVectorOpcode::Max: value = "kk_Max(kv1, kv2)"; break;
        case AluVectorOpcode::Min: value = "kk_Min(kv1, kv2)"; break;
        case AluVectorOpcode::Seq: value = "float4(kv1 == kv2)"; break;
        case AluVectorOpcode::Sgt: value = "float4(kv1 > kv2)"; break;
        case AluVectorOpcode::Sge: value = "float4(kv1 >= kv2)"; break;
        case AluVectorOpcode::Sne: value = "float4(kv1 != kv2)"; break;
        case AluVectorOpcode::Frc: value = "kv1 - floor(kv1)"; break;
        case AluVectorOpcode::Trunc: value = "trunc(kv1)"; break;
        case AluVectorOpcode::Floor: value = "floor(kv1)"; break;
        case AluVectorOpcode::Mad: value = "kk_Mul(kv1, kv2) + kv3"; break;
        case AluVectorOpcode::CndEq: value = "select(kv1 == 0.0, kv2, kv3)"; break;
        case AluVectorOpcode::CndGe: value = "select(kv1 >= 0.0, kv2, kv3)"; break;
        case AluVectorOpcode::CndGt: value = "select(kv1 > 0.0, kv2, kv3)"; break;
        case AluVectorOpcode::Dp4: value = "kk_Dot4(kv1, kv2).xxxx"; break;
        case AluVectorOpcode::Dp3: value = "kk_Dot3(kv1, kv2).xxxx"; break;
        case AluVectorOpcode::Dp2Add: value = "kk_Dot2Add(kv1, kv2, kv3).xxxx"; break;
        case AluVectorOpcode::Cube: value = "kk_Cube(kv1)"; break;
        case AluVectorOpcode::Max4: value = "kk_Max4(kv1).xxxx"; break;
        case AluVectorOpcode::SetpEqPush:
        case AluVectorOpcode::SetpNePush:
        case AluVectorOpcode::SetpGtPush:
        case AluVectorOpcode::SetpGePush:
        {
            const char* cmp = vop == AluVectorOpcode::SetpEqPush ? "==" :
                (vop == AluVectorOpcode::SetpNePush ? "!=" : (vop == AluVectorOpcode::SetpGtPush ? ">" : ">="));
            indent();
            println("bool kp = and(kv1.w == 0.0, kv2.w {} 0.0);", cmp);
            newP0 = true;
            value = fmt::format("select(and(kv1.x == 0.0, kv2.x {} 0.0), 0.0, kv1.x + 1.0).xxxx", cmp);
            break;
        }
        case AluVectorOpcode::KillEq:
        case AluVectorOpcode::KillGt:
        case AluVectorOpcode::KillGe:
        case AluVectorOpcode::KillNe:
        {
            const char* cmp = vop == AluVectorOpcode::KillEq ? "==" :
                (vop == AluVectorOpcode::KillGt ? ">" : (vop == AluVectorOpcode::KillGe ? ">=" : "!="));
            indent();
            println("bool kk = any(kv1 {} kv2);", cmp);
            if (isPixelShader)
            {
                indent();
                out += "if (kk) discard;\n";
            }
            value = "select(kk, 1.0, 0.0).xxxx";
            break;
        }
        case AluVectorOpcode::Dst: value = "kk_Dst(kv1, kv2)"; break;
        case AluVectorOpcode::MaxA:
            indent();
            out += "int ka = int(floor(clamp(kv1.w, -256.0, 255.0) + 0.5));\n";
            newA0 = true;
            value = "kk_Max(kv1, kv2)";
            break;
        default:
            fail(fmt::format("unknown vector opcode {}", uint32_t(vop)));
            value = "float4(0.0, 0.0, 0.0, 0.0)";
            break;
        }

        indent();
        if (instr.vectorSaturate)
            println("float4 kvr = saturate({});", value);
        else
            println("float4 kvr = {};", value);
    }

    // Scalar operation: always runs (it updates the previous scalar result ps).
    bool doScalar = sop != AluScalarOpcode::RetainPrev;
    bool scalarKill = false;
    if (!isValidScalarOpcode(uint32_t(sop)))
    {
        fail(fmt::format("unknown scalar opcode {}", uint32_t(sop)));
        doScalar = false;
    }
    if (doScalar)
    {
        uint32_t kind = scalarOperandKind(sop);
        if (kind == 1 || kind == 2)
        {
            std::string base = scalarOperandBase(instr);
            indent();
            println("float4 ksv = {};", base);
            indent();
            println("float ka_ = ksv.{};", SWIZZLES[((instr.src3Swizzle >> 6) + 3) & 0x3]);
            indent();
            println("float kb_ = ksv.{};", SWIZZLES[instr.src3Swizzle & 0x3]);
        }
        else if (kind == 3)
        {
            // Constant `a` (third operand index and addressing) and temporary `b` (index built
            // from the opcode, src3_sel and the swizzle); abs_constants and the negation apply to both.
            bool addressed = (instr.src1Select && instr.src2Select) ? instr.const0Relative : instr.const1Relative;
            std::string c = floatConstant(instr.src3Register, addressed, instr.constAddressRegisterRelative);
            uint32_t tempIndex = (uint32_t(sop) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
            std::string t = reg(tempIndex, false);
            if (instr.absConstants)
            {
                c = fmt::format("abs({})", c);
                t = fmt::format("abs({})", t);
            }
            if (instr.src3Negate)
            {
                c = fmt::format("(-{})", c);
                t = fmt::format("(-{})", t);
            }
            indent();
            println("float ka_ = {}.{};", c, SWIZZLES[((instr.src3Swizzle >> 6) + 3) & 0x3]);
            indent();
            println("float kb_ = {}.{};", t, SWIZZLES[instr.src3Swizzle & 0x3]);
        }

        std::string value;
        switch (sop)
        {
        case AluScalarOpcode::Adds:
        case AluScalarOpcode::Addsc0:
        case AluScalarOpcode::Addsc1:
            value = "ka_ + kb_"; break;
        case AluScalarOpcode::AddsPrev: value = "ka_ + ps"; break;
        case AluScalarOpcode::Muls:
        case AluScalarOpcode::Mulsc0:
        case AluScalarOpcode::Mulsc1:
            value = "kk_Muls(ka_, kb_)"; break;
        case AluScalarOpcode::MulsPrev: value = "kk_Muls(ka_, ps)"; break;
        case AluScalarOpcode::MulsPrev2: value = "kk_MulsPrev2(ka_, kb_, ps)"; break;
        case AluScalarOpcode::Maxs: value = "select(ka_ >= kb_, ka_, kb_)"; break;
        case AluScalarOpcode::Mins: value = "select(ka_ < kb_, ka_, kb_)"; break;
        case AluScalarOpcode::Seqs: value = "select(ka_ == 0.0, 1.0, 0.0)"; break;
        case AluScalarOpcode::Sgts: value = "select(ka_ > 0.0, 1.0, 0.0)"; break;
        case AluScalarOpcode::Sges: value = "select(ka_ >= 0.0, 1.0, 0.0)"; break;
        case AluScalarOpcode::Snes: value = "select(ka_ != 0.0, 1.0, 0.0)"; break;
        case AluScalarOpcode::Frcs: value = "(ka_ - floor(ka_))"; break;
        case AluScalarOpcode::Truncs: value = "trunc(ka_)"; break;
        case AluScalarOpcode::Floors: value = "floor(ka_)"; break;
        case AluScalarOpcode::Exp: value = "exp2(ka_)"; break;
        case AluScalarOpcode::Logc: value = "kk_LogC(ka_)"; break;
        case AluScalarOpcode::Log: value = "log2(ka_)"; break;
        case AluScalarOpcode::Rcpc: value = "kk_RcpC(ka_)"; break;
        case AluScalarOpcode::Rcpf: value = "kk_RcpF(ka_)"; break;
        case AluScalarOpcode::Rcp: value = "(1.0 / ka_)"; break;
        case AluScalarOpcode::Rsqc: value = "kk_RsqC(ka_)"; break;
        case AluScalarOpcode::Rsqf: value = "kk_RsqF(ka_)"; break;
        case AluScalarOpcode::Rsq: value = "rsqrt(ka_)"; break;
        case AluScalarOpcode::MaxAs:
        case AluScalarOpcode::MaxAsf:
            indent();
            println("int ksa = int(floor(clamp(ka_, -256.0, 255.0){}));", sop == AluScalarOpcode::MaxAs ? " + 0.5" : "");
            newA0 = true;
            value = "select(ka_ >= kb_, ka_, kb_)";
            break;
        case AluScalarOpcode::Subs:
        case AluScalarOpcode::Subsc0:
        case AluScalarOpcode::Subsc1:
            value = "ka_ - kb_"; break;
        case AluScalarOpcode::SubsPrev: value = "ka_ - ps"; break;
        case AluScalarOpcode::SetpEq:
        case AluScalarOpcode::SetpNe:
        case AluScalarOpcode::SetpGt:
        case AluScalarOpcode::SetpGe:
        {
            const char* cmp = sop == AluScalarOpcode::SetpEq ? "==" :
                (sop == AluScalarOpcode::SetpNe ? "!=" : (sop == AluScalarOpcode::SetpGt ? ">" : ">="));
            indent();
            println("bool ksp = ka_ {} 0.0;", cmp);
            newP0 = true;
            value = "select(ksp, 0.0, 1.0)";
            break;
        }
        case AluScalarOpcode::SetpInv:
            indent();
            out += "bool ksp = ka_ == 1.0;\n";
            newP0 = true;
            value = "select(ksp, 0.0, select(ka_ == 0.0, 1.0, ka_))";
            break;
        case AluScalarOpcode::SetpPop:
            indent();
            out += "float kst = ka_ - 1.0;\n";
            indent();
            out += "bool ksp = kst <= 0.0;\n";
            newP0 = true;
            value = "select(ksp, 0.0, kst)";
            break;
        case AluScalarOpcode::SetpClr:
            indent();
            out += "bool ksp = false;\n";
            newP0 = true;
            value = "KK_FLT_MAX";
            break;
        case AluScalarOpcode::SetpRstr:
            indent();
            out += "bool ksp = ka_ == 0.0;\n";
            newP0 = true;
            value = "select(ksp, 0.0, ka_)";
            break;
        case AluScalarOpcode::KillsEq:
        case AluScalarOpcode::KillsGt:
        case AluScalarOpcode::KillsGe:
        case AluScalarOpcode::KillsNe:
        case AluScalarOpcode::KillsOne:
        {
            const char* test = sop == AluScalarOpcode::KillsEq ? "ka_ == 0.0" :
                (sop == AluScalarOpcode::KillsGt ? "ka_ > 0.0" :
                (sop == AluScalarOpcode::KillsGe ? "ka_ >= 0.0" :
                (sop == AluScalarOpcode::KillsNe ? "ka_ != 0.0" : "ka_ == 1.0")));
            indent();
            println("bool ksk = {};", test);
            scalarKill = true;
            value = "select(ksk, 1.0, 0.0)";
            break;
        }
        case AluScalarOpcode::Sqrt: value = "sqrt(ka_)"; break;
        case AluScalarOpcode::Sin: value = "sin(ka_)"; break;
        case AluScalarOpcode::Cos: value = "cos(ka_)"; break;
        default:
            value = "ps";
            break;
        }
        indent();
        println("ps = {};", value);
    }

    // State changes take effect after both operations have read their sources.
    if (newP0)
    {
        predicateChanged = true;
        indent();
        out += (vop >= AluVectorOpcode::SetpEqPush && vop <= AluVectorOpcode::SetpGePush && doVector) ? "p0 = kp;\n" : "p0 = ksp;\n";
    }
    if (newA0)
    {
        indent();
        out += (vop == AluVectorOpcode::MaxA && doVector) ? "a0 = ka;\n" : "a0 = ksa;\n";
    }
    if (scalarKill && isPixelShader)
    {
        indent();
        out += "if (ksk) discard;\n";
    }

    std::string scalarResult = instr.scalarSaturate ? "saturate(ps)" : "ps";

    auto maskString = [](uint32_t mask)
        {
            std::string s;
            for (uint32_t i = 0; i < 4; i++)
                if ((mask >> i) & 1)
                    s += SWIZZLES[i];
            return s;
        };

    if (instr.exportData)
    {
        std::string target;
        uint32_t index = instr.vectorDest;
        if (isPixelShader)
        {
            if (index <= 3)
                target = fmt::format("oC{}", index);
            else if (index == uint32_t(ExportRegister::PSDepth))
                target = "oDepthV";
        }
        else
        {
            if (index == uint32_t(ExportRegister::VSPosition))
                target = "oPos";
            else if (index == uint32_t(ExportRegister::VSPointSizeEdgeFlagKillVertex))
                target = "oMisc";
            else if (index < 16)
            {
                auto slot = vsExportSlot.find(index);
                if (slot != vsExportSlot.end())
                    target = slotName(true, slot->second);
                else
                    target = "oUnlinked";
            }
        }
        if (index >= uint32_t(ExportRegister::ExportAddress) && index <= uint32_t(ExportRegister::ExportData4))
            target = "oMemExport";

        if (target.empty())
        {
            fail(fmt::format("export to unknown register {}", index));
        }
        else
        {
            if (vectorMask)
            {
                indent();
                println("{}.{} = kvr.{};", target, maskString(vectorMask), maskString(vectorMask));
            }
            if (scalarMask)
            {
                indent();
                println("{}.{} = {};", target, maskString(scalarMask), scalarResult);
            }
            if (constant0Mask)
            {
                indent();
                println("{}.{} = 0.0;", target, maskString(constant0Mask));
            }
            if (constant1Mask)
            {
                indent();
                println("{}.{} = 1.0;", target, maskString(constant1Mask));
            }
        }
    }
    else
    {
        if (vectorMask)
        {
            indent();
            println("{}.{} = kvr.{};", reg(instr.vectorDest, instr.vectorDestRelative), maskString(vectorMask), maskString(vectorMask));
        }
        if (scalarMask)
        {
            indent();
            println("{}.{} = {};", reg(instr.scalarDest, instr.scalarDestRelative), maskString(scalarMask), scalarResult);
        }
    }

    --indentation;
    indent();
    out += "}\n";

}

// ---------------------------------------------------------------------------------------------
// Control flow.

bool ShaderRecompiler::decodeControlFlow()
{
    const auto& code = input->ucode;
    if (code.empty() || code.size() % 3 != 0)
    {
        fail("microcode size is not a multiple of 3 dwords");
        return false;
    }
    instructionCount = uint32_t(code.size() / 3);

    // Control flow occupies the start of the microcode, up to the first instruction an exec uses.
    uint32_t cfEnd = instructionCount;
    for (uint32_t triple = 0; triple < cfEnd; triple++)
    {
        ControlFlowInstruction pair[2];
        uint32_t dw[4];
        dw[0] = code[triple * 3 + 0];
        dw[1] = code[triple * 3 + 1] & 0xFFFF;
        dw[2] = (code[triple * 3 + 1] >> 16) | (code[triple * 3 + 2] << 16);
        dw[3] = code[triple * 3 + 2] >> 16;
        std::memcpy(pair, dw, sizeof(dw));
        for (auto& instr : pair)
        {
            if (isExecOpcode(instr.opcode) && instr.exec.count != 0)
                cfEnd = std::min<uint32_t>(cfEnd, instr.exec.address);
            cf.push_back(instr);
        }
    }
    if (cf.empty())
    {
        fail("no control flow");
        return false;
    }
    return true;
}

bool ShaderRecompiler::analyze()
{
    uint32_t maxRegister = 0;
    auto useRegister = [&](uint32_t index, bool relative)
        {
            if (relative)
                usesRegisterArray = true;
            maxRegister = std::max(maxRegister, index);
        };

    pixelOutputs = input->pixelOutputs;
    std::map<std::array<uint32_t, 7>, uint32_t> samplerKeys;
    std::map<uint32_t, TextureDimension> slotDimension;
    uint32_t lastFullFetchConstant = UINT32_MAX;
    uint32_t lastFullFetchStride = 0;
    bool haveFullFetch = false;

    if (input->rawVertexFetch && !input->vertexElements.empty())
    {
        fail("vertex elements given for raw vertex fetch");
        return false;
    }
    for (size_t i = 0; i < input->vertexElements.size(); i++)
        elementByAddress[input->vertexElements[i].address] = uint32_t(i);

    // Every exec in order of control flow (vfetch_mini depends on the previous vfetch_full in
    // program order, which the compiler keeps within one exec sequence).
    for (uint32_t cfIndex = 0; cfIndex < cf.size(); cfIndex++)
    {
        const auto& c = cf[cfIndex];
        if (!isExecOpcode(c.opcode))
            continue;
        uint32_t address = c.exec.address, count = c.exec.count, sequence = c.exec.sequence;
        if (address + count > instructionCount)
        {
            fail(fmt::format("exec {} runs past the microcode", cfIndex));
            return false;
        }
        for (uint32_t i = 0; i < count; i++, sequence >>= 2)
        {
            const uint32_t* w = &input->ucode[(address + i) * 3];
            union
            {
                VertexFetchInstruction vfetch;
                TextureFetchInstruction tfetch;
                AluInstruction alu;
                uint32_t dwords[3];
            };
            std::memcpy(dwords, w, sizeof(dwords));
            uint32_t at = address + i;

            if (sequence & 1)
            {
                if (vfetch.opcode == FetchOpcode::VertexFetch)
                {
                    if (!vfetch.isMiniFetch)
                    {
                        useRegister(vfetch.srcRegister, vfetch.srcRegisterAm);
                        lastFullFetchConstant = vfetch.constIndex * 3 + vfetch.constIndexSelect;
                        lastFullFetchStride = vfetch.stride;
                        haveFullFetch = true;
                    }
                    else if (!haveFullFetch)
                    {
                        fail(fmt::format("vfetch_mini at {} without a previous vfetch_full", at));
                        return false;
                    }
                    useRegister(vfetch.dstRegister, vfetch.dstRegisterAam);
                    vfetchStride[at] = lastFullFetchStride;
                    if (input->rawVertexFetch)
                    {
                        auto found = rawBindingByConstant.find(lastFullFetchConstant);
                        if (found == rawBindingByConstant.end())
                        {
                            uint32_t binding = uint32_t(vertexBindings.size());
                            RecompilerVertexBinding b;
                            b.raw = true;
                            b.fetchConstant = lastFullFetchConstant;
                            vertexBindings.push_back(b);
                            found = rawBindingByConstant.emplace(lastFullFetchConstant, binding).first;
                        }
                        vfetchBinding[at] = found->second;
                    }
                    else
                    {
                        auto element = elementByAddress.find(at);
                        if (element == elementByAddress.end())
                        {
                            fail(fmt::format("vfetch at {} has no vertex element binding", at));
                            return false;
                        }
                        vfetchBinding[at] = element->second;
                    }
                }
                else
                {
                    useRegister(tfetch.srcRegister, tfetch.srcRegisterAm);
                    useRegister(tfetch.dstRegister, tfetch.dstRegisterAm);
                    switch (tfetch.opcode)
                    {
                    case FetchOpcode::TextureFetch:
                    case FetchOpcode::GetTextureComputedLod:
                    case FetchOpcode::GetTextureWeights:
                    {
                        std::array<uint32_t, 7> key = { tfetch.constIndex, uint32_t(tfetch.dimension), tfetch.magFilter,
                            tfetch.minFilter, tfetch.mipFilter, tfetch.anisoFilter, uint32_t((tfetch.volMagFilter << 2) | tfetch.volMinFilter) };
                        auto found = samplerKeys.find(key);
                        if (found == samplerKeys.end())
                        {
                            RecompilerSampler s;
                            s.slot = tfetch.constIndex;
                            s.dimension = tfetch.dimension;
                            s.magFilter = tfetch.magFilter;
                            s.minFilter = tfetch.minFilter;
                            s.mipFilter = tfetch.mipFilter;
                            s.anisoFilter = tfetch.anisoFilter;
                            s.volMagFilter = tfetch.volMagFilter;
                            s.volMinFilter = tfetch.volMinFilter;
                            found = samplerKeys.emplace(key, uint32_t(samplerBindings.size())).first;
                            samplerBindings.push_back(s);
                        }
                        tfetchSampler[at] = found->second;
                        auto dim = slotDimension.find(tfetch.constIndex);
                        if (dim == slotDimension.end())
                        {
                            slotDimension.emplace(uint32_t(tfetch.constIndex), TextureDimension(tfetch.dimension));
                            textures.push_back({ uint32_t(tfetch.constIndex), tfetch.dimension });
                        }
                        else if (dim->second != tfetch.dimension)
                        {
                            warnings.push_back(fmt::format("texture fetch constant {} used with two dimensions", uint32_t(tfetch.constIndex)));
                        }
                        break;
                    }
                    case FetchOpcode::GetTextureBorderColorFrac:
                        warnings.push_back("getBCF is not modelled (returns 0)");
                        break;
                    case FetchOpcode::GetTextureGradients:
                    case FetchOpcode::SetTextureLod:
                    case FetchOpcode::SetTextureGradientsHorz:
                    case FetchOpcode::SetTextureGradientsVert:
                        break;
                    default:
                        fail(fmt::format("unknown fetch opcode {} at {}", uint32_t(tfetch.opcode), at));
                        return false;
                    }
                }
            }
            else
            {
                const auto& a = alu;
                auto operand = [&](uint32_t regValue, bool isTemp)
                    {
                        if (isTemp)
                            useRegister(regValue & 0x3F, (regValue & 0x40) != 0);
                    };
                operand(a.src1Register, a.src1Select);
                operand(a.src2Register, a.src2Select);
                AluScalarOpcode sop = a.scalarOpcode;
                if (scalarOperandKind(sop) == 3)
                    useRegister((uint32_t(sop) & 1) | (a.src3Select << 1) | (a.src3Swizzle & 0x3C), false);
                // The third source is read by three-operand vector operations and by the scalar one.
                operand(a.src3Register, a.src3Select);
                if (!a.exportData)
                {
                    if (a.vectorWriteMask)
                        useRegister(a.vectorDest, a.vectorDestRelative);
                    if (a.scalarWriteMask)
                        useRegister(a.scalarDest, a.scalarDestRelative);
                }
                else if (isPixelShader && a.vectorDest > 3 && a.vectorDest != uint32_t(ExportRegister::PSDepth) &&
                    !(a.vectorDest >= 32 && a.vectorDest <= 37))
                {
                    fail(fmt::format("pixel shader export to register {}", uint32_t(a.vectorDest)));
                    return false;
                }
                if (a.exportData && isPixelShader)
                {
                    if (a.vectorDest <= 3)
                        pixelOutputs |= 1u << a.vectorDest;
                    else if (a.vectorDest == uint32_t(ExportRegister::PSDepth))
                        pixelOutputs |= PIXEL_SHADER_OUTPUT_DEPTH;
                }
                if (a.exportData && a.vectorDest >= 32 && a.vectorDest <= 37)
                {
                    if (std::find(warnings.begin(), warnings.end(), "memory export is not supported (ignored)") == warnings.end())
                        warnings.push_back("memory export is not supported (ignored)");
                }
                if (!isValidScalarOpcode(uint32_t(sop)) || uint32_t(a.vectorOpcode) > uint32_t(AluVectorOpcode::MaxA))
                {
                    fail(fmt::format("unknown ALU opcode at {}", at));
                    return false;
                }
            }
        }
    }

    if (isPixelShader)
    {
        for (const auto& interp : input->interpolators)
            useRegister(interp.reg, false);
        if (input->rawInterpolators)
            useRegister(15, false);
        if (input->paramGenRegister >= 0)
            useRegister(uint32_t(input->paramGenRegister), false);
    }

    tempRegisterCount = usesRegisterArray ? 64 : maxRegister + 1;
    if (samplerBindings.size() > 32)
    {
        fail("more than 32 sampler bindings");
        return false;
    }
    if (vertexBindings.size() > 16 || input->vertexElements.size() > 16)
    {
        fail("more than 16 vertex bindings");
        return false;
    }
    if (!input->rawVertexFetch)
    {
        for (size_t i = 0; i < input->vertexElements.size(); i++)
        {
            RecompilerVertexBinding b;
            b.element = uint32_t(i);
            vertexBindings.push_back(b);
        }
    }
    return true;
}

// Whether the control flow can be written as nested if / for blocks.
bool ShaderRecompiler::checkStructured() const
{
    struct Scope
    {
        bool isLoop;
        uint32_t end; // if: the label it closes at; loop: the index of its loop end
    };
    std::vector<Scope> stack;
    const uint32_t n = uint32_t(cf.size());
    for (uint32_t i = 0; i < n; i++)
    {
        while (!stack.empty() && !stack.back().isLoop && stack.back().end == i)
            stack.pop_back();
        for (const auto& s : stack)
            if (!s.isLoop && s.end == i)
                return false; // an if closing here under a loop that is still open

        const auto& c = cf[i];
        switch (c.opcode)
        {
        case ControlFlowOpcode::CondJmp:
        {
            if (c.condJmp.isUnconditional)
                return false;
            uint32_t target = c.condJmp.address;
            if (target <= i || target > n)
                return false;
            if (!stack.empty() && target > stack.back().end)
                return false;
            stack.push_back({ false, target });
            break;
        }
        case ControlFlowOpcode::LoopStart:
        {
            uint32_t after = c.loopStart.address;
            if (after <= i + 1 || after > n)
                return false;
            uint32_t endIndex = after - 1;
            const auto& e = cf[endIndex];
            if (e.opcode != ControlFlowOpcode::LoopEnd || e.loopEnd.address != i + 1 || e.loopEnd.loopId != c.loopStart.loopId)
                return false;
            if (!stack.empty() && endIndex >= stack.back().end)
                return false;
            stack.push_back({ true, endIndex });
            break;
        }
        case ControlFlowOpcode::LoopEnd:
            if (stack.empty() || !stack.back().isLoop || stack.back().end != i)
                return false;
            stack.pop_back();
            break;
        case ControlFlowOpcode::CondCall:
        case ControlFlowOpcode::Return:
            return false;
        default:
            break;
        }
    }
    for (const auto& s : stack)
        if (s.isLoop || s.end != n)
            return false;
    return true;
}

void ShaderRecompiler::emitEnd()
{
    out += epilogue;
    indent();
    out += "return;\n";
}

void ShaderRecompiler::emitInstructions(uint32_t address, uint32_t count, uint32_t sequence)
{
    for (uint32_t i = 0; i < count; i++, sequence >>= 2)
    {
        union
        {
            VertexFetchInstruction vfetch;
            TextureFetchInstruction tfetch;
            AluInstruction alu;
            uint32_t dwords[3];
        };
        std::memcpy(dwords, &input->ucode[(address + i) * 3], sizeof(dwords));
        if (sequence & 1)
        {
            if (vfetch.opcode == FetchOpcode::VertexFetch)
                recompile(vfetch, address + i);
            else
                recompile(tfetch, address + i);
        }
        else
        {
            recompile(alu);
        }
        // An instruction that sets p0 ends the run sharing one predicate test.
        if (predicateChanged)
            endPredicate();
    }
    endPredicate();
}

// Consecutive instructions predicated on the same p0 test share one if block (fewer branches
// for the SPIR-V back end, which does not flatten them as the DXIL one does).
void ShaderRecompiler::beginPredicate(bool predicated, bool condition)
{
    int want = predicated ? int(condition) : -1;
    if (want == openPredicate)
        return;
    endPredicate();
    if (want < 0)
        return;
    indent();
    println("if ({}p0)", condition ? "" : "!");
    indent();
    out += "{\n";
    ++indentation;
    openPredicate = want;
}

void ShaderRecompiler::endPredicate()
{
    predicateChanged = false;
    if (openPredicate < 0)
        return;
    --indentation;
    indent();
    out += "}\n";
    openPredicate = -1;
}

void ShaderRecompiler::emitExec(uint32_t cfIndex)
{
    const auto& c = cf[cfIndex];
    std::string condition;
    if (isBoolCondExec(c.opcode))
        condition = boolCondition(c.condExec.boolAddress, c.condExec.condition);
    else if (isPredCondExec(c.opcode))
        condition = c.condExecPred.condition ? "p0" : "!p0";

    if (!condition.empty())
    {
        indent();
        println("if ({})", condition);
        indent();
        out += "{\n";
        ++indentation;
    }
    emitInstructions(c.exec.address, c.exec.count, c.exec.sequence);
    if (isEndOpcode(c.opcode))
        emitEnd();
    if (!condition.empty())
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

bool ShaderRecompiler::recompile(const RecompilerInput& in, std::string_view include)
{
    input = &in;
    isPixelShader = in.isPixelShader;

    if (!decodeControlFlow() || !analyze())
        return false;
    generalControlFlow = !checkStructured();

    // Vertex shader output linkage.
    if (!isPixelShader)
    {
        if (in.rawInterpolators)
        {
            for (uint32_t i = 0; i < 16; i++)
                vsExportSlot[i] = i;
        }
        else
        {
            for (const auto& interp : in.interpolators)
            {
                int slot = interpolatorSlot(interp.usage, interp.usageIndex);
                if (slot < 0)
                {
                    fail(fmt::format("unsupported interpolator semantic {}{}", usageName(interp.usage), interp.usageIndex));
                    return false;
                }
                vsExportSlot[interp.reg] = uint32_t(slot);
                exportedInterpolators |= 1u << slot;
            }
        }
    }

    out += isPixelShader ? "#define KK_PIXEL_SHADER 1\n" : "#define KK_VERTEX_SHADER 1\n";
    out += include;
    out += '\n';

    // The constant table, for reading the HLSL against the game's sources.
    if (!in.constants.empty())
    {
        out += "// Constant table:\n";
        static constexpr const char* SETS[] = { "bool", "int4", "float4", "sampler" };
        for (const auto& c : in.constants)
        {
            static constexpr char PREFIX[] = { 'b', 'i', 'c', 's' };
            println("//   {:<32} {:<7} {}{}{}", c.name, SETS[uint32_t(c.registerSet) & 3], PREFIX[uint32_t(c.registerSet) & 3],
                c.registerIndex, c.registerCount > 1 ? fmt::format("-{}", c.registerIndex + c.registerCount - 1) : "");
        }
        out += '\n';
    }

    // Literal constants the shader defines.
    for (const auto& [index, value] : in.floatLiterals)
    {
        println("static const float4 kkLiteral{} = float4({}, {}, {}, {});", index, hexFloat(value[0]), hexFloat(value[1]),
            hexFloat(value[2]), hexFloat(value[3]));
    }
    if (!in.floatLiterals.empty())
    {
        println("float4 kkConstRel(int index)\n{{\n\tfloat4 v = kk_{}Const(index);", isPixelShader ? "PS" : "VS");
        for (const auto& [index, value] : in.floatLiterals)
            println("\tv = select(index == {0}, kkLiteral{0}, v);", index);
        out += "\treturn v;\n}\n\n";
    }

    // Entry point.
    out += "void main(\n";
    if (isPixelShader)
    {
        out += "\tfloat4 iPos : SV_Position,\n";
        for (uint32_t slot = 0; slot < INTERPOLATOR_SLOTS; slot++)
            println("\tfloat4 {} : {},", slotName(false, slot), slotSemantic(slot));
        out += "\tbool iFace : SV_IsFrontFace";
        for (uint32_t i = 0; i < 4; i++)
            if (pixelOutputs & (1u << i))
                print(",\n\tout float4 oC{0} : SV_Target{0}", i);
        if (pixelOutputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += ",\n\tout float oDepth : SV_Depth";
    }
    else
    {
        out += "\tuint iVertexId : SV_VertexID,\n";
        out += "\tout float4 oPos : SV_Position";
        for (uint32_t slot = 0; slot < INTERPOLATOR_SLOTS; slot++)
            print(",\n\tout float4 {} : {}", slotName(true, slot), slotSemantic(slot));
        out += ",\n\tout float4 oClip0 : SV_ClipDistance0";
        out += ",\n\tout float2 oClip1 : SV_ClipDistance1";
    }
    out += ")\n{\n";
    indentation = 1;

    // Locals.
    if (usesRegisterArray)
        println("\tfloat4 r[64];\n\t[unroll] for (int ri = 0; ri < 64; ri++) r[ri] = 0.0;");
    else
        for (uint32_t i = 0; i < tempRegisterCount; i++)
            println("\tfloat4 r{} = 0.0;", i);
    out += "\tint a0 = 0;\n\tint aL = 0;\n\tbool p0 = false;\n\tfloat ps = 0.0;\n";
    out += "\tuint vfIndex = 0;\n\tfloat kkTexLod = 0.0;\n\tfloat3 kkGradH = 0.0;\n\tfloat3 kkGradV = 0.0;\n";
    out += "\tfloat4 oMemExport = 0.0;\n";

    if (isPixelShader)
    {
        for (uint32_t i = 0; i < 4; i++)
            if (pixelOutputs & (1u << i))
                println("\toC{} = 0.0;", i);
        out += "\tfloat4 oDepthV = float4(iPos.z, 0.0, 0.0, 0.0);\n";
        if (in.rawInterpolators)
        {
            for (uint32_t i = 0; i < 16; i++)
                println("\t{} = {};", reg(i, false), slotName(false, i));
        }
        else
        {
            for (const auto& interp : in.interpolators)
            {
                int slot = interpolatorSlot(interp.usage, interp.usageIndex);
                if (slot < 0)
                {
                    fail(fmt::format("unsupported interpolator semantic {}{}", usageName(interp.usage), interp.usageIndex));
                    return false;
                }
                println("\t{} = {};", reg(interp.reg, false), slotName(false, uint32_t(slot)));
            }
        }
        if (in.paramGenRegister >= 0)
            println("\t{} = float4((iPos.xy - 0.5) * float2(select(iFace, 1.0, -1.0), 1.0), 0.0, 0.0);",
                reg(uint32_t(in.paramGenRegister), false));
    }
    else
    {
        out += "\toPos = 0.0;\n";
        for (uint32_t slot = 0; slot < INTERPOLATOR_SLOTS; slot++)
            println("\t{} = 0.0;", slotName(true, slot));
        out += "\tfloat4 oMisc = 0.0;\n\tfloat4 oUnlinked = 0.0;\n";
        println("\t{}.x = float(iVertexId);", reg(0, false));
    }
    out += '\n';

    // Epilogue run at every end of the program.
    {
        StringBuffer e;
        std::string tabs(indentation, '\t');
        if (isPixelShader)
        {
            if (pixelOutputs & PIXEL_SHADER_OUTPUT_COLOR0)
                e.println("{}kk_AlphaTest(oC0.w);", tabs);
            if (pixelOutputs & PIXEL_SHADER_OUTPUT_DEPTH)
                e.println("{}oDepth = oDepthV.x;", tabs);
        }
        else
        {
            e.println("{}oClip0 = float4(kk_ClipDistance(oPos, 0), kk_ClipDistance(oPos, 1), kk_ClipDistance(oPos, 2), kk_ClipDistance(oPos, 3));", tabs);
            e.println("{}oClip1 = float2(kk_ClipDistance(oPos, 4), kk_ClipDistance(oPos, 5));", tabs);
            e.println("{}oPos.xy += kk_PosOffset.xy * oPos.w;", tabs);
        }
        epilogue = e.out;
    }

    const uint32_t n = uint32_t(cf.size());
    if (!generalControlFlow)
    {
        // Structured: forward conditional jumps become if blocks, loops become for loops.
        struct Scope
        {
            bool isLoop;
            uint32_t end;
        };
        std::vector<Scope> stack;
        uint32_t loopTemp = 0;
        bool ended = false;
        for (uint32_t i = 0; i < n; i++)
        {
            while (!stack.empty() && !stack.back().isLoop && stack.back().end == i)
            {
                stack.pop_back();
                --indentation;
                indent();
                out += "}\n";
            }
            const auto& c = cf[i];
            if (isExecOpcode(c.opcode))
            {
                emitExec(i);
                if (c.opcode == ControlFlowOpcode::ExecEnd && stack.empty())
                {
                    ended = true;
                    break; // the rest is unreachable
                }
                continue;
            }
            switch (c.opcode)
            {
            case ControlFlowOpcode::CondJmp:
            {
                // Skip to the target when the condition holds: run the block when it does not.
                indent();
                if (c.condJmp.isPredicated)
                    println("if ({}p0)", c.condJmp.condition ? "!" : "");
                else
                    println("if ({})", boolCondition(c.condJmp.boolAddress, !c.condJmp.condition));
                indent();
                out += "{\n";
                ++indentation;
                stack.push_back({ false, uint32_t(c.condJmp.address) });
                break;
            }
            case ControlFlowOpcode::LoopStart:
            {
                uint32_t t = loopTemp++;
                indent();
                out += "{\n";
                ++indentation;
                indent();
                println("int kkSavedAL{} = aL;", t);
                indent();
                println("uint kkLoop{} = {};", t, loopConstant(c.loopStart.loopId));
                indent();
                println("[loop] for (uint kkIt{0} = 0; kkIt{0} < (kkLoop{0} & 0xFFu); kkIt{0}++)", t);
                indent();
                out += "{\n";
                ++indentation;
                indent();
                println("aL = int((kkLoop{0} >> 8) & 0xFFu) + int(kkIt{0}) * (int(kkLoop{0} << 8) >> 24);", t);
                stack.push_back({ true, uint32_t(c.loopStart.address - 1) });
                break;
            }
            case ControlFlowOpcode::LoopEnd:
            {
                if (c.loopEnd.isPredicatedBreak)
                {
                    indent();
                    println("if ({}p0) break;", c.loopEnd.condition ? "" : "!");
                }
                --indentation;
                indent();
                out += "}\n";
                uint32_t t = --loopTemp;
                indent();
                println("aL = kkSavedAL{};", t);
                --indentation;
                indent();
                out += "}\n";
                stack.pop_back();
                // Loop temporaries are numbered by depth; reuse them for the next sibling loop.
                break;
            }
            default:
                break;
            }
        }
        while (!stack.empty())
        {
            stack.pop_back();
            --indentation;
            indent();
            out += "}\n";
        }
        if (!ended)
            out += epilogue;
    }
    else
    {
        // General: a pc / switch state machine with loop and call stacks.
        out += "\tuint pc = 0;\n";
        out += "\tuint kkLoopIt[4] = { 0, 0, 0, 0 };\n\tuint kkLoopConst[4] = { 0, 0, 0, 0 };\n\tuint kkLoopDepth = 0;\n";
        out += "\tuint kkCallStack[4] = { 0, 0, 0, 0 };\n\tuint kkCallDepth = 0;\n";
        out += "\t[loop] while (pc != 0xFFFFFFFFu)\n\t{\n\t\tswitch (pc)\n\t\t{\n";
        const char* setAL = "aL = select(kkLoopDepth == 0, 0, int((kkLoopConst[(kkLoopDepth - 1) & 3] >> 8) & 0xFFu) + "
            "int(kkLoopIt[(kkLoopDepth - 1) & 3]) * (int(kkLoopConst[(kkLoopDepth - 1) & 3] << 8) >> 24));";
        for (uint32_t i = 0; i < n; i++)
        {
            const auto& c = cf[i];
            println("\t\tcase {}:", i);
            out += "\t\t{\n";
            indentation = 3;
            uint32_t next = i + 1 < n ? i + 1 : 0xFFFFFFFFu;
            if (isExecOpcode(c.opcode))
            {
                emitExec(i);
                indent();
                println("pc = 0x{:X}u;", next);
            }
            else
            {
                switch (c.opcode)
                {
                case ControlFlowOpcode::LoopStart:
                    indent();
                    println("kkLoopConst[kkLoopDepth & 3] = {};", loopConstant(c.loopStart.loopId));
                    if (!c.loopStart.isRepeat)
                    {
                        indent();
                        out += "kkLoopIt[kkLoopDepth & 3] = 0;\n";
                    }
                    indent();
                    out += "kkLoopDepth++;\n";
                    indent();
                    out += "if (kkLoopIt[(kkLoopDepth - 1) & 3] >= (kkLoopConst[(kkLoopDepth - 1) & 3] & 0xFFu))\n";
                    indent();
                    println("{{ kkLoopDepth--; {} pc = {}u; }}", setAL, c.loopStart.address);
                    indent();
                    out += "else\n";
                    indent();
                    println("{{ {} pc = 0x{:X}u; }}", setAL, next);
                    break;
                case ControlFlowOpcode::LoopEnd:
                    indent();
                    out += "kkLoopIt[(kkLoopDepth - 1) & 3]++;\n";
                    indent();
                    print("if (kkLoopIt[(kkLoopDepth - 1) & 3] < (kkLoopConst[(kkLoopDepth - 1) & 3] & 0xFFu)");
                    if (c.loopEnd.isPredicatedBreak)
                        print(" && {}p0", c.loopEnd.condition ? "!" : "");
                    out += ")\n";
                    indent();
                    println("{{ {} pc = {}u; }}", setAL, c.loopEnd.address);
                    indent();
                    out += "else\n";
                    indent();
                    println("{{ kkLoopDepth--; {} pc = 0x{:X}u; }}", setAL, next);
                    break;
                case ControlFlowOpcode::CondCall:
                {
                    std::string condition = "true";
                    if (!c.condCall.isUnconditional)
                        condition = c.condCall.isPredicated ? (c.condCall.condition ? "p0" : "!p0") :
                            boolCondition(c.condCall.boolAddress, c.condCall.condition);
                    indent();
                    println("if ({})", condition);
                    indent();
                    println("{{ kkCallStack[kkCallDepth & 3] = 0x{:X}u; kkCallDepth++; pc = {}u; }}", next, c.condCall.address);
                    indent();
                    println("else pc = 0x{:X}u;", next);
                    break;
                }
                case ControlFlowOpcode::Return:
                    indent();
                    println("if (kkCallDepth != 0) {{ kkCallDepth--; pc = kkCallStack[kkCallDepth & 3]; }} else pc = 0x{:X}u;", next);
                    break;
                case ControlFlowOpcode::CondJmp:
                {
                    std::string condition = "true";
                    if (!c.condJmp.isUnconditional)
                        condition = c.condJmp.isPredicated ? (c.condJmp.condition ? "p0" : "!p0") :
                            boolCondition(c.condJmp.boolAddress, c.condJmp.condition);
                    indent();
                    println("pc = ({}) ? {}u : 0x{:X}u;", condition, c.condJmp.address, next);
                    break;
                }
                default:
                    indent();
                    println("pc = 0x{:X}u;", next);
                    break;
                }
            }
            out += "\t\t\tbreak;\n\t\t}\n";
        }
        out += "\t\tdefault:\n\t\t\tpc = 0xFFFFFFFFu;\n\t\t\tbreak;\n\t\t}\n\t}\n";
        indentation = 1;
        out += epilogue;
    }

    out += "}\n";
    return error.empty();
}
