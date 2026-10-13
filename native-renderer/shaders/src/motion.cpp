// The motion variant of a translated vertex shader (translator.h, makeMotionVertexShader).
//
// The translator writes one entry point:
//
//   void main(
//   	uint iVertexId : SV_VertexID,
//   	out float4 oPos : SV_Position,
//   	out float4 <interpolator> : <semantic>,   (one per slot)
//   	out float4 oClip0 : SV_ClipDistance0,
//   	out float2 oClip1 : SV_ClipDistance1)
//   {
//   	<body: reads float constants as kk_VC[n], kk_VSConst(i) or kkConstRel(i), ends with the
//   	 viewport transform through kk_NdcScale / kk_NdcOffset>
//   }
//
// as the last thing in the file. The variant keeps everything before it, adds last frame's constants
// (b5), makes the body two functions (this frame's constants, last frame's) and a new entry point that
// calls both and outputs SV_Position, the two clip positions and the clip distances.
#include <string>
#include <vector>

#include "kkshaders/translator.h"

namespace kkshaders {

namespace {

void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) {
        s.replace(at, from.size(), to);
        at += to.size();
    }
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

struct Param {
    bool out = false;
    std::string type, name, semantic;
};

}  // namespace

MotionShader makeMotionVertexShader(const std::string& translated) {
    MotionShader r;
    const std::string entry = "void main(\n";
    const size_t main_at = translated.find(entry);
    if (main_at == std::string::npos || translated.find(entry, main_at + 1) != std::string::npos) {
        r.error = "no single entry point";
        return r;
    }
    if (translated.find("#define KK_VERTEX_SHADER 1") == std::string::npos) {
        r.error = "not a vertex shader";
        return r;
    }
    const std::string open = ")\n{\n";
    const size_t params_at = main_at + entry.size();
    const size_t open_at = translated.find(open, params_at);
    if (open_at == std::string::npos) {
        r.error = "entry point not understood";
        return r;
    }
    const std::string prefix = translated.substr(0, main_at);
    const std::string params_text = translated.substr(params_at, open_at - params_at);
    const std::string body = translated.substr(open_at + open.size());  // ends with main's closing brace

    // The parameters.
    std::vector<Param> params;
    size_t start = 0;
    while (start <= params_text.size()) {
        size_t end = params_text.find(",\n", start);
        if (end == std::string::npos) end = params_text.size();
        const std::string item = Trim(params_text.substr(start, end - start));
        start = end + 2;
        if (item.empty()) continue;
        Param p;
        const size_t colon = item.find(" : ");
        if (colon == std::string::npos) {
            r.error = "parameter without a semantic: " + item;
            return r;
        }
        p.semantic = Trim(item.substr(colon + 3));
        std::string decl = Trim(item.substr(0, colon));
        if (decl.rfind("out ", 0) == 0) {
            p.out = true;
            decl = Trim(decl.substr(4));
        }
        const size_t space = decl.rfind(' ');
        if (space == std::string::npos) {
            r.error = "parameter not understood: " + item;
            return r;
        }
        p.type = Trim(decl.substr(0, space));
        p.name = Trim(decl.substr(space + 1));
        params.push_back(p);
    }
    int pos = -1, clip0 = -1, clip1 = -1, vertex_id = -1;
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].semantic == "SV_Position") pos = int(i);
        else if (params[i].semantic == "SV_ClipDistance0") clip0 = int(i);
        else if (params[i].semantic == "SV_ClipDistance1") clip1 = int(i);
        else if (params[i].semantic == "SV_VertexID") vertex_id = int(i);
        else if (!params[i].out) {
            r.error = "an input other than the vertex index: " + params[i].name;
            return r;
        }
    }
    if (pos < 0 || vertex_id < 0) {
        r.error = "no SV_Position or SV_VertexID";
        return r;
    }

    // The function's parameter list (no semantics) and the body with last frame's constants.
    std::string signature;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) signature += ", ";
        signature += (params[i].out ? "out " : "") + params[i].type + " " + params[i].name;
    }
    std::string previous_body = body;
    ReplaceAll(previous_body, "kk_VC[", "kk_VCPrev[");
    ReplaceAll(previous_body, "kk_VSConst(", "kk_VSConstPrev(");
    ReplaceAll(previous_body, "kkConstRel(", "kkConstRelPrev(");

    std::string out = prefix;
    out += "// Motion variant (docs/upscalers.md): last frame's constants.\n";
    out += "cbuffer KKMotionConstants : register(b5, space0)\n{\n\tfloat4 kk_VCPrev[256];\n\tfloat4 kk_MotionParams;\n};\n\n";
    out += "float4 kk_VSConstPrev(int index)\n{\n\treturn select(uint(index) < 256u, kk_VCPrev[min(uint(index), 255u)], "
           "float4(0.0, 0.0, 0.0, 0.0));\n}\n\n";
    // The literal-aware relative read, if the shader has one, for last frame's constants too.
    const std::string rel = "float4 kkConstRel(int index)\n{";
    if (const size_t rel_at = prefix.find(rel); rel_at != std::string::npos) {
        const size_t rel_end = prefix.find("\n}\n", rel_at);
        if (rel_end == std::string::npos) {
            r.error = "kkConstRel not understood";
            return r;
        }
        std::string copy = prefix.substr(rel_at, rel_end + 3 - rel_at);
        ReplaceAll(copy, "kkConstRel(", "kkConstRelPrev(");
        ReplaceAll(copy, "kk_VSConst(", "kk_VSConstPrev(");
        out += copy + "\n";
    }
    out += "void kk_MotionCurrent(" + signature + ")\n{\n" + body + "\n";
    out += "void kk_MotionPrevious(" + signature + ")\n{\n" + previous_body + "\n";
    // The entry point.
    out += "void main(\n\tuint iVertexId : SV_VertexID,\n\tout float4 oPos : SV_Position,\n"
           "\tout float4 oMotionCurrent : KKMOTION0,\n\tout float4 oMotionPrevious : KKMOTION1";
    if (clip0 >= 0) out += ",\n\tout " + params[clip0].type + " oClip0 : SV_ClipDistance0";
    if (clip1 >= 0) out += ",\n\tout " + params[clip1].type + " oClip1 : SV_ClipDistance1";
    out += ")\n{\n";
    for (size_t i = 0; i < params.size(); ++i) {
        if (!params[i].out) continue;
        out += "\t" + params[i].type + " kkA" + std::to_string(i) + ";\n";
        out += "\t" + params[i].type + " kkB" + std::to_string(i) + ";\n";
    }
    auto call = [&](const char* fn, const char* prefix_name) {
        std::string c = std::string("\t") + fn + "(";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) c += ", ";
            c += params[i].out ? std::string(prefix_name) + std::to_string(i) : params[i].name;
        }
        return c + ");\n";
    };
    out += call("kk_MotionCurrent", "kkA");
    out += call("kk_MotionPrevious", "kkB");
    out += "\toPos = kkA" + std::to_string(pos) + ";\n";
    out += "\toMotionCurrent = kkA" + std::to_string(pos) + ";\n";
    out += "\toMotionPrevious = kkB" + std::to_string(pos) + ";\n";
    if (clip0 >= 0) out += "\toClip0 = kkA" + std::to_string(clip0) + ";\n";
    if (clip1 >= 0) out += "\toClip1 = kkA" + std::to_string(clip1) + ";\n";
    out += "}\n";
    r.hlsl = std::move(out);
    r.ok = true;
    return r;
}

}  // namespace kkshaders
