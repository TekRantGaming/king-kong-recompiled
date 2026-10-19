// The game renderer's hooks for the ambient occlusion's GTAO method (scene_ao.h, docs/ao.md):
// what each draw, clear, resolve and Present tells it, and the ambient-only shaders' constants
// (b4). The renderer's files call these at a handful of marked points; with the default
// --native_ao_method=legacy none of this runs (scene_ao_ stays null) and the legacy AO
// (ambient_occlusion.h) is used exactly as before.

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "backend/game_renderer.h"
#include "backend/shaders/generated/ao_hands_ps_dxil.h"
#include "backend/shaders/generated/ao_hands_ps_spirv.h"
#include "backend/game_upscale.h"
#include "backend/log.h"
#include "backend/photo_config.h"
#include "kknr/render_targets.h"

namespace nr {

namespace {

// XXH3-64 of the microcode of the title's two AfterEffects fog pixel shaders (as in
// game_renderer.cpp, for the legacy AO): their constants place the fog for the resolve moment.
constexpr uint64_t kFogShaderA = UINT64_C(0x1B65AC7429B67FE9);
constexpr uint64_t kFogShaderB = UINT64_C(0x9F699E763A41FE07);

// shaders/include/kkshaders: the translator's KKAmbientOcclusion buffer (b4).
struct AmbientConstants {
  uint32_t texture[4];  // x: kk_Tex2D descriptor index (0: none), yz: the picture's origin (host pixels)
  float params[4];      // x: 1 to apply, zw: the picture's size
};
static_assert(sizeof(AmbientConstants) == 32, "KKAmbientOcclusion is two float4");

}  // namespace

void GameRenderer::AoLayoutBindings(nvrhi::BindingLayoutDesc& set0) const {
  if (options_.ao.ambient()) set0.bindings.push_back(nvrhi::BindingLayoutItem::VolatileConstantBuffer(4));
}

bool GameRenderer::AoSetBindings(nvrhi::BindingSetDesc& set0) {
  if (!options_.ao.ambient()) return true;
  nvrhi::BufferDesc d;
  d.byteSize = sizeof(AmbientConstants);
  d.isConstantBuffer = true;
  d.isVolatile = true;
  d.maxVersions = options_.volatile_versions;
  d.debugName = "Game AO constants";
  ao_constants_ = device_->createBuffer(d);
  if (!ao_constants_) return false;
  set0.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(4, ao_constants_));
  return true;
}

void GameRenderer::AoInitialize() {
  if (options_.ao.gpu_time || options_.effect_settings) {
    ao_timer_ = std::make_unique<AoGpuTimer>(device_, options_.ao.gtao() ? "gtao" : "legacy");
    ao_timer_->set_quiet(!options_.ao.gpu_time);  // else for the graphics panel only
  }
  if (!options_.ao.gtao()) return;
  scene_ao_ = std::make_unique<SceneAo>(device_, options_.ao);
  if (!scene_ao_->Initialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: AO: the GTAO method is unavailable, AO is off");
    scene_ao_.reset();
    return;
  }
  scene_ao_->SetTimer(ao_timer_.get());
  // The hands' second colour draw (task 244): its pixel shader, built for the game's layout.
  nvrhi::ShaderDesc hd;
  hd.shaderType = nvrhi::ShaderType::Pixel;
  hd.entryName = "main";
  hd.debugName = "ao_hands_ps";
  ao_hands_ps_ = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN
                     ? device_->createShader(hd, k_ao_hands_ps_spirv, sizeof(k_ao_hands_ps_spirv))
                     : device_->createShader(hd, k_ao_hands_ps_dxil, sizeof(k_ao_hands_ps_dxil));
  if (!ao_hands_ps_) Logf(LogLevel::kWarning, "rexgpu-native: AO: no shader for the first-person hands; they keep no occlusion");
  // Debugging aid (before/after comparisons): KK_DEV_AO_NO_HANDS=1 leaves the hands as before.
  if (const char* v = std::getenv("KK_DEV_AO_NO_HANDS"); v && *v == '1') ao_hands_ps_ = nullptr;
}

std::shared_ptr<const kkshaders::AmbientTable> GameRenderer::ao_ambient_table() const {
  return scene_ao_ ? scene_ao_->ambient_table() : nullptr;
}

AoSettings GameRenderer::AoSettingsFor(uint64_t frame) {
  // Read once a frame (the launcher's ao_mode and ao_strength may change between frames).
  if (frame != ao_settings_frame_) {
    ao_settings_ = options_.ao_settings ? options_.ao_settings() : AoSettings{};
    // Resolve mode shades a scratch copy, while the SDK reads the original scene target.
    ao_settings_.temporal_upscaler = upscale_ && upscale_->upscaler != nullptr && options_.ao.apply != AoApply::kResolve;
    ao_settings_frame_ = frame;
  }
  return ao_settings_;
}

// A draw as the classifier and the camera finder see it (the GI's hooks use it too, game_gi.cpp).
SceneAo::Draw GameRenderer::AoDescribeDraw(const DrawCall& call, const dev::DeviceView& d,
                                           const std::array<HostTarget*, 4>& colors, HostTarget* depth,
                                           const PipelineRecord& record, const GameShader& vs, const GameShader* ps) const {
  SceneAo::Draw a;
  a.frame = call.frame;
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
  // The viewport transform as ComputeViewport reads it.
  const uint32_t vte = d.u32(dev::kPaClVteCntl), mode = d.u32(dev::kPaSuScModeCntl);
  AoScreen& sc = a.screen;
  sc.x_scale = (vte & 1) ? d.f32(dev::kPaClVport + 0) : 1.0f;
  sc.x_offset = (vte & 2) ? d.f32(dev::kPaClVport + 4) : 0.0f;
  sc.y_scale = (vte & 4) ? d.f32(dev::kPaClVport + 8) : 1.0f;
  sc.y_offset = (vte & 8) ? d.f32(dev::kPaClVport + 12) : 0.0f;
  sc.z_scale = (vte & 16) ? d.f32(dev::kPaClVport + 16) : 1.0f;
  sc.z_offset = (vte & 32) ? d.f32(dev::kPaClVport + 20) : 0.0f;
  if (mode & (1u << 16)) {  // vtx_window_offset_enable
    const uint32_t window_offset = d.u32(dev::kPaScWindowOffset);
    sc.window_x = float(int32_t(window_offset << 17) >> 17);
    sc.window_y = float(int32_t((window_offset >> 16) << 17) >> 17);
  }
  sc.half_pixel = (d.u32(dev::kPaSuVtxCntl) & 1) == 0 ? 0.5f : 0.0f;
  if (size_from) {
    sc.host_scale_x = size_from->scale_x;
    sc.host_scale_y = size_from->scale_y;
  }
  return a;
}

void GameRenderer::AoDrawHook(nvrhi::ICommandList* cl, const DrawCall& call, const dev::DeviceView& d,
                              const std::array<HostTarget*, 4>& colors, HostTarget* depth, const PipelineRecord& record,
                              const GameShader& vs, const GameShader* ps) {
  if (!scene_ao_) {
    AoWriteAmbientConstants(cl, false);  // b4 is bound all the same (the GTAO method did not start)
    return;
  }
  const AoSettings s = AoSettingsFor(call.frame);
  if (ps && (ps->ucode_hash == kFogShaderA || ps->ucode_hash == kFogShaderB) && s.mode > 0) {
    float fog[16];
    for (uint32_t i = 0; i < 16; ++i) fog[i] = LoadBEFloat(d.at(dev::kPsConstants + i * 4));
    scene_ao_->NoteFog(fog, call.frame);
  }
  SceneAo::Draw a = AoDescribeDraw(call, d, colors, depth, record, vs, ps);
  const AoDrawState& st = a.state;
  const bool reads = scene_ao_->OnDraw(cl, a, s);
  if (ao_constants_) AoWriteAmbientConstants(cl, reads);
  const bool dump = Dump(call.frame) || (options_.ao.dump_every && call.frame % options_.ao.dump_every == 0);
  if (dump && s.mode > 0) {
    // --native_dump_frame: the classifier's view of the draw (docs/ao.md, the Windows checklist).
    const SceneAo::LastDraw& l = scene_ao_->last_draw();
    Logf(LogLevel::kInfo,
         "rexgpu-native:   AO draw %u of frame %llu: %s%s%s%s%s%s%s (colour %d depth test %d write %d blend %08X alpha "
         "test %d, prim %u, vs %016llX ps %016llX, rt %llX ds %llX)",
         call.index_in_frame, static_cast<unsigned long long>(call.frame), AoDrawClassName(l.cls), l.events.in_scene ? ", scene" : "",
         l.events.depth_complete ? ", depth pre-pass ended before it" : "",
         l.events.opaque_complete ? ", opaque pass ended before it" : "", l.computed ? ", occlusion worked out" : "",
         l.applied ? ", occlusion applied" : "", reads ? ", reads the occlusion" : "", int(st.color), int(st.depth_test),
         int(st.depth_write), st.blend_control, int(st.alpha_test), st.primitive, static_cast<unsigned long long>(st.vs_hash),
         static_cast<unsigned long long>(st.ps_hash), static_cast<unsigned long long>(a.color.key),
         static_cast<unsigned long long>(a.depth.key));
    // Task 240 diagnostics: depth function, index count, the photo mode's arms mesh, the draw's projection.
    float rows[16];
    AoProjection p;
    const bool persp = FindAoPerspectiveRegister(a.vs_constants, rows) >= 0 && ExtractAoProjection(rows, p);
    const bool arms = call.indexed && PhotoMeshCountCandidate(call.count) && PhotoHidesMesh(GeometryContentHash(call), call.count);
    Logf(LogLevel::kInfo, "rexgpu-native:   AO draw %u extra: zfunc %u count %u arms %d near %.4f px %.3f",
         call.index_in_frame, (d.u32(dev::kRbDepthControl) >> 4) & 7, call.count, int(arms), persp ? p.near_plane() : -1.0f,
         persp ? p.px : -1.0f);
  }
}

void GameRenderer::AoRecordMotionHook(const DrawCall& call, const dev::DeviceView& d, const GameShader& vs) {
  if (!scene_ao_) return;
  const AoSettings s = AoSettingsFor(call.frame);
  const auto& last = scene_ao_->last_draw();
  if (s.mode <= 0 || s.temporal_upscaler || !last.events.in_scene ||
      (last.cls != AoDrawClass::kDepthOnly && last.cls != AoDrawClass::kOpaque)) return;
  MotionDrawKey key;
  key.vs_hash = vs.ucode_hash;
  bool rewritten = false;
  for (uint32_t stream = 0; stream < 2; ++stream) {
    const uint32_t object = d.u32(dev::kStreamBuffer + 8 * stream);
    VertexBufferInfo info;
    if (!object || !DecodeVertexBuffer(memory_.Virtual(object), info)) continue;
    if (const auto* entry = vertex_buffers_.Find(info.physical, info.size_bytes, 0)) {
      if (const auto* buffer = static_cast<const HostBuffer*>(entry->host); buffer && buffer->dynamic) rewritten = true;
    }
    if (stream == 0) {
      key.vertex_buffer = info.physical;
      key.vertex_offset = d.u32(dev::kStreamOffset);
    } else key.vertex_buffer2 = info.physical;
  }
  key.index_buffer = call.indexed ? call.index_info.physical : 0;
  key.start = call.start;
  key.count = call.count;
  key.base_vertex = call.base_vertex;
  key.primitive = uint32_t(call.primitive);
  if (rewritten) {
    // Identical to the SDK camera vote: streamed geometry moves through a ring of
    // buffers. Match its shader/count/order/closest constants, not its new address.
    key.vertex_buffer = key.vertex_offset = key.vertex_buffer2 = key.index_buffer = 0;
    key.start = 0;
    key.base_vertex = 0;
  }
  scene_ao_->RecordMotion(call.frame, key, d.at(dev::kVsConstants), s);
}
void GameRenderer::AoWriteAmbientConstants(nvrhi::ICommandList* cl, bool reads) {
  const SceneAo::AmbientView v = scene_ao_ ? scene_ao_->ambient_view() : SceneAo::AmbientView{};
  AmbientConstants k = {};
  if (reads && v.texture) {
    k.texture[0] = AoOcclusionSlot(v.texture);
    k.texture[1] = uint32_t(v.x);
    k.texture[2] = uint32_t(v.y);
    k.params[0] = 1.0f;
    k.params[2] = float(v.width);
    k.params[3] = float(v.height);
  }
  // A volatile buffer is written in every command list that binds it, and when it changes.
  const bool new_list = !ao_constants_valid_ || ao_constants_list_ != cl || ao_constants_frame_ != frame_;
  if (!new_list && std::memcmp(&k, ao_constants_last_.data(), sizeof(k)) == 0) return;
  std::memcpy(ao_constants_last_.data(), &k, sizeof(k));
  cl->writeBuffer(ao_constants_, &k, sizeof(k));
  ao_constants_valid_ = true;
  ao_constants_list_ = cl;
  ao_constants_frame_ = frame_;
}

nvrhi::ITexture* GameRenderer::AoResolveHook(nvrhi::ICommandList* cl, const AoResolve& r, const AoSettings& s,
                                             const ResolveCall& call, const HostTarget* source) {
  if (!scene_ao_) {  // the legacy method
    if (!ao_) return nullptr;
    if (ao_timer_) ao_timer_->Begin(cl);
    nvrhi::ITexture* scene = ao_->OnResolve(cl, r, s);
    if (ao_timer_) ao_timer_->End(cl);
    return scene;
  }
  SceneAo::Resolve sr;
  sr.facts = r;
  sr.source = {source->key, source->texture, source->scale_x, source->scale_y};
  sr.origin = !call.has_rect || (call.rect[0] == 0 && call.rect[1] == 0);
  const kknr::ResolveFlags flags{call.flags};
  if (flags.ClearDepthStencil() && call.depth_stencil) {
    dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
    if (d.valid()) {
      const uint32_t info = d.u32(dev::kRbDepthInfo), pitch = d.u32(dev::kRbSurfaceInfo) & 0x3FFF;
      if (const HostTarget* t = FindTarget(true, info & 0xFFF, pitch, (info >> 16) & 1)) {
        sr.clears_depth = true;
        sr.depth_key = t->key;
      }
    }
  }
  return scene_ao_->OnResolve(cl, sr, s);
}

void GameRenderer::AoClearHook(const ClearCall& call, const HostTarget* depth) {
  if (call.depth && depth && AoSettingsFor(call.frame).mode > 0) scene_ao_->OnDepthClear(call.frame, depth->key);
}

void GameRenderer::AoPresentHook(nvrhi::ICommandList* cl) {
  if (scene_ao_) scene_ao_->OnPresent(cl, AoSettingsFor(ao_settings_frame_));
  if (ao_timer_) ao_timer_->FrameEnd();
}


// ---------------------------------------------------------------- AO on the first-person hands (task 244)

uint32_t GameRenderer::AoOcclusionSlot(nvrhi::ITexture* texture) {
  if (!texture) return 0;
  // The occlusion in the bindless 2D table (a new slot when Gtao made a new image).
  if (texture != ao_slot_texture_ || !ao_slot_) {
    if (ao_slot_) RetireSlot(kTex2D, ao_slot_);
    ao_slot_ = AllocateSlot(kTex2D);
    if (ao_slot_) WriteSlot(kTex2D, nvrhi::BindingSetItem::Texture_SRV(ao_slot_, texture));
    ao_slot_texture_ = texture;
  }
  if (ao_slot_) TouchSlot(kTex2D, ao_slot_);
  return ao_slot_;
}

void GameRenderer::AoNoteHandsDepth(const DrawCall& call, const PipelineRecord& record, bool writes_color) {
  if (!scene_ao_ || !ao_hands_ps_ || !call.indexed) return;
  const AoSettings s = AoSettingsFor(call.frame);
  if (s.mode <= 0) return;
  if (ao_hands_frame_ != call.frame) {
    ao_hands_.clear();
    ao_hands_frame_ = call.frame;
  }
  // The pre-pass's depth-only geometry with depth func ALWAYS: the game's first-person arms (and what
  // Jack holds), drawn in front of everything whatever the world's depth (REPORT-240).
  const uint32_t dc = record.depth_control;
  if (writes_color || !record.depth_format || (dc & 6) != 6 || ((dc >> 4) & 7) != 7) return;
  const AoHandsKey key{call.index_info.physical, call.start, call.count, call.base_vertex};
  if (ao_hands_.size() >= 16 || std::find(ao_hands_.begin(), ao_hands_.end(), key) != ao_hands_.end()) return;
  ao_hands_.push_back(key);
  if (options_.ao.dump_every && call.frame % options_.ao.dump_every == 0)
    Logf(LogLevel::kInfo, "rexgpu-native:   AO hands: draw %u noted (count %u ib %08X start %u)", call.index_in_frame, call.count,
         call.index_info.physical, call.start);
  ++ao_hands_stats_.noted;
}

void GameRenderer::AoRedrawHands(nvrhi::ICommandList* cl, const DrawCall& call, const PipelineRecord& record,
                                 const GameShader& vs, const nvrhi::GraphicsState& gs, const nvrhi::DrawArguments& args,
                                 bool indexed, const HostTarget& target) {
  if (ao_hands_.empty() || ao_hands_frame_ != call.frame || !call.indexed || !scene_ao_) return;
  // Its opaque colour draw: render target 0 written without blending, depth tested and written.
  const uint32_t dc = record.depth_control, bc = record.color_control[0];
  const bool opaque = (bc & 0x1F) == 1 && ((bc >> 5) & 7) == 0 && ((bc >> 8) & 0x1F) == 0;
  if (!record.color_count || record.color_index[0] != 0 || !(record.color_mask[0] & 7) || !opaque ||
      !record.depth_format || (dc & 6) != 6 || ((dc >> 4) & 7) == 7 || (record.flags & kRecordTessellation))
    return;
  const AoHandsKey key{call.index_info.physical, call.start, call.count, call.base_vertex};
  if (std::find(ao_hands_.begin(), ao_hands_.end(), key) == ao_hands_.end()) return;
  const bool dump = options_.ao.dump_every && call.frame % options_.ao.dump_every == 0;
  auto skip = [&](const char* why) {
    if (dump) Logf(LogLevel::kInfo, "rexgpu-native:   AO hands: draw %u not drawn again: %s", call.index_in_frame, why);
  };
  // Only after the occlusion went into the scene (a colour draw before that moment gets it already).
  const AoSettings s = AoSettingsFor(call.frame);
  SceneAo::AppliedView v;
  if (s.mode <= 0 || s.mode > 2 || !scene_ao_->applied_view(call.frame, v) || !v.image.texture) return skip("no occlusion yet");
  const uint32_t slot = AoOcclusionSlot(v.image.texture);
  if (!slot || !v.depth) return skip(!slot ? "no slot" : "no depth image");
  // The depth the occlusion was worked out from: the pre-pass draws the hands' parts with ALWAYS, so where a
  // farther part was drawn after a nearer one that depth is the farther one's, and its occlusion belongs to a
  // hidden surface (sharp dark patches). The shader leaves those pixels alone.
  if (v.depth != ao_hands_depth_texture_ || !ao_hands_depth_slot_) {
    if (ao_hands_depth_slot_) RetireSlot(kTex2D, ao_hands_depth_slot_);
    ao_hands_depth_slot_ = AllocateSlot(kTex2D);
    if (ao_hands_depth_slot_) WriteSlot(kTex2D, nvrhi::BindingSetItem::Texture_SRV(ao_hands_depth_slot_, v.depth));
    ao_hands_depth_texture_ = v.depth;
  }
  if (!ao_hands_depth_slot_) return skip("no depth slot");
  TouchSlot(kTex2D, ao_hands_depth_slot_);
  // The pipeline: the hands' vertex shader and state, ao_hands.hlsl, depth EQUAL without writes, the colour
  // times the occlusion (mode 1) or the occlusion itself (mode 2); alpha and the other targets untouched.
  PipelineRecord r = record;
  r.ps_hash = UINT64_C(0x414F48414E445300) | uint64_t(s.mode);  // "AOHANDS" + mode: its own key
  r.ps_input = r.ps_hash;
  const uint64_t key_hash = PipelineKey(r);
  nvrhi::IGraphicsPipeline* pipeline = nullptr;
  if (auto known = pipelines_.find(key_hash); known != pipelines_.end()) {
    pipeline = known->second;
  } else {
    nvrhi::GraphicsPipelineDesc pd;
    nvrhi::FramebufferInfo fbi;
    BuildPipelineDesc(record, vs.handle, ao_hands_ps_, pd, fbi);
    for (uint32_t i = 0; i < 8; ++i) pd.renderState.blendState.targets[i].colorWriteMask = nvrhi::ColorMask(0);
    nvrhi::BlendState::RenderTarget& t = pd.renderState.blendState.targets[0];
    t.colorWriteMask = nvrhi::ColorMask(uint8_t(record.color_mask[0] & 7));
    t.blendEnable = s.mode == 1;
    t.srcBlend = nvrhi::BlendFactor::DstColor;
    t.destBlend = nvrhi::BlendFactor::Zero;
    t.blendOp = nvrhi::BlendOp::Add;
    t.srcBlendAlpha = nvrhi::BlendFactor::Zero;
    t.destBlendAlpha = nvrhi::BlendFactor::One;
    t.blendOpAlpha = nvrhi::BlendOp::Add;
    nvrhi::DepthStencilState& ds = pd.renderState.depthStencilState;
    ds.depthTestEnable = true;
    ds.depthWriteEnable = false;
    ds.depthFunc = nvrhi::ComparisonFunc::Equal;
    ds.stencilEnable = false;
    pipeline = GetPipeline(pd, fbi, key_hash, PipelineDemand::kSoon);
  }
  if (!pipeline) {
    ++ao_hands_stats_.waiting;
    return skip("pipeline not ready");
  }
  if (!NoteVolatileDraw(cl, false)) return skip("volatile budget");
  // The pixel constants for this draw alone (ao_hands.hlsl); the next game draw writes its own again.
  std::array<float, 1024> k = last_ps_;
  std::memcpy(&k[0], &slot, sizeof(slot));
  k[1] = target.scale_x > 0.0f ? v.host_scale_x / target.scale_x : 1.0f;
  k[2] = target.scale_y > 0.0f ? v.host_scale_y / target.scale_y : 1.0f;
  std::memcpy(&k[3], &ao_hands_depth_slot_, sizeof(uint32_t));
  k[4] = float(v.image.x);
  k[5] = float(v.image.y);
  k[6] = float(v.image.width);
  k[7] = float(v.image.height);
  k[8] = float(v.step);
  k[9] = 0.002f;  // the largest depth difference of the same surface (stored depth units; upscaled pixel centres)
  // With a temporal upscaler the occlusion is left noisy for it to settle, but the hands drawn after its point
  // never go through it: a wider average then. The tent-weighted average fades the pixels of hidden surfaces
  // (counted as 1) in over its radius: 7 x 7 with a temporal upscaler, 5 x 5 otherwise (task 306).
  k[10] = s.temporal_upscaler ? 3.0f : 2.0f;
  // Developer view (ao_mode 2 only, ao_hands.hlsl c2.w): KK_DEV_AO_HANDS_VIEW=1 the pixel's own occlusion
  // unchecked, 2 the same with the depth mismatches in red.
  static const float dev_view = [] {
    const char* e = std::getenv("KK_DEV_AO_HANDS_VIEW");
    return e ? float(std::atoi(e)) : 0.0f;
  }();
  k[11] = s.mode == 2 ? dev_view : 0.0f;
  // Both images are read through the bindless table, which tracks no states: shader resources before the draw.
  cl->setTextureState(v.image.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  cl->setTextureState(v.depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  cl->commitBarriers();
  cl->writeBuffer(ps_constants_, k.data(), sizeof(k));
  constants_valid_ = false;
  nvrhi::GraphicsState redraw = gs;
  redraw.pipeline = pipeline;
  cl->setGraphicsState(redraw);
  if (indexed) {
    cl->drawIndexed(args);
  } else {
    cl->draw(args);
  }
  ++ao_hands_stats_.drawn;
  if (dump)
    Logf(LogLevel::kInfo, "rexgpu-native:   AO hands: draw %u drawn again (count %u)", call.index_in_frame, call.count);
  if (ao_hands_stats_.drawn == 1)
    Logf(LogLevel::kInfo, "rexgpu-native: AO: first-person hands shaded (frame %llu, %u indices, occlusion scale %.3f x %.3f)",
         static_cast<unsigned long long>(call.frame), call.count, k[1], k[2]);
}

}  // namespace nr
