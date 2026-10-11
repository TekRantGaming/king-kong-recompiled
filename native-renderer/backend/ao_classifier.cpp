// The AO draw classifier: see ao_classifier.h.

#include "backend/ao_classifier.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace nr {

// First match wins; a file's rules come before these.
//  - the depth pre-pass writes depth and no colour;
//  - draws that write neither (occlusion surveys, stencil marks) do not count;
//  - without a depth test: screen passes (fog, post effects) once the scene has begun, the
//    sky or a backdrop before it (the frame tracker ignores those);
//  - unblended draws are the opaque scene (alpha-tested foliage too);
//  - the rest is blended over it: water, glass, particles, smoke, decals.
const char kDefaultAoRules[] =
    "depth        color=no depth_write=yes\n"
    "ignore       color=no\n"
    "effect       depth_test=no\n"
    "opaque       blend=off\n"
    "transparent\n";

const char* AoDrawClassName(AoDrawClass c) {
  switch (c) {
    case AoDrawClass::kIgnore: return "ignore";
    case AoDrawClass::kDepthOnly: return "depth";
    case AoDrawClass::kOpaque: return "opaque";
    case AoDrawClass::kTransparent: return "transparent";
    case AoDrawClass::kEffect: return "effect";
  }
  return "?";
}

AoBlend AoBlendKind(uint32_t control) {
  // Xenos BlendFactor: ZERO 0, ONE 1, SRCCOLOR 4, INVSRCCOLOR 5, SRCALPHA 6, INVSRCALPHA 7, DESTCOLOR 8.
  const uint32_t src = control & 0x1F, op = (control >> 5) & 7, dst = (control >> 8) & 0x1F;
  if (src == 1 && dst == 0 && op == 0) return AoBlend::kOff;
  if (op == 0 && dst == 7) return AoBlend::kAlpha;  // SRCALPHA (or premultiplied ONE), INVSRCALPHA
  if (op == 0 && dst == 1) return AoBlend::kAdditive;
  if (op == 0 && ((src == 8 && dst == 0) || (src == 0 && dst == 4))) return AoBlend::kMultiply;
  return AoBlend::kOther;
}

namespace {

bool TriMatches(AoTri want, bool have) { return want == AoTri::kAny || (want == AoTri::kYes) == have; }

struct PrimitiveName {
  const char* name;
  uint32_t value;
};
constexpr PrimitiveName kPrimitives[] = {{"point", 1}, {"line", 2}, {"linestrip", 3}, {"list", 4},
                                         {"fan", 5},   {"strip", 6}, {"rect", 8},      {"quad", 13}};

struct BlendName {
  const char* name;
  AoBlend blend;
};
constexpr BlendName kBlends[] = {{"off", AoBlend::kOff},           {"on", AoBlend::kOn},
                                 {"alpha", AoBlend::kAlpha},       {"additive", AoBlend::kAdditive},
                                 {"multiply", AoBlend::kMultiply}, {"other", AoBlend::kOther}};

struct ClassName {
  const char* name;
  AoDrawClass cls;
};
constexpr ClassName kClasses[] = {{"ignore", AoDrawClass::kIgnore},
                                  {"depth", AoDrawClass::kDepthOnly},
                                  {"opaque", AoDrawClass::kOpaque},
                                  {"transparent", AoDrawClass::kTransparent},
                                  {"effect", AoDrawClass::kEffect}};

bool ParseTri(const std::string& v, AoTri& out) {
  if (v == "yes" || v == "on" || v == "1" || v == "true") {
    out = AoTri::kYes;
    return true;
  }
  if (v == "no" || v == "off" || v == "0" || v == "false") {
    out = AoTri::kNo;
    return true;
  }
  return false;
}

bool ParseHash(const std::string& v, uint64_t& out) {
  std::string s = v;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
  if (s.empty() || s.size() > 16) return false;
  for (char c : s)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  out = std::strtoull(s.c_str(), nullptr, 16);
  return out != 0;
}

}  // namespace

bool AoRule::Matches(const AoDrawState& s) const {
  if (!TriMatches(color, s.color) || !TriMatches(depth_test, s.depth_test) ||
      !TriMatches(depth_write, s.depth_write) || !TriMatches(alpha_test, s.alpha_test))
    return false;
  if (blend != AoBlend::kAny) {
    const AoBlend kind = AoBlendKind(s.blend_control);
    if (blend == AoBlend::kOn ? kind == AoBlend::kOff : kind != blend) return false;
  }
  if (primitives && (s.primitive >= 32 || !(primitives & (1u << s.primitive)))) return false;
  if (vs_hash && vs_hash != s.vs_hash) return false;
  if (ps_hash && ps_hash != s.ps_hash) return false;
  return true;
}

std::vector<AoRule> ParseAoRules(const std::string& text, std::vector<std::string>* errors) {
  std::vector<AoRule> rules;
  std::istringstream lines(text);
  std::string line;
  int number = 0;
  auto error = [&](const std::string& what) {
    if (errors) errors->push_back("line " + std::to_string(number) + ": " + what);
  };
  while (std::getline(lines, line)) {
    ++number;
    if (size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream words(line);
    std::string word;
    if (!(words >> word)) continue;
    AoRule rule;
    rule.line = number;
    bool ok = false;
    for (const ClassName& c : kClasses) {
      if (word == c.name) {
        rule.result = c.cls;
        ok = true;
      }
    }
    if (!ok) {
      error("unknown class '" + word + "'");
      continue;
    }
    while (ok && (words >> word)) {
      const size_t eq = word.find('=');
      if (eq == std::string::npos) {
        error("'" + word + "' is not key=value");
        ok = false;
        break;
      }
      const std::string key = word.substr(0, eq), value = word.substr(eq + 1);
      if (key == "color" || key == "depth_test" || key == "depth_write" || key == "alpha_test") {
        AoTri* field = key == "color" ? &rule.color
                       : key == "depth_test" ? &rule.depth_test
                       : key == "depth_write" ? &rule.depth_write
                                              : &rule.alpha_test;
        ok = ParseTri(value, *field);
      } else if (key == "blend") {
        ok = false;
        for (const BlendName& b : kBlends) {
          if (value == b.name) {
            rule.blend = b.blend;
            ok = true;
          }
        }
      } else if (key == "prim") {
        std::istringstream list(value);
        std::string item;
        while (ok && std::getline(list, item, ',')) {
          bool found = false;
          for (const PrimitiveName& p : kPrimitives) {
            if (item == p.name) {
              rule.primitives |= 1u << p.value;
              found = true;
            }
          }
          ok = found;
        }
      } else if (key == "vs" || key == "ps") {
        ok = ParseHash(value, key == "vs" ? rule.vs_hash : rule.ps_hash);
      } else {
        ok = false;
      }
      if (!ok) error("cannot read '" + word + "'");
    }
    if (ok) rules.push_back(rule);
  }
  return rules;
}

std::string FormatAoRule(const AoRule& r) {
  std::string s = AoDrawClassName(r.result);
  auto tri = [&](const char* key, AoTri t) {
    if (t != AoTri::kAny) s += std::string(" ") + key + (t == AoTri::kYes ? "=yes" : "=no");
  };
  tri("color", r.color);
  tri("depth_test", r.depth_test);
  tri("depth_write", r.depth_write);
  tri("alpha_test", r.alpha_test);
  for (const BlendName& b : kBlends)
    if (r.blend == b.blend) s += std::string(" blend=") + b.name;
  if (r.primitives) {
    s += " prim=";
    bool first = true;
    for (const PrimitiveName& p : kPrimitives) {
      if (!(r.primitives & (1u << p.value))) continue;
      if (!first) s += ',';
      s += p.name;
      first = false;
    }
  }
  char hash[40];
  if (r.vs_hash) {
    std::snprintf(hash, sizeof(hash), " vs=%016llX", static_cast<unsigned long long>(r.vs_hash));
    s += hash;
  }
  if (r.ps_hash) {
    std::snprintf(hash, sizeof(hash), " ps=%016llX", static_cast<unsigned long long>(r.ps_hash));
    s += hash;
  }
  return s;
}

AoRuleTable::AoRuleTable() : AoRuleTable(std::vector<AoRule>{}) {}

AoRuleTable::AoRuleTable(std::vector<AoRule> first) : rules_(std::move(first)) {
  for (AoRule& r : ParseAoRules(kDefaultAoRules)) {
    r.line = -r.line;  // negative: a built-in rule
    rules_.push_back(r);
  }
}

AoDrawClass AoRuleTable::Classify(const AoDrawState& s, const AoRule** matched) const {
  for (const AoRule& r : rules_) {
    if (r.Matches(s)) {
      if (matched) *matched = &r;
      return r.result;
    }
  }
  if (matched) *matched = nullptr;
  return AoDrawClass::kOpaque;
}

void AoFrameTracker::NewFrame(uint64_t frame) {
  frame_ = frame;
  phase_ = Phase::kNone;
  scene_color_ = scene_depth_ = prepass_depth_ = 0;
  depth_valid_ = false;
  depth_only_draws_ = opaque_draws_ = 0;
}

AoFrameTracker::Events AoFrameTracker::OnDraw(const Draw& d) {
  if (d.frame != frame_) NewFrame(d.frame);
  Events e;
  if (d.cls == AoDrawClass::kIgnore) return e;
  if (!scene_color_) {
    // Not found yet: a depth pre-pass at the frame's size, then the first opaque draw at the
    // frame's size with a colour and a depth target names the scene's targets.
    if (!d.frame_size || !d.depth_key) return e;
    if (d.cls == AoDrawClass::kDepthOnly && (phase_ == Phase::kNone || d.depth_key == prepass_depth_)) {
      phase_ = Phase::kDepthPass;
      prepass_depth_ = d.depth_key;
      ++depth_only_draws_;
      if (d.writes_depth) depth_valid_ = true;
      e.in_scene = true;
      return e;
    }
    if (d.cls == AoDrawClass::kOpaque && d.color_key) {
      e.depth_complete = phase_ == Phase::kDepthPass && d.depth_key == prepass_depth_;
      if (d.depth_key != prepass_depth_) depth_valid_ = false;  // another depth target: its pre-pass is unknown
      scene_color_ = d.color_key;
      scene_depth_ = d.depth_key;
      phase_ = Phase::kOpaquePass;
      ++opaque_draws_;
      if (d.writes_depth) depth_valid_ = true;
      e.in_scene = true;
    }
    return e;
  }
  e.in_scene = d.color_key ? d.color_key == scene_color_ : d.depth_key == scene_depth_;
  if (!e.in_scene) return e;
  if (d.writes_depth && d.depth_key == scene_depth_) depth_valid_ = true;
  if (phase_ == Phase::kOpaquePass) {
    if (d.cls == AoDrawClass::kTransparent || d.cls == AoDrawClass::kEffect) {
      e.opaque_complete = true;
      phase_ = Phase::kAfterOpaque;
    } else if (d.cls == AoDrawClass::kOpaque) {
      ++opaque_draws_;
    }
  }
  return e;
}

void AoFrameTracker::OnDepthCleared(uint64_t frame, uint64_t depth_key) {
  if (frame != frame_) NewFrame(frame);
  if (depth_key && (depth_key == scene_depth_ || depth_key == prepass_depth_)) depth_valid_ = false;
}

}  // namespace nr
