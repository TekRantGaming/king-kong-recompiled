// The game's shaders on the host: every vertex and pixel shader object the
// D3D library creates, translated (stream 02's kkshaders: the prebuilt pack,
// else translated and compiled with DXC on first use) and created as an NVRHI
// shader, with the translator's bindings (vertex inputs by usage, texture and
// sampler slots, pixel outputs).
//
// Shaders are registered when they are created (the CreateVertexShader /
// CreatePixelShader hooks pass the container the engine handed over, so the
// microcode hash is the database's, before the library patches vertex
// fetches). A shader object the hooks did not see is parsed from the object
// itself at its first draw (its container copy and microcode).
//
// Thread-safe: creation comes from loader threads, lookups from the render
// thread.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

#include "backend/guest_memory.h"
#include "kkshaders/cache.h"
#include "kkshaders/translator.h"

namespace nr {

struct GameShader {
  kkshaders::ShaderKind kind = kkshaders::ShaderKind::Pixel;
  uint64_t ucode_hash = 0;
  uint32_t id = 0;  // small unique number, for pipeline keys
  kkshaders::ShaderBindings bindings;
  nvrhi::ShaderHandle handle;
  bool from_pack = false;
};

class ShaderLibrary {
 public:
  struct Stats {
    uint64_t created = 0;      // shader objects registered
    uint64_t from_pack = 0;    // found in the pack
    uint64_t compiled = 0;     // translated and compiled here
    uint64_t failed = 0;       // could not be parsed, translated or compiled
    uint64_t from_object = 0;  // parsed from the object at draw time (hook missed)
  };

  ShaderLibrary(nvrhi::IDevice* device, const GuestMemory& memory);
  ~ShaderLibrary();

  // pack: a kkshaders pack for this API (or both); dxc: DXC's folder or
  // library (empty: the default search). Both optional: without the pack
  // every shader is compiled, without DXC only the pack's shaders exist.
  void Initialize(const std::string& pack, const std::string& dxc);
  // Pixel shaders are translated render scale aware (the scaled variants of a pack). Set before
  // any shader is created.
  void SetRenderScaleAware(bool aware) { scale_aware_ = aware; }

  // A shader was created: kind 0 vertex, 1 pixel.
  void OnCreated(uint32_t kind, uint32_t container, uint32_t object);
  // The shader for a bound object (null when it cannot be made).
  std::shared_ptr<const GameShader> Get(kkshaders::ShaderKind kind, uint32_t object);

  Stats stats() const;
  bool pack_open() const { return pack_open_; }

 private:
  std::shared_ptr<GameShader> Build(const kkshaders::ShaderInfo& info);
  std::shared_ptr<GameShader> FromObject(kkshaders::ShaderKind kind, uint32_t object);

  nvrhi::IDevice* device_;
  const GuestMemory& memory_;
  kkshaders::ShaderPack pack_;
  bool pack_open_ = false;
  std::unique_ptr<kkshaders::Compiler> compiler_;
  bool dxc_ok_ = false;
  bool scale_aware_ = false;

  mutable std::mutex mutex_;
  // Object address -> shader (addresses are reused after a release; a new
  // creation replaces the entry).
  std::unordered_map<uint32_t, std::shared_ptr<GameShader>> by_object_;
  // (microcode hash, kind) -> shader: one host shader per distinct microcode.
  std::unordered_map<uint64_t, std::shared_ptr<GameShader>> by_hash_;
  std::unordered_map<uint64_t, bool> failed_;
  uint32_t next_id_ = 1;
  Stats stats_;
};

}  // namespace nr
