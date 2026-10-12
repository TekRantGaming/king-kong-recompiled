# Shader translator (stream 02)

Status: 10 October 2026. Every shader of the game's database and every shader in today's shader caches is
translated from Xbox 360 microcode to HLSL, compiled to DXIL and SPIR-V, and validated. The translated HLSL
matches the 37 HLSL sources shipped in the database structurally. A prebuilt pack holds all of them; a lookup
by hash takes about 0.01 ms. Phase 2 (same date): what the translated code computes is checked on a GPU
against a reference interpreter; nine translator fixes came out of it, one more from ucode.h, and bool literals are now inlined (translator version 4, see "What the
translated shaders compute"), so packs and caches built before must be rebuilt.

Generated shaders, packs and reports derived from the game's files live in `F:\KK-native-renderer\analysis\02\`
(the final run is `analysis\02\final*`), never in git.

## What is where

| Path | What |
|---|---|
| `native-renderer/shaders/include/kkshaders/container.h`, `src/container.cpp` | The 2005 XDK container adapter and the `xeshaders.bin` reader |
| `native-renderer/shaders/include/kkshaders/translator.h`, `src/translator.cpp` | Microcode to HLSL (drives the XenosRecomp core), bindings, cache keys |
| `native-renderer/thirdparty/XenosRecomp/XenosRecomp/shader_recompiler.*` | The translation core (XenosRecomp, MIT, reworked; changes listed in `thirdparty/NOTES.md`) |
| `native-renderer/shaders/hlsl/kk_common.hlsli` | The prelude every shader starts with: bindings, constant buffers, Xenos ALU rules, vertex pulling |
| `native-renderer/shaders/include/kkshaders/abi.h` | What the backend uploads and binds (the C++ side of the prelude) |
| `native-renderer/shaders/include/kkshaders/compiler.h`, `src/compiler.cpp` | DXC (loaded at run time) to signed DXIL and to SPIR-V; external `spirv-val` |
| `native-renderer/shaders/include/kkshaders/cache.h`, `src/cache.cpp` | Disk cache, the memory-mapped pack, `ShaderProvider` (pack, then cache, then compile) |
| `native-renderer/shaders/tool/main.cpp` | `kkshaders`: `info`, `translate`, `compile`, `db-check`, `db-build`, `db-structure`, `xsh`, `pack-lookup` |
| `native-renderer/shaders/tests/` | A Xenos microcode assembler, a container writer and a synthetic corpus (no game data) |
| `native-renderer/shaders/tests/xenos_interp.*` | The reference interpreter: Xenos microcode on the CPU, with an error bound on every value |
| `native-renderer/shaders/tests/gpu_tests.cpp`, `vk_harness.*`, `random_program.*` | `kkshaders_gpu_tests`: translated shaders run on Vulkan and compared with the interpreter |

## Building and testing on Windows

Tools (downloaded or built once, outside git):

- DXC: the official Windows release `dxc_2026_09_29.zip` (v1.9.2609) from
  github.com/microsoft/DirectXShaderCompiler/releases, unpacked in
  `F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc` (`bin\x64\dxcompiler.dll`, `dxil.dll`, `inc\dxcapi.h`).
- `spirv-val`: built from the SDK clone's `C:\rexsrc\thirdparty\spirv-tools` (with
  `SPIRV-Headers_SOURCE_DIR=C:/rexsrc/thirdparty/spirv-headers`, Ninja, Release) into
  `F:\KK-native-renderer\downloads\spirv-tools-build\tools\spirv-val.exe`. No Vulkan SDK is needed.
- fmt and xxHash come from the SDK clone at `C:\rexsrc` (the `release` preset).

Build and test (`build.bat` sets up VS 2022's clang and Ninja, then runs the `release` preset):

```
set KK_DXC_DIR=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc
set KK_SPIRV_VAL=F:/KK-native-renderer/downloads/spirv-tools-build/tools/spirv-val.exe
native-renderer\shaders\build.bat test
```

All 11 tests pass (assembler, container, corpus, cache, fixtures, the five tool tests, and `database`, which
builds the real database into the build folder when `kk/assets/Shaders/xeshaders.bin` exists; `-E "^database$"`
skips it).

The runs behind this document (`kkshaders.exe` from `out\build\release`, `$DXC`, `$SPIRV_VAL` as above):

```
kkshaders db-check   kk\assets\Shaders\xeshaders.bin
kkshaders db-build   kk\assets\Shaders\xeshaders.bin --image kk\image.bin --split --dxc $DXC --spirv-val $SPIRV_VAL --out analysis\02\final\pack
kkshaders db-structure kk\assets\Shaders\xeshaders.bin --out analysis\02\final\structure
kkshaders xsh <cache>\shaders\shareable\555307D3.xsh [...] --database kk\assets\Shaders\xeshaders.bin --all --dxc $DXC --spirv-val $SPIRV_VAL --out <dir>
kkshaders pack-lookup analysis\02\final\pack\kkshaders-spirv.pack
```

## The containers, as found in the real data

The cloud session wrote the adapter from the Windows draft without seeing a real shader. Against the 6,606
entries of `xeshaders.bin` it needed four fixes (each has a test in `tests/tests.cpp`):

1. Stripped constant tables. All 4,240 vertex containers carry a D3DX table with creator 0 and stale bytes in
   the target field (for example `_FUR`); 2,137 of them failed on that. The target is now optional.
2. No table. Two vertex shaders have constant-table offset 0.
3. Interpolators. Binding-table word 8 counts all the words after the vfetch list, not the interpolators. The
   interpolators are the first `VS_EXPORT_COUNT + 1` of them (`SQ_PROGRAM_CNTL` bits 20-23, binding-table
   word 0). In 20 shaders more words follow, instruction addresses rather than semantics (they read as usage
   15, `TESSFACTOR` or `COLOR5`). Checked on all 4,240 vertex shaders: the semantics found this way name exactly
   the export registers the microcode writes.
4. Damaged names. In the vertex containers the game uses (A), the constant table's string area is partly
   overwritten (`g_vF` for `g_vFogParams`, `Z`, `K`, garbage). The second container of each entry (B, a separate
   compile with a full table, see below) has the same registers; `Database::parse` takes the names from it.
   Names only feed comments and the structural diff, never the translation.

Which container is used: 1,596 of the 1,604 distinct vertex shaders in the shader caches are container A with
its vfetch instructions patched (the other 8 match no database container); none match B. B is a different
compile (other microcode, other literals, `vs_3_0` target, five full-mask interpolators) and is never created.

Which magic is which: in the database `0x102A0E01` is a vertex shader and `0x102A0E00` a pixel shader (every
`ps_3_0` table is in a pixel entry). The game image agrees: the D3D library's own shaders at `0x8203E800`
(`0x102A0E01`, target `vs_3_0`) go to `sub_82111D90` and the ones at `0x8203E908` / `0x8203EAC8`
(`0x102A0E00`, `ps_3_0`) to `sub_82111CA0`. So `sub_82111D90` creates vertex shaders and `sub_82111CA0` pixel
shaders. `d3d-structs.md` and `d3d-api-map.md` (stream 01) have these two, and the magic numbers, the other
way round; the hooks must use the creating function to pick the kind, and the parser uses the magic, which is
consistent with it.

The database's 23 kind-0 entries are 36-byte records, not shaders (they look like a pairing table); they are
skipped.

## The model the backend implements

`abi.h` and the top of `kk_common.hlsli` are the contract; in short:

- One layout for every shader: `b0` the 256 vertex float constants (device +1920), `b1` the 256 pixel float
  constants (+6016), `b2` the draw constants (bool and loop constants, texture and sampler descriptor indices,
  the vertex fetch table, clip planes, half-pixel offset, alpha test), bindless texture arrays (2D, 3D, cube,
  2D array for stacked 3D), samplers, and byte-address buffers holding guest vertex data as it is in memory.
  In SPIR-V the register is the binding and the space the descriptor set; constant buffers use the D3D layout.
- Vertex input (brief point 4): the game's shaders come with containers, so they are translated in binding
  mode: each vfetch is bound to the declaration element with its usage and usage index (from the container), and
  the backend fills `DrawConstants::vertexFetch[binding]` at draw time from the vertex declaration and the
  stream sources: buffer descriptor, byte offset of the element for vertex 0, stride, and a format word with the
  element's `D3DDECLTYPE` format, endian (8in32 for guest buffers) and swizzle. A usage the declaration lacks gets
  format 0 and reads (0, 0, 0, 1) without touching memory. One translated shader therefore serves every
  declaration and stride, unlike the console, which patches the microcode per declaration (`sub_82120028`).
  Microcode without a container (the emulator's cache records) is translated in instruction mode instead: the
  patched vfetch instructions carry format, stride and offset, and the table gives the vertex fetch constant's
  buffer (stream s uses fetch constant 95 - s).
- Constants (brief point 5): the float shadows are uploaded as the device holds them. A shader's literal
  constants (the register block, applied by SetVertexShader / SetPixelShader) should still be applied to the
  shadow, but the translated code no longer depends on it for floats: direct reads are inlined and relative
  reads that land on a literal register are answered in the shader (`kkConstRel`; the skinning shaders index
  c250-c254 by the loop counter). Loop literals are inlined. Bool literals are inlined too, except the bools
  the constant table names: the literal block writes whole dwords of 32 bools and the game then sets the named
  ones itself, so those are read from `b2`. No shader of the database reads a bool constant today.
- Interpolators: one signature for all shaders (TEXCOORD0-15, COLOR0-1), so any vertex shader links with any
  pixel shader; the container's semantics place each export register.

## What the translated shaders compute (differential tests)

Phase 2 (cloud session, Linux). `kkshaders_gpu_tests` runs translated shaders on a Vulkan device and compares
every output with a reference interpreter. No game data is involved: the shaders are the generated corpus
(`tests/corpus.cpp`, including its fuzz group) and random programs (`tests/random_program.cpp`).

The reference interpreter (`tests/xenos_interp.*`) decodes the microcode bit by bit, independently of
XenosRecomp, and follows the SDK's shader code: `ucode.h` for the encodings and notes,
`pipeline/shader/interpreter.cpp` for the ALU and control flow, `spirv_translator_alu.cpp` and
`spirv_translator_fetch.cpp` for vertex formats (`formats.md`) and texture coordinates. It covers the vector
and scalar ALU with their modifiers (negate, abs, saturate, swizzles, write masks, exports), predicates and
predicated exec / jump / call / loop break, bool-conditioned exec / jump / call, loops with aL (clamped to
[-256, 256]), calls up to depth 4, a0 and aL addressing of temporaries and constants, vertex fetch in every
format in both binding mode and instruction mode, and texture fetch on 1D / 2D / 3D / cube textures with point
and linear filtering, wrap or clamp, offsets, unnormalized coordinates and an explicit LOD. Not modelled (not
compared): getCompTexLOD, getWeights, getBCF, getGradients, mipmaps and gradients.

Tolerance. Integer and bit results (fetch decoding of integer and normalized formats, a0, aL, predicates,
comparisons, select, floor) are compared exactly. Every float carries an error bound: inputs are exact
dyadic values, arithmetic adds one ulp per inexact rounding (fused or separate multiply-add both accepted),
`rcp` 2^-21, `rsq` 2^-20, `sqrt` 2^-21 relative, `exp` 2^-18 relative, `log` 2^-18 absolute, `sin` / `cos` 2^-17
absolute (growing with the argument beyond pi), linear filter weights 2^-8 (8-bit sub-texel precision, evaluated over the coordinate's error
interval). A GPU value passes when it is within that bound plus 2^-20 relative (and the smallest normal, for
flushed denormals); NaN must be NaN and infinities exact. A value whose bound cannot be stated (a comparison
or floor that could go either way within its error) is "unknown" and skipped; a control decision of that kind
(predicate, kill, a0, branch) marks the invocation "fragile" and the whole invocation is skipped. Both are
counted in the report.

The harness (`tests/vk_harness.*`, plain Vulkan 1.3; lavapipe here) binds the descriptor layout of `abi.h`
(set 0: b0-b2, sets 1-6: the bindless arrays) plus a set 7 of its own. Vertex shaders are wrapped in a compute
shader that calls the translated `main` for 64 vertices and stores all outputs; pixel shaders are drawn as 64
one-pixel points with their interpolators from a harness vertex shader, into four RGBA32F targets. Each
round draws random float, bool and loop constants (now and then a loop whose aL leaves [-256, 256]), vertex
data in every format with random declarations or fetch constants, random textures and interpolators. The
float literals are not applied to the uploaded constants, so the translated code has to answer them itself.

Run (Linux here; on Windows the target is built when CMake finds the Vulkan SDK, and runs on any Vulkan driver):

```
cmake -S native-renderer/shaders -B build -G Ninja -DKKSHADERS_SDK_DIR=<sdk src> -DKKSHADERS_DXC_DIR=<dxc>
cmake --build build
build/kkshaders_gpu_tests interp                               # the interpreter's own checks
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  build/kkshaders_gpu_tests gpu --dxc <dxc> --corpus 1 --fuzz 100 --random 100 --rounds 2
```

`ctest` runs both (`interpreter`, `gpu`) when CMake finds Vulkan. A failing shader's HLSL and report go to
`--out`; `--filter NAME --trace 1` prints the interpreter's step by step trace for it.

Results (10 October 2026, lavapipe / LLVM 20.1.2, DXC v1.9.2609, validation layer on, no validation errors),
after the fixes below, all with 0 mismatches:

| Run | Shaders | Invocations compared | Fragile (skipped) | Components compared | Unknown (skipped) |
|---|---|---|---|---|---|
| `ctest` `gpu` (defaults: corpus with fuzz group, 200 random programs per kind, 3 rounds) | 1,757 | 308,112 | 28,976 | 7,429,881 | 356,191 |
| corpus with `--fuzz 100`, 3 rounds | 957 | 175,384 | 8,104 | 3,809,275 | 147,137 |
| random, 150 per kind, 2 rounds, seeds 101, 777, 2026, 31337 | 2,400 | 263,614 | 43,586 | 7,253,097 | 420,255 |

Random programs mark about one invocation in seven fragile (a predicate, kill or branch on a value within
its error, mostly comparisons of results of transcendental functions); the corpus about one in twenty.
4 of the corpus's 2,871 rounds are skipped: the program does not end within 200,000 control-flow steps on the
reference (a loop whose count comes from a random constant), so that round is not run on the GPU.

### Translator fixes from the differential tests (translator version 4)

1. Cube fetches ignored the instruction's S / T / face offsets and its unnormalized-coordinates flag. They
   are now applied before the cube coordinates become a direction (offsets in texels of the face).
2. The cube face index was rounded (`floor(z + 0.5)`); the SDK truncates it after clamping to 0-5
   (`kk_CubeDirection`).
3. In one ALU instruction, the vector half's a0 (`maxa`) and p0 (`setp_*_push`) took effect only after the
   scalar half had read its operands, and the vector predicate won over a scalar predicate write. The SDK
   runs the vector half first: its state changes are visible to the scalar half's operands and a scalar
   predicate or a0 write replaces them.
4. aL was not clamped; it is now clamped to [-256, 256] as in the SDK's interpreter (the SDK's SPIR-V
   translator does not clamp; the game's shaders never come near the limit).
5. A loop with the repeat flag reset aL to the loop constant's start (structured mode) or kept the iteration
   counter (general mode). ucode.h and the SDK's SPIR-V translator define it as "reuse the current aL instead
   of resetting it to the loop start": aL starts from the enclosing aL and the count always restarts. The
   SDK's CPU interpreter keeps the counter instead; it is the outlier and is not followed.
6. Not-equal (`sne`, `setp_ne`, `kill_ne`, `snes`, `kills_ne` and the alpha test's NOTEQUAL) must be
   unordered, true when a side is NaN (the SDK uses `OpFUnordNotEqual`). DXC lowers HLSL's `!=` to an ordered
   compare for SPIR-V (false for NaN) and to `fcmp fast une` for DXIL; it now goes through `kk_Ne`,
   `!(a == b)`, which DXC lowers to `OpFOrdEqual` + `OpLogicalNot` and to a plain `fcmp une`.
7. In instruction mode (bare microcode) a `vfetch_mini` took its binding and stride from the previous
   `vfetch_full` in program order. The SDK keeps the address of the `vfetch_full` that ran last, which in a
   loop is not the same one; the translated code now does the same (`vfBinding`, `vfStride`). Binding mode
   is unaffected (each fetch has its own declaration element).
8. Multiplication (`kk_Mul`, `kk_Muls`, so also mad, dp*, dst, muls_prev2) tested `a == 0` for the Direct3D 9
   rule "+0 or a denormal times anything is +0", leaving denormals to the host's flush mode. lavapipe keeps
   denormals; the test is now `|a| < FLT_MIN`, the SDK's stated rule.
9. Shaders are now compiled with DXC `-Gis` (IEEE strictness). Without it DXC folds `x - x` and `x * 0` to 0
   in DXIL (fast-math flags) and leaves SPIR-V open to the same in the driver; lavapipe folded `NaN - NaN` to 0.
   With it DXIL keeps plain `fsub` / `fmul` and SPIR-V arithmetic is marked `NoContraction` (no fused
   multiply-add either, which the reference accepts both ways). The cost is the driver's freedom to
   reassociate; correctness of NaN and infinity was preferred.
10. Not from a mismatch but from ucode.h while chasing one: `log`, `sqrt` and `rsq` of a negative input are
   NaN, and `log(1) = 0`, `rcp(1) = rsq(1) = 1`, `exp(0) = 1` exactly. Vulkan leaves `log2`, `sqrt` and
   `inversesqrt` of a negative undefined and allows a few ulp at those points, so the prelude makes both
   explicit (`kk_Exp`, `kk_Log`, `kk_Rcp`, `kk_Rsq`, `kk_Sqrt`); the reference now expects the exact values.
11. Bool literals (not a mismatch, the brief's request): the bits of a literal bool dword that the constant
   table does not name are inlined (see "The model the backend implements"). A unit test checks it.

What the reference does not compare, with the reason (each found as a false mismatch, then excluded):
texture coordinates beyond 1024 repeats and cube directions that are infinite, NaN, beyond 1e18 or below
1e-18 (how a GPU wraps or normalises them is its own precision; lavapipe normalises cube directions);
`rcp`, `rcpc`, `rsq`, `rsqc` of an exact zero (the sign of the result follows the sign of the zero, and
Vulkan does not preserve the sign of zero by default: lavapipe turned `0 - floor(0)` into -0).

Some mismatches were the reference's own mistakes, fixed there, not in the translator: `setp_pop` of NaN
(the SDK's `islessequal`, false for NaN), a cube direction component that had overflowed being used
although marked unknown, and the linear-filter bound never ending for coordinates past 2^24.

## Coverage

Database (`xeshaders.bin`, 6,606 entries):

| | Count |
|---|---|
| Shader entries | 6,583 (4,240 vertex, 2,343 pixel) + 23 non-shader records |
| Parsed | 6,583 (and all 4,240 second containers) |
| Plus the D3D library's own shaders found in `kk/image.bin` | 2 (1 vertex, 1 pixel; a third is a duplicate) |
| Translated | 6,585 |
| Compiled to DXIL, signed and validated (DXC validator 1.9) | 6,585 |
| Compiled to SPIR-V, validated by DXC and by `spirv-val` (Vulkan 1.2) | 6,585 |
| Failures | 0 |
| Distinct by microcode and translation inputs (pack entries) | 4,796 |

By family (all built): psgeneric 2,245, pswater 29, psaftereffects 27, pslightshaft 14, psreflection 6,
psheatshimmer 4, psshadow 4, pssprite 4, psrain 3, psshadowquad 3, psblurshadow 1, pscompositeshadow 1, psfur 1,
psocean 1; vsgeneric 4,054, vswater 51, vswyb 30, vsaftereffects 24, vsfur 22, vsshadow 16, vssymmetry 13,
vslightshaft 12, vssprite 7, vsheatshimmer 4, vsrain 4, vsblurshadow 1, vsreflection 1, vsshadowquad 1.

Shader caches (bare microcode records, keyed by the XXH3 of the big-endian microcode; all hashes check):

| Input | Records | Built and validated (DXIL and SPIR-V) |
|---|---|---|
| Today's renderer's cache, `F:\KK-native-renderer\cache` | 677 (462 vertex, 215 pixel) | 677 |
| That plus the user's cache with the shader pack (`Documents\king_kong\cache`) and the tour cache, merged | 2,478 (1,604 vertex, 874 pixel) | 2,478 |

Of the 874 cached pixel shaders, 871 are in the database, one is the D3D library's own pixel shader from the
game image, and two are two-instruction shaders (24 bytes of microcode) found in no file: most likely built by
the D3D library in code (clears); the native renderer replaces those paths. The cache's vertex records are the
patched forms of database containers, so the runtime (which creates shaders from containers) never needs them;
they are built only to prove instruction mode on real data.

Failures by cause during the work, all fixed: 2,137 parse failures (stripped table), 11 parse failures
(interpolator words), 2 parse failures (no table), 4 translation failures (`COLOR5` / `TESSFACTOR` read from the
extra binding words). There were no DXC or validator failures on real shaders at any point.

Features in the database's shaders (from `db-structure`): literal float constants 6,522; loops 1,474 (all
structured); relative constant addressing 1,467; pixel position (VPOS)
1,621; cube textures 17; predication 38; kill 9; general (unstructured) control flow 0; dynamic temporary
indexing 0; bool constants 0; 3D textures 0; depth output 0; explicit LOD or gradients 0.

## Structural diff against the 37 HLSL sources

`kkshaders db-structure` checks every shader against its family's source with all its includes:

- every constant name of the shader's table is declared in the source;
- every float constant the translated code reads directly, and every base of a relative read, lies in a
  constant-table range or is a literal register; literals never overlap the table; bool and loop constants
  likewise (none are read);
- every texture fetch constant sampled is a sampler register of the table, with the dimension of its declared
  type where the table gives one (arrays of `sampler` carry a single type, so they are not checked);
- pixel outputs written are declared by the source (`: COLORn`, output structs, `DEPTH`; a float4 return with no
  semantic counts as COLOR0);
- the container's interpolator semantics (vertex outputs, pixel inputs) are declared by the source.

Result: 6,583 of 6,583 shaders match. Notes:

- The 37 sources are 28 families with shaders (14 pixel, 14 vertex), 5 shared includes (`xeshareddefines.h`,
  `shadercommon.hlsl`, `vscommon.hlsl`, `vslighting.hlsl`, `pscommon.hlsl`) and 4 sources no entry is keyed to
  (`vsspg22`, `psspg2`, `psapplyshadow`, `psblurshadowpc`).
- 3,327 vsgeneric shaders use constants declared only in `vsspg22.hlsl` (`g_avSPG2Wind`, `g_fSPG2Ratio` and
  the other SPG2 constants), which includes `vsgeneric.hlsl` and is compiled under the generic family's keys. The check
  accepts names from sources that include the family's source and counts them separately.
- Every pixel shader writes COLOR0 only. `psheatshimmer.hlsl` declares a DEPTH output that none of its 4
  compiled shaders writes. `psapplyshadow.hlsl` declares two colour outputs but has no compiled shader.
- Some sources declare `_centroid` interpolators; the translation interpolates linearly, which is the same at
  1x MSAA (the only mode the game uses).

## The prebuilt pack

A pack is derived from the game's files, so the port never ships one: players get theirs built on their own
machine from their own copy of the game (next section). `db-build` stays the developer's tool for checking the
whole database (both compilers, both validators, failure reports).

`db-build` writes the pack (`kkshaders.pack`, both backends) and with `--split` one pack per backend. A player
needs only the one for their renderer (Vulkan first).

| Pack | Size |
|---|---|
| Both backends | 173 MB (165 MiB) |
| SPIR-V only | 119 MB |
| DXIL only | 55 MB |

Per shader: vertex DXIL 13.0 KB and SPIR-V 30.0 KB on average, pixel DXIL 8.8 KB and SPIR-V 16.6 KB. Building
the whole database takes 93 s on 16 threads (both compilers, both validators).

Load time (`pack-lookup`, every one of the 4,796 entries, file in the OS cache): open and map 0.1 ms; lookup,
checksum and copy-out average 0.014 ms (both backends), 0.010 ms (SPIR-V), 0.004 ms (DXIL); worst 0.09 ms. The
brief's limit is 1 ms. A cold read adds one disk read of 10-60 KB per shader. The `pack-lookup` test fails if
any lookup takes 1 ms or more.

Sizes, and what brought them down from the first full run (575 MB for both backends, vertex SPIR-V 61 KB on
average, a five-input shader 190 KB):

- vertex decoding driven by a small format table instead of a switch over every format, per fetch;
- `select` instead of short-circuit conditionals (in HLSL 2021 a scalar `?:`, `&&` or `||` becomes a branch,
  which DXC's SPIR-V back end keeps);
- one `if` block per run of instructions with the same predicate;
- vertex inputs fetched once at the top through one decode loop when every vfetch indexes by an untouched r0.x
  (true for all 4,240 database vertex shaders).

Keys: `(XXH3-64 of the big-endian microcode, hash of the translation inputs)`; each entry carries the
translator hash (translator version 2, ABI version, prelude text), and a pack from another translator is refused.

## The pack on the player's machine

Status: 11 October 2026, branch `nr-pack`. No pack and no other file derived from the game ships with the port.
The launcher builds the pack from the player's own `Shaders/xeshaders.bin` into their cache folder, the way it
already takes its artwork from their game files (`kk/src/launcher_art.cpp`).

### How it works

| Piece | Where | What |
|---|---|---|
| The builder | `shaders/include/kkshaders/pack_builder.h`, `src/pack_builder.cpp` | `buildPack(options, progress)`: reads the database, keeps every distinct `(microcode hash, input hash)` once (4,794 of the 6,583 shader entries), translates and compiles them for **one** backend (`buildShader` with `kBuildSpirv` or `kBuildDxil`) on N worker threads (one DXC compiler each), with progress counters and a cancel flag, then writes the pack and its stamp. `checkPack(options)` says whether the pack is current. |
| The tool | `shaders/tool/main.cpp` | `kkshaders pack-build <xeshaders.bin> --out <folder> [--backend spirv\|dxil] [--scaled] [--jobs N] [--low-priority] [--force] [--check]`: the same build from the command line (the measurements below). |
| The plugin's service | `plugin/shader_pack_service.*`, `plugin/native_shader_pack_api.h` | Exports `nr_get_shader_pack_api()`: `start(cache_dir, game_dir, threads, low_priority)` runs `buildPack` on a thread of the plugin's for the pack the current settings need, `poll`, `cancel`, `release`. The plugin has the translator and DXC already; the launcher only needs the plugin it already loads for its settings (`PreloadGpuPlugin`). No separate program. |
| The run's lookup | `plugin/native_graphics_system.cpp` (`InitializeShaderStorage`), `backend/shader_library.*` (`OpenPack`), `backend/backend.*` (`OpenShaderPack`) | See "Which pack a run uses". |
| The launcher | `kk/src/native_shaders.*`, a few lines in `kk/src/launcher.cpp` | See "The launcher's part". |

What the settings decide (`PackOptionsForSettings`):

- **Backend**: SPIR-V, unless `--native_backend=d3d12` (DXIL), as `NativeGraphicsSystem::Create` picks the API for
  `auto` (Vulkan whenever the plugin has it). Only that backend is compiled.
- **Render scale variants**: the render scale aware pixel shaders are added (`--scaled`, like `db-build
  --also-scaled`) when the renderer will scale: `native_render_scale` (with `auto` resolved by the same function
  the run uses, `ResolvedRenderScale`) is not 1, or `native_shadow_scale` is set and not 1. A pack with them also
  serves a 1:1 run.
- **DXC**: `--native_dxc`, else the program's folder (where the port puts `dxcompiler`).
- **Where**: `<cache root>/native-shaders/kkshaders-spirv.pack` (or `-dxil`), the cache root being `--cache_root`
  or `<user data>/cache` (on Windows `Documents\king_kong\cache\native-shaders`).

The 2 shaders of the D3D library's own (found in `kk/image.bin` by `db-build --image`) are not in a player's
pack: the player has no decrypted image. They are compiled on first use (a few ms each), as before.

### When it is rebuilt: the stamp

Beside the pack, `<pack>.stamp` (text):

```
kkshaders pack stamp 1
backend spirv
translator 881233E81EBCC596
dxc dxcompiler 1.9 commit 5512 01b62ad4
database 8C502BDC8662C807
scaled 0
shaders 4794
```

The pack is **current** when backend, translator hash (`translatorHash()`: translator version, ABI version,
prelude text), DXC identity (`dxcIdentity()`: the compiler's version and the commit it was built from, from
`IDxcVersionInfo2`), the database's XXH3-64 and the scaled flag (a scaled pack serves a plain request, not the
other way round) all match what a build would use now. `shaders` is for information. Anything else is
**stale** (the reason is logged: another translator, another DXC, the game's files changed, no scaled variants)
and a pack without a stamp is **missing**. So the pack is rebuilt exactly when a port update changes the
translator or ships another DXC, when the game files change, or when the settings need what it lacks; a port
update that changes neither keeps it.

Writing: the pack goes to `<pack>.partial-<pid>`, is opened once to check it, the old stamp is removed, the pack
is renamed over the old one, then the stamp is written the same way. A crash or a cancel at any point leaves
either the old pack with its stamp, a pack without a stamp (rebuilt next time) or nothing. A cancelled build
writes nothing. Shaders that fail to compile are left out (counted and logged; they are compiled on first use);
none fail on the real database.

### Which pack a run uses

At `InitializeShaderStorage` (the SDK calls it with the cache root before the game's main thread starts, so
before any shader is created) the plugin opens the first of:

1. `--native_shader_pack` (opened at `SetupGuestGpu` already);
2. the user's cache: `<cache root>/native-shaders/kkshaders-spirv.pack` (`-dxil` on D3D12), logged as
   `rexgpu-native: shader pack from the user's cache (4794 shaders, dxcompiler 1.9 commit 5512 01b62ad4): <path>`;
3. beside the game: `kkshaders-spirv.pack` / `kkshaders-dxil.pack` / `kkshaders.pack` (a developer's `db-build
   --split`), logged as `... from beside the game (a developer's pack)`;
4. none: `rexgpu-native: no shader pack (the launcher builds it into <cache>\native-shaders): shaders are compiled
   with DXC on first use`. That path is what it always was: each shader is translated and compiled by DXC when
   the game creates it; the game creates all of them at boot, on its loader thread, so a first boot without a
   pack is several minutes long (about the single-thread build time below).

A stale pack is still opened (its entries are keyed by microcode and input hash, so it can only miss, never
answer wrongly; a pack from another translator is refused by `ShaderPack::open`). The launcher rebuilds it
before Play, so this happens only when the game is started without the launcher.

### The launcher's part (for the launcher session to review)

Everything is in `kk/src/native_shaders.h` / `.cpp` (class `NativeShaderPack`, which finds
`nr_get_shader_pack_api` in the already loaded `rexgpu-native` and wraps start / poll / cancel / release). It is
only active when `gpu_plugin` is `native` and the plugin has the builder (`NativeShaderPack::Wanted()`); with the
Xenos renderer, or in a build without `KK_NATIVE_RENDERER`, it is a stub and the launcher behaves exactly as
before. `kk/CMakeLists.txt` adds `src/native_shaders.cpp`. The changes in `kk/src/launcher.cpp`:

1. `#include "native_shaders.h"`; members `native_pack_`, `setup_shaders_`, `waiting_for_native_`,
   `native_modal_`, `native_pack_checked_`, `native_wait_since_`.
2. Constructor: `if (files_ok_) native_pack_.Start(CacheDir(), paths_.game_dir);` after the art check: every time
   the launcher opens, the pack is checked (a fraction of a second when current) and built in the background
   when missing or stale (every hardware thread, below normal priority).
3. **The setup step**: `FinishArtIfDone`, in the `setup_art_` branch (the install's last step): when `Wanted()`,
   start the build and set `setup_shaders_`. `InSetup()` includes `setup_shaders_`; `SetupStage()` returns 4 for
   it (after 2, the artwork). `DrawSetup` stage 4: heading "Preparing the shaders", one sentence, the progress
   bar, "N of M shaders" and the percentage, and one entry "Continue in the background" (clears
   `setup_shaders_`; the build goes on). `KK_DEV_LAUNCHER_INPUT=setup:4` shows it with sample numbers.
4. **The status line**: `DrawHomeStatus`'s middle column is "SHADERS" with `native_pack_.StatusText()`
   ("Checking", "Preparing 45%", "Ready", "Not prepared", "Not prepared (see the log)") and its progress bar
   instead of the Xenos "SHADER PACK" column when `Wanted()`.
5. **Play**: `StartGame` checks the pack again for the settings the game starts with (they may have changed in
   the launcher, for example the render quality preset with `--native_render_scale=auto`), starting a check / build
   if none runs, and returns; `TickNativeShaders` (called from `Tick`) calls `StartGame` again when it is done.
   If it takes more than half a second (there is something to build) a pop-up "Preparing shaders" with the
   progress and "Back" opens. It also ends the setup step and sets the status message.

New English strings (no translations yet; `T()` shows English): "Shaders", "Checking", "Preparing {0}%",
"Ready", "Not prepared", "Not prepared (see the log)", "Preparing the shaders", the setup sentence, "{0} of {1}
shaders", "Continue in the background", "Preparing shaders" and its pop-up text, "Installed. The shaders were not
prepared: the game compiles them as it starts."

Decision, Play before the build is done: **wait** (with progress) rather than start and compile on first use.
The game creates every shader at boot, so starting early means compiling all of them on one loader thread, which
takes longer than the parallel build that is already running (and both would compete for the CPU). "Back"
returns to the launcher with the build still running.

### DXC with the port

The plugin needs DXC on the player's machine: to build the pack and for shaders missing from it. The port ships
**only the compiler library**: `dxcompiler.dll` (Windows x64) and `libdxcompiler.so` (Linux x86-64), unmodified,
from the official DXC release (v1.9.2609: `dxc_2026_09_29.zip`, `linux_dxc_2026_09_28.x86_x64.tar.gz` from
github.com/microsoft/DirectXShaderCompiler/releases).

Licences (read from both release archives and the repository at tag v1.9.2609):

- The release archives' own notes (`ReleaseNotes.md`, "Licenses") say `LICENSE-LLVM.txt` (the LLVM Release
  License, University of Illinois/NCSA: redistribution in binary form allowed with the copyright notice, the
  conditions and the disclaimer in the documentation) applies to all files except `d3d12shader.h` (MIT), which
  the port does not ship. `dxcompiler` is built from the open source repository, which also incorporates the
  components in its `ThirdPartyNotices.txt` (LLVM, Clang, OpenBSD regex, ...) and, through `external/`,
  SPIRV-Tools (Apache 2.0), SPIRV-Headers (Khronos MIT-style) and DirectX-Headers (MIT).
- Both archives also contain `LICENSE-MS.txt`, Microsoft's proprietary "DirectX Shader Compiler" terms (use
  "solely on Windows", distributable code only with "significant primary functionality", distributors must
  impose protective terms on end users and indemnify Microsoft). Microsoft's NuGet package's older notes mapped it
  to `dxil.dll` (the closed validator and signer). **`dxil.dll` / `libdxil.so` are not needed and not shipped**:
  from DXC 1.8.2502 the validator hash is open source and from 1.8.2505 "the compiler will now always use the
  internal validator instead of searching for an external DXIL.dll" (release notes). Checked here: with only
  `dxcompiler.dll` in a folder, every DXIL shader of the `pack-build` test is signed (`isDxilSigned`), and the
  DXIL pack of the real database builds with no failures. So nothing under `LICENSE-MS.txt` ships.
- `kk/THIRD-PARTY-NOTICES.txt` now carries DXC's `LICENSE.TXT` and `ThirdPartyNotices.txt`, the Apache 2.0 text
  for SPIRV-Tools, the SPIRV-Headers and DirectX-Headers licences, and the libraries inside `rexgpu-native`
  (XenosRecomp, NVRHI with its `ThirdPartyLicenses.txt`), which were missing too.

The build puts it there: `native-renderer/plugin/CMakeLists.txt` copies `NR_DXC_RUNTIME` (or the library found in
`NR_DXC_DIR` or `$KK_DXC_DIR`, a DXC release folder) beside the game after each plugin build, and warns when it
finds none. For example `set KK_DXC_DIR=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc` before `kk\build.bat
kk-dev-vulkan`. A new DXC changes `dxcIdentity()`, so every player's pack is rebuilt once after an update that
ships it.

### Measurements

This PC: AMD Ryzen 7 7800X3D (8 cores, 16 threads), 64 GB, Windows 11; DXC v1.9.2609 (`dxcompiler.dll` only); the
real database (4,794 distinct shaders); `kkshaders pack-build --force`, file in the OS cache. Logs:
`F:\KK-native-renderer\analysis\pack\measure.log`.

| Pack | Threads | Shaders | Time | Size |
|---|---|---|---|---|
| SPIR-V | 16 (all) | 4,794 | 48.1 s | 150 MB |
| SPIR-V | 8 | 4,794 | 60.1 s | 150 MB |
| SPIR-V | 4 | 4,794 | 100.0 s | 150 MB |
| SPIR-V | 1 | 4,794 | 279.4 s | 150 MB |
| SPIR-V with the render scale variants | 16 | 6,825 | 57.6 s | 202 MB |
| DXIL | 16 | 4,794 | 28.3 s | 72 MB |

The four SPIR-V packs are byte for byte the same (SHA-256 `C86447249AE96A93...`): the thread count does not
change the result (the `pack-build` test checks the same on the synthetic database). DXIL compiles in a little
over half the time of SPIR-V. A check of a current pack (load DXC, hash the 20 MB database, read the stamp) takes 44 ms
(`pack-build --check`, the whole process).

Steam Deck (estimate, not measured): 4 Zen 2 cores, 8 threads, up to 3.5 GHz in a 15 W budget. One Zen 2 core at
Deck clocks does roughly half the work of one 7800X3D core at 5 GHz, so the 4-core figure (100 s here, one
thread per core) becomes about 200 s, and the second thread per core gives back perhaps 20 %: **about 2.5 to 3.5
minutes** for the SPIR-V pack, once. The single-thread figure (279 s here, roughly 9 minutes on the Deck) is what
a first boot without any pack costs, since the game creates every shader at boot on one thread.

### First run, as a player sees it

End-to-end runs on this PC (12 October 2026, RTX 4070 Ti, Vulkan): the developer build (`kk-dev-vulkan`, with
`dxcompiler.dll` beside it from the build), a fresh user folder each time, `--gpu_plugin=native`, the game files
already installed, `KK_DEV_AUTOSKIP` pressing through the intros and menus. Logs and window captures (the
launcher's own `shot:` aid and PrintWindow of the game's window) in `F:\KK-native-renderer\analysis\pack\runs`.

Run A, a player who presses Play at once (`KK_AUTOPLAY`; `1012-002734-a-first`):

| Time | What |
|---|---|
| 0.0 s | `king_kong.exe` starts |
| 1.6 s | the launcher opens; `rexgpu-native: shader pack: checking ...\native-shaders\kkshaders-spirv.pack (spirv)`: missing, the build starts behind the Home screen ("SHADERS Preparing 0%") |
| about 4 s | Play: the pop-up "Preparing shaders" with its progress bar |
| 6.3 s | the launcher's artwork taken from the game files (in parallel) |
| 62.1 s | `shader pack built: 4794 shaders on 16 threads in 60.4 s, 143 MB`; the game starts at once |
| 62.4 s | `shader pack from the user's cache (...)`; the game renderer reports `6588 created, 4794 from the pack, 5 compiled, 0 failed` |
| 118.7 s | gameplay reached (the game's own boot, intros and menus: 56 s) |

Run C, the second start with the same folder (`1012-012829-c-second`): the check says current in 16 ms, Play
checks again (14 ms), the game starts 1.7 s after the program, gameplay at 57.9 s. Nothing is built again.

Run B (`1012-012829-b-screens`): Home with the status line, the setup step's screen (`setup:4`, sample numbers
only when no build runs; here the real ones, "786 of 4794 shaders, 16 %"), and Play's pop-up. The build took
31.8 s there; run A's 60.4 s shared the CPU with the artwork extraction (Media Foundation decoding the trailer)
and, like the `pack-build` measurements above, with other sessions using this PC: read the table as upper bounds.

Without a pack (`--kk_skip_launcher`, nothing built; run D): the game shows its first frame only after every
shader the game creates at boot has been compiled on the loader thread. Gameplay at **499 s**; the shader library
then compiled both DXIL and SPIR-V for each shader, so it now compiles only the device's (one change in
`backend/shader_library.cpp`): **317 s** (`1012-d2-nopack-onetarget`). With the pack: 58 s. That is the case for
waiting at Play: the parallel build (32 to 60 s here) plus the boot is far shorter than a boot that compiles.

The five shaders compiled on first use with the pack are not in the database (the D3D library's own, and the
ones it builds in code; see "Open problems" at the top), a few ms each.

### Release builds

- No pack, stamp or other game-derived file is written to a build folder by anything in this branch: the
  launcher and the plugin write only under the cache root; `kkshaders db-build` / `pack-build` write where `--out`
  says (keep that out of build folders; this work used `F:\KK-native-renderer\analysis\pack`). The shader
  library's CTest `database` test writes `native-renderer/shaders/out/build/release/db-out` (git-ignored, not a
  release folder).
- `tools/build_appimage.sh` copies `king_kong`, every `*.so*` beside it, `glyphs/` and the SDK's libraries:
  `libdxcompiler.so` will be picked up once the Linux build puts it beside the binary (the CMake copy above with
  `KK_DXC_DIR` pointing at the Linux DXC release); a `*.pack` would not be. `Build-KingKong.ps1` copies `*.exe`
  and `*.dll` from the build folder: `dxcompiler.dll` yes, a pack no.
- The Windows release zip's script is not in this repository (the release session's own). It must ship
  `dxcompiler.dll` (not `dxil.dll`, not `dxc.exe`) and the updated `THIRD-PARTY-NOTICES.txt`, and must not ship
  `*.pack`, `*.pack.stamp`, `*.kksh`, `kk/image.bin` or anything from `kk/assets`. Found while checking: the
  native renderer's test builds in `F:\KK-native-renderer\testbuilds\KingKong-native-test-1011-*` (folders and
  zips) contain `kkshaders-spirv.pack` and `kkshaders-dxil.pack` (game-derived) and `dxil.dll`; their READ ME says
  no game files are included. Builds made after this branch need neither.

### Decisions

- The builder is a library entry point inside the plugin (no separate program): the plugin already has the
  translator and loads DXC, and the launcher already loads the plugin. The C interface is versioned
  (`NR_SHADER_PACK_API_VERSION`).
- One backend per player, the one the settings use; the render scale variants only when the renderer scales.
- Version = backend + translator hash + DXC identity + database hash (+ the scaled flag), in a text stamp beside
  the pack, written last.
- The launcher checks at every start, builds in the background, makes it the install's last setup step, and Play
  waits for it. Worker threads: one per hardware thread, below normal priority.
- Ship `dxcompiler` only.
- A shader missing from the pack is compiled for the device's API only (it was compiled for both).

### Open problems

- The setup step was checked by its screen (`setup:4`) and by code reading; this PC has no disc image to run a
  real install (the step starts where the artwork step ends, which is the path a fresh install takes). The
  background build at launcher start, the wait at Play and the run with the pack were checked end to end.
- Not run on Linux or a Steam Deck: the builder and its tests were built and run on Linux (WSL, Ubuntu 24.04,
  clang 20, `libdxcompiler.so` alone from the Linux release: `pack-stamp` and `pack-build` pass, DXIL signed);
  the plugin and launcher parts only on Windows.
- On a 4-core machine the background build at launcher start takes every core (below normal priority) for a few
  minutes; the launcher stays responsive here, not measured on a Deck.
- `launcher_text_table.inc` has no translations of the new launcher strings.
- If the player changes `native_backend` or the render scale outside the launcher (command line with
  `--kk_skip_launcher`), the run uses the pack that is there and compiles what is missing on first use; the next
  launcher start rebuilds.
- The old pack stays in the cache when the backend changes (`kkshaders-dxil.pack` beside `kkshaders-spirv.pack`):
  about 70 to 150 MB until the player clears the cache.
- `loadDxc` takes a narrow path (`LoadLibraryExA`); with the port's UTF-8 code page manifest that is UTF-8, but a
  build without the manifest would fail for a non-ASCII install folder.

## Render scale (session E)

The backend can draw at a multiple of the console's size (`backend.md`, "Render scale"). Two things in a pixel
shader's translation depend on the size of a surface: the pixel position register (VPOS) and the texture size a
fetch reads with `GetDimensions` (unnormalised coordinates, fetch offsets in half texels, `GetTextureWeights`).
`ShaderInfo::renderScaleAware` (pixel shaders only) switches the translator, through
`RecompilerInput::renderScaleAware` into `XenosRecomp/shader_recompiler.cpp`, to code that works in guest units:

- the pixel position is `(iPos.xy * kk_RenderScale.zw - 0.5) * (±1, 1)` instead of `(iPos.xy - 0.5) * (±1, 1)`;
- after every fetch's `GetDimensions(ktd...)` a line `ktd.xy = kk_GuestSize(ktd.xy, <fetch constant>)`: the host
  size times `kk_TexInvScale[slot].xy`, rounded; everything after (`kc /= size`, offsets, the weights, the
  cube face, the 3D and stacked paths) is untouched and uses the guest size;
- the declarations, emitted only in aware code:
  `cbuffer KKScaleConstants : register(b3, space0) { float4 kk_RenderScale; float4 kk_TexInvScale[32]; }` and
  `kk_GuestSize`. The struct is `kkshaders::ScaleConstants` in `abi.h`; the binding exists only when the renderer
  runs scaled, so the root signature of the 1:1 renderer is unchanged.

Nothing else changed, and nothing at all when the flag is off: the prelude, `kTranslatorVersion` (4), `kAbiVersion`
(2) and so `translatorHash()` are as before, and a plain shader's input hash is unchanged (the aware flag adds
`"render-scale-aware/1"` to the hash only when set). Checked: the plain HLSL of all 1,557 translatable corpus
shaders (the test corpus at seed 20261010) and their input hashes are byte-identical to the commit before this
work (the build of that commit dumped them; `kkshaders_tests scale --dump-hlsl <dir>` writes the same files).

`kkshaders_tests scale [--dxc dir]` (a CTest test): for every corpus shader the plain translation mentions none
of the new identifiers; vertex shaders do not change with the flag; for the 822 pixel shaders the aware text
equals the plain text once the scale additions are removed (the declarations, the guest-size lines, the
position expression), every `GetDimensions` has its guest-size line, VPOS readers use the scaled position, the
hashes differ, every aware shader builds to signed DXIL and SPIR-V, and a plain and an aware variant sit in one
pack under their own hashes. The corpus has 2 VPOS readers and 1,314 size reads; the backend's GPU tests
(`Scale_PostPass...`) run both through the renderer.

The pack and the tool. `kkshaders db-build ... --also-scaled` adds an aware variant of every pixel shader to the
pack (the same file; about twice the pixel shaders: expect ~+60 MB of the 119 MB SPIR-V pack, not measured).
`ShaderLibrary` asks for the aware variant when the backend scales and then does not use the lookup that ignores
the input hash (`find(ucode, 0)`), which would answer with the plain code. `kkshaders scale-report
xeshaders.bin` prints, per pixel family, how many shaders read VPOS, fetch with unnormalised coordinates, with
offsets, ask for weights or sample cubes, and lists the constants whose names look like sizes (texel size, screen
size, `1/width`): the data for deciding per family. It could not be run on the real database in the cloud (no game
files); on the fixture database it runs and counts. The decisions it feeds are in `backend.md`.

## Decisions

- Render scale: the translator's aware mode is for pixel shaders only, makes VPOS guest pixels and fetch sizes
  guest texels, and leaves texel-size constants and normalised coordinates to the game (`backend.md`).

- Kind of a container: the magic (`0x102A0E01` vertex), confirmed by the database's CTAB targets and the
  library's own shaders. Runtime hooks should still pass what the creating function implies.
- Vertex containers: A is the one used; constant names repaired from B.
- Pack: one per backend for shipping; the both-backends pack stays the default output of `db-build`.
- No compression in the pack (none is available in the SDK bundle, and loads are already far below 1 ms);
  revisit if download size matters (DXIL and SPIR-V compress well).
- DXC v1.9.2609 (the current release) for everything; the version is in each report. Compiled with `-Gis`
  (phase 2, IEEE behaviour of NaN and infinity over fast-math).
- Where the SDK's two descriptions of Xenos disagree (its CPU interpreter and its SPIR-V translator), the
  differential tests follow ucode.h's comments, then the SPIR-V translator: loop repeat (ucode.h), vertex
  halves as IEEE and the packed formats' component offsets (SPIR-V translator). aL is clamped as the CPU
  interpreter does (the SPIR-V translator does not; no shader of the game gets near the limit).
- The SDK's semantics are the reference, not the console: where the SDK is wrong about the hardware, so are
  the tests.

## Open problems

- The differential tests (above) check what the translated code computes against the SDK's semantics as the
  reference interpreter reads them, on lavapipe. They do not check the fixed-function steps the backend owns
  (clipping, the viewport transform, the half-pixel offset, blending), derivatives (`getGradients`, LOD from
  the screen), mip selection beyond the explicit LOD, or anything the SDK itself gets wrong; those still rest
  on the golden scenes (streams 04 and 05). Only SPIR-V is executed (lavapipe); DXIL compiles from the
  same HLSL, and its two changes (`-Gis`, `kk_Ne`) were checked by reading DXC's output, not by running it.
  The alpha test (`kk_AlphaTest`) is not exercised (the harness leaves it off).
- Shaders the D3D library builds in code (the two 24-byte pixel shaders, and probably some of the 8 unmatched
  vertex records) are not in the pack; `ShaderProvider` compiles them on first use (a few ms each), or the
  backend replaces the paths that use them (Clear, Resolve).
- Stream 01's documents name `sub_82111CA0` / `sub_82111D90` and the container magic the other way round (see
  above); `SetVertexShader` / `SetPixelShader` (`sub_821108B8` / `sub_82110C28`) may be swapped as well, since
  each one's literal block marks the other stage's pending constants. To be settled in stream 01.
- The sign of zero is not guaranteed on the host: Xenos `rcp(-0)` is -infinity, but Vulkan (without the
  `SignedZeroInfNanPreserve` float controls, which DXC does not emit) and D3D may return either sign.
  Harmless unless a shader divides by a zero it computed; the differential tests exclude these cases.
- `psheatshimmer` declares a DEPTH output that no compiled shader writes; if the game expects depth from it, it
  comes from elsewhere.
