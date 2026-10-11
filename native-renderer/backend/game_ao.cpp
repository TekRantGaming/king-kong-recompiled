// The game renderer's hooks for the ambient occlusion's GTAO method (scene_ao.h, docs/ao.md):
// what each draw, clear, resolve and Present tells it, and the ambient-only shaders' constants
// (b4). The renderer's files call these at a handful of marked points; with the default
// --native_ao_method=legacy none of this runs (scene_ao_ stays null) and the legacy AO
// (ambient_occlusion.h) is used exactly as before.

#include <cstring>

#include "backend/game_renderer.h"
#include "backend/log.h"
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
  d.maxVersions = 8192;
  d.debugName = "Game AO constants";
  ao_constants_ = device_->createBuffer(d);
  if (!ao_constants_) return false;
  set0.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(4, ao_constants_));
  return true;
}

void GameRenderer::AoInitialize() {
  if (!options_.ao.gtao()) return;
  scene_ao_ = std::make_unique<SceneAo>(device_, options_.ao);
  if (!scene_ao_->Initialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: AO: the GTAO method is unavailable, AO is off");
    scene_ao_.reset();
  }
}

std::shared_ptr<const kkshaders::AmbientTable> GameRenderer::ao_ambient_table() const {
  return scene_ao_ ? scene_ao_->ambient_table() : nullptr;
}

AoSettings GameRenderer::AoSettingsFor(uint64_t frame) {
  // Read once a frame (the launcher's ao_mode and ao_strength may change between frames).
  if (frame != ao_settings_frame_) {
    ao_settings_ = options_.ao_settings ? options_.ao_settings() : AoSettings{};
    ao_settings_frame_ = frame;
  }
  return ao_settings_;
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
  const bool reads = scene_ao_->OnDraw(cl, a, s);
  if (ao_constants_) AoWriteAmbientConstants(cl, reads);
}

void GameRenderer::AoWriteAmbientConstants(nvrhi::ICommandList* cl, bool reads) {
  const SceneAo::AmbientView v = scene_ao_ ? scene_ao_->ambient_view() : SceneAo::AmbientView{};
  AmbientConstants k = {};
  if (reads && v.texture) {
    // The occlusion in the bindless 2D table (a new slot when Gtao made a new image).
    if (v.texture != ao_slot_texture_) {
      if (ao_slot_) RetireSlot(kTex2D, ao_slot_);
      ao_slot_ = AllocateSlot(kTex2D);
      WriteSlot(kTex2D, nvrhi::BindingSetItem::Texture_SRV(ao_slot_, v.texture));
      ao_slot_texture_ = v.texture;
    }
    k.texture[0] = ao_slot_;
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
  if (!scene_ao_) return ao_ ? ao_->OnResolve(cl, r, s) : nullptr;  // the legacy method
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

void GameRenderer::AoPresentHook(nvrhi::ICommandList* cl) { scene_ao_->OnPresent(cl, AoSettingsFor(ao_settings_frame_)); }

}  // namespace nr
