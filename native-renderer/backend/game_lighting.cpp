// The game renderer's hooks for the lighting options (docs/lighting.md): what each draw, resolve
// and Present tells them. The renderer's files call these at a few points marked "Lighting hook
// (game_lighting.cpp)"; with the defaults (every option original, no census) none of this runs.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "backend/game_renderer.h"
#include "backend/lighting_shaders.h"
#include "backend/log.h"
#include "backend/shaders/generated/lighting_shadow_1_pcf_dxil.h"
#include "backend/shaders/generated/lighting_shadow_1_pcf_spirv.h"
#include "backend/shaders/generated/lighting_shadow_1_pcss_dxil.h"
#include "backend/shaders/generated/lighting_shadow_1_pcss_spirv.h"
#include "backend/shaders/generated/lighting_shadow_2_pcf_dxil.h"
#include "backend/shaders/generated/lighting_shadow_2_pcf_spirv.h"
#include "backend/shaders/generated/lighting_shadow_2_pcss_dxil.h"
#include "backend/shaders/generated/lighting_shadow_2_pcss_spirv.h"
#include "backend/shaders/generated/lighting_shadow_3_pcf_dxil.h"
#include "backend/shaders/generated/lighting_shadow_3_pcf_spirv.h"
#include "backend/shaders/generated/lighting_shadow_3_pcss_dxil.h"
#include "backend/shaders/generated/lighting_shadow_3_pcss_spirv.h"
#include "kknr/xenos.h"
#include "kkshaders/lighting.h"

namespace nr {

namespace {

// The soft shadows' psshadowquad for a variant (1 to 3 lights) and quality (PCSS or PCF).
ShaderLibrary::Replacement ShadowReplacement(bool spirv, uint32_t lights, bool pcss) {
#define NR_SHADOW(n, q)                                                                        \
  (spirv ? ShaderLibrary::Replacement{k_lighting_shadow_##n##_##q##_spirv,                     \
                                      sizeof(k_lighting_shadow_##n##_##q##_spirv), 0}          \
         : ShaderLibrary::Replacement{k_lighting_shadow_##n##_##q##_dxil,                      \
                                      sizeof(k_lighting_shadow_##n##_##q##_dxil), 0})
  ShaderLibrary::Replacement r;
  if (lights == 1) r = pcss ? NR_SHADOW(1, pcss) : NR_SHADOW(1, pcf);
  if (lights == 2) r = pcss ? NR_SHADOW(2, pcss) : NR_SHADOW(2, pcf);
  if (lights == 3) r = pcss ? NR_SHADOW(3, pcss) : NR_SHADOW(3, pcf);
#undef NR_SHADOW
  // A distinct input hash per quality: the replaced shader's pipelines never meet the game's.
  r.tag = pcss ? UINT64_C(0x4C69676853534350) : UINT64_C(0x4C69676850434621);
  return r;
}

const char* BlendKindName(uint32_t blend_control) {
  switch (AoBlendKind(blend_control)) {
    case AoBlend::kOff: return "off";
    case AoBlend::kAlpha: return "alpha";
    case AoBlend::kAdditive: return "add";
    case AoBlend::kMultiply: return "mul";
    default: return "other";
  }
}

}  // namespace

bool GameRenderer::LightingCensusFrame(uint64_t frame) const {
  const uint32_t n = options_.lighting.census_every;
  return n != 0 && frame % n == 0;
}

void GameRenderer::LightingCensusDraw(const DrawCall& call, const dev::DeviceView& d,
                                      const std::array<HostTarget*, 4>& colors, HostTarget* depth,
                                      const PipelineRecord& record, const GameShader& vs, const GameShader* ps) {
  // One line per draw of a census frame (docs/lighting.md, "Finding the passes"): the family, the
  // targets with their host sizes, the state, and every texture the pixel shader reads (guest size and
  // format, and the host size of a resolved one).
  const uint64_t ps_hash = ps ? ps->ucode_hash : 0;
  const LightFamily family = LightFamilyOf(ps_hash);
  std::string line;
  char buf[256];
  auto target = [&](const char* what, const HostTarget* t) {
    if (!t) return;
    std::snprintf(buf, sizeof(buf), " %s %llX %ux%u(%ux%u)", what, static_cast<unsigned long long>(t->key), t->width,
                  t->height, t->host_width, t->host_height);
    line += buf;
  };
  target("rt", colors[0]);
  target("ds", depth);
  const uint32_t blend = record.color_count ? record.color_control[0] : 0x00010001u;
  std::snprintf(buf, sizeof(buf), " blend %s %08X mask %X dt %d dw %d prim %u", BlendKindName(blend), blend,
                record.color_count ? record.color_mask[0] : 0, int((record.depth_control & 2) != 0),
                int((record.depth_control & 4) != 0), uint32_t(call.primitive));
  line += buf;
  if (ps) {
    for (const kkshaders::TextureUse& t : ps->bindings.textures) {
      uint32_t words[6];
      d.fetch_constant(t.slot & 31, words);
      const kknr::TextureFetch fetch = kknr::TextureFetch::FromWords(words);
      if (fetch.Type() != 2) {
        std::snprintf(buf, sizeof(buf), " t%u:-", t.slot);
        line += buf;
        continue;
      }
      std::snprintf(buf, sizeof(buf), " t%u:%08X %ux%u f%u d%u", t.slot, fetch.BaseAddress(), fetch.Width(),
                    fetch.Height(), uint32_t(fetch.Format()), uint32_t(fetch.Dim()));
      line += buf;
      auto it = resolved_by_base_.find(fetch.BaseAddress());
      if (it != resolved_by_base_.end() && it->second && it->second->host) {
        const auto* host = static_cast<const HostTexture*>(it->second->host);
        if (host->texture) {
          const nvrhi::TextureDesc& desc = host->texture->getDesc();
          std::snprintf(buf, sizeof(buf), " (resolved %ux%u%s)", desc.width, desc.height, host->depth ? " depth" : "");
          line += buf;
        }
      }
    }
  }
  Logf(LogLevel::kInfo, "rexgpu-native: LIGHT draw %u of frame %llu: %s ps %016llX vs %016llX%s",
       call.index_in_frame, static_cast<unsigned long long>(call.frame), LightFamilyName(family),
       static_cast<unsigned long long>(ps_hash), static_cast<unsigned long long>(vs.ucode_hash), line.c_str());
  // The constants of the passes the options build on.
  if (family == LightFamily::kShadowQuad || family == LightFamily::kLightShaft || family == LightFamily::kFog ||
      family == LightFamily::kWater || family == LightFamily::kOcean) {
    std::string c;
    for (uint32_t i = 0; i < 16; ++i) {
      std::snprintf(buf, sizeof(buf), " c%u(%.5g %.5g %.5g %.5g)", i, d.f32(dev::kPsConstants + i * 16),
                    d.f32(dev::kPsConstants + i * 16 + 4), d.f32(dev::kPsConstants + i * 16 + 8),
                    d.f32(dev::kPsConstants + i * 16 + 12));
      c += buf;
    }
    Logf(LogLevel::kInfo, "rexgpu-native: LIGHT   ps constants%s", c.c_str());
  }
}

void GameRenderer::LightingLayoutBindings(nvrhi::BindingLayoutDesc& set0) const {
  if (options_.effect_settings || options_.lighting.any()) set0.bindings.push_back(nvrhi::BindingLayoutItem::VolatileConstantBuffer(5));
}

bool GameRenderer::LightingSetBindings(nvrhi::BindingSetDesc& set0) {
  if (!options_.effect_settings && !options_.lighting.any()) return true;
  nvrhi::BufferDesc d;
  d.byteSize = sizeof(kkshaders::LightingConstants);
  d.isConstantBuffer = true;
  d.isVolatile = true;
  d.maxVersions = 8192;
  d.debugName = "Game lighting constants";
  lighting_constants_ = device_->createBuffer(d);
  if (!lighting_constants_) return false;
  set0.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(5, lighting_constants_));
  return true;
}

nvrhi::ITexture* GameRenderer::LightingResolvedTexture(const dev::DeviceView& d, uint32_t slot) {
  uint32_t words[6];
  d.fetch_constant(slot & 31, words);
  const kknr::TextureFetch fetch = kknr::TextureFetch::FromWords(words);
  if (fetch.Type() != 2) return nullptr;
  auto it = resolved_by_base_.find(fetch.BaseAddress());
  if (it == resolved_by_base_.end() || !it->second || !it->second->host) return nullptr;
  return static_cast<HostTexture*>(it->second->host)->texture;
}

void GameRenderer::LightingNoteShadowMask(const GameShader& ps, uint32_t pitch) {
  if (LightFamilyOf(ps.ucode_hash) != LightFamily::kShadowQuad || pitch == options_.guest_frame_width) return;
  if (lighting_mask_pitches_.insert(pitch).second)
    Logf(LogLevel::kInfo, "rexgpu-native: lighting: the screen-space shadow's targets (pitch %u) at %.3g x the render scale",
         pitch, kLightingMaskScale);
}

float GameRenderer::LightingShadowScale() const {
  return options_.shadow_scale > 0.0f ? options_.shadow_scale : options_.render_scale_x;
}

void GameRenderer::LightingInitialize() {
  const LightingConfig& l = options_.lighting;
  const bool live = bool(options_.effect_settings);
  if (!l.active() && !live) return;
  if (live && l.any()) {
    // Live settings: every pass's pipelines, so the options can change later (game_effects.cpp); the
    // timers feed the graphics panel (logged only with --native_lighting_gpu_time).
    LightingConfig all = l;
    all.water = WaterReflections::kSsr;
    all.volumetrics = Volumetrics::kHigh;
    all.gpu_time = true;
    scene_lighting_ = std::make_unique<SceneLighting>(device_, all);
    if (!scene_lighting_->Initialize()) {
      scene_lighting_.reset();
    } else {
      for (int i = 0; i < 3; ++i)
        if (AoGpuTimer* t = scene_lighting_->timer(i)) t->set_quiet(!l.gpu_time);
      LightingConfig wanted = l;
      wanted.gpu_time = true;
      scene_lighting_->SetConfig(wanted);
    }
  } else if (!live && (l.water_on() || l.volumetrics_on() || l.gpu_time)) {
    scene_lighting_ = std::make_unique<SceneLighting>(device_, l);
    if (!scene_lighting_->Initialize()) scene_lighting_.reset();
  }
  if (l.water_on() || options_.effect_settings) {
    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    lighting_.sampler = device_->createSampler(sd);
    if (lighting_.sampler) {
      lighting_.sampler_slot = AllocateSlot(kSamplers);
      WriteSlot(kSamplers, nvrhi::BindingSetItem::Sampler(lighting_.sampler_slot, lighting_.sampler));
    }
    // The water's normal map (tiled) for the reflections' roughness.
    sd.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    lighting_.wrap_sampler = device_->createSampler(sd);
    if (lighting_.wrap_sampler) {
      lighting_.wrap_sampler_slot = AllocateSlot(kSamplers);
      WriteSlot(kSamplers, nvrhi::BindingSetItem::Sampler(lighting_.wrap_sampler_slot, lighting_.wrap_sampler));
    }
  }
  Logf(LogLevel::kInfo,
       "rexgpu-native: lighting: shadows %s (shadow maps x%.3g), water reflections %s, volumetrics %s%s",
       ShadowQualityName(l.shadows), LightingShadowScale(), WaterReflectionsName(l.water), VolumetricsName(l.volumetrics),
       l.census_every ? (", census every " + std::to_string(l.census_every) + " frames").c_str() : "");
}

std::unordered_map<uint64_t, ShaderLibrary::Replacement> GameRenderer::LightingReplacements() const {
  std::unordered_map<uint64_t, ShaderLibrary::Replacement> out;
  if (!options_.lighting.shadows_on()) return out;
  const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  const bool pcss = options_.lighting.shadows == ShadowQuality::kUltra;
  for (uint64_t hash : {UINT64_C(0x9F5E02F361E2A8A0), UINT64_C(0x6DC2B9F3C32C2394), UINT64_C(0x9A0FBB044BB1BD85)}) {
    const ShadowQuadLayout layout = ShadowQuadLayoutOf(hash);
    ShaderLibrary::Replacement r = ShadowReplacement(spirv, layout.lights, pcss);
    if (r.data && r.size) out[hash] = r;
  }
  return out;
}

void GameRenderer::LightingWriteConstants(nvrhi::ICommandList* cl) {
  if (!lighting_constants_) return;
  const LightingConfig& l = options_.lighting;
  kkshaders::LightingConstants k = {};
  // Shadows (lighting_shadow.hlsl). Radii in guest shadow-map texels (832 per side), so the footprint
  // is the same at every shadow-map resolution and only the sampling gets finer.
  const float soft = l.shadow_softness;
  k.shadow[0] = 1.5f * soft;
  k.shadow[1] = 5.0f * soft;
  k.shadow[2] = lighting_.penumbra_scale * soft;
  k.shadow[3] = LightingShadowScale();
  k.shadowShape[0] = lighting_.slope_limit;
  k.shadowShape[1] = 0.6f;
  k.shadowShape[2] = 6.0f * soft;
  k.shadowShape[3] = l.shadows == ShadowQuality::kUltra ? 1.0f : 0.0f;
  // Water reflections (kk_lighting.hlsli): this frame's scene, once it is made.
  const SceneLighting::SsrView* ssr = scene_lighting_ ? &scene_lighting_->ssr() : nullptr;
  if (ssr && ssr->valid && lighting_.sampler_slot) {
    auto slot = [&](nvrhi::ITexture* texture, uint32_t& slot, nvrhi::ITexture*& slot_texture) {
      if (texture != slot_texture) {
        if (slot) RetireSlot(kTex2D, slot);
        slot = AllocateSlot(kTex2D);
        WriteSlot(kTex2D, nvrhi::BindingSetItem::Texture_SRV(slot, texture));
        slot_texture = texture;
      }
      return slot;
    };
    k.ssrTextures[0] = slot(ssr->color, lighting_.color_slot, lighting_.color_slot_texture);
    k.ssrTextures[1] = slot(ssr->depth, lighting_.depth_slot, lighting_.depth_slot_texture);
    k.ssrTextures[2] = ssr->depth_mips;
    k.ssrTextures[3] = 1;
    k.ssrRect[0] = float(ssr->x);
    k.ssrRect[1] = float(ssr->y);
    k.ssrRect[2] = float(ssr->width);
    k.ssrRect[3] = float(ssr->height);
    k.ssrSampler[0] = lighting_.sampler_slot;
    k.ssrSampler[1] = ssr->color_mips;
    k.ssrDepth[0] = ssr->mapping.depth_mul;
    k.ssrDepth[1] = ssr->mapping.depth_add;
    k.ssrDepth[2] = ssr->near_plane;
    k.ssrDepth[3] = ssr->far_plane;
    k.ssrScreen[0] = ssr->mapping.mul[0];
    k.ssrScreen[1] = ssr->mapping.mul[1];
    k.ssrScreen[2] = ssr->mapping.add[0];
    k.ssrScreen[3] = ssr->mapping.add[1];
  }
  k.ssrSampler[2] = lighting_.wrap_sampler_slot;
  k.ssrParams[0] = std::min(l.ssr_strength, 1.0f);
  k.ssrParams[1] = 3.0f;    // the longest ray: three times the water's view depth
  k.ssrParams[2] = 0.03f;   // a surface is 3 % of its depth thick
  k.ssrParams[3] = 64.0f;   // steps of the march
  k.ssrMisc[0] = 0.08f;     // fade over the outer 8 % of the picture
  k.ssrMisc[1] = 4.0f;      // colour mips per unit of normal-map slope
  k.ssrMisc[2] = 0.15f;     // normal-map tilt
  k.ssrMisc[3] = 0.02f;     // F0 of water
  // A volatile buffer is written in every command list that binds it, and when it changes.
  const bool new_list = !lighting_.constants_valid || lighting_.constants_list != cl || lighting_.constants_frame != frame_;
  if (!new_list && std::memcmp(&k, lighting_.constants_last.data(), sizeof(k)) == 0) return;
  std::memcpy(lighting_.constants_last.data(), &k, sizeof(k));
  cl->writeBuffer(lighting_constants_, &k, sizeof(k));
  lighting_.constants_valid = true;
  lighting_.constants_list = cl;
  lighting_.constants_frame = frame_;
}

std::shared_ptr<const GameShader> GameRenderer::LiveLightingShader(const GameShader& base) {
  if (!options_.effect_settings) return nullptr;
  const auto family = LightFamilyOf(base.ucode_hash);
  if (family == LightFamily::kShadowQuad && options_.lighting.shadows_on()) {
    const auto layout = ShadowQuadLayoutOf(base.ucode_hash);
    const auto replacement = ShadowReplacement(device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN,
                                               layout.lights, options_.lighting.shadows == ShadowQuality::kUltra);
    return shaders_->LightingVariant(base, replacement, false);
  }
  if ((family == LightFamily::kWater || family == LightFamily::kOcean) && options_.lighting.water_on())
    return shaders_->LightingVariant(base, {}, true);
  return nullptr;
}

void GameRenderer::LightingDrawHook(nvrhi::ICommandList* cl, const DrawCall& call, const dev::DeviceView& d,
                                    const std::array<HostTarget*, 4>& colors, HostTarget* depth,
                                    const PipelineRecord& record, const GameShader& vs, const GameShader* ps) {
  if (LightingCensusFrame(call.frame)) LightingCensusDraw(call, d, colors, depth, record, vs, ps);
  if (scene_lighting_) {
    SceneLighting::Draw a;
    a.frame = call.frame;
    a.family = LightFamilyOf(ps ? ps->ucode_hash : 0);
    AoDrawState& st = a.state;
    for (uint32_t i = 0; i < record.color_count; ++i) {
      if (!record.color_mask[i] || st.color) continue;
      st.color = true;
      st.blend_control = record.color_control[i];
    }
    st.depth = depth != nullptr;
    st.depth_test = depth && (record.depth_control & 2);
    st.depth_write = st.depth_test && (record.depth_control & 4);
    st.alpha_test = (d.u32(dev::kRbColorControl) & 8) != 0;
    st.primitive = uint32_t(call.primitive);
    st.vs_hash = vs.ucode_hash;
    st.ps_hash = ps ? ps->ucode_hash : 0;
    const HostTarget* size_from = colors[0] ? colors[0] : depth;
    if (colors[0]) a.color = {colors[0]->key, colors[0]->texture, colors[0]->scale_x, colors[0]->scale_y};
    if (depth) a.depth = {depth->key, depth->texture, depth->scale_x, depth->scale_y};
    a.frame_size = size_from && size_from->width == options_.guest_frame_width;
    a.vs_constants = d.at(dev::kVsConstants);
    // The viewport transform as ComputeViewport reads it (as the AO's hook does).
    const uint32_t vte = d.u32(dev::kPaClVteCntl), mode = d.u32(dev::kPaSuScModeCntl);
    AoScreen& sc = a.screen;
    sc.x_scale = (vte & 1) ? d.f32(dev::kPaClVport + 0) : 1.0f;
    sc.x_offset = (vte & 2) ? d.f32(dev::kPaClVport + 4) : 0.0f;
    sc.y_scale = (vte & 4) ? d.f32(dev::kPaClVport + 8) : 1.0f;
    sc.y_offset = (vte & 8) ? d.f32(dev::kPaClVport + 12) : 0.0f;
    sc.z_scale = (vte & 16) ? d.f32(dev::kPaClVport + 16) : 1.0f;
    sc.z_offset = (vte & 32) ? d.f32(dev::kPaClVport + 20) : 0.0f;
    if (mode & (1u << 16)) {
      const uint32_t window_offset = d.u32(dev::kPaScWindowOffset);
      sc.window_x = float(int32_t(window_offset << 17) >> 17);
      sc.window_y = float(int32_t((window_offset >> 16) << 17) >> 17);
    }
    sc.half_pixel = (d.u32(dev::kPaSuVtxCntl) & 1) == 0 ? 0.5f : 0.0f;
    if (size_from) {
      sc.host_scale_x = size_from->scale_x;
      sc.host_scale_y = size_from->scale_y;
    }
    // What the volumetric light builds on: the screen-space shadow's light and textures, the fog
    // pass's constants, and whether the game drew its own shafts.
    if (options_.lighting.volumetrics_on()) {
      auto c = [&](uint32_t reg, uint32_t comp) { return d.f32(dev::kPsConstants + reg * 16 + comp * 4); };
      if (a.family == LightFamily::kShadowQuad) {
        SceneLighting::ShadowQuad q;
        for (uint32_t i = 0; i < 16; ++i) q.light[i] = c(i / 4, i % 4);
        q.bias = c(ShadowQuadLayoutOf(a.state.ps_hash).shadow_params, 2);
        q.shadow_map = LightingResolvedTexture(d, 2);
        q.depth = LightingResolvedTexture(d, 0);
        scene_lighting_->NoteShadowQuad(call.frame, q);
      } else if (a.family == LightFamily::kFog) {
        SceneLighting::Fog f;
        for (uint32_t i = 0; i < 4; ++i) {
          f.color[i] = c(0, i);
          f.params[i] = c(1, i);
          f.normalizing[i] = c(2, i);
        }
        scene_lighting_->NoteFog(call.frame, f);
      } else if (a.family == LightFamily::kLightShaft) {
        const LightShaftMode m = LightShaftModeOf(a.state.ps_hash);
        if (m == LightShaftMode::kRender || m == LightShaftMode::kRenderShadowed) scene_lighting_->NoteShafts(call.frame);
      }
    }
    scene_lighting_->OnDraw(cl, a);
  }
  LightingWriteConstants(cl);
}

void GameRenderer::LightingPresentHook(nvrhi::ICommandList* cl) {
  if (scene_lighting_) scene_lighting_->OnPresent(cl);
}

}  // namespace nr
