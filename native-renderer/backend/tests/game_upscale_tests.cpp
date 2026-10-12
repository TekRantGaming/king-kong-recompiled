// The temporal upscalers' frame plumbing through the whole game renderer (docs/upscalers.md): frames
// drawn the way the game draws them, through the hooks, the tracker, the Renderer and the GameRenderer,
// with real translated shaders, on the test device (lavapipe on Linux, the GPU on Windows). The
// upscaler is the built-in bilinear stretch, so every number below is the renderer's own.
//
//  - native-resolution AA without jitter gives the frame of a renderer without an upscaler, exactly;
//  - the input and output generations have the sizes the modes ask for;
//  - the jitter moves the input picture by the sub-pixel offsets the sequence says (sign and size);
//  - the motion vectors: the camera's of a static floor, a moving box's own (the vertex shader's motion
//    variant with last frame's constants), against the projected points;
//  - the reactive mask marks the water and the particle drawn after the opaque pass, not the floor;
//  - the upscale point before a post pass: the scene copy is redone from the upscaled picture, the post
//    pass and a depth-tested draw after it are at the output size, the latter tested against the
//    scene's depth carried over;
//  - the Khronos and NVRHI validation layers stay silent.
//
// The D3D library helper is a copy of game_ao_tests.cpp's (itself game_tests.cpp's), with vertex
// buffers kept from frame to frame (a draw's identity) and a world matrix per object.
// NR_TEST_IMAGES=<dir> writes the frames there as PPM files.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
    auto d = VulkanTestDevice::Create(true, false, /*upscaler_needs=*/true);
    if (d) {
      std::printf("  device: %s, Khronos validation %s\n", d->device_name().c_str(),
                  d->khronos_validation() ? "on" : "not installed");
    }
    return d;
  }();
  return device.get();
}

void MaybeSave(const Pixels& p, const std::string& name) {
  if (const char* dir = std::getenv("NR_TEST_IMAGES")) WritePpm(std::string(dir) + "/game_upscale_" + name + ".ppm", p);
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

// The position (FLOAT3) times the world-view-projection matrix in c0..c3 (dp4 per row, the game's
// static world), the colour exported as TEXCOORD0.
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

std::vector<uint8_t> PixelShaderInterpolator() {
  Program p;
  p.alloc(2, 0);
  p.exec({Export(0, 0)}, true);
  ctest::Spec spec = PixelSpec();
  spec.ucode = p.assemble();
  return ctest::writeContainer(spec);
}

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
  V3 eye, target;
  float px = 0, py = 0;
  V3 r, u, f;
  float rows[16] = {};
  explicit Camera(V3 e = {1.5f, 1.6f, -1.5f}, V3 t = {-1.0f, 0.3f, 2.6f}) : eye(e), target(t) {
    f = Normalize(target - eye);
    r = Normalize(Cross({0, 1, 0}, f));
    u = Cross(f, r);
    py = 1.0f / std::tan(1.0472f / 2);
    px = py * float(kH) / float(kW);
    const float zn = 0.1f, zf = 100.0f;
    const float a = zf / (zf - zn), b = -zn * zf / (zf - zn);
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
  // Where a world point lands, in guest pixels (continuous, pixel centres at .5; no half-pixel offset
  // in these tests: PA_SU_VTX_CNTL pix_center 1).
  void ToPixel(V3 p, float& x, float& y) const {
    const V3 v = p - eye;
    const float vz = Dot(v, f);
    x = (px * Dot(v, r) / vz + 1.0f) * 0.5f * float(kW);
    y = (1.0f - py * Dot(v, u) / vz) * 0.5f * float(kH);
  }
  // The rows for an object moved by `t` in the world (rows . (p + t)).
  void Moved(V3 t, float out[16]) const {
    std::memcpy(out, rows, sizeof(rows));
    for (int i = 0; i < 4; ++i) out[i * 4 + 3] += rows[i * 4 + 0] * t.x + rows[i * 4 + 1] * t.y + rows[i * 4 + 2] * t.z;
  }
};

struct Vertex {
  V3 p;
  uint32_t argb;
};
using Mesh = std::vector<Vertex>;

void Quad(Mesh& m, V3 a, V3 b, V3 c, V3 d, uint32_t argb) {
  for (V3 v : {a, b, c, a, c, d}) m.push_back({v, argb});
}
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
constexpr float kWater[4] = {0.10f, 0.35f, 0.55f, 0.5f};
constexpr float kParticle[4] = {0.15f, 0.12f, 0.08f, 1.0f};

Mesh WorldMesh() {
  Mesh m;
  Quad(m, {-2, 0, -6}, {6, 0, -6}, {6, 0, 4}, {-2, 0, 4}, kFloor);
  Quad(m, {-2, 0, 4}, {6, 0, 4}, {6, 3, 4}, {-2, 3, 4}, kWall);
  Quad(m, {-2, 0, -6}, {-2, 0, 4}, {-2, 3, 4}, {-2, 3, -6}, kWall);
  return m;
}
// The box, in its own space (its world matrix moves it).
const V3 kBoxLo{-1.6f, 0.0f, 2.4f}, kBoxHi{-0.6f, 0.9f, 3.4f};
Mesh BoxMesh() {
  Mesh m;
  Box(m, kBoxLo, kBoxHi, kBoxColour);
  return m;
}
Mesh WaterMesh() {
  Mesh m;
  Quad(m, {0.6f, 0.2f, 3.2f}, {3.4f, 0.2f, 3.2f}, {3.4f, 0.2f, 4.0f}, {0.6f, 0.2f, 4.0f}, 0xFFFFFFFF);
  return m;
}
const V3 kParticleCentre{-0.2f, 0.25f, 2.0f};
Mesh ParticleMesh(const Camera& cam) {
  const V3 r = cam.r * 0.15f, u = cam.u * 0.15f, c = kParticleCentre;
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

constexpr uint32_t kBlendOpaque = 0x00010001;
constexpr uint32_t kBlendAlpha = 0x00010706;
constexpr uint32_t kBlendAdditive = 0x00010101;
constexpr uint32_t kDepthPrepass = 0x2 | 0x4 | (3 << 4);  // test and write, LESS_EQUAL
constexpr uint32_t kDepthColour = 0x2 | (3 << 4);         // test, no write, LESS_EQUAL
constexpr uint32_t kDepthOff = 0;
constexpr uint32_t kClearTarget0 = 0x1, kClearZ = 0x10, kClearStencil = 0x20;

struct Options {
  bool upscale = true;
  UpscaleConfig config;
  float scale = 1.0f;  // the render scale (the output's)
  Options() {
    config.kind = UpscalerKind::kBilinear;
    config.quality = UpscaleQuality::kNative;
  }
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
  std::map<std::string, uint32_t> vbs;  // vertex buffers by name, kept across frames

  explicit Fixture(Options o) : opt(std::move(o)) {}

  bool Init() {
    if (!gpu) {
      Fail(__FILE__, __LINE__, "no Vulkan device");
      return false;
    }
    errors_before = gpu->errors();
    shaders = std::make_unique<ShaderLibrary>(gpu->device(), mem);
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
    g->options().guest_frame_height = kH;
    if (opt.upscale) g->options().upscale = opt.config;
    if (!g->Initialize()) {
      Fail(__FILE__, __LINE__, "GameRenderer::Initialize failed");
      return false;
    }
    // The shaders know whether they are scale aware only after the renderer decided its input size.
    shaders->SetRenderScaleAware(g->scaled());
    shaders->Initialize("", NR_TEST_DXC_DIR);
    g->options().async_pipelines = false;
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
  void Clear(uint32_t flags, uint32_t argb, float z = 1.0f) { Call(0x82115418, {device, 0, 0, flags, argb, 0, 0}, z); }
  // A clear of rectangles (D3DRECT: x1, y1, x2, y2).
  void ClearRect(uint32_t flags, uint32_t argb, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    const uint32_t rect = mem.AllocHeap(16);
    W32(rect, uint32_t(x0));
    W32(rect + 4, uint32_t(y0));
    W32(rect + 8, uint32_t(x1));
    W32(rect + 12, uint32_t(y1));
    Call(0x82115418, {device, 1, rect, flags, argb, 0, 0}, 1.0f);
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
  // A mesh drawn from a vertex buffer kept under `name` (the same buffer every frame: the draw's
  // identity for the motion vectors), with the given world-view-projection rows.
  void DrawMesh(const std::string& name, const Mesh& m, const float rows[16], uint32_t ps, uint32_t blend,
                uint32_t depth_control, uint32_t color_mask = 0xFFFF) {
    Reg(dev::kRbBlendControl0, blend);
    Reg(dev::kRbDepthControl, depth_control);
    Reg(dev::kRbColorMask, color_mask);
    SetConstants(true, 0, rows, 4);
    Call(0x82111E68, {device, decl});
    SetShaders(vs, ps);
    auto it = vbs.find(name);
    if (it == vbs.end()) it = vbs.emplace(name, NewVertexBuffer(Bytes(m))).first;
    SetStream(it->second, 16);
    Call(0x821154C8, {device, 4, 0, uint32_t(m.size())});
  }

  // A frame: the depth pre-pass, the opaque pass, then the water and the particle. The box is moved by
  // `box_offset` (its own world matrix).
  struct Frame {
    V3 box_offset{0, 0, 0};
    bool water = true, particle = true;
  };
  void DrawScene(const Frame& fr) {
    SetRenderTarget(0, rt);
    SetDepthStencil(ds);
    SetViewport(0, 0, kW, kH);
    Clear(kClearTarget0 | kClearZ | kClearStencil, kSky, 1.0f);
    float box_rows[16];
    cam.Moved(fr.box_offset, box_rows);
    const Mesh world = WorldMesh(), box = BoxMesh();
    DrawMesh("world", world, cam.rows, ps_lit, kBlendOpaque, kDepthPrepass, 0);
    DrawMesh("box", box, box_rows, ps_lit, kBlendOpaque, kDepthPrepass, 0);
    DrawMesh("world", world, cam.rows, ps_lit, kBlendOpaque, kDepthColour);
    DrawMesh("box", box, box_rows, ps_lit, kBlendOpaque, kDepthColour);
    if (fr.water) {
      SetConstants(false, 0, kWater, 1);
      DrawMesh("water", WaterMesh(), cam.rows, ps_water, kBlendAlpha, kDepthColour);
    }
    if (fr.particle) {
      SetConstants(false, 1, kParticle, 1);
      DrawMesh("particle", ParticleMesh(cam), cam.rows, ps_particle, kBlendAdditive, kDepthColour);
    }
    Reg(dev::kRbBlendControl0, kBlendOpaque);
    Reg(dev::kRbColorMask, 0xFFFF);
  }
  Pixels Render(const Frame& fr, const std::string& name) {
    DrawScene(fr);
    return Present(rt, name);
  }
};

float MaxDifference(const Pixels& a, const Pixels& b) {
  float worst = 0;
  for (size_t i = 0; i < a.rgba.size() && i < b.rgba.size(); ++i)
    if (i % 4 != 3) worst = std::max(worst, std::fabs(a.rgba[i] - b.rgba[i]));
  return a.rgba.size() == b.rgba.size() && !a.rgba.empty() ? worst : 1.0f;
}

double MeanDifference(const Pixels& a, const Pixels& b) {
  if (a.rgba.size() != b.rgba.size() || a.rgba.empty()) return 1.0;
  double s = 0;
  size_t n = 0;
  for (size_t i = 0; i < a.rgba.size(); ++i)
    if (i % 4 != 3) {
      s += std::fabs(a.rgba[i] - b.rgba[i]);
      ++n;
    }
  return s / double(n);
}

constexpr float kTol = 2.5f / 255.0f;

}  // namespace

TEST(GameUpscale_NativeWithoutJitterIsTheSameFrame) {
  // The plumbing alone: native-resolution AA, the bilinear stretch (a copy at 1:1), no jitter. Every
  // frame equals the frame of a renderer without an upscaler.
  Options with;
  with.config.debug = 8;
  Options without;
  without.upscale = false;
  Fixture a(with), b(without);
  if (!a.Init() || !b.Init()) return;
  for (int i = 0; i < 3; ++i) {
    const Pixels pa = a.Render({}, "native_" + std::to_string(i)), pb = b.Render({}, "reference_" + std::to_string(i));
    const float d = MaxDifference(pa, pb);
    std::printf("  frame %d: largest difference %.4f\n", i, d);
    CHECK(d <= kTol);
  }
  const GameUpscale* u = a.game->upscale();
  CHECK(u != nullptr);
  if (u) {
    CHECK_EQ(u->stats.frames, uint64_t(2));  // frame 0 had no scene before it: drawn at the output size
    CHECK_EQ(u->tracker.stats().by_trigger[uint32_t(UpscaleFrameTracker::Trigger::kPresent)], uint64_t(2));
    CHECK_EQ(u->tracker.stats().frames_2d, uint64_t(1));
  }
}

TEST(GameUpscale_InputAndOutputSizes) {
  // Render scale 2 (the output, 384x216), quality mode (1.5x): the scene at 256x144.
  Options o;
  o.scale = 2.0f;
  o.config.quality = UpscaleQuality::kQuality;
  Fixture f(o);
  if (!f.Init()) return;
  const GameUpscale* u = f.game->upscale();
  CHECK(u != nullptr);
  if (!u) return;
  CHECK_NEAR(u->in_x * kW, 256.0, 0.3);
  CHECK_NEAR(u->in_y * kH, 144.0, 0.3);
  CHECK_EQ(u->jitter_phases, 18u);
  CHECK_NEAR(u->mip_bias, -0.585, 0.01);
  CHECK(f.game->scaled());
  f.Render({}, "sizes_0");
  const Pixels p = f.Render({}, "sizes_1");
  CHECK_EQ(p.width, 384u);
  CHECK_EQ(p.height, 216u);
  bool input = false, output = false;
  for (const GameRenderer::TargetInfo& t : f.game->DebugTargets()) {
    if (t.depth || t.edram_base != 0) continue;
    if (t.host_width == 256 && t.host_height == 144) input = true;
    if (t.host_width == 384 && t.host_height == 216) output = true;
  }
  CHECK(input);
  CHECK(output);
  // The picture still shows the scene (a stretch of it): the sky, the floor and the wall where they
  // are at 1:1.
  Options ref_o;
  ref_o.upscale = false;
  ref_o.scale = 2.0f;
  Fixture ref(ref_o);
  if (!ref.Init()) return;
  ref.Render({}, "sizes_ref_0");
  const Pixels r = ref.Render({}, "sizes_ref_1");
  const double mean = MeanDifference(p, r);
  std::printf("  quality-mode stretch against native scale 2: mean difference %.4f\n", mean);
  CHECK(mean < 0.03);
}

TEST(GameUpscale_JitterMovesTheInputPicture) {
  // A vertical edge of the floor's far end against the sky moves by the jitter: native mode, the
  // stretch is a copy, so the picture is the input. Each frame the column the edge crosses must be on
  // the side the jitter says.
  Options o;  // native, bilinear, jitter on
  Fixture f(o);
  if (!f.Init()) return;
  // Screen-space geometry: identity rows, a quad from x_ndc to the right edge, depth 0.5.
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const float edge_px = 100.3f, edge_py = 60.3f;  // off the jitter values (no ties)  // guest pixels (centres at .5)
  const float xn = edge_px / float(kW) * 2.0f - 1.0f, yn = 1.0f - edge_py / float(kH) * 2.0f;
  Mesh right;
  Quad(right, {xn, 1.0f, 0.5f}, {1.0f, 1.0f, 0.5f}, {1.0f, -1.0f, 0.5f}, {xn, -1.0f, 0.5f}, 0xFFFFFFFF);
  Mesh below;  // a second quad for y: below edge_py
  Quad(below, {-1.0f, yn, 0.4f}, {1.0f, yn, 0.4f}, {1.0f, -1.0f, 0.4f}, {-1.0f, -1.0f, 0.4f}, 0xFF0000FF);
  int checked = 0, matched = 0;
  for (int frame = 0; frame < 12; ++frame) {
    f.SetRenderTarget(0, f.rt);
    f.SetDepthStencil(f.ds);
    f.SetViewport(0, 0, kW, kH);
    f.Clear(kClearTarget0 | kClearZ | kClearStencil, 0xFF000000, 1.0f);
    f.DrawMesh("right", right, identity, f.ps_lit, kBlendOpaque, kDepthPrepass);
    f.DrawMesh("below", below, identity, f.ps_lit, kBlendOpaque, kDepthPrepass);
    const Pixels p = f.Present(f.rt, "jitter_" + std::to_string(frame));
    const GameUpscale* u = f.game->upscale();
    if (!u || u->stats.frames == 0) continue;  // the first frame is drawn without the scene phase
    // With the picture moved by j, the edge is at edge + j: pixel 100's centre (100.5) is right of it
    // when j < 0.25. Row 30 is above the blue quad.
    const bool expect_white = 100.5f > edge_px + u->jitter.x;
    const bool white = p.at(100, 30)[0] > 0.5f;
    const bool expect_blue = 60.5f > edge_py + u->jitter.y;  // pixel row 60, column 20
    const bool blue = p.at(20, 60)[2] > 0.5f && p.at(20, 60)[0] < 0.5f;
    ++checked;
    if (white == expect_white && blue == expect_blue) ++matched;
    if (white != expect_white || blue != expect_blue)
      std::printf("  frame %d: jitter %.3f %.3f, column 100 white %d (expected %d), row 60 blue %d (expected %d)\n",
                  frame, u->jitter.x, u->jitter.y, int(white), int(expect_white), int(blue), int(expect_blue));
  }
  std::printf("  %d of %d frames put the edges where the jitter says\n", matched, checked);
  CHECK(checked >= 10);
  CHECK_EQ(matched, checked);
}

TEST(GameUpscale_MotionVectorsOfTheCameraAndAnObject) {
  // Three frames: the camera steps sideways and turns a little, the box slides along x. The motion
  // texture of the third frame against the points projected with the second frame's camera (and the
  // box's second-frame offset): the floor gets the camera's motion, the box its own.
  Options o;
  o.config.quality = UpscaleQuality::kQuality;  // the input at 128x72 for the 192x108 output
  o.scale = 1.0f;
  Fixture f(o);
  if (!f.Init()) return;
  const Camera c1({1.5f, 1.6f, -1.5f}), c2({1.3f, 1.62f, -1.45f}, {-1.05f, 0.3f, 2.6f});
  const V3 box1{0.0f, 0.0f, 0.0f}, box2{0.25f, 0.0f, -0.1f};
  f.cam = c1;
  f.Render({box1}, "motion_0");  // 2D: no history
  f.cam = c1;
  f.Render({box1}, "motion_1");
  f.shaders->WaitForMotionVariants();
  f.cam = c1;
  f.Render({box1}, "motion_2");  // the motion variants are ready from here
  f.shaders->WaitForMotionVariants();
  f.cam = c2;
  const Pixels frame = f.Render({box2}, "motion_3");
  const GameUpscale* u = f.game->upscale();
  CHECK(u && u->motion);
  if (!u || !u->motion) return;
  f.gpu->WaitIdle();
  const Pixels mv = f.gpu->ReadBack(u->motion);
  std::printf("  motion draws %llu, matched %llu, drawn %llu, waiting %llu; camera frames %llu\n",
              static_cast<unsigned long long>(u->stats.motion_draws), static_cast<unsigned long long>(u->stats.motion_matched),
              static_cast<unsigned long long>(u->stats.motion_drawn), static_cast<unsigned long long>(u->stats.motion_waiting),
              static_cast<unsigned long long>(u->stats.camera_frames));
  CHECK(u->stats.motion_drawn >= 2);
  // Probe a world point: where it is now (host input pixels) and where it was.
  auto probe = [&](V3 p_now, V3 p_before, const char* what, float tolerance) {
    float x2, y2, x1, y1;
    c2.ToPixel(p_now, x2, y2);
    c1.ToPixel(p_before, x1, y1);
    const float sx = u->in_x, sy = u->in_y;
    const uint32_t hx = uint32_t(std::floor(x2 * sx)), hy = uint32_t(std::floor(y2 * sy));
    if (hx >= mv.width || hy >= mv.height) {
      Fail(__FILE__, __LINE__, std::string("probe off the picture: ") + what);
      return;
    }
    const float want_x = (x1 - x2) * sx, want_y = (y1 - y2) * sy;
    const float got_x = mv.at(hx, hy)[0], got_y = mv.at(hx, hy)[1];
    std::printf("  %s: motion %.3f %.3f, projected %.3f %.3f (input pixels)\n", what, got_x, got_y, want_x, want_y);
    CHECK_NEAR(got_x, want_x, tolerance);
    CHECK_NEAR(got_y, want_y, tolerance);
  };
  probe({1.2f, 0.0f, 0.6f}, {1.2f, 0.0f, 0.6f}, "floor (camera motion)", 0.35f);
  probe({2.5f, 1.2f, 4.0f}, {2.5f, 1.2f, 4.0f}, "back wall (camera motion)", 0.35f);
  // The box's front face, a point of the object moved with it.
  const V3 on_box{-1.1f, 0.45f, 2.4f};
  probe(on_box + box2, on_box + box1, "moving box (its own motion)", 0.35f);
  (void)frame;
}

TEST(GameUpscale_CameraOnlyGetsTheBoxWrong) {
  // The same frames with the object pass off (debug bit 4): the box gets the camera's motion, which is
  // not its own (the test above would fail on it): the object pass is what fixes it.
  Options o;
  o.config.quality = UpscaleQuality::kQuality;
  o.config.debug = 4;
  Fixture f(o);
  if (!f.Init()) return;
  const Camera c1({1.5f, 1.6f, -1.5f}), c2({1.3f, 1.62f, -1.45f}, {-1.05f, 0.3f, 2.6f});
  const V3 box1{0.0f, 0.0f, 0.0f}, box2{0.25f, 0.0f, -0.1f};
  f.cam = c1;
  f.Render({box1}, "camonly_0");
  f.Render({box1}, "camonly_1");
  f.Render({box1}, "camonly_2");
  f.cam = c2;
  f.Render({box2}, "camonly_3");
  const GameUpscale* u = f.game->upscale();
  if (!u || !u->motion) return;
  f.gpu->WaitIdle();
  const Pixels mv = f.gpu->ReadBack(u->motion);
  const V3 on_box{-1.1f, 0.45f, 2.4f};
  float x2, y2, x1, y1;
  c2.ToPixel(on_box + box2, x2, y2);
  c1.ToPixel(on_box + box1, x1, y1);
  const uint32_t hx = uint32_t(std::floor(x2 * u->in_x)), hy = uint32_t(std::floor(y2 * u->in_y));
  const float want_x = (x1 - x2) * u->in_x;
  // What the camera alone gives that pixel: the box's point as if it had not moved.
  float sx1, sy1;
  c1.ToPixel(on_box + box2, sx1, sy1);
  const float camera_x = (sx1 - x2) * u->in_x;
  const float got_x = mv.at(hx, hy)[0];
  std::printf("  camera only: the box's motion %.3f, the camera's there %.3f, its own %.3f\n", got_x, camera_x, want_x);
  CHECK_NEAR(got_x, camera_x, 0.35f);
  CHECK(std::fabs(camera_x - want_x) > 1.0f);
  CHECK_EQ(u->stats.motion_drawn, uint64_t(0));
}

TEST(GameUpscale_ReactiveMaskMarksWhatCameAfterTheOpaquePass) {
  Options o;
  Fixture f(o);
  if (!f.Init()) return;
  f.Render({}, "reactive_0");
  f.Render({}, "reactive_1");
  const GameUpscale* u = f.game->upscale();
  CHECK(u && u->reactive && u->snapshot_taken);
  if (!u || !u->reactive) return;
  f.gpu->WaitIdle();
  const Pixels mask = f.gpu->ReadBack(u->reactive);
  auto at = [&](V3 p) {
    float x, y;
    f.cam.ToPixel(p, x, y);
    return mask.at(uint32_t(x * u->in_x), uint32_t(y * u->in_y))[0];
  };
  const float water = at({2.0f, 0.2f, 3.6f}), particle = at(kParticleCentre), floor = at({1.2f, 0.0f, 0.6f}),
              wall = at({2.5f, 1.2f, 4.0f});
  std::printf("  reactive: water %.3f, particle %.3f, floor %.3f, wall %.3f\n", water, particle, floor, wall);
  CHECK(water > 0.2f);
  CHECK(particle > 0.2f);
  CHECK(floor < 0.01f);
  CHECK(wall < 0.01f);
}

TEST(GameUpscale_PointBeforeThePostPass) {
  // After the scene: a full copy of it (the scene copy the post effects read), then a clear of a corner
  // of the scene target (the post chain's downsample): the upscale point. Then a depth-tested quad at
  // depth 0.5 over the whole picture (the HUD's kind of draw): behind the box and the floor, in front
  // of the sky, at the output size. Present shows the scene copy instead of the frame in a second
  // fixture, to see that it was redone from the upscaled picture.
  Options o;
  o.scale = 2.0f;
  o.config.quality = UpscaleQuality::kPerformance;  // the input at half the output
  Fixture f(o);
  if (!f.Init()) return;
  const uint32_t copy = f.NewTexture(kknr::TextureFormat::k_8_8_8_8, kW, kH);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  Mesh hud;
  Quad(hud, {-1, 1, 0.999f}, {1, 1, 0.999f}, {1, -1, 0.999f}, {-1, -1, 0.999f}, 0xFFFF00FF);
  auto frame = [&](int i) {
    f.DrawScene({});
    f.Resolve(0, copy);
    f.ClearRect(kClearTarget0, 0xFF00FF00, 0, 0, kW / 2, kH / 2);
    f.DrawMesh("hud", hud, identity, f.ps_lit, kBlendOpaque, kDepthColour);
    return f.Present(f.rt, "post_" + std::to_string(i));
  };
  frame(0);
  const Pixels p = frame(1);
  const GameUpscale* u = f.game->upscale();
  CHECK(u != nullptr);
  if (!u) return;
  CHECK_EQ(u->tracker.stats().by_trigger[uint32_t(UpscaleFrameTracker::Trigger::kPartialTarget)], uint64_t(1));
  CHECK_EQ(u->stats.scene_copies, uint64_t(1));
  CHECK_EQ(p.width, 384u);
  // The corner is the clear's colour; the sky (bottom right is floor; the top right corner is wall or
  // sky?) gets the quad where nothing nearer was drawn; the floor and the box keep their colours.
  CHECK(p.at(10, 10)[1] > 0.9f && p.at(10, 10)[0] < 0.1f);
  float fx, fy;
  f.cam.ToPixel({1.2f, 0.0f, 0.6f}, fx, fy);
  const float* floor = p.at(uint32_t(fx * 2), uint32_t(fy * 2));
  CHECK(!(floor[0] > 0.9f && floor[2] > 0.9f && floor[1] < 0.1f));  // not the quad's magenta
  // A pixel of the sky (top middle, above the back wall).
  const float* sky = p.at(210, 2);
  CHECK(sky[0] > 0.9f && sky[2] > 0.9f && sky[1] < 0.1f);  // the quad, drawn where the depth was clear
}

namespace {

// A real upscaler on the test device: a still scene drawn for `frames` jittered frames, the last output
// against the native picture at the output size (no upscaler), and the plain stretch of the same input.
void CheckRealUpscaler(UpscalerKind kind, const char* name, int frames = 24, char dlss_preset = 0) {
  Options o;
  o.config.kind = kind;
  o.config.dlss_preset = dlss_preset;
  o.config.quality = UpscaleQuality::kPerformance;  // 2x: what the upscaler has to recover shows
  o.scale = 2.0f;
  Options stretch_o = o;
  stretch_o.config.kind = UpscalerKind::kBilinear;
  stretch_o.config.debug = 8;  // no jitter: the plain stretch of the input
  Options ref_o;
  ref_o.upscale = false;
  ref_o.scale = 2.0f;
  Fixture f(o), s(stretch_o), r(ref_o);
  if (!f.Init() || !s.Init() || !r.Init()) return;
  const GameUpscale* u = f.game->upscale();
  if (!u || u->kind != kind) {
    std::printf("  %s is not available on this device (%s): skipped\n", name, u ? u->name.c_str() : "no upscaler");
    return;
  }
  Fixture::Frame still;
  still.particle = false;  // a camera-facing quad would move nothing, but keep the picture plain
  Pixels out;
  for (int i = 0; i < frames; ++i) out = f.Render(still, std::string(name) + "_" + std::to_string(i));
  const Pixels stretched = [&] {
    Pixels p;
    for (int i = 0; i < 3; ++i) p = s.Render(still, std::string(name) + "_stretch");
    return p;
  }();
  Pixels native;
  for (int i = 0; i < 2; ++i) native = r.Render(still, std::string(name) + "_native");
  const double up = MeanDifference(out, native), plain = MeanDifference(stretched, native);
  std::printf("  %s (%s): mean difference to native %.4f, the plain stretch %.4f; failures %llu\n", name,
              u->name.c_str(), up, plain, static_cast<unsigned long long>(u->stats.failures));
  CHECK_EQ(u->stats.failures, uint64_t(0));
  CHECK(up < plain);
}

}  // namespace

TEST(GameUpscale_Fsr3) { CheckRealUpscaler(UpscalerKind::kFsr3, "fsr3"); }
TEST(GameUpscale_Dlss) { CheckRealUpscaler(UpscalerKind::kDlss, "dlss"); }
TEST(GameUpscale_DlssPresetM) { CheckRealUpscaler(UpscalerKind::kDlss, "dlss_m", 24, 'M'); }

NR_TEST_MAIN()
