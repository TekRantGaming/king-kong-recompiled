// The game renderer (Phase 2) without the game: synthetic guest memory holding
// a device struct, shader objects built with the shader tests' microcode
// assembler and container writer, textures, vertex and index buffers laid out
// as docs/d3d-structs.md says, all fed through the hook table, the NrApi
// binding and the tracker into the Renderer and its GameRenderer on lavapipe.
// The pictures are read back and checked pixel by pixel; every test also
// checks that the Khronos validation layer stayed silent.
//
// The `Xdk` helper plays the D3D library: each setter writes what the real one
// writes into the device struct (the register images and the D3D-level
// fields the renderer reads) and then calls the hook, as the recompiled
// library would.
//
// NR_TEST_IMAGES=<dir> writes each read-back image there as a PPM.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "backend/api_binding.h"
#include "backend/draw_state.h"
#include "backend/frame_log.h"
#include "backend/pipeline_cache.h"
#include "backend/game_renderer.h"
#include "backend/guest_device.h"
#include "backend/renderer.h"
#include "backend/shader_library.h"
#include "backend/tests/check.h"
#include "backend/tests/fake_guest.h"
#include "backend/tests/vk_test_device.h"
#include "container_writer.h"
#include "hooks/hook_table.h"
#include "kknr/guest_texture.h"
#include "kkshaders/cache.h"
#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/vertex_patch.h"
#include "xenos_asm.h"

#ifndef NR_TEST_DXC_DIR
#define NR_TEST_DXC_DIR ""
#endif

using namespace nr;
using namespace nr::test;

namespace {

constexpr uint32_t kSize = 64;  // frame image and main surface
// One step of an 8-bit channel (the targets are RGBA8) seen through the 10-bit
// frame image, plus rounding.
constexpr double kTol = 2.5 / 255.0;

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

void MaybeSave(const Pixels& p, const char* name) {
  if (const char* dir = std::getenv("NR_TEST_IMAGES")) WritePpm(std::string(dir) + "/" + name + ".ppm", p);
}

struct Rgb {
  double r, g, b;
};
Rgb FromArgb(uint32_t argb) {
  return {((argb >> 16) & 255) / 255.0, ((argb >> 8) & 255) / 255.0, (argb & 255) / 255.0};
}

void CheckColor(const Pixels& p, uint32_t x, uint32_t y, Rgb want, const char* what, int line,
                double tol = kTol) {
  if (x >= p.width || y >= p.height) {
    Fail(__FILE__, line, std::string(what) + ": pixel outside the image");
    return;
  }
  const float* v = p.at(x, y);
  if (std::fabs(v[0] - want.r) > tol || std::fabs(v[1] - want.g) > tol || std::fabs(v[2] - want.b) > tol) {
    std::ostringstream s;
    s << what << " at (" << x << ", " << y << "): got (" << v[0] << ", " << v[1] << ", " << v[2] << "), want ("
      << want.r << ", " << want.g << ", " << want.b << ")";
    Fail(__FILE__, line, s.str());
  }
}
#define CHECK_COLOR(p, x, y, want, what) CheckColor((p), (x), (y), (want), (what), __LINE__)

// ------------------------------------------------------------- shaders ---

using namespace xasm;

enum Usage : uint32_t { kPosition = 0, kTexcoord = 5, kColor = 10 };

Op A(const Alu& a) { return {a.encode(), false}; }
Op F(const VFetch& v) { return {v.encode(), true}; }
Op F(const TFetch& t) { return {t.encode(), true}; }
Op Export(uint32_t reg, uint32_t src, const char* m = "xyzw") {
  return A(Alu().v(MAXv, reg, m, r(src), r(src)).exp(reg));
}

// The instruction indices of the vertex fetches, in order (as the shader
// tests' corpus finds them for the container's fetch table).
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

// A vertex shader: the position (FLOAT3) and one attribute (usage, format)
// fetched by declaration usage, the attribute exported as TEXCOORD0. With
// offset_c0 the position's x and y get c0.xy added.
std::vector<uint8_t> VertexShader(uint32_t attribute_usage, uint32_t attribute_format, bool offset_c0 = false) {
  VFetch pos;
  pos.dst = 1;
  pos.fetchConstant = 95;
  pos.format = F_32_32_32_FLOAT;
  pos.stride = 4;
  VFetch attr = pos;
  attr.dst = 2;
  attr.format = attribute_format;
  attr.offset = 3;
  std::vector<Op> ops = {F(pos), F(attr)};
  if (offset_c0) ops.push_back(A(Alu().v(ADDv, 1, "xy", r(1), c(0))));
  Program p;
  p.exec(ops);
  p.alloc(1, 0);
  p.exec({Export(62, 1)});
  p.alloc(2, 0);
  p.exec({Export(0, 2)}, true);
  ctest::Spec spec;
  spec.vertex = true;
  spec.ucode = p.assemble();
  spec.constants.push_back({"g_Offset", 2, 0, 1});
  spec.interpolators = {{kTexcoord, 0, 0}};
  const std::vector<uint32_t> fetches = FindVfetchAddresses(spec.ucode);
  const uint32_t usages[2] = {kPosition, attribute_usage};
  for (size_t i = 0; i < fetches.size() && i < 2; ++i) spec.fetches.push_back({fetches[i], usages[i], 0, 0});
  return ctest::writeContainer(spec);
}

ctest::Spec PixelSpec() {
  ctest::Spec spec;
  spec.vertex = false;
  spec.interpolators = {{kTexcoord, 0, 0}};
  return spec;
}

// oC0 = the interpolator.
std::vector<uint8_t> PixelShaderInterpolator() {
  Program p;
  p.alloc(2, 0);
  p.exec({Export(0, 0)}, true);
  ctest::Spec spec = PixelSpec();
  spec.ucode = p.assemble();
  return ctest::writeContainer(spec);
}

// oC<i> = c<i> for i < targets.
std::vector<uint8_t> PixelShaderConstants(uint32_t targets = 1) {
  Program p;
  std::vector<Op> body, exports;
  for (uint32_t i = 0; i < targets; ++i) {
    body.push_back(A(Alu().v(MAXv, i, "xyzw", c(i), c(i))));
    exports.push_back(Export(i, i));
  }
  p.exec(body);
  p.alloc(2, targets - 1);
  p.exec(exports, true);
  ctest::Spec spec = PixelSpec();
  spec.ucode = p.assemble();
  spec.constants.push_back({"g_Colors", 2, 0, uint16_t(targets)});
  return ctest::writeContainer(spec);
}

// oC0 = tfetch2D(texture fetch constant 0, interpolator.xy).
std::vector<uint8_t> PixelShaderTexture() {
  TFetch t;
  t.dst = 0;
  t.src = 0;
  t.srcSwizzle = fetchSrcSwizzle("xy");
  t.slot = 0;
  t.dim = Dim::D2;
  Program p;
  p.exec({F(t)});
  p.alloc(2, 0);
  p.exec({Export(0, 0)}, true);
  ctest::Spec spec = PixelSpec();
  spec.ucode = p.assemble();
  spec.constants.push_back({"g_Texture", 3, 0, 1, 4, 12, 1, 1, 1});
  return ctest::writeContainer(spec);
}

// --------------------------------------------------------- vertex data ---

void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}
void PutBEFloat(std::vector<uint8_t>& out, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  PutBE32(out, u);
}

// Vertices of a rectangle in clip space (+y up) in the order top-left,
// top-right, bottom-right, bottom-left (a quad, or a fan).
struct Rect {
  float x0, y0, x1, y1;  // left, bottom, right, top
};
// The four quadrants: top-left, top-right, bottom-left, bottom-right.
constexpr Rect kQuadrants[4] = {{-1, 0, 0, 1}, {0, 0, 1, 1}, {-1, -1, 0, 0}, {0, -1, 1, 0}};
constexpr Rect kFull = {-1, -1, 1, 1};
constexpr uint32_t kQuadrantColors[4] = {0xFF336699, 0xFFCC2211, 0xFF11EE44, 0xFF8040F0};
// Pixel centres inside each quadrant of the 64x64 picture (and near its corners).
constexpr uint32_t kQuadrantPixels[4][2] = {{16, 16}, {48, 16}, {16, 48}, {48, 48}};

// Position + D3DCOLOR vertices (stride 16).
struct ColorVertex {
  float x, y, z;
  uint32_t argb;
};
std::vector<ColorVertex> QuadVertices(const Rect& q, uint32_t argb, float z = 0.5f) {
  return {{q.x0, q.y1, z, argb}, {q.x1, q.y1, z, argb}, {q.x1, q.y0, z, argb}, {q.x0, q.y0, z, argb}};
}
// The same rectangle as a triangle list.
std::vector<ColorVertex> QuadTriangles(const Rect& q, uint32_t argb, float z = 0.5f) {
  auto v = QuadVertices(q, argb, z);
  return {v[0], v[1], v[2], v[0], v[2], v[3]};
}
std::vector<uint8_t> Bytes(const std::vector<ColorVertex>& vs) {
  std::vector<uint8_t> out;
  for (const ColorVertex& v : vs) {
    PutBEFloat(out, v.x);
    PutBEFloat(out, v.y);
    PutBEFloat(out, v.z);
    PutBE32(out, v.argb);
  }
  return out;
}

// Position + FLOAT2 texture coordinate vertices (stride 20): a full-screen
// triangle list with (0, 0) at the top left.
std::vector<uint8_t> FullScreenTextured() {
  struct V {
    float x, y, u, v;
  };
  const V tl{-1, 1, 0, 0}, tr{1, 1, 1, 0}, br{1, -1, 1, 1}, bl{-1, -1, 0, 1};
  std::vector<uint8_t> out;
  for (const V& v : {tl, tr, br, tl, br, bl}) {
    PutBEFloat(out, v.x);
    PutBEFloat(out, v.y);
    PutBEFloat(out, 0.5f);
    PutBEFloat(out, v.u);
    PutBEFloat(out, v.v);
  }
  return out;
}

// ------------------------------------------------------------- fixture ---

// Forwards the tracker's records to the renderer and shader creations to the
// shader library (as the plugin's backend does).
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

// Register values.
constexpr uint32_t kBlendOpaque = 0x00010001;  // ONE ADD ZERO, colour and alpha
constexpr uint32_t kDepthLess = 0x2 | 0x4 | (1 << 4);
constexpr uint32_t kClearTarget0 = 0x1, kClearZ = 0x10, kClearStencil = 0x20;

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

  bool Init() {
    if (!gpu) {
      Fail(__FILE__, __LINE__, "no Vulkan device (install mesa-vulkan-drivers for lavapipe)");
      return false;
    }
    errors_before = gpu->errors();
    shaders = std::make_unique<ShaderLibrary>(gpu->device(), mem);
    shaders->Initialize("", NR_TEST_DXC_DIR);
    renderer = std::make_unique<Renderer>(gpu->device());
    if (!renderer->Initialize(kSize, kSize)) {
      Fail(__FILE__, __LINE__, "Renderer::Initialize failed");
      return false;
    }
    renderer->set_submit([this](nvrhi::ICommandList* cl) { gpu->Execute(cl); });
    auto g = std::make_unique<GameRenderer>(
        gpu->device(), mem, kknr::GuestMemory{mem.PhysicalBase(), mem.PhysicalSize(), 0}, shaders.get());
    if (!g->Initialize()) {
      Fail(__FILE__, __LINE__, "GameRenderer::Initialize failed");
      return false;
    }
    // Pipelines are made on the render thread here: the tests draw once and
    // read the picture back, so a draw skipped while a worker builds its
    // pipeline (the plugin's default) would fail them. The asynchronous path
    // has its own tests (PipelineCache_*).
    g->options().async_pipelines = false;
    game = g.get();
    renderer->EnableGame(std::move(g));
    sink.renderer = renderer.get();
    sink.shaders = shaders.get();
    tracker = std::make_unique<DrawTracker>(mem, &sink);
    InitApiBinding(binding, tracker.get(), [](void* t) { return static_cast<DrawTracker*>(t); });

    // The device struct with the library's defaults for what the renderer reads.
    device = mem.AllocHeap(dev::kSize, 64);
    mem.AllocPhysical(4096);  // physical address 0 means "none" in fetch constants
    Reg(dev::kRbColorMask, 0xFFFF);  // COLORWRITEENABLE 0-3 all on
    Reg(dev::kRbBlendControl0, kBlendOpaque);
    for (uint32_t i = 0; i < 3; ++i) Reg(dev::kRbBlendControl1 + 4 * i, kBlendOpaque);
    Reg(dev::kPaClVteCntl, 0x43F);  // all six scale / offset enables, w0 format
    Reg(dev::kPaSuVtxCntl, 1);      // pixel centres at .5 (no half-pixel shift)
    Reg(dev::kRbStencilRefMask, 0xFFFF00);
    return true;
  }

  ~Fixture() {
    if (!gpu) return;
    gpu->WaitIdle();
    renderer.reset();
    shaders.reset();
    gpu->WaitIdle();
    CHECK_EQ(gpu->errors(), errors_before);  // the validation layer stayed silent
  }

  // ---- guest memory
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
  uint32_t NewRect(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    const uint32_t a = mem.AllocHeap(16);
    W32(a, uint32_t(x1));
    W32(a + 4, uint32_t(y1));
    W32(a + 8, uint32_t(x2));
    W32(a + 12, uint32_t(y2));
    return a;
  }
  uint32_t NewPoint(int32_t x, int32_t y) {
    const uint32_t a = mem.AllocHeap(8);
    W32(a, uint32_t(x));
    W32(a + 4, uint32_t(y));
    return a;
  }

  // A surface object (CreateRenderTarget / CreateDepthStencilSurface).
  uint32_t NewSurface(uint32_t width, uint32_t height, uint32_t edram_base, uint32_t format, uint32_t pitch = 0) {
    const uint32_t s = mem.AllocHeap(64);
    W32(s + 16 + 8, (width - 1) | ((height - 1) << 13));
    W32(s + 48, pitch ? pitch : width);
    W32(s + 52, edram_base | (format << 16));
    return s;
  }

  // A texture object (40 bytes, the fetch constant at +16 with CPU
  // addresses) over new physical memory holding `image` (from
  // EncodeGuestTexture); returns the object.
  uint32_t NewTexture(kknr::TextureFetchDesc desc,
                      const std::function<const uint8_t*(uint32_t, uint32_t)>& blocks = nullptr,
                      uint32_t* base_cpu = nullptr, uint32_t* base_bytes = nullptr) {
    // Room for the largest test texture.
    const uint32_t base = mem.AllocPhysical(256 * 1024);
    desc.base_address = base - FakeGuestMemory::kPhysicalView;
    const kknr::TextureFetch fetch = kknr::MakeTextureFetch(desc);
    if (blocks) {
      kknr::GuestTextureImage image;
      if (!kknr::EncodeGuestTexture(fetch, blocks, image)) {
        Fail(__FILE__, __LINE__, "EncodeGuestTexture failed");
      } else if (image.base.size() > 256 * 1024) {
        Fail(__FILE__, __LINE__, "texture too large for the test allocation");
      } else {
        std::memcpy(mem.Writable(base), image.base.data(), image.base.size());
        if (base_bytes) *base_bytes = uint32_t(image.base.size());
      }
    }
    if (base_cpu) *base_cpu = base;
    const uint32_t object = mem.AllocHeap(40);
    W32(object, (3u << 16) | 1);
    for (uint32_t i = 0; i < 6; ++i) W32(object + 16 + 4 * i, fetch.words[i]);
    // Word 1: the base address as the CPU address the object holds.
    W32(object + 16 + 4, (fetch.words[1] & 0xFFFu) | base);
    return object;
  }

  // ---- the hooks
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

  // CreateVertexShader / CreatePixelShader: the library copies the
  // container's virtual part after its header and the microcode into
  // physical memory; the hook passes the container and the new object.
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
  void SetViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float min_z = 0.0f, float max_z = 1.0f) {
    const uint32_t vp = mem.NewViewport(x, y, w, h, min_z, max_z);
    for (uint32_t i = 0; i < 4; ++i) Reg(dev::kViewport + 4 * i, R32(vp + 4 * i));
    RegF(dev::kViewport + 16, min_z);
    RegF(dev::kViewport + 20, max_z);
    RegF(dev::kPaClVport + 0, float(w) / 2);
    RegF(dev::kPaClVport + 4, float(x) + float(w) / 2);
    RegF(dev::kPaClVport + 8, -float(h) / 2);
    RegF(dev::kPaClVport + 12, float(y) + float(h) / 2);
    RegF(dev::kPaClVport + 16, max_z - min_z);
    RegF(dev::kPaClVport + 20, min_z);
    Reg(dev::kPaScWindowScissorTl, 0x80000000u | x | (y << 16));
    Reg(dev::kPaScWindowScissorBr, (x + w) | ((y + h) << 16));
    Call(0x8210BAC8, {device, vp});
  }
  void SetStream(uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride) {
    Reg(dev::kStreamOffset + 8 * stream, offset);
    Reg(dev::kStreamBuffer + 8 * stream, vb);
    mem.Writable(device + 12688 + stream)[0] = uint8_t(stride / 4);
    Call(0x8210BD38, {device, stream, vb, offset, stride});
  }
  void SetIndices(uint32_t ib) {
    Reg(dev::kIndexBuffer, ib);
    Call(0x8210BE38, {device, ib});
  }
  void SetDeclaration(uint32_t decl) { Call(0x82111E68, {device, decl}); }
  void SetShaders(uint32_t vs, uint32_t ps) {
    Call(0x82110C28, {device, vs});
    Call(0x821108B8, {device, ps});
  }
  void SetConstants(bool vertex, uint32_t reg, std::initializer_list<float> values) {
    const uint32_t data = mem.NewFloats(values);
    const uint32_t base = vertex ? dev::kVsConstants : dev::kPsConstants;
    uint32_t i = 0;
    for (float v : values) RegF(base + reg * 16 + 4 * i++, v);
    Call(vertex ? 0x82110300 : 0x82110448, {device, reg, data, uint32_t(values.size() / 4)});
  }
  void SetTexture(uint32_t slot, uint32_t texture) {
    Reg(dev::kTextures + 4 * slot, texture);
    // The device's fetch constant copy, with physical addresses.
    for (uint32_t i = 0; i < 6; ++i) {
      uint32_t w = R32(texture + 16 + 4 * i);
      if ((i == 1 || i == 5) && (w & 0xFFFFF000u)) w = (w & 0xFFFu) | (CpuToPhysical(w & 0xFFFFF000u) & 0xFFFFF000u);
      Reg(dev::kFetchConstants + 24 * slot + 4 * i, w);
    }
    Call(0x82118F78, {device, slot, texture});
  }
  void Clear(uint32_t flags, uint32_t argb, float z = 1.0f, uint32_t stencil = 0,
             const std::vector<std::array<int32_t, 4>>& rects = {}) {
    uint32_t list = 0;
    if (!rects.empty()) {
      list = mem.AllocHeap(uint32_t(rects.size() * 16));
      for (size_t i = 0; i < rects.size(); ++i)
        for (uint32_t c = 0; c < 4; ++c) W32(list + uint32_t(i) * 16 + c * 4, uint32_t(rects[i][c]));
    }
    Call(0x82115418, {device, uint32_t(rects.size()), list, flags, argb, 0, stencil}, z);
  }
  void Draw(uint32_t primitive, uint32_t start, uint32_t count) {
    Call(0x821154C8, {device, primitive, start, count});
  }
  void DrawIndexed(uint32_t primitive, int32_t base_vertex, uint32_t start, uint32_t count) {
    Call(0x82115708, {device, primitive, uint32_t(base_vertex), start, count});
  }
  void Resolve(uint32_t flags, uint32_t rect, uint32_t dest, uint32_t point = 0, uint32_t clear_color = 0,
               float clear_z = 1.0f) {
    Call(0x82116178, {device, flags, rect, dest, point, 0, 0, clear_color}, clear_z);
  }
  // Present with `back_buffer` as the device's back buffer; returns the
  // presented frame image read back.
  Pixels Present(uint32_t back_buffer, const char* save_as = nullptr) {
    Reg(dev::kBackBuffer, back_buffer);
    Call(0x821147B8, {device});
    gpu->WaitIdle();
    Pixels p;
    if (nvrhi::ITexture* image = renderer->presented_image()) p = gpu->ReadBack(image);
    if (save_as) MaybeSave(p, save_as);
    return p;
  }
  Pixels ReadTarget(bool depth, uint32_t edram_base, uint32_t format) {
    for (const GameRenderer::TargetInfo& t : game->DebugTargets()) {
      if (t.depth == depth && t.edram_base == edram_base && t.format == format) return gpu->ReadBack(t.texture);
    }
    Fail(__FILE__, __LINE__, "no such host target");
    return {};
  }

  // ---- common scenes
  struct ColorPipeline {
    uint32_t vs = 0, ps_interp = 0, ps_const = 0, decl = 0;
  };
  ColorPipeline MakeColorPipeline(bool offset_c0 = false) {
    ColorPipeline p;
    p.vs = CreateShader(true, VertexShader(kColor, F_8_8_8_8, offset_c0));
    p.ps_interp = CreateShader(false, PixelShaderInterpolator());
    p.ps_const = CreateShader(false, PixelShaderConstants());
    p.decl = mem.NewDeclaration({{0, 0, kDeclFloat3, kUsagePosition, 0}, {0, 12, kDeclColor, kUsageColor, 0}});
    return p;
  }
  // The main surface: 64x64 k_8_8_8_8 at EDRAM 0, bound with a full viewport.
  uint32_t BindMainSurface() {
    const uint32_t rt = NewSurface(kSize, kSize, 0, 0);
    SetRenderTarget(0, rt);
    SetViewport(0, 0, kSize, kSize);
    return rt;
  }
  // Draws a triangle-list vertex buffer (position + colour).
  void DrawColored(const ColorPipeline& p, const std::vector<ColorVertex>& vertices, bool constant_color = false) {
    SetDeclaration(p.decl);
    SetShaders(p.vs, constant_color ? p.ps_const : p.ps_interp);
    SetStream(0, NewVertexBuffer(Bytes(vertices)), 0, 16);
    Draw(4, 0, uint32_t(vertices.size()));
  }
  void DrawQuadrants(const ColorPipeline& p) {
    std::vector<ColorVertex> v;
    for (int q = 0; q < 4; ++q) {
      auto t = QuadTriangles(kQuadrants[q], kQuadrantColors[q]);
      v.insert(v.end(), t.begin(), t.end());
    }
    DrawColored(p, v);
  }
  void CheckQuadrants(const Pixels& px, const Rgb want[4], int line) {
    static const char* names[4] = {"top left", "top right", "bottom left", "bottom right"};
    for (int q = 0; q < 4; ++q) {
      CheckColor(px, kQuadrantPixels[q][0], kQuadrantPixels[q][1], want[q], names[q], line);
      // Near the quadrant's corners too.
      const uint32_t x0 = (q & 1) ? 33 : 1, y0 = (q & 2) ? 33 : 1;
      CheckColor(px, x0, y0, want[q], names[q], line);
      CheckColor(px, x0 + 29, y0 + 29, want[q], names[q], line);
    }
  }
  void CheckQuadrantColors(const Pixels& px, int line) {
    Rgb want[4];
    for (int q = 0; q < 4; ++q) want[q] = FromArgb(kQuadrantColors[q]);
    CheckQuadrants(px, want, line);
  }
  // Draws a full-screen quad sampling `texture` (fetch constant 0).
  void DrawTextured(uint32_t texture) {
    const uint32_t vs = CreateShader(true, VertexShader(kTexcoord, F_32_32_FLOAT));
    const uint32_t ps = CreateShader(false, PixelShaderTexture());
    const uint32_t decl =
        mem.NewDeclaration({{0, 0, kDeclFloat3, kUsagePosition, 0}, {0, 12, kDeclFloat2, kUsageTexcoord, 0}});
    SetDeclaration(decl);
    SetShaders(vs, ps);
    SetTexture(0, texture);
    SetStream(0, NewVertexBuffer(FullScreenTextured()), 0, 20);
    Draw(4, 0, 6);
  }
};

}  // namespace

// ---------------------------------------------------------------- clears ---

TEST(Clear_WholeTargetRectsAndViewport) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  f.Clear(kClearTarget0, 0xFF336699);
  Pixels px = f.Present(rt, "clear_whole");
  for (uint32_t y = 0; y < kSize; y += 9)
    for (uint32_t x = 0; x < kSize; x += 9) CHECK_COLOR(px, x, y, FromArgb(0xFF336699), "whole clear");

  // Rectangles: only they change.
  f.Clear(kClearTarget0, 0xFF000000);
  f.Clear(kClearTarget0, 0xFFFF0000, 1.0f, 0, {{0, 0, 16, 16}, {32, 32, 64, 48}});
  px = f.Present(rt, "clear_rects");
  CHECK_COLOR(px, 8, 8, FromArgb(0xFFFF0000), "first rectangle");
  CHECK_COLOR(px, 15, 15, FromArgb(0xFFFF0000), "first rectangle's last pixel");
  CHECK_COLOR(px, 16, 16, FromArgb(0xFF000000), "outside the first rectangle");
  CHECK_COLOR(px, 40, 40, FromArgb(0xFFFF0000), "second rectangle");
  CHECK_COLOR(px, 40, 50, FromArgb(0xFF000000), "below the second rectangle");
  CHECK_COLOR(px, 20, 40, FromArgb(0xFF000000), "left of the second rectangle");

  // No rectangles: the viewport.
  f.Clear(kClearTarget0, 0xFF000000);
  f.SetViewport(0, 0, kSize / 2, kSize);
  f.Clear(kClearTarget0, 0xFF0000FF);
  px = f.Present(rt, "clear_viewport");
  CHECK_COLOR(px, 10, 30, FromArgb(0xFF0000FF), "inside the viewport");
  CHECK_COLOR(px, 50, 30, FromArgb(0xFF000000), "outside the viewport");
  CHECK(f.game->stats().clears >= 5);
}

// ----------------------------------------------------------------- draws ---

TEST(Draw_VertexColoursAndOrientation) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Clear(kClearTarget0, 0xFF000000);
  f.DrawQuadrants(p);
  Pixels px = f.Present(rt, "draw_quadrants");
  // Top-left quadrant (clip x < 0, y > 0) in the top-left of the picture:
  // +y is up, D3DCOLOR's ARGB reaches the shader as RGBA.
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.game->stats().draws, uint64_t(1));
  CHECK_EQ(f.game->stats().skipped_shader, uint64_t(0));
  CHECK_EQ(f.shaders->stats().compiled, uint64_t(3));  // the vertex and the two pixel shaders
  CHECK_EQ(f.shaders->stats().from_object, uint64_t(0));
}

TEST(Draw_IndexedAndPrimitiveTypes) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  // Four quads of four vertices each (TL, TR, BR, BL).
  std::vector<ColorVertex> quads;
  for (int q = 0; q < 4; ++q) {
    auto v = QuadVertices(kQuadrants[q], kQuadrantColors[q]);
    quads.insert(quads.end(), v.begin(), v.end());
  }
  const uint32_t vb = f.NewVertexBuffer(Bytes(quads));
  f.SetDeclaration(p.decl);
  f.SetShaders(p.vs, p.ps_interp);
  f.SetStream(0, vb, 0, 16);
  f.Clear(kClearTarget0, 0xFF000000);
  // 16-bit indices: quadrants 0 and 1.
  f.SetIndices(f.mem.NewIndexBuffer({0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7}, false));
  f.DrawIndexed(4, 0, 0, 12);
  // 32-bit indices with a base vertex: quadrant 2.
  f.SetIndices(f.mem.NewIndexBuffer({0, 1, 2, 0, 2, 3}, true));
  f.DrawIndexed(4, 8, 0, 6);
  // A start index past junk, and a base vertex: quadrant 3.
  f.SetIndices(f.mem.NewIndexBuffer({9, 9, 9, 9, 9, 9, 0, 1, 2, 0, 2, 3}, false));
  f.DrawIndexed(4, 12, 6, 6);
  Pixels px = f.Present(rt, "draw_indexed");
  f.CheckQuadrantColors(px, __LINE__);

  // Frame 2: a quad list, a fan, an indexed quad list and a strip read from a
  // stream offset.
  f.Clear(kClearTarget0, 0xFF000000);
  f.Draw(13, 0, 4);  // quadrant 0
  f.Draw(5, 4, 4);   // quadrant 1 (TL, TR, BR, BL is a fan)
  f.SetIndices(f.mem.NewIndexBuffer({0, 1, 2, 3}, false));
  f.DrawIndexed(13, 8, 0, 4);  // quadrant 2
  const Rect& q3 = kQuadrants[3];
  const uint32_t c3 = kQuadrantColors[3];
  std::vector<ColorVertex> strip = quads;  // the strip after the 16 quad vertices
  strip.push_back({q3.x0, q3.y1, 0.5f, c3});
  strip.push_back({q3.x1, q3.y1, 0.5f, c3});
  strip.push_back({q3.x0, q3.y0, 0.5f, c3});
  strip.push_back({q3.x1, q3.y0, 0.5f, c3});
  f.SetStream(0, f.NewVertexBuffer(Bytes(strip)), 16 * 16, 16);
  f.Draw(6, 0, 4);  // quadrant 3
  px = f.Present(rt, "draw_primitives");
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.game->stats().draws, uint64_t(7));
  CHECK_EQ(f.tracker->stats().dropped_draws, uint64_t(0));
}

TEST(Draw_ConstantsReachShaders) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline(true);
  f.Clear(kClearTarget0, 0xFF000000);
  // A top-left quad moved by the vertex constant c0 to the bottom right, in
  // the pixel constant c0's colour.
  f.SetConstants(true, 0, {1.0f, -1.0f, 0.0f, 0.0f});
  f.SetConstants(false, 0, {0.25f, 0.5f, 0.75f, 1.0f});
  f.DrawColored(p, QuadTriangles(kQuadrants[0], 0xFFFFFFFF), true);
  // The next draw with other constants: the top right, red.
  f.SetConstants(true, 0, {1.0f, 0.0f, 0.0f, 0.0f});
  f.SetConstants(false, 0, {1.0f, 0.0f, 0.0f, 1.0f});
  f.DrawColored(p, QuadTriangles(kQuadrants[0], 0xFFFFFFFF), true);
  Pixels px = f.Present(rt, "draw_constants");
  const Rgb black{0, 0, 0};
  const Rgb want[4] = {black, {1, 0, 0}, black, {0.25, 0.5, 0.75}};
  f.CheckQuadrants(px, want, __LINE__);
}

// ------------------------------------------------- pipeline state ---

// ------------------------------------------- vertex element endian ---
//
// The declaration's element type carries its own endian mode, and the
// library's buffers need not match it: one buffer can hold a position in
// 8in32 and a colour in 16in32. With element_endian (the default) each
// element is read with its own mode; without it the buffer's mode (the
// fetch constant of the vertex buffer object, 8in32 here) is used for all.

namespace {

uint32_t SwapForEndian(uint32_t v, uint32_t endian) {
  switch (endian) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    case 2: return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
  }
}
uint32_t TypeWithEndian(uint32_t type, uint32_t endian) { return (type & ~(3u << 6)) | (endian << 6); }

// The bytes in memory for a dword that a reader of `endian` turns into `value`.
void PutStored(std::vector<uint8_t>& out, uint32_t value, uint32_t endian) {
  const uint32_t stored = SwapForEndian(value, endian);  // the modes are their own inverse
  for (int s = 0; s < 32; s += 8) out.push_back(uint8_t(stored >> s));
}

// Position (three floats) and colour, each stored for its own endian mode.
std::vector<uint8_t> BytesWithEndians(const std::vector<ColorVertex>& vs, uint32_t pos_endian, uint32_t color_endian) {
  std::vector<uint8_t> out;
  for (const ColorVertex& v : vs) {
    for (float c : {v.x, v.y, v.z}) {
      uint32_t u;
      std::memcpy(&u, &c, 4);
      PutStored(out, u, pos_endian);
    }
    PutStored(out, v.argb, color_endian);
  }
  return out;
}

// The four quadrants drawn with a declaration saying `decl_*` and a buffer
// laid out for `data_*`.
Pixels QuadrantsWithEndians(Fixture& f, uint32_t decl_pos, uint32_t decl_color, uint32_t data_pos,
                            uint32_t data_color) {
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  p.decl = f.mem.NewDeclaration({{0, 0, TypeWithEndian(kDeclFloat3, decl_pos), kUsagePosition, 0},
                                 {0, 12, TypeWithEndian(kDeclColor, decl_color), kUsageColor, 0}});
  f.Clear(kClearTarget0, 0xFF000000);
  std::vector<ColorVertex> v;
  for (int q = 0; q < 4; ++q) {
    auto t = QuadTriangles(kQuadrants[q], kQuadrantColors[q]);
    v.insert(v.end(), t.begin(), t.end());
  }
  f.SetDeclaration(p.decl);
  f.SetShaders(p.vs, p.ps_interp);
  f.SetStream(0, f.NewVertexBuffer(BytesWithEndians(v, data_pos, data_color)), 0, 16);
  f.Draw(4, 0, uint32_t(v.size()));
  return f.Present(rt);
}

}  // namespace

TEST(Endian_ElementsReadWithTheirOwnMode) {
  // The colour in each of the other three modes, the position in the buffer's.
  for (uint32_t mode : {0u, 1u, 3u}) {
    Fixture f;
    if (!f.Init()) return;
    CHECK(f.game->options().element_endian);
    Pixels px = QuadrantsWithEndians(f, 2, mode, 2, mode);
    f.CheckQuadrantColors(px, __LINE__);
    CHECK_EQ(f.game->stats().draws, uint64_t(1));
  }
}

TEST(Endian_OneBufferWithDifferentModes) {
  // Position 16in32, colour 8in16, and then position none with colour 8in32:
  // elements of one buffer that disagree with each other and with the buffer.
  const uint32_t modes[2][2] = {{3, 1}, {0, 2}};
  for (const auto& m : modes) {
    Fixture f;
    if (!f.Init()) return;
    Pixels px = QuadrantsWithEndians(f, m[0], m[1], m[0], m[1]);
    f.CheckQuadrantColors(px, __LINE__);
  }
}

TEST(Endian_DeclarationWinsOverTheBuffer) {
  // The declaration says "none" for the colour but the bytes are laid out for
  // 8in32: the colour is read as the little-endian dword, bytes reversed.
  Fixture f;
  if (!f.Init()) return;
  Pixels px = QuadrantsWithEndians(f, 2, 0, 2, 2);
  Rgb want[4];
  for (int q = 0; q < 4; ++q) want[q] = FromArgb(SwapForEndian(kQuadrantColors[q], 2));
  f.CheckQuadrants(px, want, __LINE__);
}

TEST(Endian_LegacyUsesTheBuffersMode) {
  // element_endian off: every element is read with the buffer's mode (8in32),
  // whatever the declaration says.
  Fixture f;
  if (!f.Init()) return;
  f.game->options().element_endian = false;
  Pixels px = QuadrantsWithEndians(f, 3, 1, 2, 2);
  f.CheckQuadrantColors(px, __LINE__);
}

TEST(State_Blend) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Clear(kClearTarget0, 0xFF400000);  // 0.25 red
  // Left: ONE + ONE (colour and alpha).
  f.Reg(dev::kRbBlendControl0, 0x01010101);
  f.SetConstants(false, 0, {0.5f, 0.25f, 0.0f, 1.0f});
  f.DrawColored(p, QuadTriangles({-1, -1, 0, 1}, 0), true);
  // Right: SRCALPHA, INVSRCALPHA (alpha ONE, ZERO), half white.
  f.Reg(dev::kRbBlendControl0, 0x00010706);
  f.SetConstants(false, 0, {1.0f, 1.0f, 1.0f, 0.5f});
  f.DrawColored(p, QuadTriangles({0, -1, 1, 1}, 0), true);
  // Bottom right on top of that: REVSUBTRACT (dst - src) ONE, ONE.
  f.Reg(dev::kRbBlendControl0, 0x00010000 | 1 | (4 << 5) | (1 << 8));
  f.SetConstants(false, 0, {0.25f, 0.25f, 0.25f, 1.0f});
  f.DrawColored(p, QuadTriangles(kQuadrants[3], 0), true);
  // Back to opaque.
  f.Reg(dev::kRbBlendControl0, kBlendOpaque);
  Pixels px = f.Present(rt, "state_blend");
  const double red = 0x40 / 255.0;
  const Rgb half{0.5 + red * 0.5, 0.5, 0.5};
  const Rgb want[4] = {{red + 0.5, 0.25, 0.0}, half, {red + 0.5, 0.25, 0.0}, {half.r - 0.25, 0.25, 0.25}};
  f.CheckQuadrants(px, want, __LINE__);
  CHECK_EQ(f.game->stats().pipelines, uint64_t(4));  // three blend states and Present's blit
}

TEST(State_DepthTestWriteAndClear) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  f.SetDepthStencil(f.NewSurface(kSize, kSize, 200, 0));
  auto p = f.MakeColorPipeline();
  f.Clear(kClearTarget0 | kClearZ | kClearStencil, 0xFF000000, 1.0f);
  f.Reg(dev::kRbDepthControl, kDepthLess);
  f.DrawColored(p, QuadTriangles({-1, -1, 0, 1}, 0xFFFF0000, 0.3f));  // near, left
  f.DrawColored(p, QuadTriangles(kFull, 0xFF00FF00, 0.7f));           // far: right only
  // ALWAYS without writes on the top half, then LESS at 0.5: the top half's
  // depth is still 0.3 (left) / 0.7 (right).
  f.Reg(dev::kRbDepthControl, 0x2 | (7 << 4));
  f.DrawColored(p, QuadTriangles({-1, 0, 1, 1}, 0xFF0000FF, 0.9f));
  f.Reg(dev::kRbDepthControl, kDepthLess);
  f.DrawColored(p, QuadTriangles({-1, 0, 1, 1}, 0xFFFFFFFF, 0.5f));
  Pixels px = f.Present(rt, "state_depth");
  const Rgb want[4] = {{0, 0, 1}, {1, 1, 1}, {1, 0, 0}, {0, 1, 0}};
  f.CheckQuadrants(px, want, __LINE__);

  // A depth-only clear to 0.5, then LESS at 0.7 (hidden), 0.3 (shown) and
  // GREATER at 0.7 (shown).
  f.Clear(kClearTarget0, 0xFF000000);
  f.Clear(kClearZ, 0, 0.5f);
  f.DrawColored(p, QuadTriangles(kQuadrants[0], 0xFFFF0000, 0.7f));
  f.DrawColored(p, QuadTriangles(kQuadrants[1], 0xFF00FF00, 0.3f));
  f.Reg(dev::kRbDepthControl, 0x2 | 0x4 | (4 << 4));
  f.DrawColored(p, QuadTriangles(kQuadrants[2], 0xFF0000FF, 0.7f));
  // Depth test off: drawn whatever the depth.
  f.Reg(dev::kRbDepthControl, 0);
  f.DrawColored(p, QuadTriangles(kQuadrants[3], 0xFFFFFFFF, 0.99f));
  px = f.Present(rt, "state_depth_clear");
  const Rgb want2[4] = {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};
  f.CheckQuadrants(px, want2, __LINE__);
}

TEST(State_Cull) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  // Clockwise on screen: bottom left, top, bottom right.
  const std::vector<ColorVertex> cw = {{-0.8f, -0.8f, 0.5f, 0xFFFFFFFF}, {0.0f, 0.8f, 0.5f, 0xFFFFFFFF},
                                       {0.8f, -0.8f, 0.5f, 0xFFFFFFFF}};
  const std::vector<ColorVertex> ccw = {cw[0], cw[2], cw[1]};
  struct Case {
    uint32_t mode;
    const std::vector<ColorVertex>* triangle;
    bool drawn;
    const char* what;
  };
  // PA_SU_SC_MODE_CNTL: bit 0 cull front, bit 1 cull back, bit 2 face (0 =
  // counter-clockwise is the front).
  const Case cases[] = {
      {0, &cw, true, "no culling"},
      {2, &cw, false, "cull back, CCW front: a CW triangle is a back face"},
      {1, &cw, true, "cull front, CCW front"},
      {2 | 4, &cw, true, "cull back, CW front"},
      {1 | 4, &cw, false, "cull front, CW front"},
      {2, &ccw, true, "cull back, CCW front, a CCW triangle"},
  };
  for (const Case& c : cases) {
    f.Clear(kClearTarget0, 0xFF000000);
    f.Reg(dev::kPaSuScModeCntl, c.mode);
    f.DrawColored(p, *c.triangle);
    Pixels px = f.Present(rt);
    CHECK_COLOR(px, 32, 40, (c.drawn ? Rgb{1, 1, 1} : Rgb{0, 0, 0}), c.what);
    CHECK_COLOR(px, 4, 4, (Rgb{0, 0, 0}), c.what);
  }
  f.Reg(dev::kPaSuScModeCntl, 0);
}

TEST(State_ColourMask) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Clear(kClearTarget0, 0xFF0000FF);
  f.Reg(dev::kRbColorMask, 0x1);  // red only
  f.DrawColored(p, QuadTriangles(kQuadrants[0], 0xFFFFFFFF));
  f.Reg(dev::kRbColorMask, 0x2);  // green only
  f.DrawColored(p, QuadTriangles(kQuadrants[1], 0xFFFFFFFF));
  f.Reg(dev::kRbColorMask, 0x0);  // nothing
  f.DrawColored(p, QuadTriangles(kQuadrants[2], 0xFFFFFFFF));
  f.Reg(dev::kRbColorMask, 0xFFFF);
  f.DrawColored(p, QuadTriangles(kQuadrants[3], 0xFFFFFF00));
  Pixels px = f.Present(rt, "state_mask");
  const Rgb want[4] = {{1, 0, 1}, {0, 1, 1}, {0, 0, 1}, {1, 1, 0}};
  f.CheckQuadrants(px, want, __LINE__);
}

TEST(State_MultipleRenderTargets) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt0 = f.BindMainSurface();
  f.SetRenderTarget(1, f.NewSurface(kSize, kSize, 300, 0));
  const uint32_t vs = f.CreateShader(true, VertexShader(kColor, F_8_8_8_8));
  const uint32_t ps = f.CreateShader(false, PixelShaderConstants(2));
  const uint32_t decl =
      f.mem.NewDeclaration({{0, 0, kDeclFloat3, kUsagePosition, 0}, {0, 12, kDeclColor, kUsageColor, 0}});
  f.Clear(0x3, 0xFF000000);  // both targets
  f.SetDeclaration(decl);
  f.SetShaders(vs, ps);
  f.SetConstants(false, 0, {1.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.25f, 1.0f, 1.0f});
  f.SetStream(0, f.NewVertexBuffer(Bytes(QuadTriangles({-1, -1, 0, 1}, 0))), 0, 16);
  f.Draw(4, 0, 6);  // left half, both targets
  // The right half with target 1 masked off (RB_COLOR_MASK bits 4-7).
  f.Reg(dev::kRbColorMask, 0x0F);
  f.SetStream(0, f.NewVertexBuffer(Bytes(QuadTriangles({0, -1, 1, 1}, 0))), 0, 16);
  f.Draw(4, 0, 6);
  f.Reg(dev::kRbColorMask, 0xFFFF);
  Pixels px0 = f.Present(rt0, "mrt_0");
  Pixels px1 = f.ReadTarget(false, 300, 0);
  MaybeSave(px1, "mrt_1");
  CHECK_COLOR(px0, 16, 32, (Rgb{1, 0.5, 0}), "target 0, left");
  CHECK_COLOR(px0, 48, 32, (Rgb{1, 0.5, 0}), "target 0, right");
  CHECK_COLOR(px1, 16, 32, (Rgb{0, 0.25, 1}), "target 1, left");
  CHECK_COLOR(px1, 48, 32, (Rgb{0, 0, 0}), "target 1, right (masked)");
}

// ------------------------------------------------------ render targets ---

TEST(Targets_KeyedByEdramPlacement) {
  Fixture f;
  if (!f.Init()) return;
  auto p = f.MakeColorPipeline();
  f.SetViewport(0, 0, kSize, kSize);
  auto targets = [&] { return f.game->DebugTargets(); };
  auto find = [&](bool depth, uint32_t base, uint32_t format, uint32_t pitch) -> const GameRenderer::TargetInfo* {
    static std::vector<GameRenderer::TargetInfo> list;
    list = targets();
    for (const auto& t : list)
      if (t.depth == depth && t.edram_base == base && t.format == format && t.pitch == pitch) return &t;
    return nullptr;
  };
  auto draw_into = [&](uint32_t surface, uint32_t argb) {
    f.SetRenderTarget(0, surface);
    f.DrawColored(p, QuadTriangles(kFull, argb));
  };
  const uint32_t a = f.NewSurface(kSize, kSize, 0, 0);
  draw_into(a, 0xFFFF0000);
  CHECK_EQ(targets().size(), size_t(1));
  nvrhi::ITexture* first = find(false, 0, 0, kSize) ? find(false, 0, 0, kSize)->texture : nullptr;
  // Another object at the same placement: the same host target.
  const uint32_t a2 = f.NewSurface(kSize, kSize, 0, 0);
  draw_into(a2, 0xFFFF0000);
  CHECK_EQ(targets().size(), size_t(1));
  // Another EDRAM base: a new one.
  draw_into(f.NewSurface(kSize, kSize, 100, 0), 0xFF00FF00);
  CHECK_EQ(targets().size(), size_t(2));
  // The same base in other formats: new ones, with their host formats.
  draw_into(f.NewSurface(kSize, kSize, 0, 2), 0xFF0000FF);  // k_2_10_10_10
  draw_into(f.NewSurface(kSize, kSize, 0, 7), 0xFF0000FF);  // k_16_16_16_16_FLOAT
  CHECK_EQ(targets().size(), size_t(4));
  if (auto* t = find(false, 0, 2, kSize)) CHECK(t->host_format == nvrhi::Format::R10G10B10A2_UNORM);
  else CHECK(false);
  if (auto* t = find(false, 0, 7, kSize)) CHECK(t->host_format == nvrhi::Format::RGBA16_FLOAT);
  else CHECK(false);
  // A depth surface at base 0: its own target.
  f.SetRenderTarget(0, a);
  f.SetDepthStencil(f.NewSurface(kSize, kSize, 0, 0));
  f.DrawColored(p, QuadTriangles(kFull, 0xFFFF0000));
  f.SetDepthStencil(0);
  CHECK_EQ(targets().size(), size_t(5));
  if (auto* t = find(true, 0, 0, kSize)) CHECK(t->host_format == nvrhi::Format::D32S8);
  else CHECK(false);
  // Another pitch: another target.
  draw_into(f.NewSurface(32, 32, 0, 0), 0xFFFFFFFF);
  CHECK_EQ(targets().size(), size_t(6));
  if (auto* t = find(false, 0, 0, 32)) CHECK_EQ(t->width, 32u);
  else CHECK(false);
  // The first placement, untouched by the others: still red.
  f.SetRenderTarget(0, a);
  Pixels px = f.Present(a, "targets_first");
  CHECK_COLOR(px, 32, 32, (Rgb{1, 0, 0}), "the first placement kept its contents");
  if (auto* t = find(false, 0, 0, kSize)) CHECK(t->texture == first);
  // Taller at the same placement: the target is re-created at the new height.
  draw_into(f.NewSurface(kSize, 2 * kSize, 0, 0), 0xFF00FF00);
  if (auto* t = find(false, 0, 0, kSize)) {
    CHECK_EQ(t->height, 2 * kSize);
    CHECK(t->texture != first);
  } else {
    CHECK(false);
  }
  CHECK_EQ(targets().size(), size_t(6));
  f.Present(a);
}

// ------------------------------------------------------------ resolves ---

namespace {

// Draws the quadrants into the main surface and resolves it into `texture`;
// then draws that texture into a second surface (EDRAM 400) and presents it.
Pixels ResolveAndShow(Fixture& f, uint32_t texture, const char* name, uint32_t flags = 0) {
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Clear(kClearTarget0, 0xFF000000);
  f.DrawQuadrants(p);
  f.Resolve(flags, 0, texture);
  const uint32_t shown = f.NewSurface(kSize, kSize, 400, 0);
  f.SetRenderTarget(0, shown);
  f.DrawTextured(texture);
  (void)rt;
  return f.Present(shown, name);
}

kknr::TextureFetchDesc Desc(kknr::TextureFormat format, kknr::Endian endian, uint16_t swizzle, uint32_t size = kSize,
                            bool tiled = false) {
  kknr::TextureFetchDesc d;
  d.format = format;
  d.endian = endian;
  d.width = d.height = size;
  d.swizzle = swizzle;
  d.tiled = tiled;
  return d;
}

constexpr uint16_t kSwizzleZYXW = kknr::MakeSwizzle(kknr::kSwzZ, kknr::kSwzY, kknr::kSwzX, kknr::kSwzW);

}  // namespace

TEST(Resolve_IntoEndianNoneTexture) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t tex =
      f.NewTexture(Desc(kknr::TextureFormat::k_8_8_8_8, kknr::Endian::kNone, kknr::kSwizzleXYZW));
  Pixels px = ResolveAndShow(f, tex, "resolve_endian_none");
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.game->stats().resolves, uint64_t(1));
  CHECK_EQ(f.game->stats().resolve_failures, uint64_t(0));
  CHECK_EQ(f.game->stats().texture_uploads, uint64_t(0));  // the resolve's copy is the texture
}

TEST(Resolve_IntoArgbTexture) {
  // A8R8G8B8 (8in32, X = blue): the resolve exchanges R and B, the view's
  // swizzle exchanges them back.
  Fixture f;
  if (!f.Init()) return;
  const uint32_t tex = f.NewTexture(Desc(kknr::TextureFormat::k_8_8_8_8, kknr::Endian::k8in32, kSwizzleZYXW, kSize, true));
  Pixels px = ResolveAndShow(f, tex, "resolve_argb");
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.game->stats().resolve_failures, uint64_t(0));
}

TEST(Resolve_IntoK8Texture) {
  // k_8: one channel, the source's red; the fetch swizzle spreads it (X X X 1).
  Fixture f;
  if (!f.Init()) return;
  const uint16_t xxx1 = kknr::MakeSwizzle(kknr::kSwzX, kknr::kSwzX, kknr::kSwzX, kknr::kSwz1);
  const uint32_t tex = f.NewTexture(Desc(kknr::TextureFormat::k_8, kknr::Endian::kNone, xxx1));
  Pixels px = ResolveAndShow(f, tex, "resolve_k8");
  Rgb want[4];
  for (int q = 0; q < 4; ++q) {
    const double red = FromArgb(kQuadrantColors[q]).r;
    want[q] = {red, red, red};
  }
  f.CheckQuadrants(px, want, __LINE__);
  CHECK_EQ(f.game->stats().resolve_failures, uint64_t(0));
}

TEST(Resolve_RectAndDestinationPoint) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t tex =
      f.NewTexture(Desc(kknr::TextureFormat::k_8_8_8_8, kknr::Endian::kNone, kknr::kSwizzleXYZW));
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Clear(kClearTarget0, 0xFF000000);
  f.DrawQuadrants(p);
  f.Resolve(0, 0, tex);
  // The top-right quadrant over the bottom-left one.
  f.Resolve(0, f.NewRect(32, 0, 64, 32), tex, f.NewPoint(0, 32));
  const uint32_t shown = f.NewSurface(kSize, kSize, 400, 0);
  f.SetRenderTarget(0, shown);
  f.DrawTextured(tex);
  Pixels px = f.Present(shown, "resolve_rect");
  Rgb want[4];
  for (int q = 0; q < 4; ++q) want[q] = FromArgb(kQuadrantColors[q == 2 ? 1 : q]);
  f.CheckQuadrants(px, want, __LINE__);
  (void)rt;
}

TEST(Resolve_Clears) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  f.SetDepthStencil(f.NewSurface(kSize, kSize, 200, 0));
  auto p = f.MakeColorPipeline();
  f.Clear(kClearTarget0 | kClearZ, 0xFF000000, 1.0f);
  f.Reg(dev::kRbDepthControl, kDepthLess);
  f.DrawColored(p, QuadTriangles(kFull, 0xFFFF0000, 0.3f));
  // Resolve (no destination) clearing the depth of the left half to 1.0.
  f.Resolve(0x200, f.NewRect(0, 0, 32, 64), 0, 0, 0, 1.0f);
  f.DrawColored(p, QuadTriangles(kFull, 0xFF00FF00, 0.7f));
  // Resolve clearing the colour of the top half to (0.2, 0.4, 0.6, 1).
  const uint32_t color = f.mem.NewFloats({0.2f, 0.4f, 0.6f, 1.0f});
  f.Resolve(0x100, f.NewRect(0, 0, 64, 32), 0, 0, color);
  Pixels px = f.Present(rt, "resolve_clears");
  const Rgb blue{0.2, 0.4, 0.6};
  const Rgb want[4] = {blue, blue, {0, 1, 0}, {1, 0, 0}};
  f.CheckQuadrants(px, want, __LINE__);
  CHECK_EQ(f.game->stats().resolve_failures, uint64_t(0));
}

// ------------------------------------------------------ shaders, textures ---

TEST(Shaders_ParsedFromObjectWithoutCreateHook) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  Fixture::ColorPipeline p;
  p.vs = f.ShaderObject(true, VertexShader(kColor, F_8_8_8_8));
  p.ps_interp = f.ShaderObject(false, PixelShaderInterpolator());
  p.decl = f.mem.NewDeclaration({{0, 0, kDeclFloat3, kUsagePosition, 0}, {0, 12, kDeclColor, kUsageColor, 0}});
  f.Clear(kClearTarget0, 0xFF000000);
  f.DrawQuadrants(p);
  Pixels px = f.Present(rt, "shader_from_object");
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.shaders->stats().created, uint64_t(0));
  CHECK_EQ(f.shaders->stats().from_object, uint64_t(2));
  CHECK_EQ(f.shaders->stats().failed, uint64_t(0));
}

TEST(Textures_UploadSampleAndInvalidate) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  // A tiled 4x4 A8R8G8B8 texture, one colour per texel. Host-order blocks:
  // X = blue, Y = green, Z = red, W = alpha (the swizzle reads Z as red).
  auto texel = [](uint32_t i, uint32_t salt) -> std::array<uint8_t, 4> {
    return {uint8_t(i * 16 + salt), uint8_t(255 - i * 12), uint8_t((i * 37 + salt) & 255), 255};
  };
  std::vector<uint8_t> blocks(4 * 4 * 4);
  auto fill = [&](uint32_t salt) {
    for (uint32_t i = 0; i < 16; ++i) {
      auto t = texel(i, salt);
      std::memcpy(&blocks[i * 4], t.data(), 4);
    }
  };
  fill(0);
  uint32_t base = 0, bytes = 0;
  const auto desc = Desc(kknr::TextureFormat::k_8_8_8_8, kknr::Endian::k8in32, kSwizzleZYXW, 4, true);
  const uint32_t tex = f.NewTexture(desc, [&](uint32_t, uint32_t) { return blocks.data(); }, &base, &bytes);
  auto check = [&](const Pixels& px, uint32_t salt, int line) {
    for (uint32_t i = 0; i < 16; ++i) {
      auto t = texel(i, salt);
      const Rgb want{t[2] / 255.0, t[1] / 255.0, t[0] / 255.0};
      CheckColor(px, (i % 4) * 16 + 8, (i / 4) * 16 + 8, want, "texel", line);
    }
  };
  f.Clear(kClearTarget0, 0xFF000000);
  f.DrawTextured(tex);
  Pixels px = f.Present(rt, "texture_upload");
  check(px, 0, __LINE__);
  CHECK_EQ(f.game->stats().texture_uploads, uint64_t(1));

  // Drawn again unchanged: no upload.
  f.DrawTextured(tex);
  f.Present(rt);
  CHECK_EQ(f.game->stats().texture_uploads, uint64_t(1));

  // The CPU rewrites the texels and the write watch reports it.
  fill(100);
  kknr::GuestTextureImage image;
  kknr::EncodeGuestTexture(kknr::MakeTextureFetch([&] {
                             auto d = desc;
                             d.base_address = base - FakeGuestMemory::kPhysicalView;
                             return d;
                           }()),
                           [&](uint32_t, uint32_t) { return blocks.data(); }, image);
  std::memcpy(f.mem.Writable(base), image.base.data(), image.base.size());
  f.game->InvalidateRange(base - FakeGuestMemory::kPhysicalView, bytes);
  f.DrawTextured(tex);
  px = f.Present(rt, "texture_reupload");
  check(px, 100, __LINE__);
  CHECK_EQ(f.game->stats().texture_uploads, uint64_t(2));
  CHECK_EQ(f.game->stats().texture_failures, uint64_t(0));
}

// ------------------------------------------------------------- frame log ---
//
// REX_DEV_FRAME_LOG lines from the game renderer, against values worked out here from the
// containers and the registers the fixture sets (backend/frame_log.h has the formats).

namespace {

struct LoggedFixture {
  Fixture f;
  std::vector<std::string> lines;
  bool Init(const char* option = "0") {
    if (!f.Init()) return false;
    f.game->SetFrameLog(FrameLogConfig::Parse(option), [this](const std::string& l) { lines.push_back(l); });
    return true;
  }
  std::vector<std::string> With(const char* prefix) const {
    std::vector<std::string> out;
    for (const std::string& l : lines)
      if (l.rfind(prefix, 0) == 0) out.push_back(l);
    return out;
  }
};

// The hash the Xenos log would print for a vertex shader: the container's microcode patched for
// the colour declaration (position FLOAT3 at 0, D3DCOLOR at 12) and stream 0's stride.
uint64_t ExpectedVertexHash(const std::vector<uint8_t>& container, uint32_t stride, uint32_t color_offset = 12) {
  kkshaders::ParseResult parsed = kkshaders::parseContainer(container);
  const auto element = [](uint8_t stream, uint16_t offset, uint32_t type, kkshaders::DeclUsage usage) {
    kkshaders::DeclElement e;
    e.stream = stream;
    e.offset = offset;
    e.format = uint8_t(type & 63);
    e.isSigned = ((type >> 8) & 1) != 0;
    e.integer = ((type >> 9) & 1) != 0;
    e.usage = usage;
    return e;
  };
  const kkshaders::DeclElement decl[] = {element(0, 0, kDeclFloat3, kkshaders::DeclUsage::Position),
                                         element(0, uint16_t(color_offset), kDeclColor, kkshaders::DeclUsage::Color)};
  uint32_t strides[16] = {stride};
  return kkshaders::hashMicrocode(kkshaders::patchVertexFetches(parsed.info, decl, strides).ucode);
}

}  // namespace

TEST(FrameLog_OffByDefault) {
  Fixture f;
  if (!f.Init()) return;
  CHECK(f.game->frame_log() == nullptr);
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.DrawQuadrants(p);
  f.Present(rt);
  CHECK_EQ(f.game->stats().draws, uint64_t(1));
}

TEST(FrameLog_OneFrameOfClearDrawAndPresent) {
  LoggedFixture t;
  if (!t.Init()) return;
  Fixture& f = t.f;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Present(rt);  // the first swap: the frame starts
  f.Clear(kClearTarget0, 0xFF000000);
  f.DrawQuadrants(p);
  f.Present(rt);  // ends it
  f.DrawQuadrants(p);
  f.Present(rt);  // quiet again
  if (t.lines.size() != 5) {
    for (const std::string& l : t.lines) std::printf("  %s\n", l.c_str());
  }
  CHECK_EQ(t.lines.size(), size_t(5));
  if (t.lines.size() != 5) return;
  CHECK_EQ(t.lines[0], std::string("Frame log: frame start"));
  // A clear goes through the resolve path: a null copy that clears.
  CHECK_EQ(t.lines[1],
           std::string("Frame log: resolve 0 rt0@0:f0 -> 00000000 format 0 pitch 0 height 0 command 3 clear color 1 depth 0 "
                       "off 0,0 surface pitch 64 msaa 1"));
  FrameLogDraw want;
  want.ps_hash = kkshaders::parseContainer(PixelShaderInterpolator()).info.ucodeHash;
  want.vs_hash = ExpectedVertexHash(VertexShader(kColor, F_8_8_8_8), 16);
  want.pitch = kSize;
  want.msaa = 1;
  want.scissor_w = want.scissor_h = kSize;
  want.targets = {{0, 0, 0}};
  want.count = 24;
  CHECK_EQ(t.lines[2], FrameLog::FormatDraw(0, want));
  // The Present's own copy of the back buffer (no front buffer texture in the fixture).
  CHECK(t.lines[3].rfind("Frame log: resolve 1 rt0@0:f0 -> ", 0) == 0);
  CHECK_EQ(t.lines[4], std::string("Frame log: frame end, 1 draws, 2 resolves"));
}

TEST(FrameLog_VertexHashFollowsTheStrides) {
  // The same shader and declaration with a wider vertex: the library patches another copy.
  LoggedFixture t;
  if (!t.Init()) return;
  Fixture& f = t.f;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Present(rt);
  f.DrawQuadrants(p);
  // Stride 20: each vertex followed by a padding dword.
  std::vector<uint8_t> wide;
  for (const ColorVertex& v : QuadTriangles(kFull, 0xFFFFFFFF)) {
    PutBEFloat(wide, v.x);
    PutBEFloat(wide, v.y);
    PutBEFloat(wide, v.z);
    PutBE32(wide, v.argb);
    PutBE32(wide, 0);
  }
  f.SetStream(0, f.NewVertexBuffer(wide), 0, 20);
  f.Draw(4, 0, 6);
  f.Present(rt);
  std::vector<std::string> draws = t.With("Frame log: draw ");
  CHECK_EQ(draws.size(), size_t(2));
  if (draws.size() != 2) return;
  const std::vector<uint8_t> container = VertexShader(kColor, F_8_8_8_8);
  auto vs_of = [](const std::string& line) { return line.substr(line.find(" vs ") + 4, 16); };
  char want16[17], want20[17];
  std::snprintf(want16, sizeof(want16), "%016llX", static_cast<unsigned long long>(ExpectedVertexHash(container, 16)));
  std::snprintf(want20, sizeof(want20), "%016llX", static_cast<unsigned long long>(ExpectedVertexHash(container, 20)));
  CHECK_EQ(vs_of(draws[0]), std::string(want16));
  CHECK_EQ(vs_of(draws[1]), std::string(want20));
  CHECK(vs_of(draws[0]) != vs_of(draws[1]));
  // Neither is the template's hash (what the shader library keys by).
  char tmpl[17];
  std::snprintf(tmpl, sizeof(tmpl), "%016llX", static_cast<unsigned long long>(kkshaders::parseContainer(container).info.ucodeHash));
  CHECK(vs_of(draws[0]) != std::string(tmpl));
}

TEST(FrameLog_ResolveIntoATexture) {
  LoggedFixture t;
  if (!t.Init()) return;
  Fixture& f = t.f;
  const uint32_t tex = f.NewTexture(Desc(kknr::TextureFormat::k_8_8_8_8, kknr::Endian::kNone, kknr::kSwizzleXYZW));
  const uint32_t rt = f.BindMainSurface();
  f.Present(rt);
  f.Resolve(0x100 /* clear the colour too */, 0, tex, 0, 0xFF000000);
  f.Present(rt);
  std::vector<std::string> resolves = t.With("Frame log: resolve 0 ");
  CHECK_EQ(resolves.size(), size_t(1));
  if (resolves.empty()) return;
  // Source rt0 at EDRAM 0 in k_8_8_8_8 (format 6 as a texture), a 64x64 destination, the copy
  // (the formats agree), clearing the colour.
  CHECK(resolves[0].find(" rt0@0:f0 -> ") != std::string::npos);
  CHECK(resolves[0].find(" format 6 pitch 64 height 64 command 0 clear color 1 depth 0 ") != std::string::npos);
}

TEST(FrameLog_DrawWhosePipelineIsNotReady) {
  // The plugin's default: a pipeline is made on a worker, the draw waits for the next frame, and
  // the log says so (the line the Vulkan Xenos log has for its placeholder pipelines).
  LoggedFixture t;
  if (!t.Init("0,2")) return;
  Fixture& f = t.f;
  f.game->options().async_pipelines = true;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Present(rt);
  f.DrawQuadrants(p);
  CHECK_EQ(f.game->stats().skipped_pending, uint64_t(1));
  f.Present(rt);
  for (int i = 0; i < 500 && f.game->stats().pipelines == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    f.DrawQuadrants(p);  // polls the worker's result
    if (f.game->stats().pipelines) break;
  }
  f.Present(rt);
  const std::vector<std::string> notes = t.With("Frame log: draw 0 pipeline ");
  CHECK(!notes.empty());
  if (!notes.empty()) CHECK_EQ(notes[0], std::string("Frame log: draw 0 pipeline placeholder (skipped)"));
}

TEST(FrameLog_TheHarnessReadsTheLines) {
  // The lines through tests/compare.py's own parser (the tool that diffs them against a golden
  // log): it must find the frame, the draw's shaders and no line it does not understand.
#ifdef NR_COMPARE_PY
  LoggedFixture t;
  if (!t.Init()) return;
  Fixture& f = t.f;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Present(rt);
  f.Clear(kClearTarget0, 0xFF000000);
  f.DrawQuadrants(p);
  f.Present(rt);
  const std::string path = "nr_game_tests_framelog.txt";
  {
    std::ofstream out(path);
    for (const std::string& l : t.lines) out << "[warning] [gpu] " << l << "\n";
  }
  const std::string command = std::string("python3 -I \"") + NR_COMPARE_PY + "\" framelog " + path + " > " + path + ".out 2>&1";
  CHECK_EQ(std::system(command.c_str()), 0);
  std::ifstream in(path + ".out");
  std::stringstream text;
  text << in.rdbuf();
  CHECK(text.str().find("# frame 0: 1 draws, 2 resolves") != std::string::npos);
  CHECK(text.str().find("draw ps=") != std::string::npos);
  CHECK(text.str().find("not understood") == std::string::npos && text.str().find("unknown") == std::string::npos);
#endif
}

// -------------------------------------------------------- pipeline cache ---
//
// Cold pipeline caches: the descriptions the draws need go to a file under the cache root and
// are created on the workers at the next start; with pipeline_wait a draw waits for its
// pipeline instead of being skipped. Synthetic draws throughout (the quadrant scene).

namespace {

std::string CachePath(const char* name) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "nr_game_tests_pipelines";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / (std::string(name) + ".pipelines")).string();
  std::filesystem::remove(path);
  return path;
}

// A pack with the colour scene's shaders (what db-build makes for the whole database), so the
// library can make them from their hashes alone.
std::string MakePack(const char* name) {
  const std::string path = CachePath(name) + ".pack";
  std::string error;
  CHECK(kkshaders::loadDxc(NR_TEST_DXC_DIR, &error));
  kkshaders::Compiler compiler;
  std::vector<kkshaders::CompiledShader> shaders;
  for (const std::vector<uint8_t>& container :
       {VertexShader(kColor, F_8_8_8_8), PixelShaderInterpolator(), PixelShaderConstants()}) {
    kkshaders::ParseResult parsed = kkshaders::parseContainer(container);
    kkshaders::BuildResult built = kkshaders::buildShader(parsed.info, compiler, false);
    CHECK(built.ok);
    if (built.ok) shaders.push_back(std::move(built.shader));
  }
  CHECK(kkshaders::ShaderPack::write(path, shaders, &error));
  return path;
}

// One frame of the quadrant scene, presented and read back.
Pixels DrawQuadrantFrame(Fixture& f, uint32_t rt, const Fixture::ColorPipeline& p) {
  f.Clear(kClearTarget0, 0xFF000000);
  f.DrawQuadrants(p);
  return f.Present(rt);
}

// A first run that only records: the scene drawn once into `path`.
void RecordTheScene(const std::string& path, uint64_t expect_records = 1) {
  Fixture f;
  if (!f.Init()) return;
  CHECK(f.game->SetPipelineCache(path));
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  DrawQuadrantFrame(f, rt, p);
  CHECK_EQ(f.game->stats().cache_recorded, expect_records);
}

}  // namespace

TEST(PipelineCache_RecordsEachPipelineOnce) {
  const std::string path = CachePath("record_once");
  {
    Fixture f;
    if (!f.Init()) return;
    CHECK(f.game->SetPipelineCache(path));
    const uint32_t rt = f.BindMainSurface();
    auto p = f.MakeColorPipeline();
    DrawQuadrantFrame(f, rt, p);
    DrawQuadrantFrame(f, rt, p);
    CHECK_EQ(f.game->stats().cache_recorded, uint64_t(1));
    f.Reg(dev::kRbColorMask, 0x7777);  // another write mask: another pipeline
    DrawQuadrantFrame(f, rt, p);
    DrawQuadrantFrame(f, rt, p);
    CHECK_EQ(f.game->stats().cache_recorded, uint64_t(2));
    CHECK_EQ(f.game->stats().draw_pipelines, uint64_t(2));
  }
  PipelineCacheFile file;
  CHECK(file.Open(path));
  CHECK_EQ(file.loaded().size(), size_t(2));
  if (file.loaded().size() == 2) {
    const PipelineRecord& r = file.loaded()[0];
    CHECK_EQ(r.color_count, 1u);
    CHECK_EQ(r.color_mask[0], 0xFu);
    CHECK(r.ps_hash != 0 && r.vs_hash != 0);
    CHECK_EQ(file.loaded()[1].color_mask[0], 0x7u);
  }
}

TEST(PipelineCache_NothingIsRecordedWhenOff) {
  Fixture f;
  if (!f.Init()) return;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  DrawQuadrantFrame(f, rt, p);
  CHECK_EQ(f.game->stats().cache_recorded, uint64_t(0));
  CHECK_EQ(f.game->stats().prewarm_queued, uint64_t(0));
}

TEST(PipelineCache_PrewarmFromThePackBeforeTheGameCreatesAnything) {
  const std::string path = CachePath("prewarm_pack");
  const std::string pack = MakePack("prewarm_pack");
  RecordTheScene(path);
  // The next run: the cache is read at start, before a shader or a draw exists.
  Fixture f;
  if (!f.Init()) return;
  f.shaders->Initialize(pack, NR_TEST_DXC_DIR);
  f.game->options().async_pipelines = true;
  CHECK(f.game->SetPipelineCache(path));
  CHECK_EQ(f.game->stats().prewarm_queued, uint64_t(1));
  CHECK_EQ(f.game->stats().prewarm_deferred, uint64_t(0));
  f.game->WaitForPrewarm();
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  Pixels px = DrawQuadrantFrame(f, rt, p);
  // The first draw found its pipeline made: not skipped, no wait, no second creation.
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.game->stats().draws, uint64_t(1));
  CHECK_EQ(f.game->stats().skipped_pending, uint64_t(0));
  CHECK_EQ(f.game->stats().pipeline_waits, uint64_t(0));
  CHECK_EQ(f.game->stats().draw_pipelines, uint64_t(1));
  CHECK_EQ(f.game->stats().cache_recorded, uint64_t(0));  // known already
}

TEST(PipelineCache_PrewarmWaitsForShadersTheGameCreates) {
  const std::string path = CachePath("prewarm_late");
  RecordTheScene(path);
  // No pack: the shaders exist when the game creates them, and the swap that follows tries again.
  Fixture f;
  if (!f.Init()) return;
  f.game->options().async_pipelines = true;
  CHECK(f.game->SetPipelineCache(path));
  CHECK_EQ(f.game->stats().prewarm_queued, uint64_t(0));
  CHECK_EQ(f.game->stats().prewarm_deferred, uint64_t(1));
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.Present(rt);  // the frame's end retries
  CHECK_EQ(f.game->stats().prewarm_queued, uint64_t(1));
  f.game->WaitForPrewarm();
  Pixels px = DrawQuadrantFrame(f, rt, p);
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.game->stats().draws, uint64_t(1));
  CHECK_EQ(f.game->stats().skipped_pending, uint64_t(0));
}

TEST(PipelineCache_RecordsOfShadersThatNeverComeStayHarmless) {
  const std::string path = CachePath("unknown_shaders");
  {
    PipelineCacheFile file;
    CHECK(file.Open(path));
    PipelineRecord r;
    r.vs_hash = 0xDEAD;
    r.ps_hash = 0xBEEF;
    r.topology = uint32_t(nvrhi::PrimitiveType::TriangleList);
    r.color_count = 1;
    r.color_format[0] = uint32_t(nvrhi::Format::RGBA8_UNORM);
    r.color_mask[0] = 0xF;
    file.Append(r);
  }
  Fixture f;
  if (!f.Init()) return;
  f.game->options().async_pipelines = true;
  CHECK(f.game->SetPipelineCache(path));
  CHECK_EQ(f.game->stats().prewarm_deferred, uint64_t(1));
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  f.game->options().pipeline_wait = true;
  Pixels px = DrawQuadrantFrame(f, rt, p);
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.game->stats().draws, uint64_t(1));
  CHECK_EQ(f.game->stats().prewarm_failed, uint64_t(0));
}

TEST(PipelineWait_TheDrawWaitsInsteadOfBeingSkipped) {
  // Default: the draw is skipped while its pipeline is made. With pipeline_wait it waits for it.
  {
    Fixture f;
    if (!f.Init()) return;
    f.game->options().async_pipelines = true;
    const uint32_t rt = f.BindMainSurface();
    auto p = f.MakeColorPipeline();
    DrawQuadrantFrame(f, rt, p);
    CHECK_EQ(f.game->stats().skipped_pending, uint64_t(1));
    CHECK_EQ(f.game->stats().pipeline_waits, uint64_t(0));
  }
  Fixture f;
  if (!f.Init()) return;
  f.game->options().async_pipelines = true;
  f.game->options().pipeline_wait = true;
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  Pixels px = DrawQuadrantFrame(f, rt, p);
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.game->stats().draws, uint64_t(1));
  CHECK_EQ(f.game->stats().skipped_pending, uint64_t(0));
  CHECK_EQ(f.game->stats().pipeline_waits, uint64_t(1));
  CHECK_EQ(f.game->stats().draw_pipelines, uint64_t(1));
  // The second frame finds it made: no wait.
  DrawQuadrantFrame(f, rt, p);
  CHECK_EQ(f.game->stats().pipeline_waits, uint64_t(1));
}

TEST(PipelineWait_AlsoWaitsForAPrewarmInFlight) {
  // The prewarm queued the pipeline and the worker is on it when the draw comes: the draw waits for
  // that job and does not queue a second.
  const std::string path = CachePath("wait_prewarm");
  const std::string pack = MakePack("wait_prewarm");
  RecordTheScene(path);
  Fixture f;
  if (!f.Init()) return;
  f.shaders->Initialize(pack, NR_TEST_DXC_DIR);
  f.game->options().async_pipelines = true;
  f.game->options().pipeline_wait = true;
  CHECK(f.game->SetPipelineCache(path));
  const uint32_t rt = f.BindMainSurface();
  auto p = f.MakeColorPipeline();
  Pixels px = DrawQuadrantFrame(f, rt, p);  // no WaitForPrewarm: the draw waits if it is not done
  f.CheckQuadrantColors(px, __LINE__);
  CHECK_EQ(f.game->stats().draws, uint64_t(1));
  CHECK_EQ(f.game->stats().skipped_pending, uint64_t(0));
  CHECK_EQ(f.game->stats().draw_pipelines, uint64_t(1));
}


NR_TEST_MAIN()
