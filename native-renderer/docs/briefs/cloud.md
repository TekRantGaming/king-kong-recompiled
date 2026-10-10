# Running a brief in a Claude Code cloud session

The cloud container is Linux, has no GPU and has none of the game's files. That still covers most of Phase 1:
everything that is code, build system, converters, translators and tests with synthetic data. What needs the
game (traces of the recompiled code, validation against real shaders and textures, running the game) stays on
the Windows PC and is done there with the tools the cloud builds.

## What is and is not in the container

- The repository (public): branch `native-renderer` is the base; each stream has a branch `nr-<stream>` with
  the Windows agent's draft. Start from `nr-<stream>`, work on a new branch `cloud-<stream>`, push it, and
  say in your final report what is on it. Never push to `main` or to the `nr-*` branches.
- NOT available and never to be fetched or recreated: `kk/assets`, `kk/image.bin`, `kk/generated/default`
  (the recompiled game code), `tools/rexglue` (the prebuilt Windows SDK bundle), traces, dumps, the game's
  shaders and textures. The game itself cannot be built or run here; only the `native-renderer/*` targets.
- The ReXGlue SDK is public: https://github.com/rexglue/rexglue-sdk, tag `v0.10.0` (commit `f5337cdc`). Clone it
  for reading (the Xenos plugin, providers, presenter, shader translator, texture code) and, when a brief needs
  to link against it, either build it on Linux (`tools/build_appimage.sh` lines 20-35 list the apt packages: it
  wants clang-20 and lld-20 from apt.llvm.org, plus g++, cmake, ninja-build, pkg-config) or use the official
  Linux bundle: https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-linux-amd64.zip
  (headers, libraries, cmake config, `rexgpu-xenos.so`). The port's own SDK patches are in
  `tools/rexglue-patches` (apply in order with `git am` if you need the frame-log aid's source, patches 0003
  and 0012).
- Useful packages: `libvulkan-dev vulkan-tools mesa-vulkan-drivers` (lavapipe, a CPU Vulkan driver, so Vulkan
  code can run and be verified by readback without a GPU: set `VK_ICD_FILENAMES` to the lvp json under
  `/usr/share/vulkan/icd.d` if more than one driver is present), `spirv-tools glslang-tools`, `python3-numpy
  python3-pil`. DXC with the SPIR-V back end: the Linux build from
  https://github.com/microsoft/DirectXShaderCompiler/releases (the `linux_dxc_*.x86_64.tar.gz` asset, about
  40 MB; it has `bin/dxc` and `lib/libdxcompiler.so`).
- Vendored (MIT) under `native-renderer/thirdparty`: NVRHI and XenosRecomp. See `NOTES.md` there; record any
  change to vendored code in it.

## Rules (same as the Windows briefs where they apply)

- Plain language in docs and commit messages; no em dashes. Small commits; end commit messages with the
  attribution line your environment gives you.
- Keep tests self-contained (synthetic data, generated in the test); a test must never need a game file.
- Do not ask the user questions: decide, write the decision down in the stream's doc, carry on.
- When done, or when blocked, finish with a report: what is on the `cloud-<stream>` branch, what is verified and
  how, what is left for the Windows side (list the exact commands to run there), and open questions.

## Per-stream prompts

Each prompt below is self-contained: paste it into a new cloud session on this repository.

### 02 shaders

> Read `native-renderer/docs/briefs/README.md`, `cloud.md` (this file), `02-shader-translator.md`,
> `design.md` and `d3d-api-map.md`. Start from branch `nr-02-shaders` (a container parser draft is there), work
> on `cloud-02-shaders`. Build the translator as a CMake project under `native-renderer/shaders`: the 2005
> container adapter, XenosRecomp as the translation core (vendored; its submodules are absent: use system or
> FetchContent-free copies of fmt and xxHash from the SDK clone's `thirdparty`, drop zstd and smol-v), DXC for
> DXIL and SPIR-V (download the Linux DXC release), SPIR-V validation with `spirv-val`, a disk cache keyed by
> microcode hash, and a command-line tool. Since the game's shaders are not available here, write a small Xenos
> microcode assembler in the test suite (XenosRecomp's `shader.h` has the encodings; the SDK's `xenos.h` and
> ucode headers too) and generate a corpus covering every instruction class the recompiler handles, including
> vertex fetch, texture fetch modes, dynamic indexing and loops; every generated shader must translate, compile
> to DXIL and SPIR-V and validate. Report what the Windows side must run to validate the real database.

### 03 textures and resources

> Read `native-renderer/docs/briefs/README.md`, `cloud.md`, `03-textures-resources.md`, `design.md` and
> `d3d-api-map.md`. Start from branch `nr-03-textures` (a library skeleton with tiling and a test harness is
> there), work on `cloud-03-textures`. Finish the converters for every Xbox 360 texture format (the SDK's
> texture code under `src/graphics` and `include/rex/graphics/xenos.h` in the rexglue-sdk clone are the
> reference), tiling and untiling for 2D, cube and volume textures with mips, packed and block formats, big-endian
> swaps for 8, 16 and 32-bit element layouts, vertex and index buffer endian handling driven by a vertex
> declaration description, and the render-target pool. Unit tests with synthetic data for each converter,
> round-trip tests for tiling, and a documented host format choice for every guest format in
> `native-renderer/docs/formats.md`. Report what the Windows side must run to validate against real textures.

### 04 backend plugin

> Read `native-renderer/docs/briefs/README.md`, `cloud.md`, `04-backend-plugin.md`, `design.md` and
> `d3d-api-map.md`. Start from branch `nr-04-backend` (plugin and backend drafts are there, unbuilt), work on
> `cloud-04-backend`. Build on Linux: NVRHI's Vulkan backend (set `NVRHI_WITH_DX11`, `NVRHI_WITH_DX12` off here;
> keep the D3D12 shim for Windows), the plugin as `rexgpu-native.so` against the SDK (official Linux bundle or a
> source build), and a standalone test host under `native-renderer/backend/tests` that creates a Vulkan device on
> lavapipe, puts NVRHI on it, and renders milestone 1 (a clear colour) and milestone 2 (a triangle) into an
> offscreen image that it reads back and checks pixel values of. Then milestone 3 as far as it goes without the
> game: the hook table and draw-state tracking as a library with unit tests that feed it recorded call sequences
> (use the per-draw sequence in `d3d-api-map.md`) and check what reaches NVRHI. Report exactly what the Windows
> side must do to run the plugin inside the game.

### 05 test harness

> Read `native-renderer/docs/briefs/README.md`, `cloud.md`, `05-test-harness.md`. Start from branch
> `nr-05-harness` (runner, scene list and a first compare tool are there), work on `cloud-05-harness`. Finish
> the Python compare tool: BMP and PNG input, per-frame difference scores, heat maps, draw-log diff for the
> `REX_DEV_FRAME_LOG` format (patch 0003 in `tools/rexglue-patches` shows the format), an HTML report, and tests
> with synthetic images (including a tolerance model for animated regions). The PowerShell runner and the golden
> captures are Windows-only; leave them as they are except for fixes you can verify by reading.

Stream 01 (the D3D layer map) needs the recompiled code and stays on Windows.
