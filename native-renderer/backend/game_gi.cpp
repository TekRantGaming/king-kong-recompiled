// The game renderer's hooks for the screen-space global illumination (scene_gi.h, docs/gi.md):
// what each draw, clear, resolve and Present tells it, and the frame's flat ambient light read
// from the lighting shaders' constants. The renderer's files call these at a handful of points
// marked "GI hook (game_gi.cpp)"; with the default --native_gi=off none of this runs (scene_gi_
// stays null) and every frame is what it was.

#include "backend/game_renderer.h"
#include "backend/log.h"
#include "kknr/render_targets.h"

namespace nr {

void GameRenderer::GiInitialize() {
  if (!options_.gi.enabled()) return;
  if (options_.gi.gpu_time) gi_timer_ = std::make_unique<AoGpuTimer>(device_, "gi");
  scene_gi_ = std::make_unique<SceneGi>(device_, options_.gi, options_.ao.rules);
  if (!scene_gi_->Initialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: GI: unavailable (its shaders could not be made), it stays off");
    scene_gi_.reset();
    gi_timer_.reset();
    return;
  }
  scene_gi_->SetTimer(gi_timer_.get());
}

bool GameRenderer::GiAoOn(uint64_t frame) {
  // Either method: the legacy AO darkens the scene copy later in the frame, GTAO at the same moment.
  return options_.ao_settings && AoSettingsFor(frame).mode > 0;
}

void GameRenderer::GiDrawHook(nvrhi::ICommandList* cl, const DrawCall& call, const dev::DeviceView& d,
                              const std::array<HostTarget*, 4>& colors, HostTarget* depth, const PipelineRecord& record,
                              const GameShader& vs, const GameShader* ps) {
  // The lighting shaders' flat ambient light (g_vAmbientColor), for the composite's albedo estimate.
  if (ps && ps->ambient_constant >= 0 && colors[0] && colors[0]->width == options_.guest_frame_width) {
    const uint32_t at = dev::kPsConstants + uint32_t(ps->ambient_constant) * 16;
    const float rgb[3] = {LoadBEFloat(d.at(at)), LoadBEFloat(d.at(at + 4)), LoadBEFloat(d.at(at + 8))};
    scene_gi_->NoteAmbient(call.frame, rgb);
  }
  const SceneAo::Draw a = AoDescribeDraw(call, d, colors, depth, record, vs, ps);
  // The draw's identity across frames (temporal accumulation: the camera's vote), as the upscalers'
  // motion matching keys it: its vertex shader, its streams, its index buffer and range.
  uint64_t key = 0;
  if (options_.gi.temporal) {
    const uint64_t parts[] = {vs.ucode_hash,
                              d.u32(dev::kStreamBuffer),
                              d.u32(dev::kStreamBuffer + 8),
                              d.u32(dev::kStreamOffset),
                              call.indexed ? call.index_buffer : 0u,
                              call.start,
                              call.count,
                              uint64_t(uint32_t(call.base_vertex)),
                              uint64_t(call.primitive)};
    key = 1469598103934665603ull;
    for (uint64_t v : parts) key = (key ^ v) * 1099511628211ull + (key >> 29);
    if (!key) key = 1;
  }
  scene_gi_->OnDraw(cl, a, GiAoOn(call.frame), key);
  if (scene_gi_->last_applied() && (Dump(call.frame) || (options_.ao.dump_every && call.frame % options_.ao.dump_every == 0))) {
    const float* ambient = scene_gi_->ambient();
    Logf(LogLevel::kInfo, "rexgpu-native:   GI: bounce light added before draw %u of frame %llu (ambient %.3f %.3f %.3f)",
         call.index_in_frame, static_cast<unsigned long long>(call.frame), ambient[0], ambient[1], ambient[2]);
  }
}

void GameRenderer::GiClearHook(const ClearCall& call, const HostTarget* depth) {
  if (call.depth && depth) scene_gi_->OnDepthClear(call.frame, depth->key);
}

void GameRenderer::GiResolveHook(const ResolveCall& call, const HostTarget* source, bool from_depth) {
  if (!from_depth && source) scene_gi_->OnColorCopy(call.frame, source->key);
  const kknr::ResolveFlags flags{call.flags};
  if (flags.ClearDepthStencil() && call.depth_stencil) {
    dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
    if (d.valid()) {
      const uint32_t info = d.u32(dev::kRbDepthInfo), pitch = d.u32(dev::kRbSurfaceInfo) & 0x3FFF;
      if (const HostTarget* t = FindTarget(true, info & 0xFFF, pitch, (info >> 16) & 1))
        scene_gi_->OnDepthClear(call.frame, t->key);
    }
  }
}

void GameRenderer::GiPresentHook(nvrhi::ICommandList* cl) {
  scene_gi_->OnPresent(cl, GiAoOn(ao_settings_frame_));
  if (gi_timer_) gi_timer_->FrameEnd();
}

}  // namespace nr
