#include "kkshaders/tessellation.h"
#include <algorithm>

namespace kkshaders {
namespace {
void Replace(std::string &s, const std::string &from, const std::string &to) {
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
}
} // namespace

bool annTessellationShader(uint64_t h) {
    return h == 0x5E779DBA373D121Cull || h == 0x3390D930CD168AC0ull;
}

TessellationSource makeAnnTessellationShader(const ShaderInfo &info,
                                             bool motion) {
    TessellationSource result;
    if (info.kind != ShaderKind::Vertex ||
        !annTessellationShader(info.ucodeHash)) {
        result.error = "not a verified normal-bearing Ann hair permutation";
        return result;
    }
    const bool first = info.ucodeHash == 0x5E779DBA373D121Cull;
    auto translated = translate(info, {.annotateAddresses = true});
    if (!translated.ok) {
        result.error = translated.error;
        return result;
    }
    auto &text = translated.hlsl;
    const auto at = text.find("void main(\n");
    const auto open = text.find(")\n{\n", at);
    if (at == std::string::npos || open == std::string::npos) {
        result.error = "translated entry signature changed";
        return result;
    }
    std::string prefix = text.substr(0, at);
    std::string signature = text.substr(at, open - at);
    // The translator always declares all material varyings in this order.
    const std::string end = "out float2 oClip1 : SV_ClipDistance1";
    if (signature.find(end) == std::string::npos ||
        signature.find("out float4 oT15 : TEXCOORD15") == std::string::npos) {
        result.error = "translated outputs changed";
        return result;
    }
    Replace(signature, "void main(", "void ktCurrent(");
    for (const auto &semantic :
         {"SV_VertexID", "SV_Position", "SV_ClipDistance0", "SV_ClipDistance1",
          "COLOR0", "COLOR1"})
        Replace(signature, " : " + std::string(semantic), "");
    for (int i = 15; i >= 0; --i)
        Replace(signature, " : TEXCOORD" + std::to_string(i), "");
    signature += ", out float3 ktP, out float3 ktN)\n{\n";
    std::string body = text.substr(open + 4);
    const std::string posMarker = first ? "\t// @39 alu\n" : "\t// @35 alu\n";
    const std::string normalMarker =
        first ? "\t// @47 alu\n" : "\t// @51 alu\n";
    if (body.find(posMarker) == std::string::npos ||
        body.find(normalMarker) == std::string::npos) {
        result.error = "verified split instruction missing";
        return result;
    }
    // Verified from these actual shaders: r2 / r1 is the skinned position,
    // r4.xyz is the skinned artist normal after the three palette dot products.
    const int view = first ? 56 : 44;
    const int project = first ? 52 : 36;
    const std::string p = first ? "r2" : "r1";
    const auto vc = [](int i) { return "kk_VC[" + std::to_string(i) + "]"; };
    std::string pos = "\tktP = float3(dot(float4(" + p + ".xyz, 1), " +
                      vc(view) + "), dot(float4(" + p + ".xyz, 1), " +
                      vc(view + 1) + "), dot(float4(" + p + ".xyz, 1), " +
                      vc(view + 2) + "));\n";
    std::string normal = "\tktN = ktViewNormal(r4.xyz, " + vc(view) + ".xyz, " +
                         vc(view + 1) + ".xyz, " + vc(view + 2) + ".xyz);\n";
    Replace(body, posMarker, pos + posMarker);
    Replace(body, normalMarker, normal + normalMarker);
    std::string h = prefix + R"(
float3 ktViewNormal(float3 n, float3 a, float3 b, float3 c) {
    float d = dot(a, cross(b,c));
    if (abs(d) < 1e-12) return 0;
    float3 v = float3(dot(cross(b,c),n), dot(cross(c,a),n), dot(cross(a,b),n))/d;
    return v * rsqrt(max(dot(v,v), 1e-20));
}
)";
    h += signature + body + "\n";
    if (motion) {
        h += "cbuffer KKMotionConstants : register(b5, space0) { float4 "
             "kk_VCPrev[256]; float4 kk_MotionParams; };\n";
        std::string prev = signature + body;
        Replace(prev, "ktCurrent(", "ktPrevious(");
        Replace(prev, "kk_VC[", "kk_VCPrev[");
        // Relative reads include the literal skinning-loop selectors; retain
        // the translator's literal-aware helper, changing only nonliteral
        // constants.
        auto rel = prefix.find("float4 kkConstRel(int index)\n{");
        auto relEnd = prefix.find("\n}\n", rel);
        if (rel == std::string::npos || relEnd == std::string::npos) {
            result.error = "literal-aware skinning helper missing";
            return result;
        }
        std::string helper = prefix.substr(rel, relEnd + 3 - rel);
        Replace(helper, "kkConstRel(", "ktConstPrev(");
        Replace(helper, "kk_VSConst(", "ktVSPrev(");
        h += "float4 ktVSPrev(int i) { return uint(i)<256u ? "
             "kk_VCPrev[min(uint(i),255u)] : 0; }\n" +
             helper;
        Replace(prev, "kkConstRel(", "ktConstPrev(");
        Replace(prev, "kk_VSConst(", "ktVSPrev(");
        h += prev;
    }
    h += R"(
struct KtPatch {
    float3 p : KTPOS0;
    float3 n : KTNORMAL0;
    float4 clip : KTCLIP0;
    float4 t[16] : TEXCOORD0;
    float4 c[2] : COLOR0;
)";
    if (motion)
        h +=
            "float3 pp : KTPOS1; float3 pn : KTNORMAL1; float4 pc : KTCLIP1;\n";
    h += "};\nKtPatch ktVS(uint id : SV_VertexID) { KtPatch o = (KtPatch)0; "
         "float4 c0; float2 c1;\n";
    std::string args = "id, o.clip";
    for (int i = 0; i < 16; ++i)
        args += ", o.t[" + std::to_string(i) + "]";
    args += ", o.c[0], o.c[1], c0, c1, o.p, o.n";
    h += "ktCurrent(" + args + ");\n";
    if (motion) {
        h += "KtPatch unused = (KtPatch)0;\n";
        Replace(args, "o.t[", "unused.t[");
        Replace(args, "o.c[", "unused.c[");
        Replace(args, "o.clip", "o.pc");
        Replace(args, "o.p, o.n", "o.pp, o.pn");
        h += "ktPrevious(" + args + ");\n";
    }
    h += R"(
return o; }
struct KtFactors { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
KtFactors ktFactors(InputPatch<KtPatch,3> p) {
    KtFactors f; float detail = clamp(float(kk_Flags.w & 255u), 1, 16);
    f.edge[0]=f.edge[1]=f.edge[2]=f.inside=detail; return f;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("ktFactors")]
KtPatch ktHS(InputPatch<KtPatch,3> p, uint id : SV_OutputControlPointID) { return p[id]; }
float3 ktPhong(float3 b, float3 p0, float3 p1, float3 p2, float3 n0, float3 n1, float3 n2, uint flags) {
    float3 p = b.x*p0+b.y*p1+b.z*p2;
    float alpha = float((flags>>8)&65535u)/65535.0;
    return -alpha*(b.x*dot(p-p0,n0)*n0+b.y*dot(p-p1,n1)*n1+b.z*dot(p-p2,n2)*n2);
}
)";
    auto delta = [&](const std::string &constants) {
        auto c = [&](int i) {
            return constants + "[" + std::to_string(i) + "]";
        };
        return "float3 a=" + c(view) + ".xyz, bb=" + c(view + 1) +
               ".xyz, cc=" + c(view + 2) +
               ".xyz;\n"
               "float determinant=dot(a,cross(bb,cc));\n"
               "float3 model = abs(determinant)>1e-12 ? "
               "(cross(bb,cc)*delta.x+cross(cc,a)*delta.y+cross(a,bb)*delta.z)/"
               "determinant : 0;\n"
               "float4 clipDelta=float4(dot(model," +
               c(project) + ".xyz),dot(model," + c(project + 1) +
               ".xyz),dot(model," + c(project + 2) + ".xyz),dot(model," +
               c(project + 3) +
               ".xyz));\n"
               "clipDelta.xyz=clipDelta.xyz*kk_NdcScale.xyz+kk_NdcOffset.xyz*"
               "clipDelta.w; return clipDelta;\n}\n";
    };
    h += "float4 ktDelta(float3 delta) {\n" + delta("kk_VC");
    if (motion)
        h += "float4 ktDeltaPrev(float3 delta) {\n" + delta("kk_VCPrev");
    h += "struct KtOutput { float4 pos:SV_Position;\n";
    h += motion ? "float4 current:KKMOTION0; float4 previous:KKMOTION1;\n"
                : "float4 t[16]:TEXCOORD0; float4 c[2]:COLOR0;\n";
    h += R"(
float4 clip0:SV_ClipDistance0; float2 clip1:SV_ClipDistance1;
};
[domain("tri")]
KtOutput ktDS(KtFactors f, float3 b:SV_DomainLocation, const OutputPatch<KtPatch,3> p) {
    KtOutput o=(KtOutput)0;
    float3 d=ktPhong(b,p[0].p,p[1].p,p[2].p,p[0].n,p[1].n,p[2].n,kk_Flags.w);
    o.pos=b.x*p[0].clip+b.y*p[1].clip+b.z*p[2].clip+ktDelta(d);
)";
    if (motion)
        h += R"(
    o.current=o.pos;
    float3 dp=ktPhong(b,p[0].pp,p[1].pp,p[2].pp,p[0].pn,p[1].pn,p[2].pn,asuint(kk_MotionParams.w));
    o.previous=b.x*p[0].pc+b.y*p[1].pc+b.z*p[2].pc+ktDeltaPrev(dp);
)";
    else
        h += "[unroll] for(uint i=0;i<16;i++) "
             "o.t[i]=b.x*p[0].t[i]+b.y*p[1].t[i]+b.z*p[2].t[i];\n"
             "[unroll] for(uint i=0;i<2;i++) "
             "o.c[i]=b.x*p[0].c[i]+b.y*p[1].c[i]+b.z*p[2].c[i];\n";
    // Clip distances must be evaluated from the smoothed geometry. Undo the
    // viewport transform before applying the original clip-space planes.
    h += R"(
    float4 raw=o.pos; raw.xyz=(raw.xyz-kk_NdcOffset.xyz*raw.w)/kk_NdcScale.xyz;
    o.clip0=float4(kk_ClipDistance(raw,0),kk_ClipDistance(raw,1),kk_ClipDistance(raw,2),kk_ClipDistance(raw,3));
    o.clip1=float2(kk_ClipDistance(raw,4),kk_ClipDistance(raw,5)); return o;
}
)";
    result.ok = true;
    result.hlsl = std::move(h);
    return result;
}

bool buildAnnTessellationShaders(const ShaderInfo &info, Compiler &compiler,
                                 uint32_t targets,
                                 std::vector<CompiledShader> &out,
                                 std::string &error) {
    for (bool motion : {false, true}) {
        auto source = makeAnnTessellationShader(info, motion);
        if (!source.ok) {
            error = source.error;
            return false;
        }
        for (unsigned stage = 0; stage < 3; ++stage) {
            CompiledShader shader;
            shader.kind = ShaderKind::Vertex;
            shader.ucodeHash = info.ucodeHash ^ tessPackTag(stage, motion);
            shader.inputHash = translationInputHash(info);
            const wchar_t *profiles[] = {L"vs_6_0", L"hs_6_0", L"ds_6_0"};
            const wchar_t *entries[] = {L"ktVS", L"ktHS", L"ktDS"};
            for (auto target : {CompileTarget::Dxil, CompileTarget::Spirv}) {
                if (!(targets & (target == CompileTarget::Dxil ? kBuildDxil
                                                               : kBuildSpirv)))
                    continue;
                auto compiled =
                    compiler.compileEntry(source.hlsl, profiles[stage],
                                          entries[stage], target, stage == 2);
                if (!compiled.ok) {
                    error = compiled.messages;
                    return false;
                }
                (target == CompileTarget::Dxil ? shader.dxil : shader.spirv) =
                    std::move(compiled.blob);
            }
            out.push_back(std::move(shader));
        }
    }
    return true;
}
} // namespace kkshaders
