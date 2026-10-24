#include "backend/ao_classifier.h"
#include "backend/tests/check.h"
#include "backend/tests/hotspot_bench.h"
#include <random>
using namespace nr;

// Pre-task-340 predicates share a translation unit with the linear loop, as in production.
AoBlend ReferenceBlendKind(uint32_t control) {
  // Xenos BlendFactor: ZERO 0, ONE 1, SRCCOLOR 4, INVSRCCOLOR 5, SRCALPHA 6, INVSRCALPHA 7, DESTCOLOR 8.
  const uint32_t src = control & 0x1F, op = (control >> 5) & 7, dst = (control >> 8) & 0x1F;
  if (src == 1 && dst == 0 && op == 0) return AoBlend::kOff;
  if (op == 0 && dst == 7) return AoBlend::kAlpha;  // SRCALPHA (or premultiplied ONE), INVSRCALPHA
  if (op == 0 && dst == 1) return AoBlend::kAdditive;
  if (op == 0 && ((src == 8 && dst == 0) || (src == 0 && dst == 4))) return AoBlend::kMultiply;
  return AoBlend::kOther;
}

bool ReferenceTriMatches(AoTri want, bool have) { return want == AoTri::kAny || (want == AoTri::kYes) == have; }
bool ReferenceMatches(const AoRule& r, const AoDrawState& s) {
  // Every condition must hold, so their order changes nothing: the shader hashes first, as most
  // built-in rules name a shader and a draw of another fails there at once (task 085: classifying
  // each draw against the 58 built-in rules, for the AO and the GI, was 5% of the render thread).
  if (r.ps_hash && r.ps_hash != s.ps_hash) return false;
  if (r.vs_hash && r.vs_hash != s.vs_hash) return false;
  if (!ReferenceTriMatches(r.color, s.color) || !ReferenceTriMatches(r.depth_test, s.depth_test) ||
      !ReferenceTriMatches(r.depth_write, s.depth_write) || !ReferenceTriMatches(r.alpha_test, s.alpha_test))
    return false;
  if (r.blend != AoBlend::kAny) {
    const AoBlend kind = ReferenceBlendKind(s.blend_control);
    if (r.blend == AoBlend::kOn ? kind == AoBlend::kOff : kind != r.blend) return false;
  }
  if (r.primitives && (s.primitive >= 32 || !(r.primitives & (1u << s.primitive)))) return false;
  return true;
}

// Pre-task-340 Classify, kept independent of the indexed production implementation.
NR_NOINLINE AoDrawClass LinearClassify(const AoRuleTable& table, const AoDrawState& s, const AoRule** matched) {
  for (const AoRule& r : table.rules()) if (ReferenceMatches(r, s)) {
    if (matched) *matched = &r;
    return r.result;
  }
  if (matched) *matched = nullptr;
  return AoDrawClass::kOpaque;
}
NR_NOINLINE AoDrawClass IndexedClassify(const AoRuleTable& table, const AoDrawState& s, const AoRule** matched) {
  return table.Classify(s, matched);
}

TEST(IndexedAoMatchesLinearIncludingRuleIdentity) {
  std::mt19937_64 rng(340);
  for (unsigned pass = 0; pass < 200; ++pass) {
    std::vector<AoRule> first;
    for (unsigned j = 0; j < pass % 81; ++j) {
      AoRule r;
      r.result = AoDrawClass(rng() % 6);
      r.ps_hash = rng() % 5 ? rng() % 16 : 0;
      r.vs_hash = rng() % 3 ? 0 : rng() % 8;
      r.color = AoTri(rng() % 3); r.depth_test = AoTri(rng() % 3);
      r.depth_write = AoTri(rng() % 3); r.alpha_test = AoTri(rng() % 3);
      r.blend = AoBlend(rng() % 8); r.primitives = rng() % 2 ? 0 : uint32_t(rng());
      first.push_back(r);
    }
    const AoRuleTable original(first);
    AoRuleTable table = original;  // indexes must survive copies, assignment and moves
    if (pass % 2) table = AoRuleTable(first);
    for (unsigned j = 0; j < 2000; ++j) {
      AoDrawState s;
      s.color = rng() & 1; s.depth = rng() & 1; s.depth_test = rng() & 1;
      s.depth_write = rng() & 1; s.alpha_test = rng() & 1;
      s.blend_control = uint32_t(rng()); s.primitive = uint32_t(rng() % 40);
      s.vs_hash = rng() % 8;
      s.ps_hash = j % 2 ? rng() % 16 : table.rules()[rng() % table.rules().size()].ps_hash;
      const AoRule *a = nullptr, *b = nullptr;
      CHECK_EQ(unsigned(LinearClassify(table, s, &a)), unsigned(table.Classify(s, &b)));
      CHECK(a == b);
      CHECK_EQ(unsigned(LinearClassify(table, s, nullptr)), unsigned(table.Classify(s)));
    }
  }
}

int main(int argc, char** argv) {
  if (nr::test::RunAll(1, argv)) return 1;
  if (!nr::test::BenchmarkRequested(argc, argv)) return 0;
  AoRuleTable table;
  std::vector<AoDrawState> states(1572);  // task 336 e4 logged peak draws/list
  std::mt19937_64 rng(340);
  for (size_t i = 0; i < states.size(); ++i) {
    auto& s = states[i];
    s.color = i % 7 != 0; s.depth = s.depth_test = true; s.depth_write = i % 5 != 0;
    s.alpha_test = i % 4 == 0; s.blend_control = i % 5 ? 0x10001 : 0x10706;
    s.ps_hash = i % 10 ? rng() : table.rules()[i % table.rules().size()].ps_hash;
  }
  constexpr unsigned loops = 4000;
  auto run = [&](auto fn) {
    uint64_t sum = 0;
    for (unsigned n = 0; n < loops; ++n) for (const auto& s : states) {
      const AoRule* matched = nullptr;
      sum += unsigned(fn(table, s, &matched)) + uint64_t(matched - table.rules().data());
    }
    return sum;
  };
  nr::test::Benchmark("ao-classify-58-rules", loops * states.size(), [&] { return run(LinearClassify); },
                       [&] { return run(IndexedClassify); });
}
