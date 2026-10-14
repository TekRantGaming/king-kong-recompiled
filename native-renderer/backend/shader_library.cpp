#include "backend/shader_library.h"

#include <chrono>
#include <cstring>
#include <vector>

#include "backend/guest_layout.h"
#include "backend/log.h"

namespace nr {

namespace {

// A translation: the microcode and the translation's inputs (the same microcode from two
// containers, or with and without render scale awareness, is two shaders).
uint64_t HashKey(kkshaders::ShaderKind kind, uint64_t ucode_hash, uint64_t input_hash) {
  uint64_t h = ucode_hash ^ (kind == kkshaders::ShaderKind::Vertex ? 0x9E3779B97F4A7C15ull : 0);
  h ^= input_hash + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

// The register of g_vAmbientColor in a pixel shader's constant table (-1: none).
int16_t AmbientConstant(const kkshaders::ShaderInfo& info) {
  if (info.kind != kkshaders::ShaderKind::Pixel) return -1;
  for (const kkshaders::ConstantInfo& c : info.constants)
    if (c.registerSet == kkshaders::RegisterSet::Float4 && c.name == "g_vAmbientColor" && c.registerIndex < 256)
      return int16_t(c.registerIndex);
  return -1;
}

const char* KindName(kkshaders::ShaderKind kind) {
  return kind == kkshaders::ShaderKind::Vertex ? "vertex" : "pixel";
}

}  // namespace

ShaderLibrary::ShaderLibrary(nvrhi::IDevice* device, const GuestMemory& memory)
    : device_(device), memory_(memory) {}

ShaderLibrary::~ShaderLibrary() {
  {
    std::lock_guard lock(motion_mutex_);
    motion_stop_ = true;
  }
  motion_cv_.notify_all();
  if (motion_thread_.joinable()) motion_thread_.join();
}

nvrhi::IShader* ShaderLibrary::MotionVariant(const GameShader& vs) {
  if (vs.kind != kkshaders::ShaderKind::Vertex) return nullptr;
  const uint64_t key = HashKey(vs.kind, vs.ucode_hash, vs.input_hash) ^ 0x4D4F54494F4E0001ull;
  std::lock_guard lock(motion_mutex_);
  if (auto it = motion_.find(key); it != motion_.end()) return it->second.handle.Get();
  MotionEntry& e = motion_[key];
  ++motion_stats_.requested;
  kkshaders::CompiledShader packed;
  if (pack_open_ && pack_.find(vs.ucode_hash ^ kkshaders::kMotionPackTag, vs.input_hash, packed)) {
    const auto& blob = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? packed.spirv : packed.dxil;
    nvrhi::ShaderDesc desc;
    desc.shaderType = nvrhi::ShaderType::Vertex;
    desc.entryName = "main";
    desc.debugName = "packed motion vertex";
    if (!blob.empty()) e.handle = device_->createShader(desc, blob.data(), blob.size());
    e.done = true;
    if (e.handle) ++motion_stats_.made;
    else ++motion_stats_.failed;
    return e.handle.Get();
  }
  if (!vs.source) {
    // A legacy pack may acquire its real container later; do not cache a failure yet.
    motion_.erase(key);
    --motion_stats_.requested;
    return nullptr;
  }
  if (!vs.source || !dxc_ok_) {
    e.done = true;
    ++motion_stats_.failed;
    return nullptr;
  }
  motion_queue_.emplace_back(key, vs.source);
  if (!motion_thread_.joinable()) motion_thread_ = std::thread([this] { MotionWorker(); });
  motion_cv_.notify_one();
  return nullptr;
}

void ShaderLibrary::WaitForMotionVariants() {
  for (;;) {
    {
      std::lock_guard lock(motion_mutex_);
      bool all = motion_queue_.empty();
      for (const auto& [key, e] : motion_) all = all && e.done;
      if (all) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

ShaderLibrary::MotionStats ShaderLibrary::motion_stats() const {
  std::lock_guard lock(motion_mutex_);
  return motion_stats_;
}

void ShaderLibrary::MotionWorker() {
  // DXC objects belong to one thread: this thread's own compiler.
  kkshaders::Compiler compiler;
  const kkshaders::CompileTarget target = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN
                                              ? kkshaders::CompileTarget::Spirv
                                              : kkshaders::CompileTarget::Dxil;
  for (;;) {
    std::pair<uint64_t, std::shared_ptr<const kkshaders::ShaderInfo>> job;
    {
      std::unique_lock lock(motion_mutex_);
      motion_cv_.wait(lock, [&] { return motion_stop_ || !motion_queue_.empty(); });
      if (motion_stop_) return;
      job = std::move(motion_queue_.front());
      motion_queue_.erase(motion_queue_.begin());
    }
    const auto start = std::chrono::steady_clock::now();
    nvrhi::ShaderHandle handle;
    std::string error;
    kkshaders::TranslateResult t = kkshaders::translate(*job.second);
    if (!t.ok) {
      error = "translation: " + t.error;
    } else {
      kkshaders::MotionShader m = kkshaders::makeMotionVertexShader(t.hlsl);
      if (!m.ok) {
        error = "variant: " + m.error;
      } else if (!compiler.ok()) {
        error = "no compiler";
      } else {
        kkshaders::CompileOutput o = compiler.compile(m.hlsl, kkshaders::ShaderKind::Vertex, target);
        if (!o.ok || o.blob.empty()) {
          error = "DXC: " + o.messages.substr(0, 300);
        } else {
          nvrhi::ShaderDesc desc;
          desc.shaderType = nvrhi::ShaderType::Vertex;
          desc.entryName = "main";
          char name[48];
          std::snprintf(name, sizeof(name), "motion %016llX", static_cast<unsigned long long>(job.second->ucodeHash));
          desc.debugName = name;
          handle = device_->createShader(desc, o.blob.data(), o.blob.size());
          if (!handle) error = "createShader";
        }
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard lock(motion_mutex_);
    MotionEntry& e = motion_[job.first];
    e.handle = handle;
    e.done = true;
    motion_stats_.compile_ms += ms;
    if (handle) {
      ++motion_stats_.made;
    } else {
      ++motion_stats_.failed;
      if (motion_stats_.failed < 20) {
        Logf(LogLevel::kWarning, "rexgpu-native: the motion variant of vertex shader %016llX was not made (%s)",
             static_cast<unsigned long long>(job.second->ucodeHash), error.c_str());
      }
    }
  }
}

bool ShaderLibrary::OpenPack(const std::string& pack) {
  std::lock_guard lock(mutex_);
  if (pack_open_) return false;
  std::string error;
  pack_open_ = pack_.open(pack, &error);
  if (pack_open_) {
    Logf(LogLevel::kInfo, "rexgpu-native: shader pack %s (%zu shaders)", pack.c_str(), pack_.size());
  } else {
    Logf(LogLevel::kWarning, "rexgpu-native: no shader pack: %s", error.c_str());
  }
  return pack_open_;
}

void ShaderLibrary::Initialize(const std::string& pack, const std::string& dxc) {
  if (!pack.empty()) OpenPack(pack);
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
  const uint64_t translation_hash = kkshaders::translationInputHash(info);
  // A replacement (the lighting options) keeps the translation's bindings under an input hash of its own.
  const Replacement* replacement = FindReplacement(info);
  const uint64_t input_hash = replacement ? translation_hash ^ replacement->tag : translation_hash;
  // Derive aliases from this container, never from the microcode hash alone: distinct
  // interpolator/fetch translations of the same shader must remain distinct.
  if (info.kind == kkshaders::ShaderKind::Pixel) {
    auto baseline = info;
    baseline.ambientOcclusion = {};
    baseline.waterReflection = {};
    for (bool aware : {false, true}) {
      baseline.renderScaleAware = aware;
      prewarm_inputs_[HashKey(info.kind, info.ucodeHash, kkshaders::translationInputHash(baseline))] = input_hash;
    }
  }
  const uint64_t key = HashKey(info.kind, info.ucodeHash, input_hash);
  if (auto it = by_hash_.find(key); it != by_hash_.end()) {
    // Made from the pack alone (GetByHash): a copy that also has the container's fetch table.
    if (info.kind == kkshaders::ShaderKind::Vertex && !it->second->source) {
      auto copy = std::make_shared<GameShader>(*it->second);
      copy->source = std::make_shared<const kkshaders::ShaderInfo>(info);
      it->second = copy;
    }
    // Made from the pack alone: a copy that knows its ambient constant.
    if (const int16_t ambient = AmbientConstant(info); ambient != it->second->ambient_constant) {
      auto copy = std::make_shared<GameShader>(*it->second);
      copy->ambient_constant = ambient;
      it->second = copy;
    }
    return it->second;
  }
  if (failed_.count(key)) return nullptr;

  kkshaders::CompiledShader compiled;
  bool from_pack = false;
  if (pack_open_) {
    from_pack = pack_.find(info.ucodeHash, translation_hash, compiled);
    // The ucode-only fallback answers with the 1:1 translation: wrong for an aware shader.
    if (!from_pack && !info.renderScaleAware && !info.ambientOcclusion.enabled() && !info.waterReflection.enabled())
      from_pack = pack_.find(info.ucodeHash, 0, compiled);
  }
  if (!from_pack) {
    if (!dxc_ok_) {
      failed_[key] = true;
      ++stats_.failed;
      Logf(LogLevel::kWarning, "rexgpu-native: %s shader %016llX is not in the pack and DXC is missing",
           KindName(info.kind), static_cast<unsigned long long>(info.ucodeHash));
      return nullptr;
    }
    // Only the blob this device uses (the other one would double the compile time).
    const uint32_t target = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? kkshaders::kBuildSpirv : kkshaders::kBuildDxil;
    kkshaders::BuildResult built = kkshaders::buildShader(info, *compiler_, false, target);
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
  if (replacement) {
    // The renderer's own shader for this device's API, with the translation's bindings.
    const auto* bytes = static_cast<const uint8_t*>(replacement->data);
    std::vector<uint8_t>& blob = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? compiled.spirv : compiled.dxil;
    blob.assign(bytes, bytes + replacement->size);
    Logf(LogLevel::kInfo, "rexgpu-native: pixel shader %016llX replaced (lighting options)",
         static_cast<unsigned long long>(info.ucodeHash));
  }

  std::shared_ptr<GameShader> shader = Finish(info.kind, info.ucodeHash, input_hash, std::move(compiled), from_pack);
  if (shader && info.kind == kkshaders::ShaderKind::Vertex) shader->source = std::make_shared<const kkshaders::ShaderInfo>(info);
  if (shader) shader->ambient_constant = AmbientConstant(info);
  return shader;
}

std::shared_ptr<GameShader> ShaderLibrary::Finish(kkshaders::ShaderKind kind, uint64_t hash, uint64_t input_hash,
                                                   kkshaders::CompiledShader&& compiled, bool from_pack) {
  const uint64_t key = HashKey(kind, hash, input_hash);
  const bool spirv = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  const std::vector<uint8_t>& blob = spirv ? compiled.spirv : compiled.dxil;
  if (blob.empty()) {
    failed_[key] = true;
    ++stats_.failed;
    return nullptr;
  }
  auto shader = std::make_shared<GameShader>();
  shader->kind = kind;
  shader->ucode_hash = hash;
  shader->input_hash = input_hash;
  shader->id = next_id_++;
  shader->bindings = std::move(compiled.bindings);
  shader->from_pack = from_pack;
  nvrhi::ShaderDesc desc;
  desc.shaderType = kind == kkshaders::ShaderKind::Vertex ? nvrhi::ShaderType::Vertex : nvrhi::ShaderType::Pixel;
  desc.entryName = "main";
  char name[48];
  std::snprintf(name, sizeof(name), "%s %016llX", KindName(kind), static_cast<unsigned long long>(hash));
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

std::shared_ptr<const GameShader> ShaderLibrary::GetByHash(kkshaders::ShaderKind kind, uint64_t ucode_hash,
                                                          uint64_t input_hash) {
  std::lock_guard lock(mutex_);
  const uint64_t key = HashKey(kind, ucode_hash, input_hash);
  if (auto it = by_hash_.find(key); it != by_hash_.end()) return it->second;
  if (!pack_open_ || failed_.count(key)) return nullptr;
  kkshaders::CompiledShader compiled;
  // Exactly that translation (input 0 would answer with any of them).
  if (!input_hash || !pack_.find(ucode_hash, input_hash, compiled)) return nullptr;
  ++stats_.from_pack;
  return Finish(kind, ucode_hash, input_hash, std::move(compiled), true);
}

uint64_t ShaderLibrary::created_count() const {
  std::lock_guard lock(mutex_);
  return stats_.created + stats_.from_object;
}

std::shared_ptr<const GameShader> ShaderLibrary::GetForPrewarm(kkshaders::ShaderKind kind, uint64_t hash,
                                                             uint64_t input, bool exact_kind) {
  {
    std::lock_guard lock(mutex_);
    if (auto it = prewarm_inputs_.find(HashKey(kind, hash, input)); it != prewarm_inputs_.end()) {
      input = it->second;
    } else if (kind == kkshaders::ShaderKind::Pixel && !ambient_ && !water_reflections_ &&
               !replacements_.count(hash)) {
      kkshaders::CompiledShader compiled;
      if (pack_open_ && pack_.find(hash ^ (scale_aware_ ? kkshaders::kScaledPackTag :
                                                       kkshaders::kUnscaledPackTag), input, compiled)) {
        const uint64_t actual = compiled.inputHash;
        const uint64_t key = HashKey(kind, hash, actual);
        if (auto known = by_hash_.find(key); known != by_hash_.end()) return known->second;
        ++stats_.from_pack;
        return Finish(kind, hash, actual, std::move(compiled), true);
      }
      if (!exact_kind) return nullptr;
    } else if (kind == kkshaders::ShaderKind::Pixel &&
               (!exact_kind || ambient_ ||
                (water_reflections_ && kkshaders::waterReflectionHookFor(hash).enabled()))) {
      // A base list cannot tell us the desired input hash until its container is known.
      return nullptr;
    } else if (kind == kkshaders::ShaderKind::Pixel) {
      auto replacement = replacements_.find(hash);
      if (replacement != replacements_.end()) {
        // A base list names the packed translation, while a lighting cache names the
        // replacement. Preserve that translation's bindings in either case.
        const Replacement& r = replacement->second;
        kkshaders::CompiledShader compiled;
        uint64_t replaced_input = input ^ r.tag;
        if (!pack_open_ || !input) return nullptr;
        if (!pack_.find(hash, input, compiled)) {
          replaced_input = input;
          if (!pack_.find(hash, input ^ r.tag, compiled)) return nullptr;
        }
        const uint64_t key = HashKey(kind, hash, replaced_input);
        if (auto known = by_hash_.find(key); known != by_hash_.end()) return known->second;
        const auto* bytes = static_cast<const uint8_t*>(r.data);
        auto& blob = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? compiled.spirv : compiled.dxil;
        blob.assign(bytes, bytes + r.size);
        ++stats_.from_pack;
        return Finish(kind, hash, replaced_input, std::move(compiled), true);
      }
    }
  }
  return GetByHash(kind, hash, input);
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
  if (parsed.ok) ApplyAmbient(parsed.info);
  if (parsed.ok) ApplyLighting(parsed.info);
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
    if (motion_prewarm_ && want == kkshaders::ShaderKind::Vertex) MotionVariant(*shader);
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
  ApplyAmbient(parsed.info);
  ApplyLighting(parsed.info);
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
