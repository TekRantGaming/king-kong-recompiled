# Shader translator (stream 02)

Status: 10 October 2026. Every shader of the game's database and every shader in today's shader caches is
translated from Xbox 360 microcode to HLSL, compiled to DXIL and SPIR-V, and validated. The translated HLSL
matches the 37 HLSL sources shipped in the database structurally. A prebuilt pack holds all of them; a lookup
by hash takes about 0.01 ms.

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
  c250-c254 by the loop counter). Loop literals are inlined. Bool literals go through `b2`; no shader of the
  database reads a bool constant.
- Interpolators: one signature for all shaders (TEXCOORD0-15, COLOR0-1), so any vertex shader links with any
  pixel shader; the container's semantics place each export register.

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

## Decisions

- Kind of a container: the magic (`0x102A0E01` vertex), confirmed by the database's CTAB targets and the
  library's own shaders. Runtime hooks should still pass what the creating function implies.
- Vertex containers: A is the one used; constant names repaired from B.
- Pack: one per backend for shipping; the both-backends pack stays the default output of `db-build`.
- No compression in the pack (none is available in the SDK bundle, and loads are already far below 1 ms);
  revisit if download size matters (DXIL and SPIR-V compress well).
- DXC v1.9.2609 (the current release) for everything; the version is in each report.

## Open problems

- Nothing here executes a translated shader. Correctness beyond compilation and the structural diff (ALU
  semantics, vertex decoding, cube coordinates, the half-pixel offset) is checked only when the backend draws the
  golden scenes (streams 04 and 05).
- Shaders the D3D library builds in code (the two 24-byte pixel shaders, and probably some of the 8 unmatched
  vertex records) are not in the pack; `ShaderProvider` compiles them on first use (a few ms each), or the
  backend replaces the paths that use them (Clear, Resolve).
- Bool literals are not inlined; harmless today (no shader reads a bool constant).
- Stream 01's documents name `sub_82111CA0` / `sub_82111D90` and the container magic the other way round (see
  above); `SetVertexShader` / `SetPixelShader` (`sub_821108B8` / `sub_82110C28`) may be swapped as well, since
  each one's literal block marks the other stage's pending constants. To be settled in stream 01.
- `psheatshimmer` declares a DEPTH output that no compiled shader writes; if the game expects depth from it, it
  comes from elsewhere.
