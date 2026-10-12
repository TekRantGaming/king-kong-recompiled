// The ambient-only AO hook of the translator (kkshaders/ambient.h, docs/ao.md), on the generated
// corpus (no game data):
//  - the list and term formats read and print back;
//  - without a term every translation and input hash is what it was (the term is not part of
//    either), and vertex shaders never change;
//  - for every corpus pixel shader and every kind of term it has (its colour output, a constant it
//    reads, an interpolator, a texture fetch constant, a register after an instruction) the hooked
//    translation is the plain one plus exactly the hook's additions, multiplies the term, has its
//    own input hash, and (one term per shader, in turn) builds to signed DXIL and to SPIR-V.
//
// Usage: kkshaders_ambient_tests [--dxc <dir>] [--fuzz N] [--seed N]

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "corpus.h"
#include "kkshaders/ambient.h"
#include "kkshaders/cache.h"
#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/translator.h"

using namespace kkshaders;

namespace {

int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

std::string option(int argc, char** argv, const char* name, const char* fallback = "") {
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return fallback;
}

size_t countOf(const std::string& text, const std::string& word) {
    size_t n = 0;
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + word.size())) n++;
    return n;
}

// The hooked translation with the hook's additions taken out: must be the plain translation.
std::string withoutAmbientAdditions(std::string hlsl) {
    auto cut = [&](const std::string& from, const std::string& to) {
        size_t a = hlsl.find(from);
        if (a == std::string::npos) return;
        size_t b = hlsl.find(to, a);
        if (b != std::string::npos) hlsl.erase(a, b + to.size() - a);
    };
    cut("cbuffer KKAmbientOcclusion", "};\n\n");
    cut("float kk_AmbientOcclusion(", "}\n\n");
    static const std::regex line("^\\s*(float kkAo = .*|float4 kkAoMul = .*|(r\\d+|r\\[\\d+\\]|oC\\d) = \\2 \\* kkAoMul;)$");
    std::string out;
    size_t at = 0;
    while (at < hlsl.size()) {
        size_t eol = hlsl.find('\n', at);
        size_t end = eol == std::string::npos ? hlsl.size() : eol + 1;
        std::string text = hlsl.substr(at, end - at);
        std::string bare = text.back() == '\n' ? text.substr(0, text.size() - 1) : text;
        if (!std::regex_match(bare, line)) out += text;
        at = end;
    }
    static const std::regex wrapped("\\(([A-Za-z_][A-Za-z0-9_]*(\\[\\d+\\])?) \\* kkAoMul\\)");
    out = std::regex_replace(out, wrapped, "$1");
    static const std::regex suffix(" \\* kkAoMul;");
    return std::regex_replace(out, suffix, ";");
}

void testFormats() {
    AmbientTerm t;
    std::string why;
    CHECK(parseAmbientTerm("output 0", t, &why) && t.kind == AmbientTerm::Kind::Output && t.index == 0 && t.mask == 0x7);
    CHECK(parseAmbientTerm("constant c12 rgb", t) && t.kind == AmbientTerm::Kind::Constant && t.index == 12);
    CHECK(parseAmbientTerm("interpolator color1 a", t) && t.kind == AmbientTerm::Kind::Interpolator && t.usage == 10 &&
          t.index == 1 && t.mask == 0x8);
    CHECK(parseAmbientTerm("interpolator texcoord7 xy", t) && t.usage == 5 && t.index == 7 && t.mask == 0x3);
    CHECK(parseAmbientTerm("fetch 3", t) && t.kind == AmbientTerm::Kind::Fetch && t.index == 3);
    CHECK(parseAmbientTerm("register r5 after 17 xyzw", t) && t.kind == AmbientTerm::Kind::Register && t.index == 5 &&
          t.address == 17 && t.mask == 0xF);
    for (const char* bad : {"", "output 4", "constant 12", "constant c256", "interpolator fog0", "fetch 32",
                            "register r5 17", "register r5 after", "output 0 rgbq", "output 0 rgb extra", "ambient"}) {
        AmbientTerm u;
        if (parseAmbientTerm(bad, u, &why)) {
            std::printf("FAILED: '%s' was read\n", bad);
            failures++;
        }
    }
    for (const char* text : {"output 1", "constant c200 r", "interpolator texcoord15", "interpolator color0 rgba",
                             "fetch 31 b", "register r127 after 4095 xw"}) {
        AmbientTerm a, b;
        CHECK(parseAmbientTerm(text, a));
        CHECK(parseAmbientTerm(formatAmbientTerm(a), b) && a == b);
    }
    std::vector<std::string> errors;
    const AmbientTable table = parseAmbientTable(
        "# the lighting shaders\n"
        "1B65AC7429B67FE9 constant c4        # ambient colour\n"
        "0x00000000000000FF interpolator color0 rgb\n"
        "family psgeneric constant c4\n"
        "nothex output 0\n"
        "00000000000000AB unknown 3\n"
        "0 output 0\n",
        &errors);
    CHECK(table.byHash.size() == 2);
    CHECK(table.families.size() == 1);
    CHECK(errors.size() == 3);
    const AmbientTerm* found = table.find(0x1B65AC7429B67FE9ull);
    CHECK(found && found->kind == AmbientTerm::Kind::Constant && found->index == 4);
    CHECK(table.find(0xFF) && table.find(0xFF)->kind == AmbientTerm::Kind::Interpolator);
    CHECK(!table.find(0xAB));
}

}  // namespace

int main(int argc, char** argv) {
    testFormats();
    const std::string dxc = option(argc, argv, "--dxc");
    const uint32_t fuzz = uint32_t(std::stoul(option(argc, argv, "--fuzz", "400")));
    const uint32_t seed = uint32_t(std::stoul(option(argc, argv, "--seed", "20261010")));
    std::string error;
    Compiler* compiler = nullptr;
    std::unique_ptr<Compiler> owned;
    if (loadDxc(dxc, &error)) {
        owned = std::make_unique<Compiler>();
        compiler = owned.get();
    } else {
        std::printf("ambient: no DXC (%s): the builds are skipped\n", error.c_str());
    }
    const std::vector<CorpusShader> corpus = buildCorpus(fuzz, seed);
    std::map<AmbientTerm::Kind, int> hooked;
    int pixel = 0, vertex = 0, built = 0, turn = 0;
    auto fail = [&](const CorpusShader& s, const AmbientTerm& t, const std::string& why) {
        failures++;
        std::printf("FAILED [%s] %s, %s: %s\n", s.group.c_str(), s.name.c_str(), formatAmbientTerm(t).c_str(), why.c_str());
    };
    for (const CorpusShader& s : corpus) {
        ParseResult p = s.raw ? parseMicrocode(s.vertex ? ShaderKind::Vertex : ShaderKind::Pixel, s.bytes.data(), s.bytes.size())
                              : parseContainer(s.bytes);
        if (!p.ok) continue;
        const ShaderInfo plain = p.info;
        const TranslateResult a = translate(plain);
        if (!a.ok) continue;  // the corpus test reports translation failures
        for (const char* word : {"KKAmbientOcclusion", "kk_AmbientOcclusion", "kkAo"}) {
            if (a.hlsl.find(word) != std::string::npos) fail(s, {}, std::string("the plain translation mentions ") + word);
        }
        if (plain.kind == ShaderKind::Vertex) {
            vertex++;
            ShaderInfo v = plain;
            parseAmbientTerm("output 0", v.ambientOcclusion);
            const TranslateResult b = translate(v);
            if (!b.ok || b.hlsl != a.hlsl || translationInputHash(v) != translationInputHash(plain))
                fail(s, v.ambientOcclusion, "a vertex shader changed with a term");
            continue;
        }
        pixel++;
        // The terms this shader has.
        std::vector<AmbientTerm> terms;
        AmbientTerm t;
        if (a.bindings.pixelOutputs & 1) {
            t = {};
            t.kind = AmbientTerm::Kind::Output;
            terms.push_back(t);
        }
        std::smatch m;
        if (std::regex_search(a.hlsl, m, std::regex("kk_PC\\[(\\d+)\\]"))) {
            t = {};
            t.kind = AmbientTerm::Kind::Constant;
            t.index = uint8_t(std::stoi(m[1]));
            terms.push_back(t);
        }
        for (const Interpolator& i : plain.interpolators) {
            if (i.usage != DeclUsage::Color && i.usage != DeclUsage::TexCoord) continue;
            t = {};
            t.kind = AmbientTerm::Kind::Interpolator;
            t.usage = i.usage == DeclUsage::Color ? 10 : 5;
            t.index = i.usageIndex;
            terms.push_back(t);
            break;
        }
        // A fetch constant the shader samples (getCompTexLOD and getWeights read no texels, and are
        // not multiplied): the code from its "kk_TexIndex(n)" to the next one samples.
        {
            static const std::regex index("kk_TexIndex\\((\\d+)\\)");
            for (auto it = std::sregex_iterator(a.hlsl.begin(), a.hlsl.end(), index); it != std::sregex_iterator(); ++it) {
                const size_t from = size_t(it->position(0));
                const size_t to = a.hlsl.find("kk_TexIndex(", from + 1);
                const std::string block = a.hlsl.substr(from, to == std::string::npos ? std::string::npos : to - from);
                if (block.find("kt = kto.Sample") == std::string::npos) continue;
                t = {};
                t.kind = AmbientTerm::Kind::Fetch;
                t.index = uint8_t(std::stoi((*it)[1]));
                terms.push_back(t);
                break;
            }
        }
        {
            TranslateOptions annotate;
            annotate.annotateAddresses = true;
            const TranslateResult n = translate(plain, annotate);
            if (n.ok && std::regex_search(n.hlsl, m, std::regex("// @(\\d+) alu"))) {
                t = {};
                t.kind = AmbientTerm::Kind::Register;
                t.index = 0;
                t.address = uint16_t(std::stoi(m[1]));
                terms.push_back(t);
            }
            // The annotation is debugging output only: without it, the plain text.
            if (n.ok && std::regex_replace(n.hlsl, std::regex("[ \\t]*// @\\d+ (alu|fetch)\\n"), "") != a.hlsl)
                fail(s, {}, "the annotated translation differs by more than its comments");
        }
        const uint64_t plainHash = translationInputHash(plain);
        for (size_t k = 0; k < terms.size(); k++) {
            ShaderInfo h = plain;
            h.ambientOcclusion = terms[k];
            const TranslateResult b = translate(h);
            if (!b.ok) {
                fail(s, terms[k], "translation failed: " + b.error);
                continue;
            }
            hooked[terms[k].kind]++;
            if (translationInputHash(h) == plainHash) fail(s, terms[k], "the same input hash as the plain translation");
            if (b.hlsl.find("cbuffer KKAmbientOcclusion : register(b4, space0)") == std::string::npos ||
                b.hlsl.find("float kkAo = kk_AmbientOcclusion(iPos);") == std::string::npos)
                fail(s, terms[k], "the declarations are missing");
            switch (terms[k].kind) {
                case AmbientTerm::Kind::Output:
                    if (countOf(b.hlsl, "oC0 = oC0 * kkAoMul;") == 0) fail(s, terms[k], "the output is not multiplied");
                    break;
                case AmbientTerm::Kind::Constant: {
                    const std::string c = "kk_PC[" + std::to_string(terms[k].index) + "]";
                    if (countOf(b.hlsl, "(" + c + " * kkAoMul)") != countOf(a.hlsl, c))
                        fail(s, terms[k], "not every read of the constant is multiplied");
                    break;
                }
                case AmbientTerm::Kind::Interpolator:
                    if (countOf(b.hlsl, " * kkAoMul;") == 0) fail(s, terms[k], "the interpolator is not multiplied");
                    break;
                case AmbientTerm::Kind::Fetch:
                    if (countOf(b.hlsl, "(kt * kkAoMul)") == 0) fail(s, terms[k], "the fetch is not multiplied");
                    break;
                case AmbientTerm::Kind::Register:
                    if (countOf(b.hlsl, " * kkAoMul;") == 0) fail(s, terms[k], "the register is not multiplied");
                    break;
                default:
                    break;
            }
            if (withoutAmbientAdditions(b.hlsl) != a.hlsl)
                fail(s, terms[k], "the hooked translation differs from the plain one by more than the hook");
            // One term per shader is built, each kind in turn.
            if (compiler && k == size_t(turn) % terms.size()) {
                BuildResult r = buildShader(h, *compiler, false);
                if (!r.ok) {
                    fail(s, terms[k], "build: " + r.stage + ": " + r.error.substr(0, 400));
                } else if (!isDxilSigned(r.shader.dxil) || r.shader.spirv.empty()) {
                    fail(s, terms[k], "DXIL not signed or no SPIR-V");
                } else {
                    built++;
                }
            }
        }
        turn++;
    }
    std::printf("ambient: %d pixel shaders (%d output, %d constant, %d interpolator, %d fetch, %d register terms), "
                "%d built, %d vertex shaders unchanged, %d failed\n",
                pixel, hooked[AmbientTerm::Kind::Output], hooked[AmbientTerm::Kind::Constant],
                hooked[AmbientTerm::Kind::Interpolator], hooked[AmbientTerm::Kind::Fetch],
                hooked[AmbientTerm::Kind::Register], built, vertex, failures);
    if (pixel == 0 || hooked.size() < 5) {
        std::printf("ambient: the corpus does not have every kind of term\n");
        return 1;
    }
    return failures ? 1 : 0;
}
