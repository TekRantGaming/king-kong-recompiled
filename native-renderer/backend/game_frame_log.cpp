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

void GameRenderer::LogDraw(const DrawCall& call, const dev::DeviceView& d, const GameShader& vs, const GameShader* ps) {
  frame_log_->Touch();
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
  frame_log_->Draw(out);
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
      dest.format = uint32_t(fetch.Format());
      dest.pitch = fetch.PitchTexels();
      dest.height = fetch.Height();
      const kknr::ResolveConversion conv = kknr::PlanResolveConversion(
          kknr::ResolveSource{from_depth, out.source_format}, fetch, TextureOptions());
      dest.command = conv.method == kknr::ResolveMethod::kCopy ? 0 : 1;
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

// The library clears through the resolve path (a null copy that clears): one resolve line for the
// clear, from the colour target when it clears colour, else from the depth buffer.
void GameRenderer::LogClear(const ClearCall& call, const dev::DeviceView& d) {
  frame_log_->Touch();
  if (!frame_log_->active()) return;
  FrameLogResolve out;
  out.from_depth = !call.color;
  const uint32_t info = call.color ? d.u32(dev::kRbColorInfo) : d.u32(dev::kRbDepthInfo);
  out.source_index = 0;
  out.source_base = info & 0xFFF;
  out.source_format = call.color ? (info >> 16) & 0xF : (info >> 16) & 1;
  out.command = 3;
  out.clear_color = call.color;
  out.clear_depth = call.depth || call.stencil;
  WindowOffset(d, out.offset_x, out.offset_y);
  const uint32_t surface_info = d.u32(dev::kRbSurfaceInfo);
  out.surface_pitch = surface_info & 0x3FFF;
  out.msaa = 1u << ((surface_info >> 16) & 3);
  frame_log_->Resolve(out);
}

// The game's Present: the library resolves the back buffer into the front buffer texture and
// swaps. The resolve is a line of the frame being ended; then the swap.
void GameRenderer::LogSwap(const dev::DeviceView& d) {
  frame_log_->Touch();
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
        const kknr::ResolveConversion conv = kknr::PlanResolveConversion(
            kknr::ResolveSource{false, out.source_format}, fetch, TextureOptions());
        out.command = conv.method == kknr::ResolveMethod::kCopy ? 0 : 1;
      }
    }
    frame_log_->Resolve(out);
  }
  frame_log_->OnSwap();
}

}  // namespace nr
