// The draw classifier of the ambient occlusion (docs/ao.md): which draws are the
// opaque scene and which are transparent or effects drawn after it, from the
// state the renderer already has for every draw (depth test and write, blend,
// colour mask, targets, primitive, shaders).
//
// Two parts, both without a GPU:
//
//  - AoRuleTable: a small data-driven table. Each rule matches draw states
//    (and optionally a vertex or pixel shader hash) and names a class; the
//    first rule that matches wins. The built-in rules (kDefaultAoRules) come
//    last, so rules of a file (--native_ao_rules) placed before them win.
//  - AoFrameTracker: per frame, follows the main scene target through its
//    depth pre-pass, its opaque pass and the transparent and effect draws
//    after it, and says when each part ended. The AO is worked out from the
//    depth at the end of the opaque pass (or of the depth pre-pass, for the
//    ambient-only mode) and applied before the first transparent draw.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nr {

enum class AoDrawClass : uint8_t {
  kIgnore,       // does not count (writes nothing that matters, or a rule says so)
  kDepthOnly,    // the depth pre-pass: writes depth, no colour
  kOpaque,       // the scene's opaque surfaces
  kTransparent,  // blended surfaces drawn over them: water, glass, particles, smoke
  kEffect,       // screen passes and other draws without a depth test: fog, post effects
  kEnd,          // the draw that ends the opaque pass when a frame has one (the game's fog pass)
};
const char* AoDrawClassName(AoDrawClass c);

// A condition that may be left open.
enum class AoTri : uint8_t { kAny, kNo, kYes };

// The kind of blending of RB_BLENDCONTROL (colour part).
enum class AoBlend : uint8_t {
  kAny,       // rules only: anything
  kOff,       // ONE, ZERO, ADD
  kOn,        // rules only: anything but off
  kAlpha,     // SRCALPHA, INVSRCALPHA
  kAdditive,  // anything onto the destination with factor ONE (additive glow, particles)
  kMultiply,  // destination times the source (ZERO, SRCCOLOR or DESTCOLOR, ZERO)
  kOther,
};
AoBlend AoBlendKind(uint32_t blend_control);

// A draw as the classifier sees it.
struct AoDrawState {
  bool color = false;        // writes a colour target (a write mask and a pixel shader output)
  bool depth = false;        // a depth target is bound
  bool depth_test = false;   // RB_DEPTHCONTROL z_enable, with a depth target
  bool depth_write = false;  // z_write_enable, with the test on
  bool alpha_test = false;   // RB_COLORCONTROL alpha test enable
  uint32_t blend_control = 0x00010001;  // RB_BLENDCONTROL of the first colour target written
  uint32_t primitive = 4;    // GuestPrimitive value
  uint64_t vs_hash = 0, ps_hash = 0;  // microcode hashes (ps 0: no pixel shader)
};

struct AoRule {
  AoDrawClass result = AoDrawClass::kOpaque;
  AoTri color = AoTri::kAny, depth_test = AoTri::kAny, depth_write = AoTri::kAny, alpha_test = AoTri::kAny;
  AoBlend blend = AoBlend::kAny;
  uint32_t primitives = 0;  // bit (1 << GuestPrimitive value) per primitive allowed; 0: any
  uint64_t vs_hash = 0, ps_hash = 0;  // 0: any
  int line = 0;             // the line it was read from (logs)
  bool Matches(const AoDrawState& s) const;
};

// The built-in rules (the format of ParseAoRules).
extern const char kDefaultAoRules[];

// Rule lines: "<class> [key=value]...", '#' starts a comment. Classes: depth, opaque,
// transparent, effect, end, ignore. Keys: color, depth_test, depth_write, alpha_test (yes / no),
// blend (off, on, alpha, additive, multiply, other), prim (a comma list of point, line,
// linestrip, list, fan, strip, rect, quad), vs and ps (a 64-bit microcode hash in hex). A
// line that cannot be read is skipped and described in `errors`.
std::vector<AoRule> ParseAoRules(const std::string& text, std::vector<std::string>* errors = nullptr);
std::string FormatAoRule(const AoRule& rule);

class AoRuleTable {
 public:
  // The built-in rules only.
  AoRuleTable();
  // `first` before the built-in rules.
  explicit AoRuleTable(std::vector<AoRule> first);
  // The class of the first rule that matches (kOpaque when none does, which the built-in
  // table never leaves). `matched`: that rule, or null.
  AoDrawClass Classify(const AoDrawState& s, const AoRule** matched = nullptr) const;
  const std::vector<AoRule>& rules() const { return rules_; }

 private:
  std::vector<AoRule> rules_;
};

// The main scene target through one frame.
class AoFrameTracker {
 public:
  enum class Phase : uint8_t { kNone, kDepthPass, kOpaquePass, kAfterOpaque };

  struct Draw {
    uint64_t frame = 0;
    AoDrawClass cls = AoDrawClass::kIgnore;
    uint64_t color_key = 0;   // render target 0 (0: none)
    uint64_t depth_key = 0;   // the depth target (0: none)
    bool frame_size = false;  // the targets have the frame's width (the scene, not a shadow map or a post pass)
    bool writes_depth = false;
    // Shader-specific transparent rules name overlays (water, fur, heat shimmer), not the
    // generic blended decals interleaved with opaque draws. They are a safe end even if
    // the previous frame's fog shader is absent this frame.
    bool terminal_transparent = false;
  };
  struct Events {
    bool in_scene = false;         // the draw goes to the scene's colour or depth target
    bool depth_complete = false;   // before this draw: the scene's depth pre-pass ended
    bool opaque_complete = false;  // before this draw: the scene's opaque pass ended
  };

  Events OnDraw(const Draw& d);
  // A clear or a resolve's clear of a depth target.
  void OnDepthCleared(uint64_t frame, uint64_t depth_key);

  uint64_t frame() const { return frame_; }
  Phase phase() const { return phase_; }
  uint64_t scene_color() const { return scene_color_; }
  uint64_t scene_depth() const { return scene_depth_ ? scene_depth_ : prepass_depth_; }
  // The scene depth target holds this frame's depth: written by its draws and not cleared since.
  bool depth_valid() const { return depth_valid_; }
  uint32_t depth_only_draws() const { return depth_only_draws_; }
  uint32_t opaque_draws() const { return opaque_draws_; }
  // The previous frame had a reliable end (fog or a known overlay) in its scene: generic
  // transparent/effect draws before that boundary (decals and second passes) do not end it.
  bool expects_end() const { return expect_end_; }

 private:
  void NewFrame(uint64_t frame);

  uint64_t frame_ = UINT64_MAX;
  Phase phase_ = Phase::kNone;
  uint64_t scene_color_ = 0, scene_depth_ = 0, prepass_depth_ = 0;
  bool depth_valid_ = false;
  bool expect_end_ = false, saw_end_ = false;
  uint32_t depth_only_draws_ = 0, opaque_draws_ = 0;
};

}  // namespace nr
