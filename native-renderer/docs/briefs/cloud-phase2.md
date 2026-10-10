# Phase 2 cloud sessions

Same container rules as `cloud.md` (read it first): Linux, no GPU, no game files, never fetch or recreate game
data, SDK v0.10.0 is public, lavapipe for Vulkan, DXC Linux release for SPIR-V. Phase 2 lives on branch
`nr-integration` (the game renderer: real shaders, textures and states per draw; the title screen already
matches its golden frames on Vulkan). Start from `nr-integration`, work on a new branch `cloud-p2-<name>`,
push it, never push to `main`, `native-renderer` or `nr-*`. Read `native-renderer/docs/design.md`,
`backend.md`, `shaders.md`, `formats.md` and `d3d-api-map.md` first. Plain language, no em dashes, small
commits. Do not ask questions: decide, write it down, carry on. Finish with a report: what is on the branch,
what is verified and how, what the Windows side must run.

Since the Windows SDK patches changed (`backend/sdk-patches/`, including the bindless-table patch in
`3a3aa80`), apply all of them to the SDK source build you link against.

## A: Linux build and tests of the game renderer

> Read `cloud-phase2.md` and the docs it lists. Start from `nr-integration`, work on `cloud-p2-linux`.
> The Windows agent changed the backend (NrApi version 3, shader ABI 2, the game renderer in
> `backend/game_renderer.*`, `game_resources.cpp`, `game_passes.cpp`, `guest_device.h`) and the tests without
> re-running them on Linux. Build `native-renderer` and `rexgpu-native.so` on Linux against the SDK built from
> source with all patches, re-run `nr_draw_tests`, `nr_render_tests`, `nr_plugin_tests` and `nr_spirv_val`
> under the Khronos validation layer, and fix what fails. Then add tests for the game renderer that need no
> game: synthetic guest memory holding a device struct, shader objects (built with the shader tests'
> microcode assembler and container writer), textures, vertex and index buffers laid out as `d3d-structs.md`
> says, fed through the hook API; check pipeline state from the register images (blend, depth, cull, colour
> mask), render-target keying, resolves into textures (including k_8 and endian-none targets), clears, and the
> pixels read back from lavapipe. Every test must pass with the validation layer silent.

## B: shader correctness without the game

> Read `cloud-phase2.md` and the docs it lists. Start from `nr-integration`, work on `cloud-p2-shaders`.
> Today the translated shaders are only known to compile and match their sources structurally; none has been
> checked for what it computes. Write a reference interpreter for Xenos shader microcode on the CPU in the
> shader test suite (ALU vector and scalar ops with their modifiers, predicates, flow control and loops,
> constant and register addressing, vertex fetch with the formats in `formats.md`, texture fetch against small
> synthetic textures with point and linear filtering). The SDK's shader code under `src/graphics` documents the
> semantics. Then run the translator's output on lavapipe (a compute or draw harness through NVRHI or plain
> Vulkan) on the generated corpus plus random programs, with random inputs and constants, and compare against
> the interpreter (exact for integer and bit ops, a stated tolerance for float). Fix the translator for every
> mismatch (bump the translator version so packs are rebuilt) and list each fix in `shaders.md`. Also inline
> bool literal constants, which the translator does not do yet.

## C: textures follow-ups

> Read `cloud-phase2.md` and the docs it lists. Start from `nr-integration`, work on `cloud-p2-textures`.
> Two open items from `formats.md`: (1) small textures whose whole mip chain lives in the base level's packed
> tail (8x8 and 16x16 8:8:8:8, 16x16 DXT1) are bound with mip address 0 and max level 3 or 4; the SDK loads
> only level 0. Work out from the SDK's tiling and packed-mip code where hardware reads the smaller levels, add
> an option to load them, with tests. (2) Resolve conversions (EDRAM formats to the texture formats the game
> resolves into, including k_8 and 8:8:8:8 with endian none): converters and tests with synthetic data, wired
> where `game_renderer` does resolves.

The Windows side keeps the game-dependent work: running the golden scenes, frame dumps, and checking each
cloud branch against the real game.
