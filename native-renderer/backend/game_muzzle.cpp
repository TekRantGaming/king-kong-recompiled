// The game renderer's hooks for the muzzle light (muzzle_light.h, docs/lighting.md). The renderer's files call these at
// a handful of points marked "Muzzle hook (game_muzzle.cpp)". With native_muzzle_light off nothing of it is made
// (scene_muzzle_ stays null); with it on, a frame with no flash costs one look at the list of flashes.

#include "backend/game_renderer.h"
#include "backend/log.h"
#include "backend/shaders/generated/muzzle_hands_ps_dxil.h"
#include "backend/shaders/generated/muzzle_hands_ps_spirv.h"
#include "kknr/render_targets.h"

namespace nr {

void GameRenderer::MuzzleInitialize() {
  muzzle_user_gpu_time_ = options_.muzzle.gpu_time;
  if (options_.muzzle.enabled) UpdateMuzzle(options_.muzzle);
}

void GameRenderer::UpdateMuzzle(const MuzzleConfig& want) {
  options_.muzzle = want;
  options_.muzzle.gpu_time = muzzle_user_gpu_time_;
  if (!want.enabled || scene_muzzle_ || muzzle_failed_) return;
  nvrhi::ShaderDesc hd;
  hd.shaderType = nvrhi::ShaderType::Pixel;
  hd.entryName = "main";
  hd.debugName = "muzzle_hands_ps";
  muzzle_hands_ps_ = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN
                         ? device_->createShader(hd, k_muzzle_hands_ps_spirv, sizeof(k_muzzle_hands_ps_spirv))
                         : device_->createShader(hd, k_muzzle_hands_ps_dxil, sizeof(k_muzzle_hands_ps_dxil));
  muzzle_timer_ = std::make_unique<AoGpuTimer>(device_, "muzzle");
  muzzle_timer_->set_quiet(!muzzle_user_gpu_time_);
  scene_muzzle_ = std::make_unique<SceneMuzzle>(device_, options_.ao.rules);
  if (!scene_muzzle_->Initialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: muzzle light: unavailable (its shaders could not be made), it stays off");
    scene_muzzle_.reset();
    muzzle_timer_.reset();
    muzzle_failed_ = true;
    return;
  }
  scene_muzzle_->SetTimer(muzzle_timer_.get());
}

void GameRenderer::MuzzleDrawHook(nvrhi::ICommandList* cl, const DrawCall& call, const dev::DeviceView& d,
                                  const std::array<HostTarget*, 4>& colors, HostTarget* depth, const PipelineRecord& record,
                                  const GameShader& vs, const GameShader* ps) {
  if (!options_.muzzle.enabled || !scene_muzzle_->Wanted(call.frame)) return;
  if (muzzle_hands_frame_ != call.frame) {
    muzzle_hands_.clear();
    muzzle_hands_frame_ = call.frame;
  }
  bool writes_color = false;
  for (uint32_t i = 0; i < record.color_count; ++i) writes_color = writes_color || record.color_mask[i];
  if (call.indexed && AoHandsDepthDraw(record, writes_color) && muzzle_hands_.size() < 16) {
    const AoHandsKey key{call.index_info.physical, call.start, call.count, call.base_vertex};
    if (std::find(muzzle_hands_.begin(), muzzle_hands_.end(), key) == muzzle_hands_.end()) muzzle_hands_.push_back(key);
  }
  const SceneAo::Draw a = AoDescribeDraw(call, d, colors, depth, record, vs, ps);
  const Gtao* ao = scene_ao_ && AoSettingsFor(call.frame).mode == 1 ? scene_ao_->prepared(call.frame) : nullptr;
  scene_muzzle_->OnDraw(cl, a, ao);
}

void GameRenderer::MuzzleRedrawHands(nvrhi::ICommandList* cl, const DrawCall& call, const PipelineRecord& record,
                                     const GameShader& vs, const nvrhi::GraphicsState& gs, const nvrhi::DrawArguments& args,
                                     bool indexed, const HostTarget& target) {
  if (muzzle_hands_frame_ != call.frame || !call.indexed || !muzzle_hands_ps_ || !AoHandsColourDraw(record)) return;
  const AoHandsKey key{call.index_info.physical, call.start, call.count, call.base_vertex};
  if (std::find(muzzle_hands_.begin(), muzzle_hands_.end(), key) == muzzle_hands_.end()) return;
  const SceneMuzzle::HandsLight* light = scene_muzzle_->hands(call.frame);
  if (!light) return;
  // As AoRedrawHands: the hands' vertex shader and state, depth EQUAL without writes; destination times one plus source.
  PipelineRecord r = record;
  r.ps_hash = UINT64_C(0x4D555A48414E4400);  // "MUZHAND": its own key
  r.ps_input = r.ps_hash;
  const uint64_t key_hash = PipelineKey(r);
  nvrhi::IGraphicsPipeline* pipeline = nullptr;
  if (auto known = pipelines_.find(key_hash); known != pipelines_.end()) {
    pipeline = known->second;
  } else {
    nvrhi::GraphicsPipelineDesc pd;
    nvrhi::FramebufferInfo fbi;
    BuildPipelineDesc(record, vs.handle, muzzle_hands_ps_, pd, fbi);
    for (uint32_t i = 0; i < 8; ++i) pd.renderState.blendState.targets[i].colorWriteMask = nvrhi::ColorMask(0);
    nvrhi::BlendState::RenderTarget& t = pd.renderState.blendState.targets[0];
    t.colorWriteMask = nvrhi::ColorMask(uint8_t(record.color_mask[0] & 7));
    t.blendEnable = true;
    t.srcBlend = nvrhi::BlendFactor::DstColor;
    t.destBlend = nvrhi::BlendFactor::One;
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
  if (!pipeline || !NoteVolatileDraw(cl, false)) return;
  // The pixel constants for this draw alone (muzzle_hands.hlsl); the next game draw writes its own again.
  std::array<float, 1024> k = last_ps_;
  k[0] = float(light->count);
  k[1] = target.scale_x > 0.0f ? light->host_scale[0] / target.scale_x : 1.0f;
  k[2] = target.scale_y > 0.0f ? light->host_scale[1] / target.scale_y : 1.0f;
  k[3] = MuzzleTuning::kHandsWrap;
  std::memcpy(&k[4], light->origin, sizeof(light->origin));
  std::memcpy(&k[6], light->size, sizeof(light->size));
  std::memcpy(&k[8], light->depth_unpack, sizeof(light->depth_unpack));
  std::memcpy(&k[10], light->ndc_mul, sizeof(light->ndc_mul));
  std::memcpy(&k[12], light->ndc_add, sizeof(light->ndc_add));
  k[14] = MuzzleTuning::kHandsFalloff;
  k[15] = MuzzleTuning::kHandsGain;
  std::memcpy(&k[16], MuzzleTuning::kColour, sizeof(MuzzleTuning::kColour));
  std::memcpy(&k[20], light->light, sizeof(light->light));
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
}

void GameRenderer::MuzzleClearHook(const ClearCall& call, const HostTarget* depth) {
  if (call.depth && depth && options_.muzzle.enabled && scene_muzzle_->Wanted(call.frame))
    scene_muzzle_->OnDepthClear(call.frame, depth->key);
}

void GameRenderer::MuzzleResolveHook(const ResolveCall& call) {
  const kknr::ResolveFlags flags{call.flags};
  if (!flags.ClearDepthStencil() || !call.depth_stencil || !options_.muzzle.enabled || !scene_muzzle_->Wanted(call.frame)) return;
  dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
  if (!d.valid()) return;
  const uint32_t info = d.u32(dev::kRbDepthInfo), pitch = d.u32(dev::kRbSurfaceInfo) & 0x3FFF;
  if (const HostTarget* t = FindTarget(true, info & 0xFFF, pitch, (info >> 16) & 1)) scene_muzzle_->OnDepthClear(call.frame, t->key);
}

void GameRenderer::MuzzlePresentHook() {
  if (muzzle_timer_) muzzle_timer_->FrameEnd();
}

}  // namespace nr
