// The lighting options' water reflection hook of the translator (kkshaders/lighting.h,
// docs/lighting.md), on the generated corpus (no game data):
//  - the game's water list: every listed shader has a hook, a shader off the list has none;
//  - without the hook every translation and input hash is what it was, and vertex shaders never change;
//  - for every corpus pixel shader with a 2D or cube texture fetch, the hook on that fetch constant
//    (and, in turn, on the colour output, and with the normal map) is the plain translation plus exactly
//    the hook's additions: the lighting text, the trace at the top of main, the fetch's texel through
//    kl_WaterReflection, the output through kl_OceanReflection; it has its own input hash, and in turn
//    builds to signed DXIL and to SPIR-V;
//  - the pack stamp: a pack built without the water shaders, or with another version of the hook, is
//    stale for a run that wants them; one with them still serves a run without.
//
// Usage: kkshaders_lighting_tests [--dxc <dir>] [--fuzz N] [--seed N]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "corpus.h"
#include "kkshaders/cache.h"
#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/lighting.h"
#include "kkshaders/pack_builder.h"
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
std::string withoutHook(std::string hlsl) {
    const std::string& text = lightingText();
    const size_t at = hlsl.find(text);
    if (at != std::string::npos) hlsl.erase(at, text.size() + 1);  // and the newline after it
    static const std::regex line("^\\s*(KLSsr klSsr = kl_WaterTrace\\(iPos, .*\\);|float2 klUv = float2\\(0\\.0, 0\\.0\\);|"
                                 "klUv = kc2?;|kt = kl_WaterReflection\\(kt, klSsr, klUv, (true|false)\\);|oC0 = kl_OceanReflection\\(oC0, klSsr\\);)$");
    std::string out;
    size_t pos = 0;
    while (pos < hlsl.size()) {
        size_t eol = hlsl.find('\n', pos);
        size_t end = eol == std::string::npos ? hlsl.size() : eol + 1;
        std::string piece = hlsl.substr(pos, end - pos);
        std::string bare = piece.back() == '\n' ? piece.substr(0, piece.size() - 1) : piece;
        if (!std::regex_match(bare, line)) out += piece;
        pos = end;
    }
    return out;
}

void testList() {
    size_t n = 0;
    const uint64_t* list = waterReflectionShaders(&n);
    CHECK(n == 13);
    int fetch = 0, output = 0, normal = 0;
    for (size_t i = 0; i < n; i++) {
        const WaterReflectionHook h = waterReflectionHookFor(list[i]);
        CHECK(h.enabled());
        if (h.fetchMask) fetch++;
        if (h.output) output++;
        if (h.normalMap) normal++;
        CHECK((h.fetchMask & ~0x6u) == 0);  // the planar reflection (1) and the environment cube (2) only
    }
    CHECK(fetch == 13 && output == 0 && normal == 7);
    CHECK(!waterReflectionHookFor(0x1B65AC7429B67FE9ull).enabled());  // the fog pass
    CHECK(!waterReflectionHookFor(0xAC2239A7CA9EDF1Cull).enabled());  // pswater's mask: no reflection of its own
    CHECK(!lightingText().empty() && lightingText().find("cbuffer KKLighting : register(b5, space0)") != std::string::npos);
    WaterReflectionHook a;
    a.fetchMask = 6;
    a.normalMap = true;
    CHECK(formatWaterReflectionHook(a) == "fetch 1 2 normal-map");
}

void testStamp() {
    PackStamp found;
    found.translator = 1;
    found.dxc = "dxc";
    found.database = 2;
    PackStamp wanted = found;
    CHECK(comparePackStamp(&found, wanted).state == PackState::Current);
    wanted.lighting = 0x1234;
    CHECK(comparePackStamp(&found, wanted).state == PackState::Stale);  // without the water shaders
    found.lighting = 0x9999;
    CHECK(comparePackStamp(&found, wanted).state == PackState::Stale);  // another version of the hook
    found.lighting = 0x1234;
    CHECK(comparePackStamp(&found, wanted).state == PackState::Current);
    wanted.lighting = 0;
    CHECK(comparePackStamp(&found, wanted).state == PackState::Current);  // a pack with them serves a run without
    PackStamp back;
    CHECK(PackStamp::parse(found.toText(), back) && back.lighting == 0x1234);
    found.lighting = 0;
    CHECK(PackStamp::parse(found.toText(), back) && back.lighting == 0 && found.toText().find("lighting") == std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    testList();
    testStamp();
    const std::string dxc = option(argc, argv, "--dxc");
    const uint32_t fuzz = uint32_t(std::stoul(option(argc, argv, "--fuzz", "400")));
    const uint32_t seed = uint32_t(std::stoul(option(argc, argv, "--seed", "20261010")));
    std::string error;
    std::unique_ptr<Compiler> compiler;
    if (loadDxc(dxc, &error)) {
        compiler = std::make_unique<Compiler>();
    } else {
        std::printf("lighting: no DXC (%s): the builds are skipped\n", error.c_str());
    }
    const std::vector<CorpusShader> corpus = buildCorpus(fuzz, seed);
    int pixel = 0, vertex = 0, hooked2d = 0, hookedCube = 0, hookedOutput = 0, built = 0, turn = 0;
    auto fail = [&](const CorpusShader& s, const std::string& why) {
        failures++;
        std::printf("FAILED [%s] %s: %s\n", s.group.c_str(), s.name.c_str(), why.c_str());
    };
    for (const CorpusShader& s : corpus) {
        ParseResult p = s.raw ? parseMicrocode(s.vertex ? ShaderKind::Vertex : ShaderKind::Pixel, s.bytes.data(), s.bytes.size())
                              : parseContainer(s.bytes);
        if (!p.ok) continue;
        const ShaderInfo plain = p.info;
        const TranslateResult a = translate(plain);
        if (!a.ok) continue;
        for (const char* word : {"KKLighting", "kl_Water", "klSsr", "klUv"}) {
            if (a.hlsl.find(word) != std::string::npos) fail(s, std::string("the plain translation mentions ") + word);
        }
        WaterReflectionHook any;
        any.fetchMask = 0xFFFFFFFFu;
        any.output = true;
        if (plain.kind == ShaderKind::Vertex) {
            vertex++;
            ShaderInfo v = plain;
            v.waterReflection = any;
            const TranslateResult b = translate(v);
            if (!b.ok || b.hlsl != a.hlsl || translationInputHash(v) != translationInputHash(plain))
                fail(s, "a vertex shader changed with the hook");
            continue;
        }
        pixel++;
        // A fetch constant the shader samples with a 2D or cube fetch.
        int fetch2d = -1, fetchCube = -1;
        static const std::regex index("kk_TexIndex\\((\\d+)\\)");
        for (auto it = std::sregex_iterator(a.hlsl.begin(), a.hlsl.end(), index); it != std::sregex_iterator(); ++it) {
            const size_t from = size_t(it->position(0));
            const size_t to = a.hlsl.find("kk_TexIndex(", from + 1);
            const std::string block = a.hlsl.substr(from, to == std::string::npos ? std::string::npos : to - from);
            const int slot = std::stoi((*it)[1]);
            if (block.find("Texture2D<float4> kto") != std::string::npos && block.find("kt = kto.Sample") != std::string::npos &&
                block.find("Texture3D") == std::string::npos && fetch2d < 0)
                fetch2d = slot;
            if (block.find("TextureCube<float4> kto") != std::string::npos && block.find("kt = kto.Sample") != std::string::npos &&
                fetchCube < 0)
                fetchCube = slot;
        }
        std::vector<WaterReflectionHook> hooks;
        if (fetch2d >= 0) {
            WaterReflectionHook h;
            h.fetchMask = 1u << fetch2d;
            h.normalMap = (turn & 1) != 0;
            hooks.push_back(h);
        }
        if (fetchCube >= 0) {
            WaterReflectionHook h;
            h.fetchMask = 1u << fetchCube;
            hooks.push_back(h);
        }
        if (a.bindings.pixelOutputs & 1) {
            WaterReflectionHook h;
            h.output = true;
            hooks.push_back(h);
        }
        const uint64_t plainHash = translationInputHash(plain);
        for (size_t k = 0; k < hooks.size(); k++) {
            ShaderInfo h = plain;
            h.waterReflection = hooks[k];
            const TranslateResult b = translate(h);
            const std::string what = formatWaterReflectionHook(hooks[k]);
            if (!b.ok) {
                fail(s, what + ": translation failed: " + b.error);
                continue;
            }
            if (translationInputHash(h) == plainHash) fail(s, what + ": the same input hash as the plain translation");
            if (b.hlsl.find(lightingText()) == std::string::npos || countOf(b.hlsl, "KLSsr klSsr = kl_WaterTrace(iPos, ") != 1)
                fail(s, what + ": the declarations or the trace are missing");
            if (hooks[k].normalMap && b.hlsl.find("kk_TexIndex(3)].Sample(kk_Sampler[kl_SsrSampler.z], iT1.zw)") == std::string::npos)
                fail(s, what + ": the normal map is not read");
            if (hooks[k].fetchMask) {
                const bool cube = fetchCube >= 0 && hooks[k].fetchMask == (1u << fetchCube) && fetch2d != fetchCube;
                if (countOf(b.hlsl, cube ? "klSsr, klUv, false)" : "klSsr, klUv, true)") == 0) {
                    fail(s, what + ": the fetch does not go through the reflection");
                    const size_t at = b.hlsl.find("kk_TexIndex(" + std::to_string(cube ? fetchCube : fetch2d) + ")");
                    if (std::getenv("KK_LIGHTING_TEST_VERBOSE")) std::printf("at %zu\n%s\n", at, at == std::string::npos ? b.hlsl.c_str() : b.hlsl.substr(at > 600 ? at - 600 : 0, 2500).c_str());
                }
                (cube ? hookedCube : hooked2d)++;
            } else {
                if (countOf(b.hlsl, "oC0 = kl_OceanReflection(oC0, klSsr);") != countOf(a.hlsl, "kk_AlphaTest(oC0.w);"))
                    fail(s, what + ": the output does not go through the reflection at every end");
                hookedOutput++;
            }
            if (withoutHook(b.hlsl) != a.hlsl) fail(s, what + ": the hooked translation differs from the plain one by more than the hook");
            if (compiler && k == size_t(turn) % hooks.size()) {
                BuildResult r = buildShader(h, *compiler, false);
                if (!r.ok) {
                    fail(s, what + ": build: " + r.stage + ": " + r.error.substr(0, 400));
                } else if (!isDxilSigned(r.shader.dxil) || r.shader.spirv.empty()) {
                    fail(s, what + ": DXIL not signed or no SPIR-V");
                } else {
                    built++;
                }
            }
        }
        turn++;
    }
    std::printf("lighting: %d pixel shaders (%d with the hook on a 2D fetch, %d on a cube fetch, %d on the output), %d built, "
                "%d vertex shaders unchanged, %d failed\n",
                pixel, hooked2d, hookedCube, hookedOutput, built, vertex, failures);
    if (pixel == 0 || hooked2d == 0 || hookedOutput == 0) {
        std::printf("lighting: the corpus does not have every kind of hook\n");
        return 1;
    }
    return failures ? 1 : 0;
}
