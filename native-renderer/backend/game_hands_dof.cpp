// The game renderer's hooks for the depth of field on the first-person hands (hands_dof.h, docs/lighting.md). The renderer's
// files call these at a handful of points marked "Hands depth of field hook (game_hands_dof.cpp)". With native_hands_dof off
// nothing of it is made (hands_dof_ stays null).

#include "backend/game_renderer.h"
#include "backend/log.h"
#include "backend/shaders/generated/hands_dof_mask_ps_dxil.h"
#include "backend/shaders/generated/hands_dof_mask_ps_spirv.h"

namespace nr {

void GameRenderer::HandsDofInitialize() {
  hands_dof_user_gpu_time_ = options_.hands_dof.gpu_time;
  if (options_.hands_dof.enabled) UpdateHandsDof(options_.hands_dof);
}

void GameRenderer::UpdateHandsDof(const HandsDofConfig& want) {
  options_.hands_dof = want;
  options_.hands_dof.gpu_time = hands_dof_user_gpu_time_;
  if (!want.enabled || hands_dof_ || hands_dof_failed_) return;
  nvrhi::ShaderDesc hd;
  hd.shaderType = nvrhi::ShaderType::Pixel;
  hd.entryName = "main";
  hd.debugName = "hands_dof_mask_ps";
  hands_dof_mask_ps_ = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN
                           ? device_->createShader(hd, k_hands_dof_mask_ps_spirv, sizeof(k_hands_dof_mask_ps_spirv))
                           : device_->createShader(hd, k_hands_dof_mask_ps_dxil, sizeof(k_hands_dof_mask_ps_dxil));
  hands_dof_timer_ = std::make_unique<AoGpuTimer>(device_, "hands depth of field");
  hands_dof_timer_->set_quiet(!hands_dof_user_gpu_time_);
  hands_dof_ = std::make_unique<HandsDof>(device_);
  if (!hands_dof_mask_ps_ || !hands_dof_->Initialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: hands depth of field: unavailable (its shaders could not be made), it stays off");
    hands_dof_.reset();
    hands_dof_timer_.reset();
    hands_dof_failed_ = true;
    return;
  }
  hands_dof_->SetTimer(hands_dof_timer_.get());
}

void GameRenderer::HandsDofFinish(nvrhi::ICommandList* cl) {
  if (!hands_dof_color_) return;
  hands_dof_->Apply(cl, hands_dof_color_);
  hands_dof_color_ = nullptr;
}

void GameRenderer::HandsDofDrawHook(nvrhi::ICommandList* cl, const DrawCall& call, const PipelineRecord& record) {
  if (hands_dof_frame_ != call.frame) {
    hands_dof_keys_.clear();
    hands_dof_frame_ = call.frame;
    hands_dof_masked_ = false;
    hands_dof_color_ = nullptr;
  }
  if (!options_.hands_dof.enabled || photo_.active) return;
  bool writes_color = false;
  for (uint32_t i = 0; i < record.color_count; ++i) writes_color = writes_color || record.color_mask[i];
  const AoHandsKey key{call.index_info.physical, call.start, call.count, call.base_vertex};
  if (call.indexed && AoHandsDepthDraw(record, writes_color) && hands_dof_keys_.size() < kHandsKeys &&
      std::find(hands_dof_keys_.begin(), hands_dof_keys_.end(), key) == hands_dof_keys_.end())
    hands_dof_keys_.push_back(key);
  if (hands_dof_color_ && !(call.indexed && AoHandsColourDraw(record) &&
                            std::find(hands_dof_keys_.begin(), hands_dof_keys_.end(), key) != hands_dof_keys_.end()))
    HandsDofFinish(cl);
}

void GameRenderer::HandsDofRedraw(nvrhi::ICommandList* cl, const DrawCall& call, const PipelineRecord& record, const GameShader& vs,
                                  const nvrhi::GraphicsState& gs, const nvrhi::DrawArguments& args, bool indexed) {
  if (hands_dof_frame_ != call.frame || !call.indexed || !options_.hands_dof.enabled || photo_.active ||
      !AoHandsColourDraw(record))
    return;
  const AoHandsKey key{call.index_info.physical, call.start, call.count, call.base_vertex};
  if (std::find(hands_dof_keys_.begin(), hands_dof_keys_.end(), key) == hands_dof_keys_.end()) return;
  nvrhi::IFramebuffer* mask = hands_dof_->MaskFramebuffer(cl, gs.framebuffer->getDesc(), !hands_dof_masked_);
  if (!mask) return;
  hands_dof_masked_ = true;
  const nvrhi::FramebufferDesc& game = gs.framebuffer->getDesc();
  // As AoRedrawHands: the hands' vertex shader and state, depth EQUAL without writes, into the mask.
  PipelineRecord r = record;
  r.ps_hash = UINT64_C(0x484E44444F464D00);  // "HNDDOFM": its own key
  r.ps_input = r.ps_hash;
  const uint64_t key_hash = PipelineKey(r);
  nvrhi::IGraphicsPipeline* pipeline = nullptr;
  if (auto known = pipelines_.find(key_hash); known != pipelines_.end()) {
    pipeline = known->second;
  } else {
    nvrhi::GraphicsPipelineDesc pd;
    nvrhi::FramebufferInfo fbi;
    BuildPipelineDesc(record, vs.handle, hands_dof_mask_ps_, pd, fbi);
    fbi = mask->getFramebufferInfo();
    for (uint32_t i = 0; i < 8; ++i) pd.renderState.blendState.targets[i] = nvrhi::BlendState::RenderTarget();
    pd.renderState.blendState.targets[0].colorWriteMask = nvrhi::ColorMask::Red;
    nvrhi::DepthStencilState& ds = pd.renderState.depthStencilState;
    ds.depthTestEnable = true;
    ds.depthWriteEnable = false;
    ds.depthFunc = nvrhi::ComparisonFunc::Equal;
    ds.stencilEnable = false;
    pipeline = GetPipeline(pd, fbi, key_hash, PipelineDemand::kSoon);
  }
  if (!pipeline) return;
  nvrhi::GraphicsState redraw = gs;
  redraw.pipeline = pipeline;
  redraw.framebuffer = mask;
  cl->setGraphicsState(redraw);
  if (indexed) {
    cl->drawIndexed(args);
  } else {
    cl->draw(args);
  }
  hands_dof_color_ = game.colorAttachments[0].texture;
}

void GameRenderer::HandsDofPresentHook(nvrhi::ICommandList* cl) {
  HandsDofFinish(cl);
  if (hands_dof_timer_) hands_dof_timer_->FrameEnd();
}

}  // namespace nr
