#include "backend/api_binding.h"

#include "backend/draw_state.h"

namespace nr {

namespace {

DrawTracker* Resolve(void* self) {
  auto* binding = static_cast<ApiBinding*>(self);
  return binding && binding->resolve ? binding->resolve(binding->owner) : nullptr;
}

#define NR_FORWARD(call)               \
  if (DrawTracker* d = Resolve(self)) { \
    d->call;                          \
  }

int IsActive(void* self) { return Resolve(self) ? 1 : 0; }
void SetRenderTarget(void* self, uint32_t index, uint32_t surface) {
  NR_FORWARD(SetRenderTarget(index, surface));
}
void SetDepthStencilSurface(void* self, uint32_t surface) {
  NR_FORWARD(SetDepthStencilSurface(surface));
}
void SetViewport(void* self, uint32_t viewport) { NR_FORWARD(SetViewport(viewport)); }
void SetTexture(void* self, uint32_t sampler, uint32_t texture) {
  NR_FORWARD(SetTexture(sampler, texture));
}
void SetIndices(void* self, uint32_t ib) { NR_FORWARD(SetIndices(ib)); }
void SetStreamSource(void* self, uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride) {
  NR_FORWARD(SetStreamSource(stream, vb, offset, stride));
}
void SetPixelShader(void* self, uint32_t ps) { NR_FORWARD(SetPixelShader(ps)); }
void SetVertexShader(void* self, uint32_t vs) { NR_FORWARD(SetVertexShader(vs)); }
void SetVertexDeclaration(void* self, uint32_t decl) { NR_FORWARD(SetVertexDeclaration(decl)); }
void SetVsConstantsF(void* self, uint32_t reg, uint32_t data, uint32_t count) {
  NR_FORWARD(SetVsConstantsF(reg, data, count));
}
void SetPsConstantsF(void* self, uint32_t reg, uint32_t data, uint32_t count) {
  NR_FORWARD(SetPsConstantsF(reg, data, count));
}
void SetVsConstantsI(void* self, uint32_t reg, uint32_t data, uint32_t count) {
  NR_FORWARD(SetVsConstantsI(reg, data, count));
}
void SetBlendControl(void* self, uint32_t rt, uint32_t value) {
  NR_FORWARD(SetBlendControl(rt, value));
}
void SetRenderState(void* self, uint32_t state, uint32_t value) {
  NR_FORWARD(SetRenderState(state, value));
}
void SetSamplerState(void* self, uint32_t sampler, uint32_t type, uint32_t value) {
  NR_FORWARD(SetSamplerState(sampler, type, value));
}
void BeginConditionalRendering(void* self, uint32_t id) {
  NR_FORWARD(BeginConditionalRendering(id));
}
void EndConditionalRendering(void* self) { NR_FORWARD(EndConditionalRendering()); }
void Clear(void* self, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
  NR_FORWARD(Clear(flags, color, z, stencil));
}
void ApiResolve(void* self, uint32_t flags, uint32_t dest) { NR_FORWARD(Resolve(flags, dest)); }
void DrawIndexed(void* self, uint32_t prim, int32_t base_vertex, uint32_t start_index,
                 uint32_t index_count) {
  NR_FORWARD(DrawIndexed(prim, base_vertex, start_index, index_count));
}
void Draw(void* self, uint32_t prim, uint32_t start_vertex, uint32_t vertex_count) {
  NR_FORWARD(Draw(prim, start_vertex, vertex_count));
}
void Present(void* self) { NR_FORWARD(Present()); }

#undef NR_FORWARD

}  // namespace

void InitApiBinding(ApiBinding& binding, void* owner, DrawTracker* (*resolve)(void* owner)) {
  binding.owner = owner;
  binding.resolve = resolve;
  NrApi& a = binding.api;
  a = {};
  a.version = NR_API_VERSION;
  a.size = sizeof(NrApi);
  a.self = &binding;
  a.is_active = IsActive;
  a.set_render_target = SetRenderTarget;
  a.set_depth_stencil_surface = SetDepthStencilSurface;
  a.set_viewport = SetViewport;
  a.set_texture = SetTexture;
  a.set_indices = SetIndices;
  a.set_stream_source = SetStreamSource;
  a.set_pixel_shader = SetPixelShader;
  a.set_vertex_shader = SetVertexShader;
  a.set_vertex_declaration = SetVertexDeclaration;
  a.set_vs_constants_f = SetVsConstantsF;
  a.set_ps_constants_f = SetPsConstantsF;
  a.set_vs_constants_i = SetVsConstantsI;
  a.set_blend_control = SetBlendControl;
  a.set_render_state = SetRenderState;
  a.set_sampler_state = SetSamplerState;
  a.begin_conditional_rendering = BeginConditionalRendering;
  a.end_conditional_rendering = EndConditionalRendering;
  a.clear = Clear;
  a.resolve = ApiResolve;
  a.draw_indexed = DrawIndexed;
  a.draw = Draw;
  a.present = Present;
}

}  // namespace nr
