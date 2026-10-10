// REX_DEV_FRAME_LOG from the game renderer: fills the frame log's records from the device
// registers and the game's shaders (frame_log.h has the line formats and the frame rules).
//
// What the Xenos plugin reads from its register file, the device struct holds as the
// register images the D3D library keeps (guest_device.h). Where the plugin has no register
// to read (the library's own resolves, the patched vertex microcode) the value is derived
// and the assumption is written in docs/backend.md, "The frame log".
#include <algorithm>
#include <cstring>

#include "backend/game_renderer.h"
#include "backend/guest_layout.h"
#include "backend/log.h"
#include "kknr/buffers.h"
#include "kknr/render_targets.h"
#include "kknr/resolve.h"
#include "kkshaders/vertex_patch.h"

namespace nr {

namespace {

uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xFF51AFD7ED558CCDull;
}

// The texture object's fetch constant with physical addresses (as game_passes.cpp reads it).
kknr::TextureFetch FetchFromObject(const uint8_t* object) {
  uint32_t w[6];
  for (int i = 0; i < 6; ++i) w[i] = LoadBE32(object + 16 + 4 * i);
  if (w[1] & 0xFFFFF000u) w[1] = (w[1] & 0xFFFu) | (CpuToPhysical(w[1] & 0xFFFFF000u) & 0xFFFFF000u);
  if (w[5] & 0xFFFFF000u) w[5] = (w[5] & 0xFFFu) | (CpuToPhysical(w[5] & 0xFFFFF000u) & 0xFFFFF000u);
  return kknr::TextureFetch::FromWords(w);
}

int32_t SignExtend15(uint32_t v) { return int32_t(v << 17) >> 17; }

void EmitToLog(const std::string& line) { Logf(LogLevel::kWarning, "%s", line.c_str()); }

// The window offset register: x in bits 0-14, y in bits 16-30, both signed.
void WindowOffset(const dev::DeviceView& d, int32_t& x, int32_t& y) {
  const uint32_t v = d.u32(dev::kPaScWindowOffset);
  x = SignExtend15(v & 0x7FFF);
  y = SignExtend15((v >> 16) & 0x7FFF);
}

}  // namespace

void GameRenderer::SetFrameLog(const FrameLogConfig& config, FrameLog::Emit emit) {
  if (!config.enabled) {
    frame_log_.reset();
    return;
  }
  frame_log_ = std::make_unique<FrameLog>(config, emit ? std::move(emit) : FrameLog::Emit(EmitToLog));
}

uint64_t GameRenderer::PatchedVertexShaderHash(const GameShader& vs, uint32_t declaration, const dev::DeviceView& d) {
  if (!vs.source) return vs.ucode_hash;
  std::vector<kkshaders::DeclElement> elements;
  if (const uint8_t* decl = declaration ? memory_.Virtual(declaration) : nullptr) {
    for (const kknr::VertexElement& e : kknr::DecodeVertexDeclaration(decl)) {
      if (e.stream >= 16 || e.type.Unused()) continue;
      kkshaders::DeclElement out;
      out.stream = uint8_t(e.stream);
      out.offset = e.offset;
      out.format = uint8_t(e.type.Format());
      out.isSigned = e.type.Signed();
      out.integer = e.type.Integer();
      out.usage = kkshaders::DeclUsage(e.usage);
      out.usageIndex = e.usage_index;
      elements.push_back(out);
    }
  }
  uint32_t strides[16];
  for (uint32_t s = 0; s < 16; ++s) strides[s] = uint32_t(*d.at(12688 + s)) * 4;
  // The library keys its copies by (shader, declaration, strides); so do we, by content.
  uint64_t key = Mix(0, vs.id);
  for (const kkshaders::DeclElement& e : elements) {
    key = Mix(key, (uint64_t(e.stream) << 56) | (uint64_t(e.offset) << 32) | (uint64_t(e.format) << 16) |
                       (uint64_t(e.isSigned) << 9) | (uint64_t(e.integer) << 8) | (uint64_t(e.usage) << 4) | e.usageIndex);
  }
  for (uint32_t s : strides) key = Mix(key, s);
  if (auto it = patched_hashes_.find(key); it != patched_hashes_.end()) return it->second;
  const kkshaders::VertexPatchResult patched = kkshaders::patchVertexFetches(*vs.source, elements, strides);
  const uint64_t hash = kkshaders::hashMicrocode(patched.ucode);
  if (patched_hashes_.size() > (1u << 16)) patched_hashes_.clear();
  patched_hashes_[key] = hash;
  return hash;
}

// XXH3 of the vertex shader object's microcode as it sits in memory: the library patches it in place
// for the declaration and strides of its draws, and that is what the Xenos plugin hashes. Found on the
// game's frames: it equals the Xenos log's hash for every draw of the title, the reproduced patching
// (kkshaders/vertex_patch) does not. 0 when the object cannot be read. Before the library's draw of
// this call: a shader drawn with a new declaration still shows the previous patch (rare).
uint64_t GameRenderer::ObjectVertexShaderHash(uint32_t object_address, uint32_t* bytes_out) {
  uint64_t live = 0;
  uint32_t live_bytes = 0;
  if (const uint8_t* object = object_address ? memory_.Virtual(object_address) : nullptr) {
    // The vertex shader object: a 592-byte header, the container's virtual part, the microcode's
    // physical address at +40 (d3d-structs.md, corrected by the game's frames).
    live_bytes = LoadBE32(object + 592 + 8);
    const uint8_t* ucode = memory_.Physical(CpuToPhysical(LoadBE32(object + 40)));
    if (ucode && live_bytes && live_bytes <= (1u << 20)) live = kkshaders::hashBytes(ucode, live_bytes);
  }
  if (bytes_out) *bytes_out = live_bytes;
  return live;
}

void GameRenderer::DumpVertexShaderHashes(const DrawCall& call, const dev::DeviceView& d, const GameShader& vs) {
  uint32_t live_bytes = 0;
  const uint64_t live = ObjectVertexShaderHash(call.vertex_shader, &live_bytes);
  Logf(LogLevel::kInfo,
       "rexgpu-native:   vs hashes: database %016llX, object memory %016llX (%u bytes), reproduced patch %016llX",
       static_cast<unsigned long long>(vs.ucode_hash), static_cast<unsigned long long>(live), live_bytes,
       static_cast<unsigned long long>(PatchedVertexShaderHash(vs, call.vertex_declaration, d)));
}

// The library copies a vertex shader's microcode into the command ring (or patches the object's
// own copy) and rewrites its fetches for the declaration, inside the draw that binds it
// (sub_82120608 / sub_82120A70 call sub_82120028). That patched microcode is what the GPU runs and
// what the Xenos plugin hashes.
namespace {
// The streams' strides the library binds a vertex shader with (device +12688, 16 bytes; copied to
// +11424 after the binding, d3d-structs.md).
constexpr uint32_t kBindingStrides = 12688;
inline uint64_t Mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 29);
}
uint64_t VertexVariantKey(uint32_t shader, uint32_t declaration, const uint8_t* device) {
  uint64_t h = Mix64(0x5653, shader);
  h = Mix64(h, declaration);
  for (uint32_t i = 0; i < 4; ++i) h = Mix64(h, device ? LoadBE32(device + kBindingStrides + 4 * i) : 0);
  return h;
}
}  // namespace

void GameRenderer::VertexShaderPatched(uint32_t shader, uint32_t microcode, uint32_t declaration) {
  if (!frame_log_ || !frame_log_->enabled()) return;
  const uint8_t* object = shader ? memory_.Virtual(shader) : nullptr;
  const uint8_t* ucode = microcode ? memory_.Virtual(microcode) : nullptr;
  if (!object || !ucode) return;
  const uint32_t bytes = LoadBE32(object + 600);
  if (!bytes || bytes > (1u << 20)) return;
  // The device: the global the library keeps it in (the call's r3 is the same).
  const uint8_t* global = memory_.Virtual(0x82D62324);
  const uint32_t device_address = global ? LoadBE32(global) : 0;
  const uint8_t* device = device_address ? memory_.Virtual(device_address) : nullptr;
  patched_vs_hash_[VertexVariantKey(shader, declaration, device)] = kkshaders::hashBytes(ucode, bytes);
}

void GameRenderer::FlushPendingDraw() {
  if (!has_pending_draw_) return;
  has_pending_draw_ = false;
  // The microcode as the library patched it for this draw (its draw has returned by now). Without
  // the patch hook (the tests' synthetic device), the object's microcode when it differs from the
  // template, else the reproduced patching computed at the draw.
  if (auto it = patched_vs_hash_.find(pending_vs_key_); it != patched_vs_hash_.end()) {
    pending_draw_.vs_hash = it->second;
  } else {
    const uint64_t live = ObjectVertexShaderHash(pending_vs_object_, nullptr);
    if (live && live != pending_vs_template_) pending_draw_.vs_hash = live;
  }
  frame_log_->Draw(pending_draw_);
  if (pending_not_drawn_ >= 0) frame_log_->DrawNotDrawn(pending_not_drawn_ == 1);
}

void GameRenderer::NoteDrawNotDrawn(bool pending) {
  if (has_pending_draw_) {
    pending_not_drawn_ = pending ? 1 : 0;
  } else {
    frame_log_->DrawNotDrawn(pending);
  }
}

void GameRenderer::LogDraw(const DrawCall& call, const dev::DeviceView& d, const GameShader& vs, const GameShader* ps) {
  frame_log_->Touch();
  FlushPendingDraw();
  if (!frame_log_->active()) return;
  FrameLogDraw out;
  out.ps_hash = ps ? ps->ucode_hash : 0;
  out.vs_hash = PatchedVertexShaderHash(vs, call.vertex_declaration, d);
  const uint32_t surface_info = d.u32(dev::kRbSurfaceInfo);
  out.pitch = surface_info & 0x3FFF;
  out.msaa = 1u << ((surface_info >> 16) & 3);
  const uint32_t depth_control = d.u32(dev::kRbDepthControl);
  out.z_test = (depth_control & 2) != 0;
  out.z_write = (depth_control & 4) != 0;
  out.z_func = (depth_control >> 4) & 7;
  const uint32_t depth_info = d.u32(dev::kRbDepthInfo);
  out.depth_base = depth_info & 0xFFF;
  out.depth_format = (depth_info >> 16) & 1;
  WindowOffset(d, out.offset_x, out.offset_y);
  const uint32_t scissor_br = d.u32(dev::kPaScWindowScissorBr);
  out.scissor_w = scissor_br & 0x7FFF;
  out.scissor_h = (scissor_br >> 16) & 0x7FFF;
  const uint32_t color_mask = d.u32(dev::kRbColorMask);
  for (uint32_t i = 0; i < 4; ++i) {
    if (!ps || !(ps->bindings.pixelOutputs & (1u << i)) || !((color_mask >> (4 * i)) & 0xF)) continue;
    const uint32_t info = d.u32(i == 0 ? dev::kRbColorInfo : dev::kRbColor1Info + 4 * (i - 1));
    out.targets.push_back({i, info & 0xFFF, (info >> 16) & 0xF});
  }
  out.count = call.count;
  pending_draw_ = std::move(out);
  pending_vs_object_ = call.vertex_shader;
  pending_vs_key_ = VertexVariantKey(call.vertex_shader, call.vertex_declaration,
                                     call.device ? memory_.Virtual(call.device) : nullptr);
  pending_vs_template_ = vs.ucode_hash;
  pending_not_drawn_ = -1;
  has_pending_draw_ = true;
}

// A resolve's destination and command from the texture object, shared by the resolves the game
// asks for and the front buffer copy the Present makes.
namespace {
struct ResolveDestination {
  uint32_t address = 0, format = 0, pitch = 0, height = 0, command = 3;
};
}  // namespace

void GameRenderer::LogResolve(const ResolveCall& call, const dev::DeviceView& d) {
  frame_log_->Touch();
  FlushPendingDraw();
  if (!frame_log_->active()) return;
  const kknr::ResolveFlags flags{call.flags};
  const bool from_depth = flags.DepthStencil();
  const uint32_t index = flags.RenderTargetIndex();
  FrameLogResolve out;
  out.from_depth = from_depth;
  out.source_index = from_depth ? 0 : index;
  const uint32_t info = from_depth ? d.u32(dev::kRbDepthInfo)
                                   : d.u32(index == 0 ? dev::kRbColorInfo : dev::kRbColor1Info + 4 * (index - 1));
  out.source_base = info & 0xFFF;
  out.source_format = from_depth ? (info >> 16) & 1 : (info >> 16) & 0xF;
  // The destination: the texture's fetch constant. No texture (a resolve that only clears) is the
  // null copy command.
  ResolveDestination dest;
  if (const uint8_t* object = call.dest_texture ? memory_.Virtual(call.dest_texture) : nullptr) {
    const kknr::TextureFetch fetch = FetchFromObject(object);
    if (fetch.Type() == 2) {
      dest.address = fetch.BaseAddress();
      // A depth resolve is a raw copy the library sets up with k_8_8_8_8 as the destination
      // format whatever the texture's (every depth resolve in the Xenos logs: format 6).
      dest.format = from_depth ? 6u : uint32_t(fetch.Format());
      dest.pitch = fetch.PitchTexels();
      dest.height = fetch.Height();
      // The library's copy command (RB_COPY_CONTROL), as the Xenos logs of the golden scenes show
      // it: convert for every colour resolve, raw for depth, whatever the formats.
      dest.command = from_depth ? 0 : 1;
    }
  }
  out.dest_address = dest.address;
  out.dest_format = dest.format;
  out.dest_pitch = dest.pitch;
  out.dest_height = dest.height;
  out.command = dest.command;
  out.clear_color = flags.ClearRenderTarget();
  out.clear_depth = flags.ClearDepthStencil();
  WindowOffset(d, out.offset_x, out.offset_y);
  const uint32_t surface_info = d.u32(dev::kRbSurfaceInfo);
  out.surface_pitch = surface_info & 0x3FFF;
  out.msaa = 1u << ((surface_info >> 16) & 3);
  frame_log_->Resolve(out);
}

// The library's Clear is a draw of its own shaders (pixel 2B4F1D2C5EDCC366, vertex 6DD9DD041F740550,
// three vertices: a rectangle list) with depth test and write on when it clears depth or stencil,
// and the colour targets written when it clears colour: that is what the Xenos plugin's log shows
// for each Clear the game calls (two in the title's frame, eight in V-Rex's).
void GameRenderer::LogClear(const ClearCall& call, const dev::DeviceView& d) {
  frame_log_->Touch();
  FlushPendingDraw();
  if (!frame_log_->active()) return;
  FrameLogDraw out;
  out.ps_hash = 0x2B4F1D2C5EDCC366ull;
  out.vs_hash = 0x6DD9DD041F740550ull;
  const uint32_t surface_info = d.u32(dev::kRbSurfaceInfo);
  out.pitch = surface_info & 0x3FFF;
  out.msaa = 1u << ((surface_info >> 16) & 3);
  out.z_test = out.z_write = call.depth || call.stencil;
  out.z_func = 7;
  const uint32_t depth_info = d.u32(dev::kRbDepthInfo);
  out.depth_base = depth_info & 0xFFF;
  out.depth_format = (depth_info >> 16) & 1;
  WindowOffset(d, out.offset_x, out.offset_y);
  const uint32_t scissor_br = d.u32(dev::kPaScWindowScissorBr);
  out.scissor_w = scissor_br & 0x7FFF;
  out.scissor_h = (scissor_br >> 16) & 0x7FFF;
  for (uint32_t i = 0; i < 4; ++i) {
    // Only the targets bound: the game's flags name all four (0x3F, 0xF).
    if (!(call.flags & (1u << i)) || !d.u32(dev::kRenderTargets + 4 * i)) continue;
    const uint32_t info = d.u32(i == 0 ? dev::kRbColorInfo : dev::kRbColor1Info + 4 * (i - 1));
    out.targets.push_back({i, info & 0xFFF, (info >> 16) & 0xF});
  }
  out.count = 3;
  frame_log_->Draw(out);
}

// The game's Present: the library resolves the back buffer into the front buffer texture and
// swaps. The resolve is a line of the frame being ended; then the swap.
void GameRenderer::LogSwap(const dev::DeviceView& d) {
  frame_log_->Touch();
  FlushPendingDraw();
  if (frame_log_->active()) {
    FrameLogResolve out;
    const uint32_t back = d.u32(dev::kBackBuffer);
    dev::SurfaceInfo s;
    if (back && dev::DecodeSurface(memory_.Virtual(back), false, s)) {
      out.source_base = s.edram_base;
      out.source_format = s.format;
      out.surface_pitch = s.pitch ? s.pitch : s.width;
      out.msaa = 1;
    }
    const uint32_t front = d.u32(dev::kFrontBuffer);
    if (const uint8_t* object = front ? memory_.Virtual(front) : nullptr) {
      const kknr::TextureFetch fetch = FetchFromObject(object);
      if (fetch.Type() == 2) {
        out.dest_address = fetch.BaseAddress();
        out.dest_format = uint32_t(fetch.Format());
        out.dest_pitch = fetch.PitchTexels();
        out.dest_height = fetch.Height();
        out.command = 1;  // a colour resolve: convert (see LogResolve)
      }
    }
    frame_log_->Resolve(out);
  }
  frame_log_->OnSwap();
}

}  // namespace nr
