// The game renderer's hooks for the red band on human skin (skin_band.h, docs/lighting.md). The renderer's files call these
// at a handful of points marked "Skin hook (game_skin.cpp)". With native_skin_band off nothing of it is made (skin_band_
// stays null).

#include "backend/game_renderer.h"
#include "backend/log.h"
#include "backend/shaders/generated/skin_band_mask_ps_dxil.h"
#include "backend/shaders/generated/skin_band_mask_ps_spirv.h"
#include "kknr/xenos.h"

namespace nr {

namespace {
// XXH3-64 of the microcode of the title's two AfterEffects fog pixel shaders (as in game_ao.cpp).
constexpr uint64_t kFogShaderA = UINT64_C(0x1B65AC7429B67FE9);
constexpr uint64_t kFogShaderB = UINT64_C(0x9F699E763A41FE07);
}  // namespace

void GameRenderer::SkinInitialize() {
  skin_user_gpu_time_ = options_.skin.gpu_time;
  if (options_.skin.enabled) UpdateSkin(options_.skin);
}

void GameRenderer::UpdateSkin(const SkinBandConfig& want) {
  options_.skin = want;
  options_.skin.gpu_time = skin_user_gpu_time_;
  if (!want.enabled || skin_band_ || skin_failed_) return;
  nvrhi::ShaderDesc hd;
  hd.shaderType = nvrhi::ShaderType::Pixel;
  hd.entryName = "main";
  hd.debugName = "skin_band_mask_ps";
  skin_mask_ps_ = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN
                      ? device_->createShader(hd, k_skin_band_mask_ps_spirv, sizeof(k_skin_band_mask_ps_spirv))
                      : device_->createShader(hd, k_skin_band_mask_ps_dxil, sizeof(k_skin_band_mask_ps_dxil));
  skin_timer_ = std::make_unique<AoGpuTimer>(device_, "skin band");
  skin_timer_->set_quiet(!skin_user_gpu_time_);
  skin_band_ = std::make_unique<SkinBand>(device_);
  if (!skin_mask_ps_ || !skin_band_->Initialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: skin band: unavailable (its shaders could not be made), it stays off");
    skin_band_.reset();
    skin_timer_.reset();
    skin_failed_ = true;
    return;
  }
  skin_band_->SetTimer(skin_timer_.get());
}

void GameRenderer::SkinFinish(nvrhi::ICommandList* cl) {
  if (!skin_color_) return;
  skin_band_->Apply(cl, skin_color_);
  skin_color_ = nullptr;
}

void GameRenderer::SkinDrawHook(nvrhi::ICommandList* cl, const DrawCall& call, const PipelineRecord& record, const GameShader* ps) {
  if (skin_frame_ != call.frame) {
    skin_frame_ = call.frame;
    skin_fog_ = false;
    skin_color_ = nullptr;
  }
  if (ps && (ps->ucode_hash == kFogShaderA || ps->ucode_hash == kFogShaderB)) {
    SkinFinish(cl);
    skin_fog_ = true;
  } else if (skin_fog_ && skin_color_ && !AoHandsColourDraw(record)) {
    SkinFinish(cl);
  }
}

void GameRenderer::SkinRedraw(nvrhi::ICommandList* cl, const DrawCall& call, const PipelineRecord& record, const GameShader& vs,
                              const nvrhi::GraphicsState& gs, const nvrhi::DrawArguments& args, bool indexed,
                              const dev::DeviceView& d) {
  if (skin_frame_ != call.frame || !options_.skin.enabled) return;
  PipelineRecord plain = record;
  plain.flags &= ~kRecordTessellation;  // Ann's tessellated draws are drawn again tessellated
  if (!AoHandsColourDraw(plain)) return;
  uint32_t words[6];
  d.fetch_constant(0, words);
  const auto fetch = kknr::TextureFetch::FromWords(words);
  if (fetch.Type() != 2) return;
  auto* entry = textures_.Find(fetch);
  if (!entry || !entry->host || !SkinTexture(static_cast<HostTexture*>(entry->host)->pack_hash)) return;
  nvrhi::IFramebuffer* mask = skin_band_->MaskFramebuffer(cl, gs.framebuffer->getDesc(), !skin_color_);
  if (!mask) return;
  // As AoRedrawHands: the draw's vertex shader and state, depth EQUAL without writes, into the skin image.
  PipelineRecord r = record;
  r.ps_hash = UINT64_C(0x534B494E42414E44);  // "SKINBAND": its own key
  r.ps_input = r.ps_hash;
  const uint64_t key_hash = PipelineKey(r);
  nvrhi::IGraphicsPipeline* pipeline = nullptr;
  if (auto known = pipelines_.find(key_hash); known != pipelines_.end()) {
    pipeline = known->second;
  } else {
    nvrhi::GraphicsPipelineDesc pd;
    nvrhi::FramebufferInfo fbi;
    BuildPipelineDesc(record, vs.handle, skin_mask_ps_, pd, fbi);
    fbi = mask->getFramebufferInfo();
    for (uint32_t i = 0; i < 8; ++i) pd.renderState.blendState.targets[i] = nvrhi::BlendState::RenderTarget();
    pd.renderState.blendState.targets[0].colorWriteMask = nvrhi::ColorMask::All;
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
  skin_color_ = gs.framebuffer->getDesc().colorAttachments[0].texture;
}

void GameRenderer::SkinPresentHook(nvrhi::ICommandList* cl) {
  SkinFinish(cl);
  if (skin_timer_) skin_timer_->FrameEnd();
}

}  // namespace nr
