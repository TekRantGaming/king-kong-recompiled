// Milestone 3 without a GPU: recorded call sequences go through the hook
// table, the NrApi binding and the draw-state tracker, and the tests check
// the records that reach the renderer's interface (DrawSink).

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "backend/api_binding.h"
#include "backend/draw_state.h"
#include "backend/primitive.h"
#include "backend/tests/check.h"
#include "backend/tests/fake_guest.h"
#include "backend/tests/scene.h"
#include "hooks/hook_table.h"
#include "hooks/trace_replay.h"

using namespace nr;
using namespace nr::test;

namespace {

class RecordingSink final : public DrawSink {
 public:
  void OnClear(const ClearCall& c) override {
    clears.push_back(c);
    order += 'C';
  }
  void OnDraw(const DrawCall& d) override {
    draws.push_back(d);
    // The guest pointers are valid only during the call: keep the indices.
    std::vector<uint32_t> idx;
    PrimitiveInput in{d.primitive, d.indexed ? d.index_data : nullptr, d.index_info.index32,
                      d.start, d.count, d.indexed ? d.base_vertex : 0};
    BuildTriangleList(in, idx);
    draw_indices.push_back(idx);
    order += 'D';
  }
  void OnResolve(const ResolveCall& r) override {
    resolves.push_back(r);
    order += 'R';
  }
  void OnPresent(uint32_t frame, uint32_t rt0) override {
    presents.push_back({frame, rt0});
    order += 'P';
  }
  std::vector<ClearCall> clears;
  std::vector<DrawCall> draws;
  std::vector<std::vector<uint32_t>> draw_indices;
  std::vector<ResolveCall> resolves;
  std::vector<std::pair<uint32_t, uint32_t>> presents;
  std::string order;
};

// A tracker bound through the same NrApi table the plugin exports.
struct Harness {
  FakeGuestMemory memory;
  RecordingSink sink;
  DrawState state{memory, &sink};
  ApiBinding binding;
  bool active = true;
  Harness() {
    InitApiBinding(binding, this, [](void* owner) -> DrawState* {
      auto* h = static_cast<Harness*>(owner);
      return h->active ? &h->state : nullptr;
    });
  }
  const NrApi* api() const { return &binding.api; }
  hooks::ReplayStats Replay(const std::string& log) {
    std::istringstream in(log);
    return hooks::ReplayTrace(in, api());
  }
  void Call(uint32_t address, std::initializer_list<uint32_t> regs_from_r4, double f1 = 0.0) {
    hooks::GuestArgs a;
    a.r[0] = kDevice;
    int i = 1;
    for (uint32_t v : regs_from_r4) a.r[i++] = v;
    a.f1 = f1;
    const hooks::HookEntry* e = hooks::Find(address);
    CHECK(e != nullptr);
    if (e) hooks::Run(*e, api(), a, [] {});
  }
};

}  // namespace

// ------------------------------------------------------------ hook table ---

TEST(HookTableIsSortedAndComplete) {
  auto table = hooks::Table();
  CHECK_EQ(table.size(), size_t(39));
  std::set<std::string> names;
  for (size_t i = 0; i < table.size(); ++i) {
    CHECK(table[i].handler != nullptr);
    CHECK(names.insert(table[i].name).second);
    CHECK((table[i].address & 3) == 0);
    CHECK(table[i].address >= 0x82108000 && table[i].address < 0x82128000);
    if (i) CHECK(table[i - 1].address < table[i].address);
    CHECK(hooks::Find(table[i].address) == &table[i]);
  }
  CHECK(hooks::Find(0x82115708) != nullptr);
  CHECK_EQ(std::string(hooks::Find(0x82115708)->name), std::string("DrawIndexedVertices"));
  CHECK(hooks::Find(0x821241A8) == nullptr);  // XMMatrixMultiply: not graphics
  CHECK(hooks::Find(0) == nullptr);
}

// kk_native_hooks.cpp (the game-side wrappers) must hook exactly the table.
TEST(GameSideHookListMatchesTable) {
  std::ifstream f(NR_SOURCE_DIR "/hooks/kk_native_hooks.cpp");
  CHECK(f.good());
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::set<uint32_t> listed;
  const std::string kStart = "#define NR_HOOK_LIST(X)";
  size_t pos = text.find(kStart);
  CHECK(pos != std::string::npos);
  size_t end = text.find("\n\n", pos);
  std::string list = text.substr(pos, end - pos);
  for (size_t p = 0; (p = list.find("X(", p)) != std::string::npos; p += 2) {
    if (p > 0 && list[p - 1] == '(') continue;  // "(X)" in the macro head
    listed.insert(uint32_t(std::stoul(list.substr(p + 2, 8), nullptr, 16)));
  }
  CHECK_EQ(listed.size(), hooks::Table().size());
  for (const auto& e : hooks::Table()) CHECK(listed.count(e.address) == 1);
}

TEST(TraceLineParsing) {
  auto call = hooks::ParseTraceLine(
      "i> 00001234 KK d3d: sub_82115418 lr=82797A10 r3=4006A580 r4=00000000 r5=00000000 "
      "r6=0000000F r7=FF000000 r8=00000000 r9=00000000 r10=00000001 f1=1");
  CHECK(call.has_value());
  if (call) {
    CHECK_EQ(call->address, 0x82115418u);
    CHECK_EQ(call->args.r3(), 0x4006A580u);
    CHECK_EQ(call->args.r6(), 0xFu);
    CHECK_EQ(call->args.r7(), 0xFF000000u);
    CHECK_EQ(call->args.r10(), 1u);
    CHECK_NEAR(call->args.f1, 1.0, 0.0);
  }
  CHECK(!hooks::ParseTraceLine("i> KK d3d: frame 3 end: sub_82115708=641").has_value());
  CHECK(!hooks::ParseTraceLine("unrelated").has_value());
  CHECK(!hooks::ParseTraceLine("KK d3d: sub_8211XYZ0 lr=0 r3=0").has_value());
}

// ------------------------------------------------- the per-draw sequence ---

TEST(PerDrawSequenceReachesTheSink) {
  Harness h;
  TriangleScene scene(h.memory, 1280, 720);
  hooks::ReplayStats stats = h.Replay(scene.Log());
  CHECK_EQ(stats.unhooked, uint64_t(1));  // the XMMatrixMultiply line
  CHECK_EQ(stats.hooked, uint64_t(22));
  CHECK_EQ(h.sink.order, std::string("CDDP"));

  // The clear: colour from the ARGB, z from f1, stencil from r9.
  CHECK_EQ(h.sink.clears.size(), size_t(1));
  if (!h.sink.clears.empty()) {
    const ClearCall& c = h.sink.clears[0];
    CHECK(c.color && !c.depth && !c.stencil);
    CHECK_NEAR(c.rgba[0], 0x10 / 255.0, 1e-6);
    CHECK_NEAR(c.rgba[1], 0x20 / 255.0, 1e-6);
    CHECK_NEAR(c.rgba[2], 0x30 / 255.0, 1e-6);
    CHECK_NEAR(c.rgba[3], 1.0, 1e-6);
    CHECK_NEAR(c.z, 1.0, 0.0);
    CHECK_EQ(c.stencil_value, 0x55u);
    CHECK_EQ(c.render_target0, scene.backbuffer);
  }

  CHECK_EQ(h.sink.draws.size(), size_t(2));
  if (h.sink.draws.size() == 2) {
    const DrawCall& d = h.sink.draws[0];
    CHECK(d.indexed);
    CHECK(d.primitive == GuestPrimitive::kTriangleList);
    CHECK_EQ(d.base_vertex, int32_t(TriangleScene::kBaseVertex));
    CHECK_EQ(d.start, TriangleScene::kStartIndex);
    CHECK_EQ(d.count, 3u);
    CHECK_EQ(d.index_buffer, scene.ib);
    CHECK(!d.index_info.index32);
    CHECK_EQ(d.index_info.size_bytes, 12u);
    CHECK_EQ(d.vertex_buffer, scene.vb);
    CHECK_EQ(d.vertex_info.physical, scene.vb_data - FakeGuestMemory::kPhysicalView);
    CHECK_EQ(d.vertex_info.size_bytes, TriangleScene::kStride * 5);
    CHECK_EQ(d.stride, TriangleScene::kStride);
    CHECK(d.position_from_declaration);
    CHECK_EQ(d.position_offset, 4u);
    CHECK(d.vertex_data == h.memory.Virtual(scene.vb_data));
    CHECK_NEAR(d.vs_c0_c3[0], 1.6, 1e-6);
    CHECK_NEAR(d.vs_c0_c3[3], 0.1, 1e-6);
    CHECK_NEAR(d.vs_c0_c3[5], 1.6, 1e-6);
    CHECK_NEAR(d.vs_c0_c3[15], 1.0, 1e-6);
    CHECK_EQ(d.vertex_shader, scene.vertex_shader);
    CHECK_EQ(d.pixel_shader, scene.pixel_shader);
    CHECK_EQ(d.vertex_declaration, scene.decl);
    CHECK_EQ(d.textures[0], scene.texture);
    CHECK_EQ(d.blend_control[0], TriangleScene::kBlend);
    CHECK_EQ(d.render_targets[0], scene.backbuffer);
    CHECK(d.viewport == (Viewport{0, 0, 1280, 720, 0.0f, 1.0f}));
    CHECK_EQ(d.states.cull_mode, 6u);
    CHECK_EQ(d.states.z_enable, 1u);
    CHECK_EQ(d.states.z_func, 7u);
    CHECK_EQ(d.conditional_id, int32_t(TriangleScene::kConditionalId));
    CHECK_EQ(d.index_in_frame, 0u);
    // Base vertex and start index applied: vertices 2, 3, 4.
    CHECK(h.sink.draw_indices[0] == (std::vector<uint32_t>{2, 3, 4}));

    const DrawCall& d2 = h.sink.draws[1];
    CHECK_EQ(d2.render_targets[0], scene.shadow_map);
    CHECK_EQ(d2.conditional_id, -1);  // after EndConditionalRendering
    CHECK_EQ(d2.index_in_frame, 1u);
  }
  CHECK_EQ(h.sink.presents.size(), size_t(1));
  if (!h.sink.presents.empty()) {
    CHECK_EQ(h.sink.presents[0].first, 0u);
    CHECK_EQ(h.sink.presents[0].second, scene.backbuffer);
  }
  CHECK_EQ(h.state.frame(), 1u);
  CHECK_EQ(h.state.sampler_state(0, ss::kMagFilter), 1u);
  CHECK_EQ(h.state.stats().dropped_draws, uint64_t(0));
}

TEST(ConstantsAreCopiedAtCallTime) {
  Harness h;
  TriangleScene scene(h.memory, 640, 480);
  uint32_t data = h.memory.NewFloats({1, 2, 3, 4, 5, 6, 7, 8});
  h.Call(0x82110300, {10, data, 2});  // SetVertexShaderConstantF(10, data, 2)
  h.memory.WriteFloats(data, {-1, -1, -1, -1, -1, -1, -1, -1});  // the engine reuses its stack
  CHECK_NEAR(h.state.vs_constant(10)[0], 1.0, 0);
  CHECK_NEAR(h.state.vs_constant(11)[3], 8.0, 0);
  h.Call(0x82110448, {255, data, 4});  // PS: clamped at the last register
  CHECK_NEAR(h.state.ps_constant(255)[0], -1.0, 0);
  uint32_t ints = h.memory.AllocHeap(32);
  h.memory.Write32(ints, 0x20);  // count
  h.memory.Write32(ints + 4, 3);  // start
  h.memory.Write32(ints + 8, 1);  // step
  h.Call(0x82110640, {1, ints, 1});  // SetVertexShaderConstantI(1, ints, 1)
  CHECK_EQ(h.state.vs_int_constant(1), 0x010320u);
}

TEST(RenderAndSamplerStateSetters) {
  Harness h;
  struct {
    uint32_t address, state, value;
  } render[] = {{0x82109788, rs::kCullMode, 2},       {0x821097F8, rs::kAlphaTestEnable, 1},
                {0x82109CB0, rs::kAlphaRef, 0x79},    {0x82109D20, rs::kAlphaFunc, 6},
                {0x82109E78, rs::kZEnable, 0},        {0x82109EC8, rs::kZWriteEnable, 0},
                {0x82109F00, rs::kZFunc, 4},          {0x82109F48, rs::kStencilEnable, 1},
                {0x8210A1E0, rs::kStencilRef, 0xFF},  {0x8210A2D0, rs::kClipPlaneEnable, 1},
                {0x8210A4E8, rs::kColorWriteEnable, 7}};
  for (auto& r : render) {
    h.Call(r.address, {r.value});
    CHECK_EQ(h.state.render_state(r.state), r.value);
  }
  const RenderStates& s = h.state.render_states();
  CHECK_EQ(s.cull_mode, 2u);
  CHECK_EQ(s.alpha_test_enable, 1u);
  CHECK_EQ(s.alpha_ref, 0x79u);
  CHECK_EQ(s.alpha_func, 6u);
  CHECK_EQ(s.z_enable, 0u);
  CHECK_EQ(s.z_write_enable, 0u);
  CHECK_EQ(s.z_func, 4u);
  CHECK_EQ(s.stencil_enable, 1u);
  CHECK_EQ(s.stencil_ref, 0xFFu);
  CHECK_EQ(s.color_write_enable, 7u);

  struct {
    uint32_t address, type;
  } sampler[] = {{0x8210B750, ss::kAddressU},  {0x8210B7A0, ss::kAddressV},
                 {0x8210B7F0, ss::kAddressW},  {0x8210B6E0, ss::kBorderColor},
                 {0x8210B270, ss::kMagFilter}, {0x8210B160, ss::kMinFilter},
                 {0x8210B378, ss::kMipFilter}, {0x8210B540, ss::kMipMapLodBias}};
  uint32_t v = 100;
  for (auto& e : sampler) {
    h.Call(e.address, {3, v});  // (dev, sampler 3, value)
    CHECK_EQ(h.state.sampler_state(3, e.type), v);
    CHECK_EQ(h.state.sampler_state(2, e.type), 0u);
    ++v;
  }
}

TEST(NestedCallsForwardStateButNotDraws) {
  Harness h;
  TriangleScene scene(h.memory, 320, 240);
  const hooks::HookEntry* set_rt = hooks::Find(0x8210C378);
  const hooks::HookEntry* set_vp = hooks::Find(0x8210BAC8);
  const hooks::HookEntry* present = hooks::Find(0x821147B8);
  const hooks::HookEntry* resolve = hooks::Find(0x82116178);
  hooks::GuestArgs rt_args, vp_args, none, resolve_args;
  rt_args.r[1] = 0;
  rt_args.r[2] = scene.backbuffer;
  vp_args.r[1] = scene.viewport;
  resolve_args.r[1] = 0x200;
  resolve_args.r[3] = 0x4D000080;
  // SetRenderTarget's original applies the viewport through SetViewport:
  // that nested state call is forwarded.
  bool ran_original = false;
  hooks::Run(*set_rt, h.api(), rt_args, [&] {
    hooks::Run(*set_vp, h.api(), vp_args, [] {});
    ran_original = true;
  });
  CHECK(ran_original);
  CHECK_EQ(h.state.render_target(0), scene.backbuffer);
  CHECK(h.state.viewport() == (Viewport{0, 0, 320, 240, 0.0f, 1.0f}));
  // Present's original resolves the frontbuffer: that nested resolve is not
  // forwarded (the renderer would see a resolve the engine never asked for).
  hooks::Run(*present, h.api(), none, [&] { hooks::Run(*resolve, h.api(), resolve_args, [] {}); });
  CHECK_EQ(h.sink.order, std::string("P"));
  // The same resolve from the engine is forwarded.
  hooks::Run(*resolve, h.api(), resolve_args, [] {});
  CHECK_EQ(h.sink.order, std::string("PR"));
  if (!h.sink.resolves.empty()) {
    CHECK_EQ(h.sink.resolves[0].flags, 0x200u);
    CHECK_EQ(h.sink.resolves[0].dest_texture, 0x4D000080u);
  }
}

TEST(InactivePluginOnlyRunsOriginals) {
  Harness h;
  h.active = false;
  TriangleScene scene(h.memory, 320, 240);
  CHECK_EQ(h.api()->is_active(h.api()->self), 0);
  int originals = 0;
  for (const auto& e : hooks::Table()) {
    hooks::GuestArgs a;
    hooks::Run(e, h.api(), a, [&] { ++originals; });
  }
  CHECK_EQ(originals, int(hooks::Table().size()));
  CHECK_EQ(h.sink.order, std::string(""));
  CHECK_EQ(h.state.stats().draws, uint64_t(0));
  // No plugin at all (the Xenos path): the same.
  originals = 0;
  for (const auto& e : hooks::Table()) {
    hooks::Run(e, nullptr, hooks::GuestArgs{}, [&] { ++originals; });
  }
  CHECK_EQ(originals, int(hooks::Table().size()));
}

TEST(ApiVersionCheck) {
  Harness h;
  CHECK(hooks::IsCompatible(h.api()));
  NrApi old = h.binding.api;
  old.version = 1;
  CHECK(!hooks::IsCompatible(&old));
  CHECK(!hooks::IsCompatible(nullptr));
}

TEST(DrawsWithoutBuffersAreDropped) {
  Harness h;
  h.Call(0x82115708, {4, 0, 0, 3});  // nothing bound
  h.Call(0x821154C8, {4, 0, 3});
  CHECK_EQ(h.state.stats().draws, uint64_t(2));
  CHECK_EQ(h.state.stats().dropped_draws, uint64_t(2));
  CHECK(h.sink.draws.empty());
  // An index range past the end of the index buffer.
  TriangleScene scene(h.memory, 320, 240);
  h.Call(0x8210BE38, {scene.ib});
  h.Call(0x8210BD38, {0, scene.vb, 0, TriangleScene::kStride});
  h.Call(0x82115708, {4, 0, 5, 3});
  CHECK_EQ(h.state.stats().dropped_draws, uint64_t(3));
  // An unmapped vertex buffer object.
  h.Call(0x8210BD38, {0, 0x12345678, 0, 16});
  h.Call(0x821154C8, {4, 0, 3});
  CHECK_EQ(h.state.stats().dropped_draws, uint64_t(4));
  CHECK(h.sink.draws.empty());
}

TEST(NonIndexedDrawAndStreamOffset) {
  Harness h;
  TriangleScene scene(h.memory, 320, 240);
  h.Call(0x8210BD38, {0, scene.vb, TriangleScene::kStride, TriangleScene::kStride});  // offset = 1 vertex
  h.Call(0x821154C8, {13, 0, 4});  // quad list, 4 vertices
  CHECK_EQ(h.sink.draws.size(), size_t(1));
  if (!h.sink.draws.empty()) {
    const DrawCall& d = h.sink.draws[0];
    CHECK(!d.indexed);
    CHECK(d.primitive == GuestPrimitive::kQuadList);
    CHECK(d.vertex_data == h.memory.Virtual(scene.vb_data) + TriangleScene::kStride);
    CHECK_EQ(d.vertex_data_size, TriangleScene::kStride * 4);
    CHECK(!d.position_from_declaration);  // no declaration set
    CHECK_EQ(d.position_offset, 0u);
    CHECK(h.sink.draw_indices[0] == (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
  }
}

// ------------------------------------------------------------ primitives ---

TEST(PrimitiveExpansion) {
  std::vector<uint32_t> out;
  auto run = [&](GuestPrimitive p, uint32_t start, uint32_t count) {
    PrimitiveInput in;
    in.primitive = p;
    in.start = start;
    in.count = count;
    return BuildTriangleList(in, out);
  };
  CHECK(run(GuestPrimitive::kTriangleList, 0, 7));
  CHECK(out == (std::vector<uint32_t>{0, 1, 2, 3, 4, 5}));  // the partial triangle is dropped
  CHECK(run(GuestPrimitive::kTriangleStrip, 10, 5));
  CHECK(out == (std::vector<uint32_t>{10, 11, 12, 12, 11, 13, 12, 13, 14}));
  CHECK(run(GuestPrimitive::kTriangleFan, 0, 5));
  CHECK(out == (std::vector<uint32_t>{0, 1, 2, 0, 2, 3, 0, 3, 4}));
  CHECK(run(GuestPrimitive::kQuadList, 4, 8));
  CHECK(out == (std::vector<uint32_t>{4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11}));
  CHECK(!run(GuestPrimitive::kLineList, 0, 4));
  CHECK(!run(GuestPrimitive::kPointList, 0, 4));
  CHECK(!run(GuestPrimitive::kRectList, 0, 3));
  CHECK(run(GuestPrimitive::kTriangleList, 0, 2));
  CHECK(out.empty());
}

TEST(IndexedExpansionWithResetAndBaseVertex) {
  FakeGuestMemory m;
  // 16-bit strip with a reset in the middle: 0 1 2 3 | 4 5 6
  uint32_t ib16 = m.NewIndexBuffer({0, 1, 2, 3, 0xFFFF, 4, 5, 6}, false);
  IndexBufferInfo info;
  CHECK(DecodeIndexBuffer(m.Virtual(ib16), info));
  CHECK(!info.index32);
  PrimitiveInput in;
  in.primitive = GuestPrimitive::kTriangleStrip;
  in.index_data = m.Physical(info.physical);
  in.count = 8;
  in.base_vertex = 100;
  std::vector<uint32_t> out;
  CHECK(BuildTriangleList(in, out));
  CHECK(out == (std::vector<uint32_t>{100, 101, 102, 102, 101, 103, 104, 105, 106}));
  // 32-bit list, negative base vertex, start index.
  uint32_t ib32 = m.NewIndexBuffer({9, 9, 10, 11, 12}, true);
  CHECK(DecodeIndexBuffer(m.Virtual(ib32), info));
  CHECK(info.index32);
  CHECK_EQ(info.size_bytes, 20u);
  in.primitive = GuestPrimitive::kTriangleList;
  in.index_data = m.Physical(info.physical);
  in.index32 = true;
  in.start = 2;
  in.count = 3;
  in.base_vertex = -10;
  CHECK(BuildTriangleList(in, out));
  CHECK(out == (std::vector<uint32_t>{0, 1, 2}));
}

// ------------------------------------------------------- guest decoders ---

TEST(GuestObjectDecoders) {
  FakeGuestMemory m;
  uint32_t data = 0;
  uint32_t vb = m.NewVertexBuffer(96, data);
  VertexBufferInfo vbi;
  CHECK(DecodeVertexBuffer(m.Virtual(vb), vbi));
  CHECK_EQ(vbi.physical, data - FakeGuestMemory::kPhysicalView);
  CHECK_EQ(vbi.size_bytes, 96u);
  CHECK_EQ(vbi.endian, 2u);
  // Not a vertex fetch constant (type bits != 3).
  m.Write32(vb + 12, 0x1000);
  CHECK(!DecodeVertexBuffer(m.Virtual(vb), vbi));

  uint32_t decl = m.NewDeclaration({{1, 0, kDeclFloat3, kUsagePosition, 0},
                                    {0, 12, kDeclFloat3, kUsagePosition, 1},
                                    {0, 24, kDeclFloat3, kUsagePosition, 0}});
  DeclElement el;
  CHECK(FindPositionElement(m.Virtual(decl), 0, el));
  CHECK_EQ(el.offset, 24);
  CHECK_EQ(DeclTypeFormat(el.type), 57u);
  CHECK(FindPositionElement(m.Virtual(decl), 1, el));
  CHECK_EQ(el.offset, 0);
  CHECK(!FindPositionElement(m.Virtual(decl), 2, el));

  uint32_t vp = m.NewViewport(8, 16, 640, 360, 0.25f, 0.75f);
  Viewport v = DecodeViewport(m.Virtual(vp));
  CHECK(v == (Viewport{8, 16, 640, 360, 0.25f, 0.75f}));

  CHECK_EQ(CpuToPhysical(0xA0001000), 0x00001000u);
  CHECK_EQ(CpuToPhysical(0xE0001000), 0x00002000u);  // the 0xE0000000 view is 4 KB ahead
  CHECK_EQ(CpuToPhysical(0x40001000), 0x00001000u);
}

NR_TEST_MAIN()
