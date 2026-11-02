// The game renderer's hooks for parallax occlusion mapping (kkshaders/parallax.h, docs/lighting.md). The draw path asks
// ParallaxDraw for each draw and then uses the hooked variant of its pixel shader once it is ready (game_renderer.cpp).

#include <cstring>

#include "backend/game_renderer.h"
#include "backend/lighting_shaders.h"
#include "thirdparty/XeGTAO/XeGTAO.h"

namespace nr {

bool GameRenderer::ParallaxDraw(const dev::DeviceView& d, const PipelineRecord& record, const GameShader& vs,
                                const GameShader& ps) const {
  // The world's opaque colour passes with a base texture and a normal map: not a character (skinned), not alpha tested
  // (leaves, grass), not one of the lighting families (water, fog, sprites and the like).
  if (LightFamilyOf(ps.ucode_hash) != LightFamily::kNone || (d.u32(dev::kRbColorControl) & 8)) return false;
  const uint32_t dc = record.depth_control, bc = record.color_control[0];
  const bool opaque = (bc & 0x1F) == 1 && ((bc >> 5) & 7) == 0 && ((bc >> 8) & 0x1F) == 0;
  if (!record.color_count || record.color_index[0] != 0 || !(record.color_mask[0] & 7) || !opaque || !record.depth_format ||
      !(dc & 2) || ((dc >> 4) & 7) == 7 || (record.flags & kRecordTessellation))
    return false;
  bool base = false, normal = false;
  for (const auto& t : ps.bindings.textures) {
    base = base || t.slot == 0;
    normal = normal || t.slot == 1;
  }
  if (!base || !normal || !vs.source) return false;
  for (const auto& c : vs.source->constants)
    if (c.name == "g_amSkinning") return false;
  return true;
}

void GameRenderer::ParallaxNoteView(const Gtao& gtao) {
  const std::vector<uint8_t>& g = gtao.constants_bytes();
  constexpr size_t kShared = 96 + 16 + 16;  // XeGTAO's constants, the picture, the working size (gtao.hlsl)
  if (g.size() < kShared + 16) return;
  XeGTAO::GTAOConstants c;
  std::memcpy(&c, g.data(), sizeof(c));
  int32_t source[4];
  uint32_t working[4];
  float params[4];
  std::memcpy(source, g.data() + 96, sizeof(source));
  std::memcpy(working, g.data() + 96 + 16, sizeof(working));
  std::memcpy(params, g.data() + kShared, sizeof(params));
  parallax_view_.depth[0] = c.DepthUnpackConsts.x;
  parallax_view_.depth[1] = c.DepthUnpackConsts.y;
  parallax_view_.screen[0] = c.NDCToViewMul.x;
  parallax_view_.screen[1] = c.NDCToViewMul.y;
  parallax_view_.screen[2] = c.NDCToViewAdd.x;
  parallax_view_.screen[3] = c.NDCToViewAdd.y;
  parallax_view_.rect[0] = float(source[0]);
  parallax_view_.rect[1] = float(source[1]);
  parallax_view_.rect[2] = params[0] * float(working[2]);
  parallax_view_.rect[3] = params[0] * float(working[3]);
  parallax_view_.valid = parallax_view_.rect[2] > 0.0f && parallax_view_.rect[3] > 0.0f;
}

}  // namespace nr
