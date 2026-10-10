# kkshaders: the shader translator

Xbox 360 shader microcode to HLSL, DXIL (D3D12) and SPIR-V (Vulkan), for the native renderer. Brief:
`../docs/briefs/02-shader-translator.md`.

```
2005 container ──> container adapter ──> XenosRecomp core ──> HLSL ──> DXC ──> DXIL (signed) + SPIR-V
(xeshaders.bin,     src/container.cpp      (vendored, reworked)    hlsl/kk_common.hlsli
 CreateVertexShader)                                                prelude: the binding model
                                                        └─> ShaderBindings (what the backend binds)
cache: ShaderCache (one file per shader) and ShaderPack (one memory-mapped file), keyed by
XXH3-64 of the microcode + a hash of the translation inputs + the translator hash
```

## Building and testing

Linux (what the cloud sessions use):

```
cmake -S . -B out/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
  -DKKSHADERS_SDK_DIR=<ReXGlue SDK clone with thirdparty/fmt and thirdparty/xxHash checked out> \
  -DKKSHADERS_DXC_DIR=<DXC release, e.g. linux_dxc_2025_07_14 from DXC v1.8.2505.1>
cmake --build out/build/linux && (cd out/build/linux && ctest)
```

Windows: `build.bat test` (VS 2022's clang + Ninja, the SDK clone at `C:\rexsrc`; set `KK_DXC_DIR` to a
DXC release folder, or put `dxcompiler.dll` and `dxil.dll` on `PATH`). `spirv-val` comes from the Vulkan
SDK (`%VULKAN_SDK%\Bin`) when it is installed.

The tests need no game data:

| Test | What it checks |
|---|---|
| `assembler` | the test assembler's encodings (written from the SDK's `ucode.h`) against XenosRecomp's bitfield structs, 20,000 random instructions of each kind plus every control flow instruction |
| `container` | container writer / parser round trip, the database reader, malformed and truncated input (2,000 mutated containers through the parser and the translator) |
| `corpus` | every generated shader (each vector and scalar operation in both stages, constants, predication, every control flow form, relative registers, vertex fetch in both modes and every format, every texture dimension and fetch mode, outputs, 400 random programs), as a container and as bare microcode: translate, DXIL (signed and validated by DXC's validator), SPIR-V (DXC's validator and `spirv-val`), cache entry round trip |
| `cache` | cache store / load, damaged files, pack lookups (all found, each under a millisecond) |
| `execution` | when a Vulkan device is available (Mesa's lavapipe on Linux): translated shaders run on the device and their results are compared with a CPU reference interpreter of Xenos microcode (`tests/xenos_ref.cpp`) for ALU operations, random programs with every control flow form, vertex fetch decoding of every format, and 1D / 2D texture addressing |
| `fixtures`, `tool-*` | the tool's database commands on a synthetic `xeshaders.bin` and shader storage file |
| `database` | only when `KKSHADERS_DATABASE` (default `kk/assets/Shaders/xeshaders.bin`) exists: `db-build` on the real database |

## The tool

```
kkshaders db-check <xeshaders.bin>                          parse every entry (sizes add up, no errors)
kkshaders db-build <xeshaders.bin> --out <dir> [--dxc <dir>] [--spirv-val <exe>] [--jobs N] [--both]
                                                            translate + compile + validate all, write
                                                            <dir>/report.txt (failures by cause, per
                                                            family), <dir>/failed/*, <dir>/kkshaders.pack
kkshaders db-structure <xeshaders.bin>                      constants / samplers / outputs per family,
                                                            checked against the shipped HLSL sources
kkshaders xsh <file.xsh>... --database <xeshaders.bin> --out <dir>
                                                            the emulator's shader storage records not in the
                                                            database, as bare microcode
kkshaders pack-lookup <pack>                                time a lookup of every entry
kkshaders info|translate|compile <container> [--raw-vs|--raw-ps]
```

Everything written under `--out` is derived from the game's files: keep it out of git (`out/` and `work/`
are ignored).

## The binding model (`include/kkshaders/abi.h`, `hlsl/kk_common.hlsli`)

One layout for every shader, so one root signature / pipeline layout serves all:

| Binding | Contents |
|---|---|
| `b0 space0` | vertex float constants c0-c255, the device's shadow at +1920 as is |
| `b1 space0` | pixel float constants c0-c255, the device's shadow at +6016 as is |
| `b2 space0` | `DrawConstants`: 256 bool constants, 32 loop constants (+10144), a descriptor index per texture fetch constant, the samplers of each stage, the vertex fetch table, clip planes, half-pixel offset, alpha test |
| `t0 space1-4` | bindless `Texture2D[]`, `Texture3D[]`, `TextureCube[]`, `Texture2DArray[]` (stacked 3D: bit 31 of the index) |
| `s0 space5` | bindless `SamplerState[]` |
| `t0 space6` | bindless `ByteAddressBuffer[]`: vertex data straight from guest memory, big-endian |

In SPIR-V the register is the binding and the space the descriptor set; constant buffers use the D3D layout.

Choices worth knowing about:

- **Vertex input is pulled, not declared.** The game's vertex shaders reach `CreateVertexShader` with
  unpatched `vfetch` templates (format 0, fetch slot 0); the XDK patches them for each declaration at
  draw time. Here every `vfetch` reads its declaration element through `DrawConstants::vertexFetch[k]`
  (buffer, offset, stride, format word with the Xenos format, number format, endian and a component
  swizzle) and decodes big-endian data in the shader. One compiled shader serves every declaration, no
  input layouts, no vertex buffer conversion, every Xenos vertex format works (10:11:11, 16-bit floats,
  normalised and integer forms). The backend fills the table from the declaration object (+11408) and
  the stream sources (+12556) at draw time; `ShaderBindings::vertexBindings` says which usage each entry
  is. This replaces "vertex declarations become input layouts" in `docs/design.md`.
- **Textures and samplers are bindless.** `ShaderBindings::samplers` lists one entry per (fetch
  constant, filter overrides of the fetch instruction) the shader uses; the backend makes a sampler for
  each from the fetch constant (+1152 + slot * 24) with the overrides applied. Cube maps go through the
  hardware `cube` instruction and back to a direction for `TextureCube`, exactly, so they also work in
  loops and branches.
- **Constants are the device's register files**, indexed directly (`kk_VC[n]`, `kk_PC[n]`), relative
  reads (`a0`, `aL`) bounds-checked to 0. The float literals of a shader (`def`) are inlined where read
  directly; the backend must still write the shader's register block (`ShaderBindings::registerWrites`)
  into its shadow at SetVertexShader / SetPixelShader as the XDK does.
- **No specialisation constants or DXIL linking.** Alpha test, clip planes and the half-pixel offset are
  uniform branches on `DrawConstants`.
- **One interpolator signature** (TEXCOORD0-15, COLOR0-1) in every vertex and pixel shader, so any pair
  links. Other semantics are reported as errors.

## Semantics

Instruction behaviour follows the notes in the SDK's `graphics/format/ucode.h` (Xenia's research):
Direct3D 9 multiplication (0 times anything is +0, also in `mad` and the dot products), `max` / `min` as
comparisons, both operations of an ALU instruction reading their sources before either writes, the
clamping variants of `rcp` / `rsq` / `log`, `setp_*` / `kill*` / `maxa` / `cube` / `dst` / `max4` as
specified, conditional ends that end only when they run, loops with the loop constant's count, start and
step (aL clamped to [-256, 256]), predicated breaks, calls and returns, relative temporaries (a 64-entry
register array when a shader uses them).

Not modelled: memory export (dropped, with a warning), `getBCF` (returns 0), the vertex kill flag and
point size of export 63, `fetchValidOnly`, `kill` in vertex shaders.
