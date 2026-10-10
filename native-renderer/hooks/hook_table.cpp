#include "hooks/hook_table.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "backend/guest_layout.h"

namespace nr::hooks {

namespace {

thread_local int g_depth = 0;

// Draw and frame.
void DrawIndexedVertices(const NrApi& a, const GuestArgs& g) {
  // dev, primType, baseVertex, startIndex, indexCount
  a.draw_indexed(a.self, g.r4(), int32_t(g.r5()), g.r6(), g.r7());
}
void DrawVertices(const NrApi& a, const GuestArgs& g) {
  // dev, primType, startVertex, vertexCount
  a.draw(a.self, g.r4(), g.r5(), g.r6());
}
void Resolve(const NrApi& a, const GuestArgs& g) {
  // dev, flags, srcRect, destTexture, destPoint, slice, level, clear values...
  a.resolve(a.self, g.r4(), g.r6());
}
void Clear(const NrApi& a, const GuestArgs& g) {
  // dev, count, rects, flags, color, z (f1), stencil
  a.clear(a.self, g.r6(), g.r7(), float(g.f1), g.r[kClearStencilRegister - 3]);
}
void Present(const NrApi& a, const GuestArgs&) { a.present(a.self); }

// State.
void SetTexture(const NrApi& a, const GuestArgs& g) { a.set_texture(a.self, g.r4(), g.r5()); }
void SetIndices(const NrApi& a, const GuestArgs& g) { a.set_indices(a.self, g.r4()); }
void SetStreamSource(const NrApi& a, const GuestArgs& g) {
  a.set_stream_source(a.self, g.r4(), g.r5(), g.r6(), g.r7());
}
void SetPixelShader(const NrApi& a, const GuestArgs& g) { a.set_pixel_shader(a.self, g.r4()); }
void SetVertexShader(const NrApi& a, const GuestArgs& g) { a.set_vertex_shader(a.self, g.r4()); }
void SetVertexDeclaration(const NrApi& a, const GuestArgs& g) {
  a.set_vertex_declaration(a.self, g.r4());
}
void SetVsConstantsF(const NrApi& a, const GuestArgs& g) {
  a.set_vs_constants_f(a.self, g.r4(), g.r5(), g.r6());
}
void SetPsConstantsF(const NrApi& a, const GuestArgs& g) {
  a.set_ps_constants_f(a.self, g.r4(), g.r5(), g.r6());
}
void SetVsConstantsI(const NrApi& a, const GuestArgs& g) {
  a.set_vs_constants_i(a.self, g.r4(), g.r5(), g.r6());
}
void SetBlendControl(const NrApi& a, const GuestArgs& g) {
  a.set_blend_control(a.self, g.r4(), g.r5());
}
void SetRenderTarget(const NrApi& a, const GuestArgs& g) {
  a.set_render_target(a.self, g.r4(), g.r5());
}
void SetDepthStencilSurface(const NrApi& a, const GuestArgs& g) {
  a.set_depth_stencil_surface(a.self, g.r4());
}
void SetViewport(const NrApi& a, const GuestArgs& g) { a.set_viewport(a.self, g.r4()); }
void BeginConditionalRendering(const NrApi& a, const GuestArgs& g) {
  a.begin_conditional_rendering(a.self, g.r4());
}
void EndConditionalRendering(const NrApi& a, const GuestArgs&) {
  a.end_conditional_rendering(a.self);
}

// The device's render-state setters: (dev, value). One handler per state,
// since the setter does not receive the state number.
template <uint32_t kState>
void RenderState(const NrApi& a, const GuestArgs& g) {
  a.set_render_state(a.self, kState, g.r4());
}
// Sampler-state setters: (dev, sampler, value).
template <uint32_t kType>
void SamplerState(const NrApi& a, const GuestArgs& g) {
  a.set_sampler_state(a.self, g.r4(), kType, g.r5());
}

// clang-format off
constexpr HookEntry kEntries[] = {
    {0x82109788, "SetRenderState(CULLMODE)", Kind::kRenderState, RenderState<rs::kCullMode>},
    {0x821097F8, "SetRenderState(ALPHATESTENABLE)", Kind::kRenderState, RenderState<rs::kAlphaTestEnable>},
    {0x82109CB0, "SetRenderState(ALPHAREF)", Kind::kRenderState, RenderState<rs::kAlphaRef>},
    {0x82109D20, "SetRenderState(ALPHAFUNC)", Kind::kRenderState, RenderState<rs::kAlphaFunc>},
    {0x82109E78, "SetRenderState(ZENABLE)", Kind::kRenderState, RenderState<rs::kZEnable>},
    {0x82109EC8, "SetRenderState(ZWRITEENABLE)", Kind::kRenderState, RenderState<rs::kZWriteEnable>},
    {0x82109F00, "SetRenderState(ZFUNC)", Kind::kRenderState, RenderState<rs::kZFunc>},
    {0x82109F48, "SetRenderState(STENCILENABLE)", Kind::kRenderState, RenderState<rs::kStencilEnable>},
    {0x8210A1E0, "SetRenderState(STENCILREF)", Kind::kRenderState, RenderState<rs::kStencilRef>},
    {0x8210A2D0, "SetRenderState(CLIPPLANEENABLE)", Kind::kRenderState, RenderState<rs::kClipPlaneEnable>},
    {0x8210A4E8, "SetRenderState(COLORWRITEENABLE)", Kind::kRenderState, RenderState<rs::kColorWriteEnable>},
    {0x8210B160, "SetSamplerState(MINFILTER)", Kind::kSamplerState, SamplerState<ss::kMinFilter>},
    {0x8210B270, "SetSamplerState(MAGFILTER)", Kind::kSamplerState, SamplerState<ss::kMagFilter>},
    {0x8210B378, "SetSamplerState(MIPFILTER)", Kind::kSamplerState, SamplerState<ss::kMipFilter>},
    {0x8210B540, "SetSamplerState(MIPMAPLODBIAS)", Kind::kSamplerState, SamplerState<ss::kMipMapLodBias>},
    {0x8210B6E0, "SetSamplerState(BORDERCOLOR)", Kind::kSamplerState, SamplerState<ss::kBorderColor>},
    {0x8210B750, "SetSamplerState(ADDRESSU)", Kind::kSamplerState, SamplerState<ss::kAddressU>},
    {0x8210B7A0, "SetSamplerState(ADDRESSV)", Kind::kSamplerState, SamplerState<ss::kAddressV>},
    {0x8210B7F0, "SetSamplerState(ADDRESSW)", Kind::kSamplerState, SamplerState<ss::kAddressW>},
    {0x8210BAC8, "SetViewport", Kind::kState, SetViewport},
    {0x8210BD38, "SetStreamSource", Kind::kState, SetStreamSource},
    {0x8210BE38, "SetIndices", Kind::kState, SetIndices},
    {0x8210C130, "SetBlendControl", Kind::kState, SetBlendControl},
    {0x8210C378, "SetRenderTarget", Kind::kState, SetRenderTarget},
    {0x8210C6E0, "SetDepthStencilSurface", Kind::kState, SetDepthStencilSurface},
    {0x8210CB50, "BeginConditionalRendering", Kind::kState, BeginConditionalRendering},
    {0x8210CB70, "EndConditionalRendering", Kind::kState, EndConditionalRendering},
    {0x82110300, "SetVertexShaderConstantF", Kind::kState, SetVsConstantsF},
    {0x82110448, "SetPixelShaderConstantF", Kind::kState, SetPsConstantsF},
    {0x82110640, "SetVertexShaderConstantI", Kind::kState, SetVsConstantsI},
    {0x821108B8, "SetVertexShader", Kind::kState, SetVertexShader},
    {0x82110C28, "SetPixelShader", Kind::kState, SetPixelShader},
    {0x82111E68, "SetVertexDeclaration", Kind::kState, SetVertexDeclaration},
    {0x821147B8, "Present", Kind::kFrame, Present},
    {0x82115418, "Clear", Kind::kDraw, Clear},
    {0x821154C8, "DrawVertices", Kind::kDraw, DrawVertices},
    {0x82115708, "DrawIndexedVertices", Kind::kDraw, DrawIndexedVertices},
    {0x82116178, "Resolve", Kind::kDraw, Resolve},
    {0x82118F78, "SetTexture", Kind::kState, SetTexture},
};
// clang-format on

constexpr bool IsSorted() {
  for (size_t i = 1; i < std::size(kEntries); ++i) {
    if (kEntries[i - 1].address >= kEntries[i].address) return false;
  }
  return true;
}

static_assert(IsSorted(), "kEntries must be sorted by address (Find searches it)");

}  // namespace

std::span<const HookEntry> Table() { return kEntries; }

const HookEntry* Find(uint32_t address) {
  auto table = Table();
  auto it = std::lower_bound(table.begin(), table.end(), address,
                             [](const HookEntry& e, uint32_t a) { return e.address < a; });
  return it != table.end() && it->address == address ? &*it : nullptr;
}

CallScope::CallScope() : outermost_(g_depth == 0) { ++g_depth; }
CallScope::~CallScope() { --g_depth; }

}  // namespace nr::hooks
