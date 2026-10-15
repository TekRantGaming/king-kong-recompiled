// The screen-space global illumination through the whole game renderer (docs/gi.md), on the
// real GPU or lavapipe: a frame drawn the way the game draws one, through the hooks, the tracker,
// the Renderer and the GameRenderer, with real translated shaders. The scene: a white floor in the
// shade and a red wall standing on it in the sun, drawn as a depth pre-pass and then as the opaque
// colour pass (depth writes off), then an alpha-blended quad over the floor by the wall (water, glass,
// smoke: anything drawn after the opaque pass).
//
// What is checked:
//  - the red of the wall bleeds onto the floor beside it, and not onto the floor far away;
//  - the blended quad drawn after the opaque pass is not touched: it is half its colour plus half the
//    lit scene behind it;
//  - the flat ambient light is read from the lighting shader's g_vAmbientColor constant;
//  - with the ambient occlusion on (GTAO at the opaque moment) the GI adds light only;
//  - without the AO, where the wall hides the floor's sky the flat ambient light is taken away
//    (the visibility), and the debug view shows the bounce light;
//  - render scale 2 matches scale 1 after an area average;
//  - --native_gi=off is the frame without any of it, to the bit;
//  - the Khronos and NVRHI validation layers stay silent.
//
// The D3D library helper is a copy of game_ao_tests.cpp's.
// NR_TEST_IMAGES=<dir> writes the frames there as PPM files.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "backend/api_binding.h"
#include "backend/draw_state.h"
#include "backend/game_renderer.h"
#include "backend/gi_config.h"
#include "backend/guest_device.h"
#include "backend/log.h"
#include "backend/render_scale.h"
#include "backend/renderer.h"
#include "backend/shader_library.h"
#include "backend/tests/check.h"
#include "backend/tests/fake_guest.h"
#include "backend/tests/vk_test_device.h"
#include "container_writer.h"
#include "hooks/hook_table.h"
#include "kknr/guest_texture.h"
#include "kkshaders/container.h"
#include "xenos_asm.h"

#ifndef NR_TEST_DXC_DIR
#define NR_TEST_DXC_DIR ""
#endif

using namespace nr;
using namespace nr::test;
using namespace xasm;

namespace {

VulkanTestDevice* Device() {
  static std::unique_ptr<VulkanTestDevice> device = [] {
    auto d = VulkanTestDevice::Create(true);
    if (d) {
      std::printf("  device: %s, Khronos validation %s\n", d->device_name().c_str(),
                  d->khronos_validation() ? "on" : "not installed");
    }
    return d;
  }();
  return device.get();
}

void MaybeSave(const Pixels& p, const std::string& name) {
  if (const char* dir = std::getenv("NR_TEST_IMAGES")) WritePpm(std::string(dir) + "/game_gi_" + name + ".ppm", p);
}

// ------------------------------------------------------------- shaders ---

enum Usage : uint32_t { kPosition = 0, kTexcoord = 5, kColor = 10 };

Op A(const Alu& a) { return {a.encode(), false}; }
Op F(const VFetch& v) { return {v.encode(), true}; }
Op Export(uint32_t reg, uint32_t src, const char* m = "xyzw") {
  return A(Alu().v(MAXv, reg, m, r(src), r(src)).exp(reg));
}

std::vector<uint32_t> FindVfetchAddresses(const std::vector<uint32_t>& ucode) {
  std::vector<uint32_t> addresses;
  uint32_t cf_end = uint32_t(ucode.size() / 3);
  for (uint32_t t = 0; t < cf_end; t++) {
    uint64_t a = ucode[t * 3] | (uint64_t(ucode[t * 3 + 1] & 0xFFFF) << 32);
    uint64_t b = (ucode[t * 3 + 1] >> 16) | (uint64_t(ucode[t * 3 + 2]) << 16);
    for (uint64_t cf : {a, b}) {
      uint32_t op = uint32_t(cf >> 44) & 0xF;
      bool is_exec = (op >= 1 && op <= 6) || op == 13 || op == 14;
      if (!is_exec) continue;
      uint32_t address = uint32_t(cf & 0xFFF), count = uint32_t(cf >> 12) & 7, sequence = uint32_t(cf >> 16) & 0xFFF;
      if (count) cf_end = std::min(cf_end, address);
      for (uint32_t i = 0; i < count; i++) {
        if (!((sequence >> (i * 2)) & 1)) continue;
        if ((ucode[(address + i) * 3] & 0x1F) == 0) addresses.push_back(address + i);
      }
    }
  }
  return addresses;
}

// The scene's vertex shader: the position (FLOAT3) times the world-view-projection matrix in
// c0..c3 (dp4 per row, as the game's static world does), the colour exported as TEXCOORD0.
std::vector<uint8_t> CameraVertexShader() {
  VFetch pos;
  pos.dst = 1;
  pos.fetchConstant = 95;
  pos.format = F_32_32_32_FLOAT;
  pos.stride = 4;
  VFetch color = pos;
  color.dst = 2;
  color.format = F_8_8_8_8;
  color.offset = 3;
  Program p;
  p.exec({F(pos), F(color), A(Alu().v(DP4v, 3, "x", r(1), c(0))), A(Alu().v(DP4v, 3, "y", r(1), c(1))),
          A(Alu().v(DP4v, 3, "z", r(1), c(2))), A(Alu().v(DP4v, 3, "w", r(1), c(3)))});
  p.alloc(1, 0);
  p.exec({Export(62, 3)});
  p.alloc(2, 0);
  p.exec({Export(0, 2)}, true);
  ctest::Spec spec;
  spec.vertex = true;
  spec.ucode = p.assemble();
  spec.constants.push_back({"g_WorldViewProj", 2, 0, 4});
  spec.interpolators = {{kTexcoord, 0, 0}};
  const std::vector<uint32_t> fetches = FindVfetchAddresses(spec.ucode);
  const uint32_t usages[2] = {kPosition, kColor};
  for (size_t i = 0; i < fetches.size() && i < 2; ++i) spec.fetches.push_back({fetches[i], usages[i], 0, 0});
  return ctest::writeContainer(spec);
}

ctest::Spec PixelSpec() {
  ctest::Spec spec;
  spec.vertex = false;
  spec.interpolators = {{kTexcoord, 0, 0}};
  return spec;
}

// oC0 = the interpolated colour (the scene's lit surfaces: albedo times the light, baked into the
// vertices). Its constant table names g_vAmbientColor at c0, as the game's lighting shaders do: the GI
// reads the frame's flat ambient light from there.
std::vector<uint8_t> PixelShaderLit() {
  Program p;
  p.alloc(2, 0);
  p.exec({Export(0, 0)}, true);
  ctest::Spec spec = PixelSpec();
  spec.ucode = p.assemble();
  spec.constants.push_back({"g_vAmbientColor", 2, 0, 1});
  return ctest::writeContainer(spec);
}

// oC0 = c<reg> (the blended quad).
std::vector<uint8_t> PixelShaderConstant(uint32_t reg) {
  Program p;
  p.exec({A(Alu().v(MAXv, 0, "xyzw", c(reg), c(reg)))});
  p.alloc(2, 0);
  p.exec({Export(0, 0)}, true);
  ctest::Spec spec = PixelSpec();
  spec.ucode = p.assemble();
  spec.constants.push_back({"g_Color", 2, uint16_t(reg), 1});
  return ctest::writeContainer(spec);
}

void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}
void PutBEFloat(std::vector<uint8_t>& out, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  PutBE32(out, u);
}

// ------------------------------------------------------------- the scene ---

struct V3 {
  float x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }

// The picture: 192 x 108 guest pixels.
constexpr uint32_t kW = 192, kH = 108;

// D3D's left-handed look-at and perspective (depth 0..1), as rows: clip = rows . position.
struct Camera {
  V3 eye{0.4f, 1.5f, -1.0f}, target{0.0f, 0.2f, 2.0f};
  float px = 0, py = 0, a = 0, b = 0;
  V3 r, u, f;
  float rows[16] = {};
  Camera() { Setup(); }
  // Again after moving the eye.
  void Setup() {
    f = Normalize(target - eye);
    r = Normalize(Cross({0, 1, 0}, f));
    u = Cross(f, r);
    py = 1.0f / std::tan(1.0472f / 2);
    px = py * float(kH) / float(kW);
    const float zn = 0.1f, zf = 100.0f;
    a = zf / (zf - zn);
    b = -zn * zf / (zf - zn);
    const V3 axes[3] = {r, u, f};
    float view[12];
    for (int i = 0; i < 3; ++i) {
      view[i * 4 + 0] = axes[i].x;
      view[i * 4 + 1] = axes[i].y;
      view[i * 4 + 2] = axes[i].z;
      view[i * 4 + 3] = -Dot(axes[i], eye);
    }
    for (int c = 0; c < 4; ++c) {
      rows[0 * 4 + c] = px * view[0 * 4 + c];
      rows[1 * 4 + c] = py * view[1 * 4 + c];
      rows[2 * 4 + c] = a * view[2 * 4 + c] + (c == 3 ? b : 0.0f);
      rows[3 * 4 + c] = view[2 * 4 + c];
    }
  }
  // The guest pixel a world point lands on (the D3D viewport over the picture, no half-pixel
  // offset in these tests: PA_SU_VTX_CNTL pix_center 1).
  void ToPixel(V3 p, int& x, int& y) const {
    const V3 v = p - eye;
    const float vz = Dot(v, f);
    const float nx = px * Dot(v, r) / vz, ny = py * Dot(v, u) / vz;
    x = int(std::floor((nx + 1.0f) * 0.5f * float(kW)));
    y = int(std::floor((1.0f - ny) * 0.5f * float(kH)));
  }
};

struct Vertex {
  V3 p;
  uint32_t argb;
};
using Mesh = std::vector<Vertex>;

// Two triangles over four corners.
void Quad(Mesh& m, V3 a, V3 b, V3 c, V3 d, uint32_t argb) {
  for (V3 v : {a, b, c, a, c, d}) m.push_back({v, argb});
}
// An axis-aligned box's six faces.
void Box(Mesh& m, V3 lo, V3 hi, uint32_t argb) {
  const V3 p[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                   {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
  Quad(m, p[0], p[1], p[2], p[3], argb);
  Quad(m, p[5], p[4], p[7], p[6], argb);
  Quad(m, p[4], p[0], p[3], p[7], argb);
  Quad(m, p[1], p[5], p[6], p[2], argb);
  Quad(m, p[3], p[2], p[6], p[7], argb);
  Quad(m, p[4], p[5], p[1], p[0], argb);
}
// The flat ambient light (the lighting shader's g_vAmbientColor) and the surfaces: a white floor
// (albedo 0.8) in the shade, so its colour is 0.8 times the ambient light; a red wall in the sun.
constexpr float kAmbient[4] = {0.3f, 0.3f, 0.3f, 0.0f};
constexpr uint32_t kFloor = 0xFF3D3D3D, kRedWall = 0xFFD91F14, kSky = 0xFF406090;
// The blended quad's colour and alpha.
constexpr float kQuad[4] = {0.10f, 0.35f, 0.55f, 0.5f};

Mesh SceneMesh() {
  Mesh m;
  Quad(m, {-6, 0, -6}, {6, 0, -6}, {6, 0, 2}, {-6, 0, 2}, kFloor);
  Quad(m, {-1.5f, 0, 2}, {1.5f, 0, 2}, {1.5f, 2, 2}, {-1.5f, 2, 2}, kRedWall);
  return m;
}
// The blended quad: horizontal, just over the floor by the wall.
Mesh QuadMesh() {
  Mesh m;
  Quad(m, {0.1f, 0.05f, 1.45f}, {0.9f, 0.05f, 1.45f}, {0.9f, 0.05f, 1.9f}, {0.1f, 0.05f, 1.9f}, 0xFFFFFFFF);
  return m;
}

std::vector<uint8_t> Bytes(const Mesh& m) {
  std::vector<uint8_t> out;
  for (const Vertex& v : m) {
    PutBEFloat(out, v.p.x);
    PutBEFloat(out, v.p.y);
    PutBEFloat(out, v.p.z);
    PutBE32(out, v.argb);
  }
  return out;
}

// Probes (world): the floor beside the wall's foot, the floor far from it (beyond the tests' GI radius),
// the middle of the wall, and the blended quad (over the floor by the wall).
const V3 kNearFloor{-0.6f, 0.0f, 1.75f}, kFarFloor{-0.6f, 0.0f, 0.5f}, kWallMiddle{-0.6f, 1.2f, 2.0f},
    kQuadCentre{0.5f, 0.05f, 1.75f};

// ------------------------------------------------------------- fixture ---

class Sink final : public DrawSink {
 public:
  Renderer* renderer = nullptr;
  ShaderLibrary* shaders = nullptr;
  void OnClear(const ClearCall& call) override { renderer->OnClear(call); }
  void OnDraw(const DrawCall& call) override { renderer->OnDraw(call); }
  void OnResolve(const ResolveCall& call) override { renderer->OnResolve(call); }
  void OnPresent(uint32_t frame, uint32_t rt0, uint32_t device) override { renderer->OnPresent(frame, rt0, device); }
  void OnShaderCreated(uint32_t kind, uint32_t container, uint32_t object) override {
    shaders->OnCreated(kind, container, object);
  }
};

constexpr uint32_t kBlendOpaque = 0x00010001;    // ONE, ZERO, ADD
constexpr uint32_t kBlendAlpha = 0x00010706;     // SRCALPHA, INVSRCALPHA (alpha ONE, ZERO)
constexpr uint32_t kBlendAdditive = 0x00010101;  // ONE, ONE
constexpr uint32_t kDepthPrepass = 0x2 | 0x4 | (3 << 4);  // test and write, LESS_EQUAL
constexpr uint32_t kDepthColour = 0x2 | (3 << 4);         // test, no write, LESS_EQUAL
constexpr uint32_t kClearTarget0 = 0x1, kClearZ = 0x10, kClearStencil = 0x20;

struct Options {
  AoConfig ao;
  GiConfig gi;
  int mode = 1;      // ao_mode
  int strength = 1;  // ao_strength
  bool settings = true;  // false: no ao_settings at all (the plugin without the Xenos plugin's cvars)
  float scale = 1.0f;
  std::function<EffectSettings()> live;  // live settings (effect_settings.h), read at each frame's first call
};

struct Fixture {
  VulkanTestDevice* gpu = Device();
  FakeGuestMemory mem{8u << 20, 16u << 20};
  std::unique_ptr<ShaderLibrary> shaders;
  std::unique_ptr<Renderer> renderer;
  GameRenderer* game = nullptr;
  Sink sink;
  std::unique_ptr<DrawTracker> tracker;
  ApiBinding binding;
  uint32_t device = 0;
  int errors_before = 0;
  Options opt;
  Camera cam;
  uint32_t rt = 0, ds = 0;
  uint32_t vs = 0, ps_lit = 0, ps_quad = 0, decl = 0;

  explicit Fixture(Options o) : opt(std::move(o)) {}

  bool Init() {
    if (!gpu) {
      Fail(__FILE__, __LINE__, "no Vulkan device (install mesa-vulkan-drivers for lavapipe)");
      return false;
    }
    errors_before = gpu->errors();
    shaders = std::make_unique<ShaderLibrary>(gpu->device(), mem);
    shaders->SetRenderScaleAware(opt.scale != 1.0f || bool(opt.live));
    shaders->Initialize("", NR_TEST_DXC_DIR);
    renderer = std::make_unique<Renderer>(gpu->device());
    if (!renderer->Initialize(ScaledSize(kW, opt.scale), ScaledSize(kH, opt.scale))) {
      Fail(__FILE__, __LINE__, "Renderer::Initialize failed");
      return false;
    }
    renderer->set_submit([this](nvrhi::ICommandList* cl) { gpu->Execute(cl); });
    auto g = std::make_unique<GameRenderer>(
        gpu->device(), mem, kknr::GuestMemory{mem.PhysicalBase(), mem.PhysicalSize(), 0}, shaders.get());
    g->options().render_scale_x = g->options().render_scale_y = opt.scale;
    g->options().guest_frame_width = kW;
    g->options().ao = opt.ao;  // before Initialize, as the backend does
    g->options().gi = opt.gi;
    g->options().effect_settings = opt.live;
    if (!g->Initialize()) {
      Fail(__FILE__, __LINE__, "GameRenderer::Initialize failed");
      return false;
    }
    shaders->SetAmbientOcclusion(g->ao_ambient_table());
    g->options().async_pipelines = false;  // the tests read the first frame back
    if (opt.settings) {
      const int mode = opt.mode, strength = opt.strength;
      g->options().ao_settings = [mode, strength] {
        AoSettings s;
        s.mode = mode;
        s.strength = strength;
        return s;
      };
    }
    game = g.get();
    renderer->EnableGame(std::move(g));
    sink.renderer = renderer.get();
    sink.shaders = shaders.get();
    tracker = std::make_unique<DrawTracker>(mem, &sink);
    InitApiBinding(binding, tracker.get(), [](void* t) { return static_cast<DrawTracker*>(t); });

    device = mem.AllocHeap(dev::kSize, 64);
    mem.AllocPhysical(4096);
    Reg(dev::kRbColorMask, 0xFFFF);
    Reg(dev::kRbBlendControl0, kBlendOpaque);
    for (uint32_t i = 0; i < 3; ++i) Reg(dev::kRbBlendControl1 + 4 * i, kBlendOpaque);
    Reg(dev::kPaClVteCntl, 0x43F);
    Reg(dev::kPaSuVtxCntl, 1);
    Reg(dev::kRbStencilRefMask, 0xFFFF00);

    vs = CreateShader(true, CameraVertexShader());
    ps_lit = CreateShader(false, PixelShaderLit());
    ps_quad = CreateShader(false, PixelShaderConstant(1));
    decl = mem.NewDeclaration({{0, 0, kDeclFloat3, kUsagePosition, 0}, {0, 12, kDeclColor, kUsageColor, 0}});
    rt = NewSurface(kW, kH, 0, 0);
    ds = NewSurface(kW, kH, 600, 0);
    return true;
  }

  ~Fixture() {
    if (!gpu) return;
    gpu->WaitIdle();
    renderer.reset();
    shaders.reset();
    gpu->WaitIdle();
    CHECK_EQ(gpu->errors(), errors_before);  // the validation layers stayed silent
  }

  // The hashes of the scene's shaders (for lists and rules).
  uint64_t Hash(bool vertex, uint32_t object) {
    auto s = shaders->Get(vertex ? kkshaders::ShaderKind::Vertex : kkshaders::ShaderKind::Pixel, object);
    return s ? s->ucode_hash : 0;
  }

  void W32(uint32_t va, uint32_t v) { mem.Write32(va, v); }
  uint32_t R32(uint32_t va) { return LoadBE32(mem.Virtual(va)); }
  void Reg(uint32_t offset, uint32_t value) { W32(device + offset, value); }
  void RegF(uint32_t offset, float value) { mem.WriteFloat(device + offset, value); }
  uint32_t NewBytes(const std::vector<uint8_t>& bytes) {
    const uint32_t a = mem.AllocHeap(uint32_t(bytes.size()));
    std::memcpy(mem.Writable(a), bytes.data(), bytes.size());
    return a;
  }
  uint32_t NewVertexBuffer(const std::vector<uint8_t>& bytes) {
    uint32_t data = 0;
    const uint32_t vb = mem.NewVertexBuffer(uint32_t((bytes.size() + 3) & ~size_t(3)), data);
    std::memcpy(mem.Writable(data), bytes.data(), bytes.size());
    return vb;
  }
  uint32_t NewSurface(uint32_t width, uint32_t height, uint32_t edram_base, uint32_t format) {
    const uint32_t s = mem.AllocHeap(64);
    W32(s + 16 + 8, (width - 1) | ((height - 1) << 13));
    W32(s + 48, width);
    W32(s + 52, edram_base | (format << 16));
    return s;
  }
  // A 2D texture object over new physical memory (the resolve's destination).
  uint32_t NewTexture(kknr::TextureFormat format, uint32_t width, uint32_t height) {
    const uint32_t base = mem.AllocPhysical(width * height * 4 + 65536);
    kknr::TextureFetchDesc desc;
    desc.format = format;
    desc.endian = kknr::Endian::k8in32;
    desc.width = width;
    desc.height = height;
    desc.swizzle = kknr::MakeSwizzle(kknr::kSwzX, kknr::kSwzY, kknr::kSwzZ, kknr::kSwzW);
    desc.base_address = base - FakeGuestMemory::kPhysicalView;
    const kknr::TextureFetch fetch = kknr::MakeTextureFetch(desc);
    const uint32_t object = mem.AllocHeap(40);
    W32(object, (3u << 16) | 1);
    for (uint32_t i = 0; i < 6; ++i) W32(object + 16 + 4 * i, fetch.words[i]);
    W32(object + 16 + 4, (fetch.words[1] & 0xFFFu) | base);
    return object;
  }

  void Call(uint32_t address, std::initializer_list<uint32_t> regs, double f1 = 0.0, uint32_t ret = 0) {
    const hooks::HookEntry* entry = hooks::Find(address);
    if (!entry) {
      Fail(__FILE__, __LINE__, "no hook entry");
      return;
    }
    hooks::GuestArgs args;
    size_t i = 0;
    for (uint32_t v : regs) args.r[i++] = v;
    args.f1 = f1;
    hooks::Run(*entry, &binding.api, args, [ret] { return ret; });
  }
  uint32_t ShaderObject(bool vertex, const std::vector<uint8_t>& container) {
    const uint32_t virtual_size = LoadBE32(container.data() + 4);
    const uint32_t physical_size = LoadBE32(container.data() + 8);
    const uint32_t header = vertex ? 592 : 52;
    const uint32_t object = mem.AllocHeap(header + virtual_size);
    std::memcpy(mem.Writable(object + header), container.data(), virtual_size);
    const uint32_t ucode = mem.AllocPhysical(physical_size, 256);
    std::memcpy(mem.Writable(ucode), container.data() + virtual_size, physical_size);
    W32(object + (vertex ? 40 : 12), ucode);
    return object;
  }
  uint32_t CreateShader(bool vertex, const std::vector<uint8_t>& container) {
    const uint32_t guest_container = NewBytes(container);
    const uint32_t object = ShaderObject(vertex, container);
    Call(vertex ? 0x82111D90 : 0x82111CA0, {guest_container}, 0.0, object);
    return object;
  }
  void SetRenderTarget(uint32_t index, uint32_t surface) {
    W32(device + dev::kRenderTargets + 4 * index, surface);
    if (surface) {
      Reg(index == 0 ? dev::kRbColorInfo : dev::kRbColor1Info + 4 * (index - 1), R32(surface + 52));
      if (index == 0) Reg(dev::kRbSurfaceInfo, R32(surface + 48));
    }
    Call(0x8210C378, {device, index, surface});
  }
  void SetDepthStencil(uint32_t surface) {
    W32(device + dev::kDepthStencil, surface);
    if (surface) Reg(dev::kRbDepthInfo, R32(surface + 52));
    Call(0x8210C6E0, {device, surface});
  }
  void SetViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const uint32_t vp = mem.NewViewport(x, y, w, h, 0.0f, 1.0f);
    for (uint32_t i = 0; i < 4; ++i) Reg(dev::kViewport + 4 * i, R32(vp + 4 * i));
    RegF(dev::kViewport + 16, 0.0f);
    RegF(dev::kViewport + 20, 1.0f);
    RegF(dev::kPaClVport + 0, float(w) / 2);
    RegF(dev::kPaClVport + 4, float(x) + float(w) / 2);
    RegF(dev::kPaClVport + 8, -float(h) / 2);
    RegF(dev::kPaClVport + 12, float(y) + float(h) / 2);
    RegF(dev::kPaClVport + 16, 1.0f);
    RegF(dev::kPaClVport + 20, 0.0f);
    Reg(dev::kPaScWindowScissorTl, 0x80000000u | x | (y << 16));
    Reg(dev::kPaScWindowScissorBr, (x + w) | ((y + h) << 16));
    Call(0x8210BAC8, {device, vp});
  }
  void SetStream(uint32_t vb, uint32_t stride) {
    Reg(dev::kStreamOffset, 0);
    Reg(dev::kStreamBuffer, vb);
    mem.Writable(device + 12688)[0] = uint8_t(stride / 4);
    Call(0x8210BD38, {device, 0, vb, 0, stride});
  }
  void SetShaders(uint32_t v, uint32_t p) {
    Call(0x82110C28, {device, v});
    Call(0x821108B8, {device, p});
  }
  void SetConstants(bool vertex, uint32_t reg, const float* values, uint32_t count) {
    const uint32_t data = mem.AllocHeap(count * 16);
    for (uint32_t i = 0; i < count * 4; ++i) mem.WriteFloat(data + i * 4, values[i]);
    const uint32_t base = vertex ? dev::kVsConstants : dev::kPsConstants;
    for (uint32_t i = 0; i < count * 4; ++i) RegF(base + reg * 16 + 4 * i, values[i]);
    Call(vertex ? 0x82110300 : 0x82110448, {device, reg, data, count});
  }
  void SetTexture(uint32_t slot, uint32_t texture) {
    Reg(dev::kTextures + 4 * slot, texture);
    for (uint32_t i = 0; i < 6; ++i) {
      uint32_t w = R32(texture + 16 + 4 * i);
      if ((i == 1 || i == 5) && (w & 0xFFFFF000u)) w = (w & 0xFFFu) | (CpuToPhysical(w & 0xFFFFF000u) & 0xFFFFF000u);
      Reg(dev::kFetchConstants + 24 * slot + 4 * i, w);
    }
    Call(0x82118F78, {device, slot, texture});
  }
  void Clear(uint32_t flags, uint32_t argb, float z = 1.0f) {
    Call(0x82115418, {device, 0, 0, flags, argb, 0, 0}, z);
  }
  void Resolve(uint32_t flags, uint32_t dest, float clear_z = 1.0f) {
    Call(0x82116178, {device, flags, 0, dest, 0, 0, 0, 0}, clear_z);
  }
  Pixels Present(uint32_t back_buffer, const std::string& save_as) {
    Reg(dev::kBackBuffer, back_buffer);
    Call(0x821147B8, {device});
    gpu->WaitIdle();
    Pixels p;
    if (nvrhi::ITexture* image = renderer->presented_image()) p = gpu->ReadBack(image);
    MaybeSave(p, save_as);
    return p;
  }
  void DrawMesh(const Mesh& m, uint32_t ps, uint32_t blend, uint32_t depth_control, uint32_t color_mask = 0xFFFF) {
    Reg(dev::kRbBlendControl0, blend);
    Reg(dev::kRbDepthControl, depth_control);
    Reg(dev::kRbColorMask, color_mask);
    Call(0x82111E68, {device, decl});
    SetShaders(vs, ps);
    // One vertex buffer per mesh, kept across frames (as the game's static meshes are).
    const std::vector<uint8_t> bytes = Bytes(m);
    uint32_t& vb = vertex_buffers[bytes];
    if (!vb) vb = NewVertexBuffer(bytes);
    SetStream(vb, 16);
    Call(0x821154C8, {device, 4, 0, uint32_t(m.size())});
  }
  std::map<std::vector<uint8_t>, uint32_t> vertex_buffers;

  // The frame: the depth pre-pass, the opaque colour pass (the lighting shader with the ambient
  // light in its c0), then (optionally) the blended quad.
  struct Frame {
    bool prepass = true, quad = true;
    bool clear_scene_depth = false, quad_writes_depth = false;
    const Mesh* scene = nullptr;     // null: SceneMesh()
    const float* ambient = nullptr;  // null: kAmbient
  };
  void DrawFrame(const Frame& fr) {
    SetRenderTarget(0, rt);
    SetDepthStencil(ds);
    SetViewport(0, 0, kW, kH);
    SetConstants(true, 0, cam.rows, 4);
    Clear(kClearTarget0 | kClearZ | kClearStencil, kSky, 1.0f);
    const Mesh scene = fr.scene ? *fr.scene : SceneMesh();
    SetConstants(false, 0, fr.ambient ? fr.ambient : kAmbient, 1);
    if (fr.prepass) DrawMesh(scene, ps_lit, kBlendOpaque, kDepthPrepass, 0);
    DrawMesh(scene, ps_lit, kBlendOpaque, fr.prepass ? kDepthColour : kDepthPrepass);
    if (fr.clear_scene_depth) Clear(kClearZ, 0, 1.0f);
    if (fr.quad) {
      SetConstants(false, 1, kQuad, 1);
      DrawMesh(QuadMesh(), ps_quad, kBlendAlpha, fr.quad_writes_depth ? kDepthPrepass : kDepthColour);
    }
    Reg(dev::kRbBlendControl0, kBlendOpaque);
    Reg(dev::kRbColorMask, 0xFFFF);
  }
  Pixels Render(const Frame& fr, const std::string& name) {
    DrawFrame(fr);
    return Present(rt, name);
  }
};

// The mean of a channel over a 3x3 square of guest pixels (at any render scale: the scaled frame's
// pixels inside them).
// The mean of a channel over a 3x3 square of guest pixels (at any render scale: the scaled frame's
// pixels inside them).
float At(const Pixels& p, const Camera& cam, V3 world, int channel, float scale = 1.0f) {
  int x, y;
  cam.ToPixel(world, x, y);
  float s = 0;
  int n = 0;
  const int x0 = int(std::lround((x - 1) * scale)), x1 = int(std::lround((x + 2) * scale));
  const int y0 = int(std::lround((y - 1) * scale)), y1 = int(std::lround((y + 2) * scale));
  for (int j = y0; j < y1; ++j)
    for (int i = x0; i < x1; ++i, ++n)
      s += p.at(uint32_t(std::clamp(i, 0, int(p.width) - 1)), uint32_t(std::clamp(j, 0, int(p.height) - 1)))[channel];
  return s / float(n);
}

struct Rgb {
  float r, g, b;
};
Rgb Colour(const Pixels& p, const Camera& cam, V3 world, float scale = 1.0f) {
  return {At(p, cam, world, 0, scale), At(p, cam, world, 1, scale), At(p, cam, world, 2, scale)};
}

// The GI's radius in these tests: 1 world unit (the floor far from the wall stays out of reach).
GiConfig Gi(GiLevel level, float occlusion = 0.0f) {
  GiConfig c;
  c.level = level;
  c.occlusion = occlusion;
  c.radius = 1.0f;
  return c;
}

float MaxDifference(const Pixels& a, const Pixels& b) {
  float worst = 0;
  for (size_t i = 0; i < a.rgba.size() && i < b.rgba.size(); ++i)
    if (i % 4 != 3) worst = std::max(worst, std::fabs(a.rgba[i] - b.rgba[i]));
  return a.rgba.size() == b.rgba.size() ? worst : 1.0f;
}

float MeanDifference(const Pixels& a, const Pixels& b) {
  double sum = 0;
  size_t n = 0;
  for (size_t i = 0; i < a.rgba.size() && i < b.rgba.size(); ++i)
    if (i % 4 != 3) sum += std::fabs(a.rgba[i] - b.rgba[i]), ++n;
  return n ? float(sum / double(n)) : 1.0f;
}

constexpr float kTol = 2.5f / 255.0f;

}  // namespace

TEST(GameGi_RedWallBleedsOntoTheFloor) {
  // Light only (no occlusion of the ambient): the floor beside the red wall turns red, the floor far
  // away does not change, and neither does the sky.
  for (GiLevel level : {GiLevel::kLow, GiLevel::kMedium, GiLevel::kHigh}) {
    Options on, off;
    on.mode = off.mode = 0;  // ambient occlusion off
    on.gi = Gi(level);
    Fixture a(on), b(off);
    if (!a.Init() || !b.Init()) return;
    Fixture::Frame plain;
    plain.quad = false;
    const std::string name = GiLevelName(level);
    const Pixels with = a.Render(plain, "bleed_" + name), without = b.Render(plain, "bleed_off_" + name);
    const Camera& cam = a.cam;
    const Rgb n1 = Colour(with, cam, kNearFloor), n0 = Colour(without, cam, kNearFloor);
    const Rgb f1 = Colour(with, cam, kFarFloor), f0 = Colour(without, cam, kFarFloor);
    const Rgb w1 = Colour(with, cam, kWallMiddle), w0 = Colour(without, cam, kWallMiddle);
    std::printf("  %s: floor by the wall %.3f %.3f %.3f (was %.3f %.3f %.3f), far floor %+.4f, wall %+.3f %+.3f %+.3f\n",
                name.c_str(), n1.r, n1.g, n1.b, n0.r, n0.g, n0.b, f1.r - f0.r, w1.r - w0.r, w1.g - w0.g, w1.b - w0.b);
    // Red bleeds: the red channel rises much more than green and blue.
    CHECK(n1.r - n0.r > 0.02f);
    CHECK(n1.r - n0.r > 3.0f * std::max(n1.g - n0.g, n1.b - n0.b));
    CHECK(n1.g - n0.g > -kTol);  // light only: never darker
    // Far from the wall nothing changes.
    CHECK(std::fabs(f1.r - f0.r) < kTol && std::fabs(f1.g - f0.g) < kTol && std::fabs(f1.b - f0.b) < kTol);
    const SceneGi* gi = a.game->scene_gi();
    CHECK(gi != nullptr);
    if (gi) {
      // No transparent draw this frame: the light came at Present (the scene was still there).
      CHECK_EQ(gi->stats().applied_present, uint64_t(1));
      CHECK_EQ(gi->stats().no_camera, uint64_t(0));
      // The ambient light came from the lighting shader's constant.
      CHECK_EQ(gi->stats().ambient_frames, uint64_t(1));
      CHECK(std::fabs(gi->ambient()[0] - kAmbient[0]) < 1e-4f);
    }
    // The sky (the clear colour in the top corner): untouched.
    for (int c = 0; c < 3; ++c) CHECK(std::fabs(with.at(2, 2)[c] - without.at(2, 2)[c]) < 1e-6f);
  }
}

TEST(GameGi_TransparentDrawAfterIsUntouched) {
  // The blended quad comes after the opaque pass: the bounce light goes in before it, so the quad is
  // half its colour plus half the lit scene (with its bounce light) behind it.
  Options on;
  on.mode = 0;
  on.gi = Gi(GiLevel::kMedium);
  Options off = on;
  off.gi = GiConfig{};
  Fixture a(on), plain_on(on), b(off);
  if (!a.Init() || !plain_on.Init() || !b.Init()) return;
  const Pixels frame = a.Render({}, "quad_on");
  Fixture::Frame no_quad;
  no_quad.quad = false;
  const Pixels scene = plain_on.Render(no_quad, "quad_scene_on");
  const Pixels scene_off = b.Render(no_quad, "quad_scene_off");
  const Camera& cam = a.cam;
  float worst = 0, painted_gap = 0;
  for (int c = 0; c < 3; ++c) {
    const float q = At(frame, cam, kQuadCentre, c), s = At(scene, cam, kQuadCentre, c);
    worst = std::max(worst, std::fabs(q - (kQuad[3] * kQuad[c] + (1 - kQuad[3]) * s)));
    // Painting the bounce light over the finished picture would move the quad's pixels by (about)
    // the scene's change there again, at full strength instead of behind the quad's alpha.
    const float added = s - At(scene_off, cam, kQuadCentre, c);
    painted_gap = std::max(painted_gap, std::fabs(added) * kQuad[3]);
  }
  std::printf("  quad %.4f off half its colour plus half the lit scene; painting over would move it by %.4f\n", worst,
              painted_gap);
  CHECK(worst < kTol);
  CHECK(painted_gap > 0.004f);  // the scene behind the quad did get light: the test sees the difference
  const SceneGi* gi = a.game->scene_gi();
  if (gi) {
    CHECK_EQ(gi->stats().applied_opaque, uint64_t(1));
    CHECK_EQ(gi->stats().applied_present, uint64_t(0));
  }
}

TEST(GameGi_WithAoItOnlyAddsLight) {
  // GTAO at the opaque moment darkens the creases; the GI then adds the red bounce on top and takes
  // nothing away itself.
  Options ao_only, both;
  ao_only.ao.method = both.ao.method = AoMethod::kGtao;
  ao_only.ao.apply = both.ao.apply = AoApply::kOpaque;
  both.gi = Gi(GiLevel::kMedium, 1.0f);  // occlusion asked for, but the AO is on: light only
  Fixture a(both), b(ao_only);
  if (!a.Init() || !b.Init()) return;
  const Pixels with = a.Render({}, "ao_gi"), without = b.Render({}, "ao_only");
  const Rgb n1 = Colour(with, a.cam, kNearFloor), n0 = Colour(without, a.cam, kNearFloor);
  std::printf("  with GTAO: floor by the wall %.3f %.3f %.3f against the AO alone %.3f %.3f %.3f\n", n1.r, n1.g, n1.b,
              n0.r, n0.g, n0.b);
  CHECK(n1.r - n0.r > 0.02f);
  CHECK(n1.g - n0.g > -kTol && n1.b - n0.b > -kTol);
}

TEST(GameGi_VisibilityTakesTheHiddenAmbientAway) {
  // AO off and the GI's occlusion on: in the corner where the wall hides part of the floor's sky, the
  // flat ambient light is partly taken away (green and blue, which the red wall hardly sends, get
  // darker); the open floor far away keeps it.
  Options on, off;
  on.mode = off.mode = 0;
  on.gi = Gi(GiLevel::kMedium, 1.0f);
  Fixture a(on), b(off);
  if (!a.Init() || !b.Init()) return;
  Fixture::Frame plain;
  plain.quad = false;
  const Pixels with = a.Render(plain, "visibility"), without = b.Render(plain, "visibility_off");
  const Rgb n1 = Colour(with, a.cam, kNearFloor), n0 = Colour(without, a.cam, kNearFloor);
  const Rgb f1 = Colour(with, a.cam, kFarFloor), f0 = Colour(without, a.cam, kFarFloor);
  std::printf("  occlusion on: floor by the wall %.3f %.3f %.3f (was %.3f %.3f %.3f), far floor %+.4f\n", n1.r, n1.g,
              n1.b, n0.r, n0.g, n0.b, f1.g - f0.g);
  CHECK(n1.g < n0.g - 0.01f);
  CHECK(n1.r - n0.r > (n1.g - n0.g) + 0.02f);  // still redder than it was
  CHECK(std::fabs(f1.g - f0.g) < kTol);
  // The debug view: the bounce light alone, red by the wall, black far away.
  Options dbg = on;
  dbg.gi.debug = 1;
  Fixture d(dbg);
  if (!d.Init()) return;
  const Pixels bounce = d.Render(plain, "debug_bounce");
  const Rgb bn = Colour(bounce, d.cam, kNearFloor), bf = Colour(bounce, d.cam, kFarFloor);
  std::printf("  bounce light: by the wall %.3f %.3f %.3f, far %.3f %.3f %.3f\n", bn.r, bn.g, bn.b, bf.r, bf.g, bf.b);
  CHECK(bn.r > 0.05f && bn.r > 3.0f * bn.g);
  CHECK(bf.r < 0.03f);  // gamma encoded: a trace of light reads larger than it is
}

// ------------------------------------------------- a scene like the game's ---
//
// The game's frames are dark: its flat ambient light (g_vAmbientColor) reads about 0.08 in the
// chapters measured (docs/gi.md, "In the game"), surfaces in the shade are a few percent of white,
// and the light comes from the sun on a few surfaces. The yard: a dark floor in the shade, a sunlit
// orange wall at its end (4 m wide, 3 m high), a shaded dark crate on the floor in front of it and a
// shaded corner of two dark walls far to the side, seen from 10 m back and 4 m up. No radius given:
// each level's own.
constexpr float kYardAmbient[4] = {0.08f, 0.08f, 0.08f, 0.0f};
constexpr uint32_t kYardFloor = 0xFF0C0C0C;  // albedo 0.6 (gamma) times the ambient 0.08: 0.047
constexpr uint32_t kYardWall = 0xFFBF5A26;   // sunlit: 0.75 0.35 0.15
constexpr uint32_t kYardDark = 0xFF080808;   // shaded crate and corner: 0.031

Mesh YardMesh() {
  Mesh m;
  Quad(m, {-12, 0, -14}, {12, 0, -14}, {12, 0, 2}, {-12, 0, 2}, kYardFloor);
  Quad(m, {-2, 0, 2}, {2, 0, 2}, {2, 3, 2}, {-2, 3, 2}, kYardWall);
  Box(m, {-1.5f, 0, -3.0f}, {-0.5f, 1, -2.0f}, kYardDark);
  // The shaded corner, far from the sunlit wall: the GI may only darken it a little.
  Quad(m, {7, 0, -4}, {7, 0, 1}, {7, 2.5f, 1}, {7, 2.5f, -4}, kYardDark);
  Quad(m, {4, 0, 1}, {7, 0, 1}, {7, 2.5f, 1}, {4, 2.5f, 1}, kYardDark);
  return m;
}

void YardCamera(Camera& c) {
  c.eye = {0.0f, 4.0f, -10.0f};
  c.target = {0.5f, 0.5f, 2.0f};
  c.Setup();
}

float Luminance(const Rgb& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

// Over the picture's non-sky pixels (the sky keeps its clear colour, kSky): the mean luminance change
// and the largest darkening of any pixel.
void FrameChange(const Pixels& with, const Pixels& without, float& mean, float& darkest, float& brightest) {
  double sum = 0;
  size_t n = 0;
  darkest = 0;
  brightest = 0;
  for (uint32_t y = 0; y < without.height; ++y)
    for (uint32_t x = 0; x < without.width; ++x) {
      const float* a = without.at(x, y);
      const float* b = with.at(x, y);
      if (std::fabs(a[0] - 0x40 / 255.0f) < 1e-3f && std::fabs(a[2] - 0x90 / 255.0f) < 1e-3f) continue;  // sky
      const float la = 0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2];
      const float lb = 0.2126f * b[0] + 0.7152f * b[1] + 0.0722f * b[2];
      sum += lb - la;
      ++n;
      darkest = std::min(darkest, lb - la);
      brightest = std::max(brightest, lb - la);
    }
  mean = n ? float(sum / double(n)) : 0.0f;
}

TEST(GameGi_GameLikeDarkSceneBounceIsVisible) {
  // The default settings of each level (radius, intensity 1, occlusion 0) in the dark yard: the
  // sunlit wall lights the shaded floor in front of it with its own colour, visibly (several times
  // the floor's own light), less with the distance and not at all 7 m away; the picture as a whole
  // gets brighter, no pixel loses more than a trace (no black halos, no crushed shadows), the sunlit
  // wall itself barely changes (no glow), the shaded corner never darkens, and the
  // sky is untouched.
  const Mesh yard = YardMesh();
  Fixture::Frame fr;
  fr.quad = false;
  fr.scene = &yard;
  fr.ambient = kYardAmbient;
  const V3 floor_near{0.0f, 0.0f, 1.4f}, floor_mid{0.0f, 0.0f, 0.0f}, floor_far{-3.5f, 0.0f, -5.0f},
      wall{0.5f, 2.0f, 2.0f}, corner{6.8f, 0.1f, 0.8f};
  for (GiLevel level : {GiLevel::kLow, GiLevel::kMedium, GiLevel::kHigh}) {
    Options on, off;
    on.mode = off.mode = 0;  // ambient occlusion off: the GI's own occlusion at its default
    on.gi.level = level;
    Fixture a(on), b(off);
    if (!a.Init() || !b.Init()) return;
    YardCamera(a.cam);
    YardCamera(b.cam);
    const std::string name = GiLevelName(level);
    Pixels with, without = b.Render(fr, "yard_off_" + name);
    for (int i = 0; i < 4; ++i) with = a.Render(fr, "yard_" + name);  // the history settles
    const Camera& cam = a.cam;
    const Rgb n1 = Colour(with, cam, floor_near), n0 = Colour(without, cam, floor_near);
    const Rgb m1 = Colour(with, cam, floor_mid), m0 = Colour(without, cam, floor_mid);
    const Rgb f1 = Colour(with, cam, floor_far), f0 = Colour(without, cam, floor_far);
    const Rgb w1 = Colour(with, cam, wall), w0 = Colour(without, cam, wall);
    const Rgb c1 = Colour(with, cam, corner), c0 = Colour(without, cam, corner);
    float mean, darkest, brightest;
    FrameChange(with, without, mean, darkest, brightest);
    std::printf("  %s: floor 0.6 m from the wall %.3f %.3f %.3f (was %.3f %.3f %.3f); 2 m %.3f (was %.3f); 7 m %+.4f; "
                "wall %+.3f; corner %+.3f; picture mean %+.4f, darkest pixel %+.3f, brightest %+.3f\n",
                name.c_str(), n1.r, n1.g, n1.b, n0.r, n0.g, n0.b, Luminance(m1), Luminance(m0),
                Luminance(f1) - Luminance(f0), Luminance(w1) - Luminance(w0), Luminance(c1) - Luminance(c0), mean,
                darkest, brightest);
    // Visible: the floor by the wall at least twice as bright as it was, tinted like the wall.
    CHECK(Luminance(n1) > 2.0f * Luminance(n0));
    CHECK(n1.r - n0.r > 0.08f);
    CHECK(n1.r - n0.r > 1.5f * (n1.b - n0.b));
    // It falls off: less 2 m away, still there; nothing 7 m away.
    CHECK(Luminance(m1) - Luminance(m0) > 0.01f);
    CHECK(Luminance(m1) - Luminance(m0) < Luminance(n1) - Luminance(n0));
    CHECK(std::fabs(Luminance(f1) - Luminance(f0)) < kTol);
    // No glow on the lit wall, no crushed shade.
    CHECK(std::fabs(Luminance(w1) - Luminance(w0)) < 0.03f);
    CHECK(Luminance(c1) - Luminance(c0) > -0.012f);
    CHECK(darkest >= -1e-6f);
    CHECK(mean > 0.0f);
    for (int c = 0; c < 3; ++c) CHECK(std::fabs(with.at(2, 2)[c] - without.at(2, 2)[c]) < 1e-6f);
  }
}

TEST(GameGi_BrightOutdoorBounce) {
  // A bright outdoor receiver already contains substantial baked/ambient light. Check the
  // actual RGB addition, rather than a ratio against an almost black floor.
  Mesh yard;
  Quad(yard, {-12, 0, -14}, {12, 0, -14}, {12, 0, 2}, {-12, 0, 2}, 0xFF4C4C4C);
  Quad(yard, {-2, 0, 2}, {2, 0, 2}, {2, 3, 2}, {-2, 3, 2}, kYardWall);
  const float ambient[4] = {0.5f, 0.5f, 0.5f, 0};
  Fixture::Frame fr;
  fr.quad = false;
  fr.scene = &yard;
  fr.ambient = ambient;
  for (GiLevel level : {GiLevel::kLow, GiLevel::kMedium, GiLevel::kHigh}) {
    Options off, on;
    off.mode = on.mode = 0;
    on.gi.level = level;
    Fixture a(on), b(off);
    if (!a.Init() || !b.Init()) return;
    YardCamera(a.cam);
    YardCamera(b.cam);
    const std::string name = std::string("bright_") + GiLevelName(level);
    const Pixels base = b.Render(fr, name + "_off");
    Pixels p;
    for (int i = 0; i < 8; ++i) p = a.Render(fr, name);
    for (V3 point : {V3{0, 0, 1.4f}, V3{0, 0, 0}}) {
      const Rgb c = Colour(p, a.cam, point), c0 = Colour(base, b.cam, point);
      std::printf("  %s z=%.1f: RGB %.4f %.4f %.4f, addition %+.4f %+.4f %+.4f\n",
                  name.c_str(), point.z, c.r, c.g, c.b, c.r-c0.r, c.g-c0.g, c.b-c0.b);
      CHECK(c.r > c0.r);
      CHECK(c.r-c0.r > c.b-c0.b);
      // Bright receivers must gain visible colour, including beyond the contact strip.
      if (level == GiLevel::kMedium) CHECK(c.r - c0.r > (point.z > 1 ? 0.12f : 0.06f));
    }
    float mean, darkest, brightest;
    FrameChange(p, base, mean, darkest, brightest);
    std::printf("  %s picture mean %+.5f, darkest %+.5f, brightest %+.5f\n", name.c_str(), mean, darkest, brightest);
    CHECK(darkest >= -1e-6f);
    for (int c = 0; c < 4; ++c) CHECK(p.at(2, 2)[c] == base.at(2, 2)[c]);
    if (level == GiLevel::kMedium) {
      Options strong = on, zero = on;
      strong.gi.intensity = 4;
      zero.gi.intensity = 0;
      Fixture s(strong), z(zero);
      if (!s.Init() || !z.Init()) return;
      YardCamera(s.cam);
      YardCamera(z.cam);
      Pixels p4;
      for (int i = 0; i < 8; ++i) p4 = s.Render(fr, "bright_medium_intensity4");
      const Pixels p0 = z.Render(fr, "bright_medium_intensity0");
      CHECK(MaxDifference(p0, base) == 0);
      const Rgb c4 = Colour(p4, s.cam, {0, 0, 0}), c1 = Colour(p, a.cam, {0, 0, 0});
      std::printf("  bright_medium intensity4 at 2 m: RGB %.4f %.4f %.4f\n", c4.r, c4.g, c4.b);
      CHECK(c4.r - 76.0f / 255 > 2 * (c1.r - 76.0f / 255));
      for (size_t i = 0; i < p4.rgba.size(); ++i) {
        if (i % 4 == 3) CHECK(p4.rgba[i] == base.rgba[i]);
        else CHECK(p4.rgba[i] >= p.rgba[i]);
      }
      for (int c = 0; c < 4; ++c) CHECK(p4.at(2, 2)[c] == base.at(2, 2)[c]);
    }
  }
}

TEST(GameGi_RenderScaleTwoMatchesOne) {
  for (GiLevel level : {GiLevel::kMedium, GiLevel::kHigh}) {
    Options one, two;
    one.mode = two.mode = 0;
    one.gi = two.gi = Gi(level);
    two.scale = 2.0f;
    Fixture a(one), b(two);
    if (!a.Init() || !b.Init()) return;
    Fixture::Frame plain;
    plain.quad = false;
    const Pixels p1 = a.Render(plain, std::string("scale1_") + GiLevelName(level));
    const Pixels p2 = b.Render(plain, std::string("scale2_") + GiLevelName(level));
    CHECK_EQ(p2.width, kW * 2);
    double sum = 0;
    for (uint32_t y = 0; y < kH; ++y)
      for (uint32_t x = 0; x < kW; ++x)
        for (int c = 0; c < 3; ++c) {
          const float d = 0.25f * (p2.at(2 * x, 2 * y)[c] + p2.at(2 * x + 1, 2 * y)[c] + p2.at(2 * x, 2 * y + 1)[c] +
                                   p2.at(2 * x + 1, 2 * y + 1)[c]);
          sum += std::fabs(d - p1.at(x, y)[c]);
        }
    const double mae = sum / double(kW * kH * 3);
    const float near1 = At(p1, a.cam, kNearFloor, 0), near2 = At(p2, a.cam, kNearFloor, 0, 2.0f);
    std::printf("  %s: scale 2 against 1: mean difference %.4f, red by the wall %.3f against %.3f\n", GiLevelName(level),
                mae, near2, near1);
    CHECK(mae < 0.012);
    CHECK(std::fabs(near2 - near1) < 0.03f);
  }
}

TEST(GameGi_TemporalStaticCameraSettles) {
  // Eight frames from the same camera: from the second on the history is used (the camera's vote
  // finds no motion), and the light settles where a frame without temporal accumulation puts it.
  Options t, plain;
  t.mode = plain.mode = 0;
  t.gi = plain.gi = Gi(GiLevel::kMedium);
  plain.gi.temporal = false;
  Fixture a(t), b(plain);
  if (!a.Init() || !b.Init()) return;
  Fixture::Frame fr;
  fr.quad = false;
  Pixels last;
  for (int i = 0; i < 8; ++i) last = a.Render(fr, "temporal_static_" + std::to_string(i));
  const Pixels single = b.Render(fr, "temporal_static_single");
  const Rgb n1 = Colour(last, a.cam, kNearFloor), n0 = Colour(single, b.cam, kNearFloor);
  const float mae = MeanDifference(last, single);
  std::printf("  after 8 frames: floor by the wall %.3f %.3f %.3f, one frame without history %.3f %.3f %.3f, mean "
              "difference %.4f\n", n1.r, n1.g, n1.b, n0.r, n0.g, n0.b, mae);
  const SceneGi* gi = a.game->scene_gi();
  CHECK(gi != nullptr);
  if (gi) CHECK_EQ(gi->stats().history_frames, uint64_t(7));
  CHECK(std::fabs(n1.r - n0.r) < 0.02f);
  CHECK(mae < 0.003f);
}

TEST(GameGi_TemporalMovingCameraFollows) {
  // The camera moves sideways and forwards a little every frame: the history is reprojected (the
  // vote finds the camera), and the last frame's light sits where a fresh frame from that camera
  // puts it (no trail where the wall's red was).
  Options t, plain;
  t.mode = plain.mode = 0;
  t.gi = plain.gi = Gi(GiLevel::kMedium);
  plain.gi.temporal = false;
  Fixture a(t), b(plain);
  if (!a.Init() || !b.Init()) return;
  Fixture::Frame fr;
  fr.quad = false;
  Pixels last;
  for (int i = 0; i < 8; ++i) {
    a.cam.eye = {0.4f - 0.06f * float(i), 1.5f, -1.0f + 0.05f * float(i)};
    a.cam.Setup();
    last = a.Render(fr, "temporal_moving_" + std::to_string(i));
  }
  b.cam = a.cam;
  const Pixels fresh = b.Render(fr, "temporal_moving_fresh");
  const Rgb n1 = Colour(last, a.cam, kNearFloor), n0 = Colour(fresh, b.cam, kNearFloor);
  const Rgb f1 = Colour(last, a.cam, kFarFloor), f0 = Colour(fresh, b.cam, kFarFloor);
  const float mae = MeanDifference(last, fresh);
  std::printf("  moving camera: floor by the wall %.3f %.3f %.3f against a fresh frame %.3f %.3f %.3f; far floor "
              "%+.4f; mean difference %.4f\n", n1.r, n1.g, n1.b, n0.r, n0.g, n0.b, f1.r - f0.r, mae);
  const SceneGi* gi = a.game->scene_gi();
  if (gi) CHECK_EQ(gi->stats().history_frames, uint64_t(7));
  CHECK(std::fabs(n1.r - n0.r) < 0.02f);
  CHECK(std::fabs(f1.r - f0.r) < kTol);
  CHECK(mae < 0.004f);
}

TEST(GameGi_OffChangesNothing) {
  // --native_gi=off (the default): nothing of it is made, the same frame to the bit.
  Options plain;
  plain.mode = 0;
  Fixture reference(plain);
  if (!reference.Init()) return;
  const Pixels want = reference.Render({}, "off_reference");
  CHECK(reference.game->scene_gi() == nullptr);
  Options off = plain;
  off.gi.level = GiLevel::kOff;
  off.gi.intensity = 3.0f;  // ignored when off
  Fixture f(off);
  if (!f.Init()) return;
  const Pixels got = f.Render({}, "off");
  CHECK_EQ(MaxDifference(got, want), 0.0f);
  CHECK(f.game->scene_gi() == nullptr);
}

TEST(GameGi_LiveOnOffAndIntensity) {
  // The graphics panel turns GI on and off and changes its intensity while the game runs: on, the picture
  // is the one a renderer started with GI draws (once its passes, made on a worker, are in); off again, the
  // one without; a higher intensity bleeds more red.
  auto settings = std::make_shared<EffectSettings>();
  settings->gi = Gi(GiLevel::kOff);
  Options live, on, off;
  live.mode = on.mode = off.mode = 0;
  live.live = [settings] { return *settings; };
  on.gi = Gi(GiLevel::kMedium);
  Fixture f(live), a(on), b(off);
  if (!f.Init() || !a.Init() || !b.Init()) return;
  Fixture::Frame plain;
  plain.quad = false;
  Pixels with;
  // Compare settled histories across a complete 64-pattern noise cycle. A single initial
  // frame versus a two-frame live history hid sampling differences at the old weak strength.
  for (int i = 0; i < 64; ++i) with = a.Render(plain, "live_ref_on");
  const Pixels without = b.Render(plain, "live_ref_off");
  Pixels p = f.Render(plain, "live_off");
  CHECK(MaxDifference(p, without) <= kTol);
  const Camera& cam = f.cam;
  for (int round = 0; round < 3; ++round) {
    settings->gi = Gi(GiLevel::kMedium);
    int frames = 0;
    for (; frames < 400 && !f.game->scene_gi(); ++frames) p = f.Render(plain, "live_on");
    for (int i = 0; i < 64; ++i) p = f.Render(plain, "live_on");
    const float d_on = MaxDifference(p, with);
    const Rgb n1 = Colour(p, cam, kNearFloor), n0 = Colour(without, cam, kNearFloor);
    std::printf("  round %d: GI on after %d frames: red bleed %+.3f, largest difference to GI from the start %.4f\n",
                round, frames, n1.r - n0.r, d_on);
    CHECK(f.game->scene_gi() != nullptr);
    CHECK(n1.r - n0.r > 0.02f);
    CHECK(d_on <= 0.02f);  // the temporal GI history of a live start differs by a few levels at most
    settings->gi.intensity = 3.0f;
    for (int i = 0; i < 3; ++i) p = f.Render(plain, "live_bright");
    const Rgb n3 = Colour(p, cam, kNearFloor);
    std::printf("    intensity 3: red bleed %+.3f\n", n3.r - n0.r);
    CHECK(n3.r - n0.r > 1.5f * (n1.r - n0.r));
    settings->gi = Gi(GiLevel::kOff);
    for (int i = 0; i < 2; ++i) p = f.Render(plain, "live_off_again");
    const float d_off = MaxDifference(p, without);
    std::printf("    off again: largest difference to a renderer without GI %.4f\n", d_off);
    CHECK(d_off <= kTol);
  }
}

TEST(GameGi_UpcomingWaterDepthWriteCannotValidateClearedDepth) {
  Options on;
  on.mode = 0;
  on.gi = Gi(GiLevel::kMedium);
  Fixture f(on);
  if (!f.Init()) return;
  Fixture::Frame fr;
  fr.clear_scene_depth = fr.quad_writes_depth = true;
  f.Render(fr, "cleared_depth_before_water");
  const SceneGi* gi = f.game->scene_gi();
  CHECK(gi != nullptr);
  if (gi) {
    CHECK_EQ(gi->stats().no_depth, uint64_t(1));
    CHECK_EQ(gi->stats().applied_opaque, uint64_t(0));
    CHECK_EQ(gi->stats().applied_present, uint64_t(0));
  }
}

TEST(GameGi_MissingFogUsesTerminalWaterBoundary) {
  Options on;
  on.mode = 0;
  on.gi = Gi(GiLevel::kMedium);
  Fixture hashes(on);
  if (!hashes.Init()) return;
  const uint32_t fog_shader = hashes.CreateShader(false, PixelShaderConstant(2));
  char rules[128];
  std::snprintf(rules, sizeof(rules), "end ps=%016llX\ntransparent ps=%016llX\n",
                static_cast<unsigned long long>(hashes.Hash(false, fog_shader)),
                static_cast<unsigned long long>(hashes.Hash(false, hashes.ps_quad)));
  // The first frame sees an end marker; the next lacks it but has known water.
  on.ao.rules = ParseAoRules(rules);
  Fixture f(on);
  if (!f.Init()) return;
  Fixture::Frame fr;
  fr.quad = false;
  f.DrawFrame(fr);
  const uint32_t fog = f.CreateShader(false, PixelShaderConstant(2));
  f.DrawMesh(QuadMesh(), fog, kBlendAlpha, kDepthColour);
  f.Present(f.rt, "fog_frame");
  fr.quad = true;
  f.Render(fr, "fog_free_water_frame");
  const SceneGi* gi = f.game->scene_gi();
  CHECK(gi != nullptr);
  if (gi) {
    CHECK_EQ(gi->stats().applied_opaque, uint64_t(2));
    CHECK_EQ(gi->stats().applied_present, uint64_t(0));
  }
}

TEST(GameGi_IgnoredOverlayPreventsPresentFallback) {
  Options on;
  on.mode = 0;
  on.gi = Gi(GiLevel::kMedium);
  Fixture hashes(on);
  if (!hashes.Init()) return;
  char rule[80];
  std::snprintf(rule, sizeof(rule), "ignore ps=%016llX\n",
                static_cast<unsigned long long>(hashes.Hash(false, hashes.ps_quad)));
  on.ao.rules = ParseAoRules(rule);
  Fixture f(on);
  if (!f.Init()) return;
  f.Render({}, "ignored_overlay");
  const SceneGi* gi = f.game->scene_gi();
  CHECK(gi != nullptr);
  if (gi) {
    CHECK_EQ(gi->stats().applied_opaque, uint64_t(0));
    CHECK_EQ(gi->stats().applied_present, uint64_t(0));
  }
}

NR_TEST_MAIN()
