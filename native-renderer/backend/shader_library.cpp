#include "backend/shader_library.h"

#include <cstring>
#include <vector>

#include "backend/guest_layout.h"
#include "backend/log.h"

namespace nr {

namespace {

uint64_t HashKey(kkshaders::ShaderKind kind, uint64_t ucode_hash) {
  return ucode_hash ^ (kind == kkshaders::ShaderKind::Vertex ? 0x9E3779B97F4A7C15ull : 0);
}

const char* KindName(kkshaders::ShaderKind kind) {
  return kind == kkshaders::ShaderKind::Vertex ? "vertex" : "pixel";
}

}  // namespace

ShaderLibrary::ShaderLibrary(nvrhi::IDevice* device, const GuestMemory& memory)
    : device_(device), memory_(memory) {}

ShaderLibrary::~ShaderLibrary() = default;

void ShaderLibrary::Initialize(const std::string& pack, const std::string& dxc) {
  if (!pack.empty()) {
    std::string error;
    pack_open_ = pack_.open(pack, &error);
    if (pack_open_) {
      Logf(LogLevel::kInfo, "rexgpu-native: shader pack %s (%zu shaders)", pack.c_str(), pack_.size());
    } else {
      Logf(LogLevel::kWarning, "rexgpu-native: no shader pack: %s", error.c_str());
    }
  }
  std::string error;
  if (kkshaders::loadDxc(dxc, &error)) {
    compiler_ = std::make_unique<kkshaders::Compiler>();
    dxc_ok_ = compiler_->ok();
    Logf(LogLevel::kInfo, "rexgpu-native: DXC %s for shaders missing from the pack",
         kkshaders::dxcVersion().c_str());
  } else {
    Logf(LogLevel::kWarning,
         "rexgpu-native: DXC not available (%s): only the pack's shaders can be drawn",
         error.c_str());
  }
}

std::shared_ptr<GameShader> ShaderLibrary::Build(const kkshaders::ShaderInfo& info) {
  // Called with mutex_ held.
  const uint64_t key = HashKey(info.kind, info.ucodeHash);
  if (auto it = by_hash_.find(key); it != by_hash_.end()) return it->second;
  if (failed_.count(key)) return nullptr;

  kkshaders::CompiledShader compiled;
  bool from_pack = false;
  if (pack_open_) {
    from_pack = pack_.find(info.ucodeHash, kkshaders::translationInputHash(info), compiled);
    // The ucode-only fallback answers with the 1:1 translation: wrong for an aware shader.
    if (!from_pack && !info.renderScaleAware) from_pack = pack_.find(info.ucodeHash, 0, compiled);
  }
  if (!from_pack) {
    if (!dxc_ok_) {
      failed_[key] = true;
      ++stats_.failed;
      Logf(LogLevel::kWarning, "rexgpu-native: %s shader %016llX is not in the pack and DXC is missing",
           KindName(info.kind), static_cast<unsigned long long>(info.ucodeHash));
      return nullptr;
    }
    kkshaders::BuildResult built = kkshaders::buildShader(info, *compiler_, false);
    if (!built.ok) {
      failed_[key] = true;
      ++stats_.failed;
      Logf(LogLevel::kWarning, "rexgpu-native: %s shader %016llX failed (%s): %s", KindName(info.kind),
           static_cast<unsigned long long>(info.ucodeHash), built.stage.c_str(),
           built.error.substr(0, 400).c_str());
      return nullptr;
    }
    compiled = std::move(built.shader);
    ++stats_.compiled;
  } else {
    ++stats_.from_pack;
  }

  const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  const std::vector<uint8_t>& blob = spirv ? compiled.spirv : compiled.dxil;
  if (blob.empty()) {
    failed_[key] = true;
    ++stats_.failed;
    return nullptr;
  }
  auto shader = std::make_shared<GameShader>();
  shader->kind = info.kind;
  shader->ucode_hash = info.ucodeHash;
  shader->id = next_id_++;
  shader->bindings = std::move(compiled.bindings);
  shader->from_pack = from_pack;
  nvrhi::ShaderDesc desc;
  desc.shaderType = info.kind == kkshaders::ShaderKind::Vertex ? nvrhi::ShaderType::Vertex
                                                               : nvrhi::ShaderType::Pixel;
  desc.entryName = "main";
  char name[48];
  std::snprintf(name, sizeof(name), "%s %016llX", KindName(info.kind),
                static_cast<unsigned long long>(info.ucodeHash));
  desc.debugName = name;
  shader->handle = device_->createShader(desc, blob.data(), blob.size());
  if (!shader->handle) {
    failed_[key] = true;
    ++stats_.failed;
    return nullptr;
  }
  by_hash_[key] = shader;
  return shader;
}

void ShaderLibrary::OnCreated(uint32_t kind, uint32_t container, uint32_t object) {
  if (!object || !container) return;
  const uint8_t* p = memory_.Virtual(container);
  if (!p) return;
  kkshaders::ParseResult parsed = kkshaders::parseContainer(p, SIZE_MAX);
  const kkshaders::ShaderKind want = kind == 0 ? kkshaders::ShaderKind::Vertex : kkshaders::ShaderKind::Pixel;
  std::lock_guard lock(mutex_);
  ++stats_.created;
  if (parsed.ok) parsed.info.renderScaleAware = scale_aware_ && parsed.info.kind == kkshaders::ShaderKind::Pixel;
  if (!parsed.ok || parsed.info.kind != want) {
    ++stats_.failed;
    by_object_[object] = nullptr;
    Logf(LogLevel::kWarning, "rexgpu-native: %s shader container %08X not parsed: %s",
         KindName(want), container, parsed.ok ? "kind differs" : parsed.error.c_str());
    return;
  }
  std::shared_ptr<GameShader> shader = Build(parsed.info);
  if (shader) {
    by_object_[object] = shader;
  } else {
    by_object_[object] = nullptr;
  }
}

std::shared_ptr<GameShader> ShaderLibrary::FromObject(kkshaders::ShaderKind kind, uint32_t object) {
  // Called with mutex_ held. The object is a header followed by the
  // container's virtual part; the microcode sits in physical memory
  // (d3d-structs.md, with the two headers the other way round: the pixel
  // shader has the 52-byte header and its microcode address at +12, the
  // vertex shader the 592-byte one with it at +40).
  const bool vertex = kind == kkshaders::ShaderKind::Vertex;
  const uint8_t* o = memory_.Virtual(object);
  if (!o) return nullptr;
  const uint8_t* copy = o + (vertex ? 592 : 52);
  const uint32_t virtual_size = LoadBE32(copy + 4);
  const uint32_t physical_size = LoadBE32(copy + 8);
  if (virtual_size < 24 || virtual_size > (1u << 20) || physical_size > (1u << 20)) return nullptr;
  const uint32_t ucode_address = LoadBE32(o + (vertex ? 40 : 12));
  const uint8_t* ucode = memory_.Physical(CpuToPhysical(ucode_address));
  if (!ucode) return nullptr;
  std::vector<uint8_t> container(size_t(virtual_size) + physical_size);
  std::memcpy(container.data(), copy, virtual_size);
  std::memcpy(container.data() + virtual_size, ucode, physical_size);
  kkshaders::ParseResult parsed = kkshaders::parseContainer(container.data(), container.size());
  if (!parsed.ok || parsed.info.kind != kind) return nullptr;
  parsed.info.renderScaleAware = scale_aware_ && kind == kkshaders::ShaderKind::Pixel;
  ++stats_.from_object;
  return Build(parsed.info);
}

std::shared_ptr<const GameShader> ShaderLibrary::Get(kkshaders::ShaderKind kind, uint32_t object) {
  if (!object) return nullptr;
  std::lock_guard lock(mutex_);
  if (auto it = by_object_.find(object); it != by_object_.end()) {
    // A null entry: this object failed before (until it is created again).
    if (!it->second || it->second->kind == kind) return it->second;
  }
  std::shared_ptr<GameShader> shader = FromObject(kind, object);
  by_object_[object] = shader;
  return shader;
}

ShaderLibrary::Stats ShaderLibrary::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

}  // namespace nr
