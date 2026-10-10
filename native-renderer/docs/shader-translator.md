# Shader translator: status and what the Windows side must run

Brief 02, cloud session of 10 October 2026, branch `cloud-02-shaders` (started from `nr-02-shaders`).
The code is `native-renderer/shaders/` (see its README); the translation core is the vendored XenosRecomp,
reworked (`thirdparty/NOTES.md` lists what changed).

## What exists

- The 2005 container adapter (`src/container.cpp`, reviewed and extended from the draft on
  `nr-02-shaders`): header, register block (float, bool and loop literals), D3DX constant table, binding
  table (vertex fetch bindings, interpolators, pixel position), the `SDB2` database reader, and a bare
  microcode path for the emulator's shader storage records (`.xsh`).
- The translator (XenosRecomp core) to HLSL with one binding model for all shaders (bindless textures,
  samplers and vertex buffers; the device's constant register files as constant buffers), DXC to signed
  DXIL (vs/ps 6.0) and SPIR-V (Vulkan 1.2), a disk cache and a memory-mapped pack keyed by the microcode
  hash, and the `kkshaders` tool.
- Tests that need no game data, including a Xenos microcode assembler, a generated corpus and execution
  tests against a CPU reference interpreter.

## Verified here (Linux, DXC v1.8.2505.1, spirv-val 2025.1, Mesa lavapipe)

| Check | Result |
|---|---|
| Assembler encodings against XenosRecomp's structs (20,000 random ALU / vfetch / tfetch, all control flow) | pass |
| Container round trip, database reader, 2,000 mutated containers through parser and translator | pass, no crashes |
| Corpus: 757 directed shaders (every vector and scalar operation in both stages, constants, predication, every control flow form, relative registers, vertex fetch in binding and instruction mode with every format, every texture dimension and fetch mode, outputs) + 400 random programs, each as a container and as bare microcode: translate, DXIL signed and validated, SPIR-V validated by DXC and spirv-val | 1,557 / 1,557 |
| Execution on lavapipe vs the reference: ALU operations with random operands, swizzles and modifiers; random programs with loops, jumps, conditional execs, calls, backward jumps, predication, relative constants and temporaries; vertex fetch of every format and number format; 1D / 2D texture addressing | all pass (see below for the bugs these found) |
| Cache: store / load / damaged file; pack: every entry found | pass, lookup + decode worst 0.13 ms |
| Tool on a synthetic `xeshaders.bin` (411 entries) and `.xsh` | db-check, db-build, db-structure, xsh, pack-lookup pass |

The execution tests found and fixed: a harness bug, the predicate / address register when both
operations of an instruction set it, saturation of NaN, a missing vertex element reading through the
element swizzle, and the general control flow form's handling of loops nested more than 4 deep, loop ends
outside a loop and calls deeper than 4. XenosRecomp's own bugs fixed on the way are in
`thirdparty/NOTES.md` (most visibly: conditional execs ran unconditionally).

What the tests cannot show: that the real containers parse as the draft understood them, and that the
game's shaders render as on the 360. Those need the database and the game.

## What the Windows side must run

Prerequisites (no downloads beyond what is listed; each needs the user's yes per the briefs' README):

1. The SDK clone at `C:\rexsrc` with `thirdparty\fmt` and `thirdparty\xxHash` checked out (they are
   submodules; `git -C C:\rexsrc submodule update --init thirdparty/fmt thirdparty/xxHash` if missing).
2. DXC with its validator: either the Windows SDK's (`C:\Program Files (x86)\Windows Kits\10\bin\<version>\x64`
   has `dxcompiler.dll` and `dxil.dll`; no download) or the DXC v1.8.2505.1 release
   (`dxc_2025_07_14.zip` from github.com/microsoft/DirectXShaderCompiler, the version tested here). Set
   `KK_DXC_DIR` to the folder (the tool also takes `--dxc <folder>`).
3. Optional: the Vulkan SDK, for `spirv-val` and the execution tests on the real GPU.

Then, from `F:\KK-native-renderer\kk-recomp\native-renderer\shaders`:

```
build.bat test
```

This builds and runs every test; the `database` test runs `db-build` on `kk\assets\Shaders\xeshaders.bin`
when it is there. Then the individual steps, each writing under `work\` (git-ignored; derived from the
game's files, never commit it):

```
set K=out\build\release\kkshaders.exe
set DB=..\..\kk\assets\Shaders\xeshaders.bin
%K% db-check %DB%
%K% db-build %DB% --out work\db --spirv-val "%VULKAN_SDK%\Bin\spirv-val.exe"
%K% db-build %DB% --out work\db-both --both
%K% db-structure %DB%
%K% xsh <cache>\shaders\shareable\*.xsh --database %DB% --out work\xsh
%K% pack-lookup work\db\kkshaders.pack
```

(`<cache>` is a game cache folder or the shader pack from the `shader-packs` release; drop
`--spirv-val` if the Vulkan SDK is not installed. `--both` also builds the unused second vertex
containers.)

What to bring back: the console output of `db-check` and `db-structure`, `work\db\report.txt`,
`work\xsh\report.txt`, the pack-lookup line, and for any failures the first few
`work\db\failed\*.txt` (the error and the shader's label; the `.hlsl` next to them is generated from the
game's microcode, so read it locally rather than committing it).

## Assumptions the real database will confirm or break

| Assumption | Where | If wrong |
|---|---|---|
| Register block offsets 8960-8991 are the bool constants and 8992-9119 the loop constants (the float ones at 768-8959 are the draft's finding) | `src/container.cpp` | db-check reports "register write outside the shader constants (offset N)": add the range |
| Every vfetch, mini fetches included, has a binding table entry | translator | db-build fails with "vfetch at N has no vertex element binding": mini fetches then take the element from the preceding full fetch's binding plus their own offset |
| Interpolators are TEXCOORD0-15 and COLOR0-1 | translator | "unsupported interpolator semantic": extend the shared signature |
| Bool constants are addressed absolutely (vertex 0-127, pixel 128-255) by `cexec` / `jmp` / `call`, and the backend fills `DrawConstants::boolConstants` the same way | prelude, backend | wrong branches in shaders with static branching; check the device's bool shadow and SetPixelShaderConstantB |
| Pixel literal constants address c0-c255 (the device's pixel file is 256 registers from +6016) | container adapter | db-check reports "literal constant register outside this stage's range" |

## Decisions that change the design document

- **Vertex input is pulled in the shader**, not declared as input layouts: the game's vertex shaders carry
  unpatched `vfetch` templates, and the XDK patches them per declaration. Here each `vfetch` reads its
  declaration element from a per-draw table (buffer, offset, stride, format word); one compiled shader
  serves every declaration and every Xenos vertex format is decoded from big-endian guest memory. The
  backend fills the table from the declaration (+11408) and the stream sources (+12556). Brief 04 should
  follow `include/kkshaders/abi.h` rather than "vertex declarations become input layouts".
- **No specialisation constants or DXIL linking** (XenosRecomp's approach): alpha test, clip planes and the
  half-pixel offset are uniform branches on `DrawConstants`.
- **Bindless everything**, with one root signature / pipeline layout (abi.h); samplers come as a list per
  shader (fetch constant + the instruction's filter overrides) for the backend to build.

## Not done

- Running on the real database and the shader pack (the commands above).
- Comparing translated HLSL with the 37 shipped sources beyond names (`db-structure` checks that every
  constant and sampler a family's shaders declare appears in its source and lists the outputs); a closer
  diff needs the sources.
- Memory export, `getBCF`, the vertex kill flag and point size, `fetchValidOnly`: not modelled (the game is
  not expected to use them; db-build reports them as warnings).
- The backend side (brief 04): building samplers, the vertex fetch table and applying register writes.
