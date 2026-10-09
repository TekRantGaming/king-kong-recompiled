# Brief 03: textures and resources

## Goal

Host textures and buffers for the game's guest resources: every guest texture format, tiling mode and
endianness the game uses converted correctly on upload; vertex and index buffers endian-swapped as the
backend needs; render-target surfaces mapped to pooled host targets. Output: `native-renderer/resources/`
(converters with unit tests) and `docs/formats.md`.

## Inputs

- The guest object layouts from brief 01 (the 6-word GPU texture fetch constant at texture+16..+36: type,
  sign, clamp, pitch, tiling in word 0; format, endianness, base address in word 1; size in word 2; swizzle
  and filters in word 3; mip levels and LOD bias in word 4; border colour and mip address in word 5; the SDK's
  `xenos.h` has the bitfields; surface objects from CreateRenderTarget with packed format words such as
  0x2DA2ABA4, 0x28280186, 0x18280186 and EDRAM parameters).
- The SDK's Xenos texture code (read-only reference): `C:\rexsrc\src\graphics\format\` and the texture
  caches in `src/graphics/d3d12` and `src/graphics/vulkan` (untiling, format tables, endian swaps). The
  texture formats the game uses can be listed from a trace of SetTexture calls plus a dump of the bound
  textures' fetch constants (extend `kk/src/dev_d3d_trace.cpp`).
- `XenonTexture.DLL` in the PC Gamer's Edition (`F:\KK-native-renderer\The PC Game Files\...`): the PC game
  converts 360-format textures on PC; its behaviour is a second reference for the formats (read-only).
- The port's existing art extraction (`kk/src/launcher_art.cpp`: tiled 8:8:8:8 and DXT5 textures read from
  `KKTextures.bf`) as worked examples of the game's texture layout.

## Method

1. From traces, collect the set of (format, tiling, dimensions, mips, endian) combinations the game binds, per
   scene type (menus, videos, Jack, Kong chapters).
2. Implement the untiling and conversion for each combination to a host format NVRHI supports; where a format
   has no host equivalent (for example 10:10:10:2 variants or depth formats used as textures), pick a
   conversion and document it.
3. Vertex and index buffers: the 360 stores big-endian data; the vertex declaration says the element formats.
   Define the swap strategy (per element, at upload) with brief 02's input layout work.
4. Upload policy: create on first bind, re-upload on Unlock / after a write the engine makes through the
   streaming reads (brief 01 says where Lock / Unlock are); detect writes to resident guest memory cheaply.
5. Render targets: the engine creates 12 transient EDRAM surfaces per gameplay frame; map (width, height,
   format, msaa) to a pooled host render target; Resolve becomes a copy or a clear.
6. Unit tests with synthetic data for each converter, plus a dump-and-compare against today's renderer's
   texture cache for a set of real textures.

## Done when

Every format combination seen in the traces converts with a test, uploads happen only when data changed, and
a side-by-side of ten real textures matches today's renderer.
