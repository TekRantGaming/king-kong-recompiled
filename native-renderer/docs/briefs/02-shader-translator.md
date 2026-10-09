# Brief 02: the shader translator

## Goal

Every shader the game uses, converted from Xbox 360 microcode to HLSL and compiled to DXIL and SPIR-V, with a
disk cache keyed by the microcode hash. Output: `native-renderer/shaders/` (the translator as a library used
at CreateVertexShader / CreatePixelShader time, plus a command-line tool), and a report of coverage.

## Inputs

- XenosRecomp (hedge-dev, MIT): the base. Permission to clone it is granted in `docs/design.md` once the user
  has agreed; vendor it under `native-renderer/thirdparty/XenosRecomp` with its licence file.
- The disc's `Shaders/xeshaders.bin` (`kk/assets/Shaders/xeshaders.bin`): 6,606 shader containers (`SDB2`
  database: header, HLSL sources, a table of 36-byte entries {kind, vertex key, pixel key, extra, offset,
  size}; `tools/shader_coverage.py` parses it) and the 37 HLSL sources they were compiled from: the answer key.
- The container layout differences: the 2005 containers are not XenosRecomp's `ShaderContainer` word for
  word (pixel: {0x102A0E00, virtualSize, physicalSize, 0x18, constantTableOffset, definitionTableOffset,...};
  vertex entries have an 8-byte prefix and 0x102A0E01). Their constant table is a standard D3DX `ps_3_0` /
  `vs_3_0` CTAB. `sub_82111CA0` / `sub_82111D90` (CreateVertexShader / CreatePixelShader) show how the game
  reads them: word 1 = virtual size, word 2 = physical (microcode) size, microcode follows the virtual part.
- DXC (the DirectX Shader Compiler): the SDK already uses it; check `C:\rexsrc` for how it is found.
- Today's renderer's shader dumps (`--dump_shaders=<dir>`) and the port's notes on the game's shader families
  (`docs/` in the main repo, the AO work) for cross-checks.

## Method

1. Write the container adapter: parse the 2005 container into what XenosRecomp expects (microcode, constant
   table, vertex elements, interpolators, outputs). Verify on all 6,606 entries: no parse errors, sizes add up.
2. Translate all pixel shaders and all vertex shaders; compile with DXC to DXIL and SPIR-V; count failures by
   cause. Fix the recompiler for this game's instruction mix (dynamic indexing, vertex fetch bindings and
   mini-fetches, texture fetch modes, cube maps, integer / boolean constants; the README lists what is missing).
3. Diff translated HLSL against the shipped HLSL sources for the 37 families: the structure must match
   (constants, texture usage, outputs).
4. Vertex declarations: define how the game's vertex declaration objects become input layouts (the library
   builds the GPU vertex fetch constants at draw time from the stream sources at device +12556 and the
   declaration object at +11408; the translated vertex shader's `vfetch` instructions say which fetch slot and
   element each input reads; coordinate with brief 01).
5. Constant model: vertex constants 256 float4 (device shadow +1920), pixel constants 224 float4 (+6016),
   integer / loop constants (+10144); define the constant buffer layout the backend uploads.
6. Cache: hash -> compiled blobs on disk under the user data root, plus a prebuilt pack of everything in
   `xeshaders.bin` so first runs never compile.

## Done when

All shaders in `xeshaders.bin` and all cached pixel shaders from the shader pack translate and compile for both
backends, the 37 families match their HLSL sources structurally, and a test program can load any shader by
hash in under a millisecond.
