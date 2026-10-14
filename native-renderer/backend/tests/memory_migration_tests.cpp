#include "backend/game_renderer.h"
#include "backend/shader_library.h"
#include "backend/tests/fake_guest.h"
#include "backend/tests/vk_test_device.h"
#include "backend/tests/memory_d3d12_test_device.h"
#include "backend/shaders/generated/memory_probe_ps_spirv.h"
#include "backend/shaders/generated/memory_probe_ps_dxil.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nr {
struct MemoryMigrationTestAccess {
  static void Seed(GameRenderer& g, nvrhi::ICommandList* cl) {
    auto* color = g.GetTarget(false, 0, 16, 0, 16);
    auto* depth = g.GetTarget(true, 0, 16, 0, 16);
    if (!color || !depth) std::exit(2);
    std::vector<uint8_t> pixels(32 * 32 * 4);
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x) {
      const size_t i = (y * 32 + x) * 4;
      pixels[i] = uint8_t((x / 2) * 17); pixels[i + 1] = uint8_t((y / 2) * 17);
      pixels[i + 2] = uint8_t(x / 2 + (y / 2) * 16); pixels[i + 3] = 255;
    }
    cl->writeTexture(color->texture, 0, 0, pixels.data(), 32 * 4);
    const float zero[4] = {};
    for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x) {
      const unsigned v = x + y * 16;
      g.ClearRect(cl, nullptr, depth, nvrhi::Rect(x, x + 1, y, y + 1), zero,
                  false, true, true, float(v + 1) / 257, v);
    }
  }
  static void Scale(GameRenderer& g, nvrhi::ICommandList* cl) {
    g.pending_memory_scale_ = 0.5f;
    for (unsigned frame = 0; frame < 16 && g.options().render_scale_x != 1; ++frame) {
      g.ApplyMemoryScale(cl);
      if (g.options().render_scale_x != 1) g.EndFrame();
    }
  }
  static bool CancelReplacedSource(GameRenderer& g, nvrhi::ICommandList* cl) {
    g.pending_memory_scale_ = 0.5f;
    g.ApplyMemoryScale(cl);
    if (g.options().render_scale_x != 2 || g.memory_staged_.empty()) return false;
    auto* color = g.FindTarget(false, 0, 16, 0);
    auto replacement = g.device_->createTexture(color->texture->getDesc());
    if (!replacement) return false;
    cl->copyTexture(replacement, nvrhi::TextureSlice(), color->texture, nvrhi::TextureSlice());
    g.RetireResource(color->texture); color->texture = replacement;
    g.ApplyMemoryScale(cl);
    return g.memory_staged_.empty() && g.options().render_scale_x == 2;
  }
  static nvrhi::ITexture* Texture(GameRenderer& g, bool depth) {
    auto* t = g.FindTarget(depth, 0, 16, 0);
    return t ? t->texture.Get() : nullptr;
  }
  static nvrhi::TextureHandle Probe(GameRenderer& g, nvrhi::ICommandList* cl, bool stencil) {
    auto* depth = Texture(g, true);
    nvrhi::TextureDesc d;
    d.width = d.height = 16; d.format = nvrhi::Format::R32_FLOAT;
    d.isRenderTarget = true; d.keepInitialState = true; d.initialState = nvrhi::ResourceStates::RenderTarget;
    auto out = g.device_->createTexture(d);
    auto fb = g.device_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(out));
    nvrhi::ShaderDesc sd; sd.shaderType = nvrhi::ShaderType::Pixel; sd.entryName = "ps_probe";
    const bool vk = g.device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
    auto ps = g.device_->createShader(sd, vk ? k_memory_probe_ps_spirv : k_memory_probe_ps_dxil,
                                    vk ? sizeof(k_memory_probe_ps_spirv) : sizeof(k_memory_probe_ps_dxil));
    nvrhi::GraphicsPipelineDesc pd;
    pd.primType = nvrhi::PrimitiveType::TriangleList; pd.VS = g.blit_vs_; pd.PS = ps;
    pd.bindingLayouts = {g.memory_depth_layout_};
    pd.renderState.depthStencilState.depthTestEnable = pd.renderState.depthStencilState.depthWriteEnable = false;
    pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    auto pipeline = g.device_->createGraphicsPipeline(pd, fb);
    nvrhi::BindingSetDesc bd;
    bd.bindings = {nvrhi::BindingSetItem::PushConstants(0, 16), nvrhi::BindingSetItem::Texture_SRV(0, depth),
                   nvrhi::BindingSetItem::Texture_SRV(1, depth, nvrhi::Format::X32G8_UINT)};
    auto set = g.device_->createBindingSet(bd, g.memory_depth_layout_);
    nvrhi::GraphicsState state; state.pipeline = pipeline; state.framebuffer = fb; state.bindings = {set};
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(16.f, 16.f));
    cl->setGraphicsState(state);
    struct {float ratio[2]; uint32_t mask, pad;} p{{1, 1}, stencil ? 1u : 0u, 0};
    cl->setPushConstants(&p, sizeof(p));
    nvrhi::DrawArguments draw; draw.vertexCount = 3; cl->draw(draw);
    return out;
  }
  static void EvictClear(GameRenderer& g, nvrhi::ICommandList* cl) {
    auto* color = g.FindTarget(false, 0, 16, 0);
    auto* depth = g.FindTarget(true, 0, 16, 0);
    const float rgba[4] = {0.2f, 0.4f, 0.6f, 1.f};
    g.ClearRect(cl, color, nullptr, nvrhi::Rect(0, 16, 0, 16), rgba, true, false, false, 1, 0);
    g.ClearRect(cl, nullptr, depth, nvrhi::Rect(0, 16, 0, 16), rgba, false, true, true, 0.25f, 173);
    g.frame_ += 200;
    g.TrimClearTargets();
    if (color->texture || depth->texture || !color->clear_only || !depth->clear_only) std::exit(3);
  }
  static void Restore(GameRenderer& g, nvrhi::ICommandList* cl) {
    g.current_cl_ = cl;
    if (!g.FindTarget(false, 0, 16, 0) || !g.FindTarget(true, 0, 16, 0)) std::exit(4);
  }
};
}

static unsigned checks = 0;
static void Check(bool ok) {
  if (!ok) { std::fprintf(stderr, "check %u failed\n", checks); std::exit(1); }
  ++checks;
}
template<class Host>
int RunMemoryMigration(Host& host, const char* validation, bool native_validation) {
  using namespace nr;
  if (!host) return 2;
  auto* device = host->device();
  test::FakeGuestMemory guest;
  kknr::GuestMemory physical;
  physical.base = guest.PhysicalBase(); physical.size = guest.PhysicalSize();
  ShaderLibrary shaders(device, guest);
  GameRenderer game(device, guest, physical, &shaders);
  game.options().render_scale_x = game.options().render_scale_y = 2;
  game.options().guest_frame_width = game.options().guest_frame_height = 16;
  game.options().memory_budget = [] { return MemoryBudget{1ull << 30, 0, true}; };
  Check(game.Initialize());
  auto seed = device->createCommandList(); seed->open(); game.UploadDummies(seed);
  MemoryMigrationTestAccess::Seed(game, seed); seed->close(); host->Execute(seed); host->WaitIdle();
  game.WaitForPrewarm();
  auto migration = device->createCommandList(); migration->open();
  Check(MemoryMigrationTestAccess::CancelReplacedSource(game, migration));
  MemoryMigrationTestAccess::Scale(game, migration);
  Check(game.options().render_scale_x == 1);
  // Stress command-list ownership: remove the explicit retirement references
  // before submitting the recorded copy. No raw target pointer may keep it alive.
  for (unsigned i = 0; i < 12; ++i) game.EndFrame();
  migration->close(); host->Execute(migration); host->WaitIdle();
  auto probe = device->createCommandList(); probe->open();
  auto depth = MemoryMigrationTestAccess::Probe(game, probe, false);
  auto stencil = MemoryMigrationTestAccess::Probe(game, probe, true);
  probe->close(); host->Execute(probe); host->WaitIdle();
  const auto z = host->ReadBack(depth), s = host->ReadBack(stencil);
  const auto color = host->ReadBack(MemoryMigrationTestAccess::Texture(game, false));
  for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x) {
    const unsigned v = x + y * 16;
    Check(std::abs(z.at(x, y)[0] - float(v + 1) / 257) < 1e-6f);
    Check(s.at(x, y)[0] == float(v));
    Check(std::abs(color.at(x, y)[0] - float(x * 17) / 255) < 1e-6f);
    Check(std::abs(color.at(x, y)[1] - float(y * 17) / 255) < 1e-6f);
    Check(std::abs(color.at(x, y)[2] - float(v) / 255) < 1e-6f);
  }
  auto clear = device->createCommandList(); clear->open();
  MemoryMigrationTestAccess::EvictClear(game, clear); clear->close(); host->Execute(clear);
  auto restore = device->createCommandList(); restore->open();
  MemoryMigrationTestAccess::Restore(game, restore); restore->close(); host->Execute(restore);
  host->WaitIdle();
  const auto restored = host->ReadBack(MemoryMigrationTestAccess::Texture(game, false));
  const float expected[4] = {0.2f, 0.4f, 0.6f, 1.f};
  for (size_t i = 0; i < restored.rgba.size(); ++i) Check(std::abs(restored.rgba[i] - expected[i % 4]) < 1e-6f);
  auto restored_probe = device->createCommandList(); restored_probe->open();
  auto restored_depth = MemoryMigrationTestAccess::Probe(game, restored_probe, false);
  auto restored_stencil = MemoryMigrationTestAccess::Probe(game, restored_probe, true);
  restored_probe->close(); host->Execute(restored_probe); host->WaitIdle();
  const auto rz = host->ReadBack(restored_depth), rs = host->ReadBack(restored_stencil);
  for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x) {
    Check(rz.at(x, y)[0] == 0.25f); Check(rs.at(x, y)[0] == 173.f);
  }
  Check(host->errors() == 0);
  std::printf("memory migration: %u checks passed on %s; %s validation %s\n", checks,
              host->device_name().c_str(), validation, native_validation ? "on" : "unavailable (NVRHI on)");
  return 0;
}
int main(int argc, char** argv) {
#if NR_HAS_D3D12
  if (argc == 2 && std::strcmp(argv[1], "--d3d12") == 0) {
    auto host = nr::test::MemoryD3D12TestDevice::Create();
    return host ? RunMemoryMigration(host, "native D3D12", host->native_validation()) : 2;
  }
#endif
  auto host = nr::test::VulkanTestDevice::Create(true);
  return host ? RunMemoryMigration(host, "Khronos", host->khronos_validation()) : 2;
}
