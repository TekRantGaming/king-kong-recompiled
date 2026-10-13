// The motion variant of the translated vertex shaders (kkshaders::makeMotionVertexShader,
// docs/upscalers.md): every vertex shader of the translator's generated corpus (containers and bare
// microcode, fixed and fuzzed programs) gets a variant that compiles to DXIL and SPIR-V, whose entry
// point has exactly the outputs the motion pass needs, and whose second copy of the body reads only
// last frame's constants. What the variant computes is checked on the GPU by nr_upscale_gpu_tests.
#include <cstdio>
#include <string>

#include "backend/tests/check.h"
#include "corpus.h"
#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/translator.h"

namespace {

size_t Count(const std::string& s, const std::string& what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

// The body of a function in the text (from its opening brace to the matching closing one).
std::string FunctionBody(const std::string& s, const std::string& signature_start) {
  const size_t at = s.find(signature_start);
  if (at == std::string::npos) return {};
  const size_t open = s.find('{', at);
  int depth = 0;
  for (size_t i = open; i < s.size(); ++i) {
    if (s[i] == '{') ++depth;
    if (s[i] == '}' && --depth == 0) return s.substr(open, i + 1 - open);
  }
  return {};
}

}  // namespace

TEST(MotionShader_EveryCorpusVertexShader) {
  std::string error;
  if (!kkshaders::loadDxc(NR_TEST_DXC_DIR, &error)) {
    std::printf("  DXC not loaded (%s): compile checks skipped\n", error.c_str());
  }
  kkshaders::Compiler compiler;
  const bool compile = kkshaders::dxcLoaded() && compiler.ok();
  const std::vector<CorpusShader> corpus = buildCorpus(60, 1);
  size_t vertex = 0, made = 0, compiled = 0;
  for (const CorpusShader& s : corpus) {
    if (!s.vertex) continue;
    kkshaders::ParseResult p = s.raw ? kkshaders::parseMicrocode(kkshaders::ShaderKind::Vertex, s.bytes.data(), s.bytes.size())
                                     : kkshaders::parseContainer(s.bytes.data(), s.bytes.size());
    if (!p.ok) continue;
    kkshaders::TranslateResult t = kkshaders::translate(p.info);
    if (!t.ok) continue;  // the corpus test owns translation failures
    ++vertex;
    kkshaders::MotionShader m = kkshaders::makeMotionVertexShader(t.hlsl);
    if (!m.ok) {
      std::printf("  %s/%s: %s\n", s.group.c_str(), s.name.c_str(), m.error.c_str());
      CHECK(m.ok);
      continue;
    }
    ++made;
    CHECK_EQ(Count(m.hlsl, "void main("), size_t(1));
    CHECK_EQ(Count(m.hlsl, ": KKMOTION0"), size_t(1));
    CHECK_EQ(Count(m.hlsl, ": KKMOTION1"), size_t(1));
    // The previous-frame copy reads no current constant; the current copy no previous one.
    const std::string prev = FunctionBody(m.hlsl, "void kk_MotionPrevious(");
    const std::string cur = FunctionBody(m.hlsl, "void kk_MotionCurrent(");
    CHECK(!prev.empty() && !cur.empty());
    CHECK_EQ(Count(prev, "kk_VC["), size_t(0));
    CHECK_EQ(Count(prev, "kk_VSConst("), size_t(0));
    CHECK_EQ(Count(cur, "kk_VCPrev["), size_t(0));
    CHECK_EQ(Count(cur, "kk_VSConstPrev("), size_t(0));
    CHECK_EQ(Count(prev, "kk_VCPrev[") + Count(prev, "kk_VSConstPrev(") + Count(prev, "kkConstRelPrev("),
             Count(cur, "kk_VC[") + Count(cur, "kk_VSConst(") + Count(cur, "kkConstRel("));
    if (!compile) continue;
    bool ok = true;
    for (kkshaders::CompileTarget target : {kkshaders::CompileTarget::Dxil, kkshaders::CompileTarget::Spirv}) {
      kkshaders::CompileOutput o = compiler.compile(m.hlsl, kkshaders::ShaderKind::Vertex, target);
      if (!o.ok || o.blob.empty()) {
        ok = false;
        std::printf("  %s/%s (%s): %s\n", s.group.c_str(), s.name.c_str(),
                    target == kkshaders::CompileTarget::Dxil ? "DXIL" : "SPIR-V", o.messages.substr(0, 600).c_str());
      }
    }
    CHECK(ok);
    if (ok) ++compiled;
  }
  std::printf("  %zu vertex shaders, %zu motion variants, %zu compiled to DXIL and SPIR-V\n", vertex, made, compiled);
  CHECK(vertex > 50);
  CHECK_EQ(made, vertex);
  if (compile) CHECK_EQ(compiled, vertex);
}

TEST(MotionShader_RefusesOtherText) {
  CHECK(!kkshaders::makeMotionVertexShader("").ok);
  CHECK(!kkshaders::makeMotionVertexShader("#define KK_PIXEL_SHADER 1\nvoid main(\n\tfloat4 iPos : SV_Position)\n{\n}\n").ok);
  CHECK(!kkshaders::makeMotionVertexShader("#define KK_VERTEX_SHADER 1\nvoid main(\n\tout float4 oPos)\n{\n}\n").ok);
}

NR_TEST_MAIN()
