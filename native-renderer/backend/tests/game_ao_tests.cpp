// The ambient occlusion through the whole game renderer (docs/ao.md), on lavapipe: a frame drawn
// the way the game draws one, through the hooks, the tracker, the Renderer and the GameRenderer,
// with real translated shaders. The scene: a box on a floor in the corner of two walls, drawn
// first as a depth pre-pass and then as the opaque colour pass (depth writes off), then a
// transparent water plane (alpha blended) and a particle quad (additive) drawn after it.
//
// What is checked, with the GTAO method:
//  - the creases are darkened and open floor is not;
//  - at the end of the opaque pass (apply=opaque) the water and the particle draw over the
//    occlusion: their own colour is not darkened (water = half its colour plus half the occluded
//    scene, particle = occluded scene plus its colour), where the legacy moment would darken both;
//  - the ambient-only mode (the colour pass's pixel shader on the list) gives the same picture,
//    with the occlusion multiplied inside the shader;
//  - a rule file can move the moment (the water's pixel shader counted as opaque);
//  - the resolve moment puts the occlusion into the scene copy only;
//  - render scale 2 matches scale 1 after an area average;
//  - with ao_mode 0 the frame is exactly the frame without any of it;
//  - the Khronos and NVRHI validation layers stay silent.
//
// The D3D library helper is a copy of game_tests.cpp's (the parts these scenes need).
// NR_TEST_IMAGES=<dir> writes the frames there as PPM files.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend/api_binding.h"
#include "backend/draw_state.h"
#include "backend/game_renderer.h"
#include "backend/guest_device.h"
#include "backend/log.h"
#include "backend/render_scale.h"
#include "backend/renderer.h"
#include "backend/shader_library.h"
#include "backend/tests/check.h"
#include "backend/tests/descriptor_test_options.h"
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
  if (const char* dir = std::getenv("NR_TEST_IMAGES")) WritePpm(std::string(dir) + "/game_ao_" + name + ".ppm", p);
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

// oC0 = the interpolated colour (the scene's lit surfaces).
std::vector<uint8_t> PixelShaderInterpolator() {
  Program p;
  p.alloc(2, 0);
  p.exec({Export(0, 0)}, true);
  ctest::Spec spec = PixelSpec();
  spec.ucode = p.assemble();
  return ctest::writeContainer(spec);
}

// oC0 = c<reg> (the water and the particle; reg tells their two shaders apart).
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
  V3 eye{1.5f, 1.6f, -1.5f}, target{-1.0f, 0.3f, 2.6f};
  float px = 0, py = 0, a = 0, b = 0;
  V3 r, u, f;
  float rows[16] = {};
  Camera() {
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

constexpr uint32_t kFloor = 0xFFA89C8C, kWall = 0xFFB4ACA0, kBoxColour = 0xFF8C94A8, kSky = 0xFF406090;
// The water's colour and alpha, and the particle's (additive) colour.
constexpr float kWater[4] = {0.10f, 0.35f, 0.55f, 0.5f};
constexpr float kParticle[4] = {0.15f, 0.12f, 0.08f, 1.0f};

Mesh SceneMesh() {
  Mesh m;
  Quad(m, {-2, 0, -6}, {6, 0, -6}, {6, 0, 4}, {-2, 0, 4}, kFloor);
  Quad(m, {-2, 0, 4}, {6, 0, 4}, {6, 3, 4}, {-2, 3, 4}, kWall);
  Quad(m, {-2, 0, -6}, {-2, 0, 4}, {-2, 3, 4}, {-2, 3, -6}, kWall);
  Box(m, {-1.6f, 0.0f, 2.4f}, {-0.6f, 0.9f, 3.4f}, kBoxColour);
  return m;
}
// The water: a horizontal plane at y 0.2 over the floor by the back wall.
Mesh WaterMesh() {
  Mesh m;
  Quad(m, {0.6f, 0.2f, 3.2f}, {3.4f, 0.2f, 3.2f}, {3.4f, 0.2f, 4.0f}, {0.6f, 0.2f, 4.0f}, 0xFFFFFFFF);
  return m;
}
// The particle: a quad facing the camera beside the box's foot.
const V3 kParticleCentre{-0.42f, 0.18f, 2.8f};
Mesh ParticleMesh(const Camera& cam) {
  const V3 r = cam.r * 0.12f, u = cam.u * 0.12f, c = kParticleCentre;
  Mesh m;
  Quad(m, c - r - u, c + r - u, c + r + u, c - r + u, 0xFFFFFFFF);
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

// Probes (world): the floor in the corner by the back wall (under the water), the floor beside
// the box (under the particle), open floor, the water's surface away from the wall, the
// particle's centre.
const V3 kCrease{2.0f, 0.0f, 3.95f}, kBoxFoot{-0.52f, 0.0f, 2.9f}, kOpenFloor{0.8f, 0.0f, 1.2f},
    kWaterSurface{2.0f, 0.2f, 3.5f}, kWallCrease{-1.95f, 1.4f, 3.95f};

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
  int mode = 1;      // ao_mode
  int strength = 1;  // ao_strength
  bool settings = true;  // false: no ao_settings at all (the plugin without the Xenos plugin's cvars)
  float scale = 1.0f;
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
  uint32_t vs = 0, ps_lit = 0, ps_water = 0, ps_particle = 0, decl = 0;

  explicit Fixture(Options o) : opt(std::move(o)) {}

  bool Init() {
    if (!gpu) {
      Fail(__FILE__, __LINE__, "no Vulkan device (install mesa-vulkan-drivers for lavapipe)");
      return false;
    }
    errors_before = gpu->errors();
    shaders = std::make_unique<ShaderLibrary>(gpu->device(), mem);
    shaders->SetRenderScaleAware(opt.scale != 1.0f);
    shaders->Initialize("", NR_TEST_DXC_DIR);
    renderer = std::make_unique<Renderer>(gpu->device());
    if (!renderer->Initialize(ScaledSize(kW, opt.scale), ScaledSize(kH, opt.scale))) {
      Fail(__FILE__, __LINE__, "Renderer::Initialize failed");
      return false;
    }
    renderer->set_submit([this](nvrhi::ICommandList* cl) { gpu->Execute(cl); });
    auto g = std::make_unique<GameRenderer>(
        gpu->device(), mem, kknr::GuestMemory{mem.PhysicalBase(), mem.PhysicalSize(), 0}, shaders.get());
    ConfigureTestDescriptors(g->options(), gpu);
    g->options().render_scale_x = g->options().render_scale_y = opt.scale;
    g->options().guest_frame_width = kW;
    g->options().ao = opt.ao;  // before Initialize, as the backend does
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
    ps_lit = CreateShader(false, PixelShaderInterpolator());
    ps_water = CreateShader(false, PixelShaderConstant(0));
    ps_particle = CreateShader(false, PixelShaderConstant(1));
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
    SetStream(NewVertexBuffer(Bytes(m)), 16);
    Call(0x821154C8, {device, 4, 0, uint32_t(m.size())});
  }

  // The frame: the depth pre-pass, the opaque colour pass, then (optionally) the water and the
  // particle; resolve_dest: a full-screen depth resolve after the pre-pass and the scene copied
  // into it at the end (the legacy moment).
  struct Frame {
    bool prepass = true, water = true, particle = true;
    bool clear_prepass_depth = false, water_writes_depth = false;
    uint32_t scene_copy = 0, depth_copy = 0;
  };
  void DrawFrame(const Frame& fr) {
    SetRenderTarget(0, rt);
    SetDepthStencil(ds);
    SetViewport(0, 0, kW, kH);
    SetConstants(true, 0, cam.rows, 4);
    Clear(kClearTarget0 | kClearZ | kClearStencil, kSky, 1.0f);
    const Mesh scene = SceneMesh();
    if (fr.prepass) DrawMesh(scene, ps_lit, kBlendOpaque, kDepthPrepass, 0);
    if (fr.depth_copy) Resolve(0x4, fr.depth_copy);  // the depth, as the game resolves it
    if (fr.clear_prepass_depth) Clear(kClearZ, 0, 1.0f);
    DrawMesh(scene, ps_lit, kBlendOpaque, fr.prepass ? kDepthColour : kDepthPrepass);
    if (fr.water) {
      SetConstants(false, 0, kWater, 1);
      DrawMesh(WaterMesh(), ps_water, kBlendAlpha, fr.water_writes_depth ? kDepthPrepass : kDepthColour);
    }
    if (fr.particle) {
      SetConstants(false, 1, kParticle, 1);
      DrawMesh(ParticleMesh(cam), ps_particle, kBlendAdditive, kDepthColour);
    }
    if (fr.scene_copy) Resolve(0, fr.scene_copy);
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
float At(const Pixels& p, const Camera& cam, V3 world, int channel, float scale = 1.0f) {
  int x, y;
  cam.ToPixel(world, x, y);
  float s = 0;
  int n = 0;
  const int x0 = int(std::lround((x - 1) * scale)), x1 = int(std::lround((x + 2) * scale));
  const int y0 = int(std::lround((y - 1) * scale)), y1 = int(std::lround((y + 2) * scale));
  for (int j = y0; j < y1; ++j)
    for (int i = x0; i < x1; ++i, ++n) s += p.at(uint32_t(std::clamp(i, 0, int(p.width) - 1)), uint32_t(std::clamp(j, 0, int(p.height) - 1)))[channel];
  return s / float(n);
}

// Mean over the colour channels.
float Grey(const Pixels& p, const Camera& cam, V3 world, float scale = 1.0f) {
  return (At(p, cam, world, 0, scale) + At(p, cam, world, 1, scale) + At(p, cam, world, 2, scale)) / 3.0f;
}

AoConfig GtaoConfig(AoApply apply, AoQuality quality = AoQuality::kFull) {
  AoConfig c;
  c.method = AoMethod::kGtao;
  c.apply = apply;
  c.quality = quality;
  return c;
}

float MaxDifference(const Pixels& a, const Pixels& b) {
  float worst = 0;
  for (size_t i = 0; i < a.rgba.size() && i < b.rgba.size(); ++i)
    if (i % 4 != 3) worst = std::max(worst, std::fabs(a.rgba[i] - b.rgba[i]));
  return a.rgba.size() == b.rgba.size() ? worst : 1.0f;
}

constexpr float kTol = 2.5f / 255.0f;

}  // namespace

TEST(GameAo_CreasesDarkenedOpenFloorNot) {
  Options on, off;
  on.ao = GtaoConfig(AoApply::kOpaque);
  off.ao = GtaoConfig(AoApply::kOpaque);
  off.mode = 0;
  Fixture a(on), b(off);
  if (!a.Init() || !b.Init()) return;
  Fixture::Frame plain;
  plain.water = plain.particle = false;
  const Pixels with = a.Render(plain, "plain_on"), without = b.Render(plain, "plain_off");
  const Camera& cam = a.cam;
  const float crease = Grey(with, cam, kCrease) / Grey(without, cam, kCrease);
  const float foot = Grey(with, cam, kBoxFoot) / Grey(without, cam, kBoxFoot);
  const float wall = Grey(with, cam, kWallCrease) / Grey(without, cam, kWallCrease);
  const float open = Grey(with, cam, kOpenFloor) / Grey(without, cam, kOpenFloor);
  std::printf("  occlusion: crease %.3f, box foot %.3f, wall corner %.3f, open floor %.3f\n", crease, foot, wall, open);
  CHECK(crease < 0.85f);
  CHECK(foot < 0.85f);
  CHECK(wall < 0.85f);
  CHECK(open > 0.98f);
  // No transparent draw this frame: the occlusion came at Present (the scene was still there).
  const SceneAo* ao = a.game->scene_ao();
  CHECK(ao != nullptr);
  if (ao) {
    CHECK_EQ(ao->stats().applied_present, uint64_t(1));
    CHECK_EQ(ao->stats().applied_opaque, uint64_t(0));
    CHECK_EQ(ao->stats().no_camera, uint64_t(0));
  }
}

TEST(GameAo_TransparentDrawsAreNotDarkened) {
  Options on, off;
  on.ao = off.ao = GtaoConfig(AoApply::kOpaque);
  off.mode = 0;
  Fixture a(on), plain_on(on), b(off);
  if (!a.Init() || !plain_on.Init() || !b.Init()) return;
  const Pixels frame = a.Render({}, "opaque_on");
  Fixture::Frame no_effects;
  no_effects.water = no_effects.particle = false;
  const Pixels scene = plain_on.Render(no_effects, "opaque_scene_on");  // the occluded scene alone
  const Pixels scene_off = b.Render(no_effects, "opaque_scene_off");
  const Camera& cam = a.cam;
  // At the end of the opaque pass: the water (alpha 0.5) is half its colour plus half the occluded
  // scene, the particle the occluded scene plus its colour.
  float worst_water = 0, worst_particle = 0, legacy_gap = 0;
  for (int c = 0; c < 3; ++c) {
    const float w = At(frame, cam, kWaterSurface, c), s = At(scene, cam, kWaterSurface, c);
    worst_water = std::max(worst_water, std::fabs(w - (kWater[3] * kWater[c] + (1 - kWater[3]) * s)));
    const float p = At(frame, cam, kParticleCentre, c), q = At(scene, cam, kParticleCentre, c);
    worst_particle = std::max(worst_particle, std::fabs(p - (q + kParticle[c])));
    // What the legacy moment would give: the water darkened with what is behind it.
    const float ao = At(scene, cam, kWaterSurface, c) / At(scene_off, cam, kWaterSurface, c);
    const float legacy = (kWater[3] * kWater[c] + (1 - kWater[3]) * At(scene_off, cam, kWaterSurface, c)) * ao;
    legacy_gap = std::max(legacy_gap, std::fabs(w - legacy));
  }
  std::printf("  water %.4f, particle %.4f off their unoccluded colours; the legacy moment would be %.4f off\n",
              worst_water, worst_particle, legacy_gap);
  CHECK(worst_water < kTol);
  CHECK(worst_particle < kTol);
  CHECK(legacy_gap > 0.02f);  // the scene behind the water is occluded: the test sees the difference
  const SceneAo* ao = a.game->scene_ao();
  if (ao) {
    CHECK_EQ(ao->stats().applied_opaque, uint64_t(1));
    CHECK_EQ(ao->stats().applied_present, uint64_t(0));
    CHECK_EQ(ao->stats().computed, uint64_t(1));
  }
  // Away from the water and the particle the frame is the occluded scene.
  CHECK(std::fabs(Grey(frame, cam, kWallCrease) - Grey(scene, cam, kWallCrease)) < kTol);
}

TEST(GameAo_MissingEndAppliesBeforeWaterNotAtTheLateCopy) {
  Options o;
  o.ao = GtaoConfig(AoApply::kOpaque);
  Fixture hashes(o);
  if (!hashes.Init()) return;
  char rules[128];
  std::snprintf(rules, sizeof(rules), "end ps=%016llX\ntransparent ps=%016llX\n",
                static_cast<unsigned long long>(hashes.Hash(false, hashes.ps_particle)),
                static_cast<unsigned long long>(hashes.Hash(false, hashes.ps_water)));
  o.ao.rules = ParseAoRules(rules);
  Fixture f(o), reference(o);
  if (!f.Init() || !reference.Init()) return;
  f.Render({}, "end_present");
  Fixture::Frame fr;
  fr.particle = false;  // This frame lacks the end shader seen in the previous frame.
  fr.scene_copy = f.NewTexture(kknr::TextureFormat::k_8_8_8_8, kW, kH);
  const Pixels got = f.Render(fr, "missing_end_water");
  fr.scene_copy = 0;
  const Pixels want = reference.Render(fr, "missing_end_reference");
  CHECK(MaxDifference(got, want) < kTol);
  const SceneAo* ao = f.game->scene_ao();
  CHECK(ao != nullptr);
  if (ao) {
    CHECK_EQ(ao->stats().applied_opaque, uint64_t(2));
    CHECK_EQ(ao->stats().applied_resolve, uint64_t(0));
    CHECK_EQ(ao->stats().applied_present, uint64_t(0));
  }
}

TEST(GameAo_UpcomingWaterDepthWriteCannotHideResolvedDepth) {
  Options o;
  o.ao = GtaoConfig(AoApply::kOpaque);
  Fixture hashes(o);
  if (!hashes.Init()) return;
  char rules[128];
  std::snprintf(rules, sizeof(rules), "end ps=%016llX\ntransparent ps=%016llX\n",
                static_cast<unsigned long long>(hashes.Hash(false, hashes.ps_particle)),
                static_cast<unsigned long long>(hashes.Hash(false, hashes.ps_water)));
  o.ao.rules = ParseAoRules(rules);
  Fixture a(o), b(o);
  if (!a.Init() || !b.Init()) return;
  Fixture::Frame fr;
  fr.particle = false;
  fr.depth_copy = a.NewTexture(kknr::TextureFormat::k_24_8, kW, kH);
  fr.clear_prepass_depth = fr.water_writes_depth = true;
  const Pixels copied = a.Render(fr, "resolved_depth_before_water");
  fr.depth_copy = 0;
  fr.clear_prepass_depth = false;
  const Pixels live = b.Render(fr, "live_depth_before_water");
  CHECK(MaxDifference(copied, live) < kTol);
  CHECK(Grey(copied, a.cam, kWallCrease) < 0.5f);
  const SceneAo* ao = a.game->scene_ao();
  CHECK(ao != nullptr);
  if (ao) {
    CHECK_EQ(ao->stats().applied_opaque, uint64_t(1));
    CHECK_EQ(ao->stats().no_depth, uint64_t(0));
  }
}

TEST(GameAo_FailedEarlyMomentNeverDarkensWaterAtALateFallback) {
  for (bool copy : {false, true}) {
    Options on, off;
    on.ao = off.ao = GtaoConfig(AoApply::kOpaque);
    on.mode = 2; off.mode = 0;
    Fixture a(on), b(off);
    if (!a.Init() || !b.Init()) return;
    Fixture::Frame fr;
    fr.particle = false;
    fr.clear_prepass_depth = fr.water_writes_depth = true;
    if (copy) fr.scene_copy = a.NewTexture(kknr::TextureFormat::k_8_8_8_8, kW, kH);
    const Pixels got = a.Render(fr, "failed_early_water");
    fr.scene_copy = 0;
    const Pixels plain = b.Render(fr, "failed_early_water_off");
    CHECK(MaxDifference(got, plain) < kTol);
    const SceneAo* ao = a.game->scene_ao();
    CHECK(ao != nullptr);
    if (ao) {
      CHECK_EQ(ao->stats().no_depth, uint64_t(1));
      CHECK_EQ(ao->stats().computed, uint64_t(0));
      CHECK_EQ(ao->stats().applied_resolve, uint64_t(0));
      CHECK_EQ(ao->stats().applied_present, uint64_t(0));
    }
  }
}

TEST(GameAo_AmbientOnlyInsideTheLightingShader) {
  // The colour pass's pixel shader on the list, its interpolated colour as the ambient term: the
  // occlusion is multiplied inside the shader (after the depth pre-pass), the same picture as the
  // opaque moment, and the transparent draws are untouched again.
  Options opaque_opt;
  opaque_opt.ao = GtaoConfig(AoApply::kOpaque);
  Fixture reference(opaque_opt);
  if (!reference.Init()) return;
  const Pixels want = reference.Render({}, "ambient_reference");
  char line[64];
  std::snprintf(line, sizeof(line), "%016llX interpolator texcoord0\n",
                static_cast<unsigned long long>(reference.Hash(false, reference.ps_lit)));
  Options amb;
  amb.ao = GtaoConfig(AoApply::kAmbient);
  amb.ao.ambient_shaders = line;
  Fixture f(amb);
  if (!f.Init()) return;
  const Pixels got = f.Render({}, "ambient");
  const float worst = MaxDifference(got, want);
  std::printf("  ambient-only against the opaque moment: worst %.4f\n", worst);
  CHECK(worst < kTol);
  const SceneAo* ao = f.game->scene_ao();
  if (ao) {
    CHECK_EQ(ao->stats().ambient_frames, uint64_t(1));
    CHECK_EQ(ao->stats().applied_opaque, uint64_t(0));
  }
  // Without a pre-pass the occlusion is not ready for the lit draws: the opaque moment instead.
  Fixture g(amb);
  if (!g.Init()) return;
  Fixture::Frame no_prepass;
  no_prepass.prepass = false;
  g.Render(no_prepass, "ambient_no_prepass");
  if (const SceneAo* s = g.game->scene_ao()) {
    CHECK_EQ(s->stats().ambient_frames, uint64_t(0));
    CHECK_EQ(s->stats().applied_opaque, uint64_t(1));
  }
}

TEST(GameAo_RulesFromAFileMoveTheMoment) {
  // The water's pixel shader counted as opaque: the opaque pass ends at the particle, so the water
  // is darkened with the scene and the particle is not.
  Options probe_opt;
  probe_opt.ao = GtaoConfig(AoApply::kOpaque);
  Fixture probe(probe_opt);
  if (!probe.Init()) return;
  char rule[64];
  std::snprintf(rule, sizeof(rule), "opaque ps=%016llX\n", static_cast<unsigned long long>(probe.Hash(false, probe.ps_water)));
  std::vector<std::string> errors;
  Options o;
  o.ao = GtaoConfig(AoApply::kOpaque);
  o.ao.rules = ParseAoRules(rule, &errors);
  CHECK(errors.empty());
  Options off = o;
  off.mode = 0;
  Fixture f(o), f_off(off);
  if (!f.Init() || !f_off.Init()) return;
  const Pixels got = f.Render({}, "rules");
  const Pixels unoccluded = f_off.Render({}, "rules_off");
  Fixture::Frame no_effects;
  no_effects.water = no_effects.particle = false;
  Fixture s_on(o), s_off(off);
  if (!s_on.Init() || !s_off.Init()) return;
  const Pixels scene = s_on.Render(no_effects, "rules_scene_on"), scene_off = s_off.Render(no_effects, "rules_scene_off");
  const Camera& cam = f.cam;
  float worst = 0;
  for (int c = 0; c < 3; ++c) {
    const float ao = At(scene, cam, kWaterSurface, c) / At(scene_off, cam, kWaterSurface, c);
    worst = std::max(worst, std::fabs(At(got, cam, kWaterSurface, c) - At(unoccluded, cam, kWaterSurface, c) * ao));
  }
  std::printf("  water counted as opaque: %.4f off the darkened water\n", worst);
  CHECK(worst < 2 * kTol);
  float particle = 0;
  for (int c = 0; c < 3; ++c)
    particle = std::max(particle, std::fabs(At(got, cam, kParticleCentre, c) - (At(scene, cam, kParticleCentre, c) + kParticle[c])));
  CHECK(particle < kTol);
}

TEST(GameAo_ResolveMomentGoesIntoTheCopy) {
  // apply=resolve with the GTAO method: the depth resolve after the pre-pass is kept and the scene
  // copy at the end gets the occlusion (water and particle included, as today); the game's own
  // target does not.
  Options o, off;
  o.ao = off.ao = GtaoConfig(AoApply::kResolve);
  off.mode = 0;
  Fixture f(o), f_off(off);
  if (!f.Init() || !f_off.Init()) return;
  for (Fixture* x : {&f, &f_off}) {
    Fixture::Frame fr;
    fr.depth_copy = x->NewTexture(kknr::TextureFormat::k_24_8, kW, kH);
    fr.scene_copy = x->NewTexture(kknr::TextureFormat::k_8_8_8_8, kW, kH);
    x->DrawFrame(fr);
    x->Present(x->rt, x == &f ? "resolve_target" : "resolve_target_off");
  }
  const SceneAo* ao = f.game->scene_ao();
  CHECK(ao != nullptr);
  if (!ao) return;
  CHECK_EQ(ao->stats().applied_resolve, uint64_t(1));
  CHECK(ao->gtao() && ao->gtao()->stats().depth_copies == 1);
  CHECK(ao->gtao() && ao->gtao()->stats().copies == 1);
  // The game's own target: no occlusion (the presented frame equals the frame with AO off).
  Pixels target_on, target_off;
  for (const GameRenderer::TargetInfo& t : f.game->DebugTargets())
    if (!t.depth && t.edram_base == 0) target_on = f.gpu->ReadBack(t.texture);
  for (const GameRenderer::TargetInfo& t : f_off.game->DebugTargets())
    if (!t.depth && t.edram_base == 0) target_off = f_off.gpu->ReadBack(t.texture);
  CHECK(MaxDifference(target_on, target_off) < 1.0f / 255.0f);
}

TEST(GameAo_RenderScaleTwoMatchesOne) {
  for (AoQuality q : {AoQuality::kFull, AoQuality::kHalf}) {
    Options one, two;
    one.ao = two.ao = GtaoConfig(AoApply::kOpaque, q);
    two.scale = 2.0f;
    Fixture a(one), b(two);
    if (!a.Init() || !b.Init()) return;
    const std::string name = q == AoQuality::kHalf ? "half" : "full";
    const Pixels p1 = a.Render({}, "scale1_" + name), p2 = b.Render({}, "scale2_" + name);
    CHECK_EQ(p2.width, kW * 2);
    // Scale 2 reduced to the guest's size by an area average.
    double sum = 0;
    for (uint32_t y = 0; y < kH; ++y) {
      for (uint32_t x = 0; x < kW; ++x) {
        for (int c = 0; c < 3; ++c) {
          const float d = 0.25f * (p2.at(2 * x, 2 * y)[c] + p2.at(2 * x + 1, 2 * y)[c] + p2.at(2 * x, 2 * y + 1)[c] +
                                   p2.at(2 * x + 1, 2 * y + 1)[c]);
          sum += std::fabs(d - p1.at(x, y)[c]);
        }
      }
    }
    const double mae = sum / double(kW * kH * 3);
    float worst_probe = 0;
    for (V3 p : {kCrease, kBoxFoot, kOpenFloor, kWaterSurface, kParticleCentre})
      worst_probe = std::max(worst_probe, std::fabs(Grey(p2, a.cam, p, 2.0f) - Grey(p1, a.cam, p)));
    std::printf("  %s resolution: scale 2 against 1: mean difference %.4f, worst probe %.3f\n", name.c_str(), mae, worst_probe);
    // The geometry's edges are partly covered at one scale and whole at the other (the renderer's
    // own scale tests state 1% mean); the occlusion adds little to that. At half resolution the
    // working image of scale 1 is 96 x 54: the box's thin contact shadow is softer there than at
    // scale 2 (nr_gtao_tests shows the same).
    CHECK(mae < 0.012);
    CHECK(worst_probe < (q == AoQuality::kHalf ? 0.08f : 0.04f));
  }
}

std::vector<std::string>& Captured() {
  static std::vector<std::string> lines;
  return lines;
}

TEST(GameAo_DumpFrameNamesTheMoments) {
  // --native_dump_frame logs the classifier's view of every draw: the depth pre-pass, the opaque pass
  // (whose first draw ends the pre-pass), the water (before which the occlusion is worked out and
  // applied) and the particle.
  Options o;
  o.ao = GtaoConfig(AoApply::kOpaque);
  Fixture f(o);
  if (!f.Init()) return;
  f.game->options().dump_frame = 0;
  Captured().clear();
  SetLogSink([](LogLevel, const char* text) {
    // The "AO draw N extra:" detail lines (task 240: depth func, count, arms, projection) are not moments.
    if (std::strstr(text, "AO draw") && !std::strstr(text, " extra:")) Captured().push_back(text);
  });
  f.Render({}, "dump");
  SetLogSink(nullptr);
  CHECK_EQ(Captured().size(), size_t(4));
  if (Captured().size() == 4) {
    CHECK(Captured()[0].find(": depth, scene") != std::string::npos);
    CHECK(Captured()[1].find(": opaque, scene, depth pre-pass ended before it") != std::string::npos);
    CHECK(Captured()[2].find(": transparent, scene, opaque pass ended before it, occlusion worked out, occlusion applied") !=
          std::string::npos);
    CHECK(Captured()[3].find(": transparent, scene (") != std::string::npos);
  }
  for (const std::string& l : Captured()) std::printf("  %s\n", l.c_str());
}

TEST(GameAo_OffChangesNothing) {
  // ao_mode 0 with every method and moment, and no AO settings at all: the same frame as the
  // renderer without any of it, to the bit.
  Options plain;
  plain.settings = false;
  Fixture reference(plain);
  if (!reference.Init()) return;
  const Pixels want = reference.Render({}, "off_reference");
  char line[64];
  std::snprintf(line, sizeof(line), "%016llX interpolator texcoord0\n",
                static_cast<unsigned long long>(reference.Hash(false, reference.ps_lit)));
  std::vector<Options> variants(5);
  variants[0].mode = 0;  // the legacy method, mode 0
  variants[1].ao = GtaoConfig(AoApply::kOpaque);
  variants[1].mode = 0;
  variants[2].ao = GtaoConfig(AoApply::kResolve);
  variants[2].mode = 0;
  variants[3].ao = GtaoConfig(AoApply::kAmbient);
  variants[3].ao.ambient_shaders = line;  // the lit shader translated with its hook, reading nothing
  variants[3].mode = 0;
  variants[4].ao = GtaoConfig(AoApply::kOpaque, AoQuality::kHalf);
  variants[4].settings = false;
  for (size_t i = 0; i < variants.size(); ++i) {
    Fixture f(variants[i]);
    if (!f.Init()) return;
    const Pixels got = f.Render({}, "off_" + std::to_string(i));
    CHECK(got.rgba == want.rgba);
    if (const SceneAo* ao = f.game->scene_ao()) CHECK_EQ(ao->stats().computed, uint64_t(0));
  }
}

NR_TEST_MAIN()
