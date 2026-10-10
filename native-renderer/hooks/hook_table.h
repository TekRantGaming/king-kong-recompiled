// The D3D hook table: every Direct3D library entry point the native renderer
// follows (docs/d3d-api-map.md), with the handler that turns the guest call's
// registers into an NrApi call. No SDK or game dependency: the game-side
// wrappers (kk_native_hooks.cpp, REX_HOOK_RAW) copy r3-r10 / f1 into
// GuestArgs and call Run(); the tests call Run() with recorded arguments.
//
// Milestone 3 runs the game's own library as well (the hooks forward, then
// call the original): the library keeps its device struct, fences and swaps
// working, and the ring skimmer in the plugin drains its packets. Replacing
// the originals comes with Phase 2.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "plugin/native_api.h"

namespace nr::hooks {

// The PowerPC argument registers of a guest call: r3..r10 and f1.
struct GuestArgs {
  uint32_t r[8] = {};  // r[0] = r3 (the device for every entry here)
  double f1 = 0.0;
  uint32_t r3() const { return r[0]; }
  uint32_t r4() const { return r[1]; }
  uint32_t r5() const { return r[2]; }
  uint32_t r6() const { return r[3]; }
  uint32_t r7() const { return r[4]; }
  uint32_t r8() const { return r[5]; }
  uint32_t r9() const { return r[6]; }
  uint32_t r10() const { return r[7]; }
};

using Handler = void (*)(const NrApi& api, const GuestArgs& args);

enum class Kind : uint8_t { kDraw, kFrame, kState, kRenderState, kSamplerState };

struct HookEntry {
  uint32_t address;  // guest address: the recompiled function is sub_<address>
  const char* name;  // the XDK name it behaves like
  Kind kind;
  Handler handler;
};

// All entries, sorted by address.
std::span<const HookEntry> Table();
const HookEntry* Find(uint32_t address);

// Clear's stencil argument. The XDK signature is Clear(dev, Count, pRects,
// Flags, Color, Z, Stencil): Z is in f1; whether Stencil is in r8 (GPRs
// counted separately from FPRs) or r9 (a float also takes a GPR slot, as in
// the 64-bit PowerPC ELF ABI) is not settled yet. r9 is assumed; see the
// stream's doc.
constexpr int kClearStencilRegister = 9;

// Calls nest inside the library (SetRenderTarget applies the viewport through
// SetViewport, Present resolves the frontbuffer, Clear goes through the
// resolve path). Nested state calls are real state changes and are forwarded;
// a draw, clear, resolve or present is forwarded only when no other hooked
// call is running on the thread, so the renderer sees each one the engine
// asked for, once.
class CallScope {
 public:
  CallScope();
  ~CallScope();
  CallScope(const CallScope&) = delete;
  CallScope& operator=(const CallScope&) = delete;
  bool outermost() const { return outermost_; }

 private:
  bool outermost_;
};

inline bool IsStateKind(Kind kind) {
  return kind == Kind::kState || kind == Kind::kRenderState || kind == Kind::kSamplerState;
}

// Forwards the call to `api` (when it is active, and for draws and frame
// calls only when not nested), then runs the game's original.
template <typename Original>
void Run(const HookEntry& entry, const NrApi* api, const GuestArgs& args, Original&& original) {
  CallScope scope;
  if ((scope.outermost() || IsStateKind(entry.kind)) && api && api->is_active(api->self)) {
    entry.handler(*api, args);
  }
  original();
}

// Checks that the plugin's table is one this hook build understands.
inline bool IsCompatible(const NrApi* api) {
  return api && api->version == NR_API_VERSION && api->size >= sizeof(NrApi);
}

}  // namespace nr::hooks
