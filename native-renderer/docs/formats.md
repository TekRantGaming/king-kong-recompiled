# Guest formats and host formats (brief 03)

How the game's Xbox 360 resources become host resources: every guest texture format with its host format and
conversion, how tiling, mips and endianness are undone, how vertex and index buffers are swapped, and how
EDRAM render targets map to pooled host targets. The code is `native-renderer/resources` (library
`kknr_resources`, namespace `kknr`); the references are the SDK's `include/rex/graphics/xenos.h`,
`src/graphics/pipeline/texture/` and the D3D12 / Vulkan texture caches (rexglue-sdk v0.10.0, Xenia-derived,
BSD), read but not copied.

Status: everything below is implemented and unit-tested with synthetic data on Linux (`ctest`: 58 converter,
tiling, buffer, cache and render-target tests plus 3 NVRHI format-table tests). Nothing here has been run
against the game's own textures yet; "Validating on Windows" at the end lists what to run there. Choices
marked "unverified" have no hardware or SDK reference and are the first to check when a real texture looks
wrong.

## Conventions

- **Component order.** Host channel R holds the guest's component X, G holds Y, B holds Z, A holds W, for
  every format. X is the lowest field of a packed texel after the endian swap (for an A8R8G8B8 word
  0xAARRGGBB, X is blue). The fetch constant's swizzle is not baked into the data: `HostTexturePlan::view_swizzle`
  is the SRV component mapping (3 bits per channel, values R, G, B, A, 0, 1 in the order of
  `nvrhi::ComponentMapping`, `kknr/nvrhi_format.h` converts it). So one host texture serves every swizzle the
  engine binds it with (the A8R8G8B8 and X8R8G8B8 views of one render target, for instance).
- **Missing components.** A format with fewer than four components returns, for the absent ones, what the
  360 returns: single-component formats replicate X (`XXXX`), two-component formats give `XYYY`, three give
  `XYZZ` (the SDK's RRRR / RGGG / RGBB). The "Channels" column of the table is that mapping; the view swizzle
  composes it with the fetch swizzle.
- **Signs.** When every component the swizzle reads is signed and the format has a signed host format, the
  texture is created in that (SNORM) format. Otherwise the data stays unsigned and the plan's
  `shader_flags` tell the translated shader what the 360 did before filtering: `kShaderBias` (unsigned
  biased: x * 2 - 1), `kShaderGamma` (PWL degamma), `kShaderMixedSigns` (signed components in unsigned data),
  `kShaderExpAdjust` (multiply by 2^exp_adjust), `kShaderInteger` (integer number format), `kShaderNorm32`
  (32-bit fixed formats read as integers, normalised in the shader). Brief 02's translator reads these.
- **Host formats.** Names are `nvrhi::Format`'s. `tests/test_nvrhi_format.cpp` checks every host format's
  name, bytes per block and block size against NVRHI's own table, so `ConvertTexture`'s output is exactly what
  `writeTexture` expects.

## Endianness

The fetch constant's 2-bit endian field says how the GPU swaps memory as it reads it:

| Mode | Swap | Used for (XDK defaults) |
|---|---|---|
| `kNone` (0) | none | 8-bit formats in some cases |
| `k8in16` (1) | bytes within each 16-bit unit | 16-bit texels, DXT / DXN / CTX1 blocks, 16-bit indices, SHORT vertex elements |
| `k8in32` (2) | bytes within each 32-bit unit | 32-bit texels (A8R8G8B8, R32F ...), 32-bit indices, float and colour vertex elements |
| `k16in32` (3) | 16-bit halves within each 32-bit unit | rare |

The swap is applied to the raw memory before untiling (`LoadSwappedRegion`), which is what the GPU does: the
units are memory units, not texel units. Consequences, all covered by `endian_*` tests:

- **8-bit texels** (k_8, k_8_8 ...) are swapped together with their neighbours: with `k8in32` the texel at
  x = 0 is the last byte of each memory word.
- **16-bit texels** with `k8in16` read as big-endian 16-bit values. A 16:16 texel stored as one big-endian
  32-bit word reads right with `k8in32` (X is the low half of the word).
- **32-bit texels** with `k8in32` read as big-endian 32-bit values.
- **64- and 128-bit texels** are two or four 32-bit units, each swapped on its own: X and Y in the first unit,
  Z and W in the second.

Regions are read in whole 32-bit units (rounded up), so the last texels of a tiny linear 8- or 16-bit texture
still get their swap partners (a bug the round-trip tests found in the first draft). Both regions start 4 KB
aligned, so the units line up with memory as the GPU reads it.

## Storage layout

`kknr/tiling.h` computes where every level and layer lives; it follows the SDK's `GetGuestTextureLayout`
exactly (the same rules, re-derived and cross-checked against an independent formulation of the address
function):

- **Tiled 2D** surfaces are 32x32-block tiles in row-major order with the bits of x and y mixed inside a tile
  (`TiledOffset2D`, the XGAddress2DTiledOffset function). **Tiled 3D** uses 32x32x4 tiles
  (`TiledOffset3D`, from XGRAPHICS::TileVolume). Both are checked to be permutations of whole tiles for 1 to
  16-byte blocks.
- **Level 0** is at the base address with the fetch constant's row pitch (texels / 32). **Levels 1+** are at
  the mip address, one after another, each padded to max(next_pow2(size) >> level, 1) and to whole tiles, every
  array layer / level 4 KB aligned. **Linear** mip rows are 256-byte aligned; the base keeps the guest pitch
  (it may be wider than the texture: 1408 texels for a 1280-wide DXT5 in the SDK's example; tested).
- **Packed mips**: levels whose shorter side is 16 texels or less share one 32x32 tail stored like the level
  where packing starts, each at a fixed offset (16, 8, 4 texels along the shorter axis, then smaller offsets
  along the longer one; 1x1 volume levels along Z).
- **Cube maps** are 6 layers; **stacked 2D** textures are arrays; **3D** textures have depth slices inside one
  volume; **1D** textures are linear rows.
- A fetch constant whose base address is 0 stores mips only (the minimum level becomes 1); a minimum level
  above 0 drops the base; a mip address with max level 0 means no mips (as the SDK's
  `GetSubresourcesFromFetchConstant`).

Untiling and tiling share one address walk (`ReadGuestBlocks` / `WriteGuestBlocks` in
`kknr/guest_texture.h`). `EncodeGuestTexture` is the full inverse of reading (tiling plus the endian swap); the
round-trip tests push random blocks for 11 formats (1 to 16-byte blocks, 4x4, 2x1, 4x1 and 8x1 blocks) through
13 shapes (2D power-of-two and odd sizes, 1x1, stacked, two cubes, three volumes, 1D) tiled and linear, with
and without packed tails, with all four endian modes (over 400 cases), and check that no two blocks share
bytes and that every block lies inside the extent the cache watches.

Known limits of the layout (kept as the SDK has them, tested as such):

- The packed tail of a volume thinner than a 4x4 block (4x4x16 DXT, for instance) puts its 1x1x2 and 1x1x1
  levels in the same block. No hardware reference here; the game is not known to use such volumes.
- 1 bpp formats with packed mips are not supported (8-texel blocks are wider than the tail's small offsets).

## Guest format -> host format

Generated by `kknr_formats` from the library's rules (`GetFormatChoice`); regenerate it after changing a rule.
Columns: the guest block and its bytes; the host format and conversion for unsigned (and biased / gamma / mixed)
data; the signed host format used when every read component is signed ("shader fix-up": no signed host format,
the data stays unsigned and `kShaderMixedSigns` is set); the texel format used instead of a BC format when level
0 is not a multiple of 4 (D3D12 needs that for BC); the guest component each of the four host reads returns.

| # | Guest format | Block | Bytes | Host format | Conversion | Signed host | Signed conversion | Unaligned BC fallback | Channels | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `k_1_REVERSE` | 8x1 | 1 | R8_UNORM | 1_reverse_to_r8 | shader fix-up | - | - | XXXX | 1 bpp treated as 8x1-texel byte blocks; bit order assumed (k_1 bit 0 first, REVERSE bit 7 first); no reference, SDK has no support; packed tails not supported |
| 1 | `k_1` | 8x1 | 1 | R8_UNORM | 1_to_r8 | shader fix-up | - | - | XXXX | 1 bpp treated as 8x1-texel byte blocks; bit order assumed (k_1 bit 0 first, REVERSE bit 7 first); no reference, SDK has no support; packed tails not supported |
| 2 | `k_8` | 1x1 | 1 | R8_UNORM | copy | R8_SNORM | copy | - | XXXX |  |
| 3 | `k_1_5_5_5` | 1x1 | 2 | RGBA8_UNORM | 1555_to_rgba8 | shader fix-up | - | - | XYZW | X..W from bit 0 up; widened to RGBA8 (the 16-bit packed host formats differ in layout between D3D12 and NVRHI's Vulkan mapping) |
| 4 | `k_5_6_5` | 1x1 | 2 | RGBA8_UNORM | 565_to_rgba8 | shader fix-up | - | - | XYZZ | X..W from bit 0 up; widened to RGBA8 (the 16-bit packed host formats differ in layout between D3D12 and NVRHI's Vulkan mapping) |
| 5 | `k_6_5_5` | 1x1 | 2 | RGBA8_UNORM | 655_to_rgba8 | shader fix-up | - | - | XYZZ | X 5 bits, Y 5, Z 6; widened to 8 bits (the SDK packs it into B5G6R5 with an RBGA view instead) |
| 6 | `k_8_8_8_8` | 1x1 | 4 | RGBA8_UNORM | copy | RGBA8_SNORM | copy | - | XYZW |  |
| 7 | `k_2_10_10_10` | 1x1 | 4 | R10G10B10A2_UNORM | copy | RGBA16_SNORM | 2_10_10_10_to_rgba16s | - | XYZW | signed: no host SNORM 10:10:10:2, widened to RGBA16_SNORM |
| 8 | `k_8_A` | 1x1 | 1 | R8_UNORM | copy | R8_SNORM | copy | - | XXXX |  |
| 9 | `k_8_B` | 1x1 | 1 | R8_UNORM | copy | R8_SNORM | copy | - | XXXX | the SDK has no host format; same bits as k_8 |
| 10 | `k_8_8` | 1x1 | 2 | RG8_UNORM | copy | RG8_SNORM | copy | - | XYYY |  |
| 11 | `k_Cr_Y1_Cb_Y0_REP` | 2x1 | 4 | RGBA8_UNORM | cr_y1_cb_y0_to_rgba8 | shader fix-up | - | - | XYZZ | 4:2:2 pairs expanded per texel (R = Cr, G = Y, B = Cb); no colour conversion, the shader does it as on the 360 |
| 12 | `k_Y1_Cr_Y0_Cb_REP` | 2x1 | 4 | RGBA8_UNORM | y1_cr_y0_cb_to_rgba8 | shader fix-up | - | - | XYZZ | 4:2:2 pairs expanded per texel (R = Cr, G = Y, B = Cb); no colour conversion, the shader does it as on the 360 |
| 13 | `k_16_16_EDRAM` | 1x1 | 4 | RG16_FLOAT | fixed16_to_half | RG16_FLOAT | fixed16_to_half | - | XYYY | EDRAM fixed point -32..32 (snorm16 x 32, the SDK's convention) as half floats; the SDK does not sample these |
| 14 | `k_8_8_8_8_A` | 1x1 | 4 | RGBA8_UNORM | copy | RGBA8_SNORM | copy | - | XYZW | SDK: unknown; read as k_8_8_8_8 |
| 15 | `k_4_4_4_4` | 1x1 | 2 | RGBA8_UNORM | 4444_to_rgba8 | shader fix-up | - | - | XYZW | X..W from bit 0 up; widened to RGBA8 (the 16-bit packed host formats differ in layout between D3D12 and NVRHI's Vulkan mapping) |
| 16 | `k_10_11_11` | 1x1 | 4 | RGBA16_UNORM | 11_11_10_to_rgba16 | RGBA16_SNORM | 11_11_10_to_rgba16s | - | XYZZ | no host unorm / snorm 11:11:10; widened to 16 bits by bit replication |
| 17 | `k_11_11_10` | 1x1 | 4 | RGBA16_UNORM | 10_11_11_to_rgba16 | RGBA16_SNORM | 10_11_11_to_rgba16s | - | XYZZ | no host unorm / snorm 11:11:10; widened to 16 bits by bit replication |
| 18 | `k_DXT1` | 4x4 | 8 | BC1_UNORM | copy | shader fix-up | - | RGBA8_UNORM (dxt1_to_rgba8) | XYZW | BC blocks after the 8in16 swap are byte-identical to the PC's; decoded to RGBA8 when level 0 is not a multiple of 4 |
| 19 | `k_DXT2_3` | 4x4 | 16 | BC2_UNORM | copy | shader fix-up | - | RGBA8_UNORM (dxt3_to_rgba8) | XYZW | BC blocks after the 8in16 swap are byte-identical to the PC's; decoded to RGBA8 when level 0 is not a multiple of 4 |
| 20 | `k_DXT4_5` | 4x4 | 16 | BC3_UNORM | copy | shader fix-up | - | RGBA8_UNORM (dxt5_to_rgba8) | XYZW | BC blocks after the 8in16 swap are byte-identical to the PC's; decoded to RGBA8 when level 0 is not a multiple of 4 |
| 21 | `k_16_16_16_16_EDRAM` | 1x1 | 8 | RGBA16_FLOAT | fixed16_to_half | RGBA16_FLOAT | fixed16_to_half | - | XYZW | EDRAM fixed point -32..32 (snorm16 x 32, the SDK's convention) as half floats; the SDK does not sample these |
| 22 | `k_24_8` | 1x1 | 4 | R32_FLOAT | depth24_to_float | R32_FLOAT | depth24_to_float | - | XXXX | depth as a texture: top 24 bits to float (stencil dropped; the SDK does the same) |
| 23 | `k_24_8_FLOAT` | 1x1 | 4 | R32_FLOAT | depth20e4_to_float | R32_FLOAT | depth20e4_to_float | - | XXXX | depth as a texture: top 24 bits to float (stencil dropped; the SDK does the same) |
| 24 | `k_16` | 1x1 | 2 | R16_UNORM | copy | R16_SNORM | copy | - | XXXX |  |
| 25 | `k_16_16` | 1x1 | 4 | RG16_UNORM | copy | RG16_SNORM | copy | - | XYYY |  |
| 26 | `k_16_16_16_16` | 1x1 | 8 | RGBA16_UNORM | copy | RGBA16_SNORM | copy | - | XYZW |  |
| 27 | `k_16_EXPAND` | 1x1 | 2 | R16_FLOAT | copy | R16_FLOAT | copy | - | XXXX |  |
| 28 | `k_16_16_EXPAND` | 1x1 | 4 | RG16_FLOAT | copy | RG16_FLOAT | copy | - | XYYY |  |
| 29 | `k_16_16_16_16_EXPAND` | 1x1 | 8 | RGBA16_FLOAT | copy | RGBA16_FLOAT | copy | - | XYZW |  |
| 30 | `k_16_FLOAT` | 1x1 | 2 | R16_FLOAT | copy | R16_FLOAT | copy | - | XXXX |  |
| 31 | `k_16_16_FLOAT` | 1x1 | 4 | RG16_FLOAT | copy | RG16_FLOAT | copy | - | XYYY |  |
| 32 | `k_16_16_16_16_FLOAT` | 1x1 | 8 | RGBA16_FLOAT | copy | RGBA16_FLOAT | copy | - | XYZW |  |
| 33 | `k_32` | 1x1 | 4 | R32_UINT | copy | R32_SINT | copy | - | XXXX | 32-bit fixed: integer data; with the fraction number format the shader normalises (kShaderNorm32) |
| 34 | `k_32_32` | 1x1 | 8 | RG32_UINT | copy | RG32_SINT | copy | - | XYYY | 32-bit fixed: integer data; with the fraction number format the shader normalises (kShaderNorm32) |
| 35 | `k_32_32_32_32` | 1x1 | 16 | RGBA32_UINT | copy | RGBA32_SINT | copy | - | XYZW | 32-bit fixed: integer data; with the fraction number format the shader normalises (kShaderNorm32) |
| 36 | `k_32_FLOAT` | 1x1 | 4 | R32_FLOAT | copy | R32_FLOAT | copy | - | XXXX |  |
| 37 | `k_32_32_FLOAT` | 1x1 | 8 | RG32_FLOAT | copy | RG32_FLOAT | copy | - | XYYY |  |
| 38 | `k_32_32_32_32_FLOAT` | 1x1 | 16 | RGBA32_FLOAT | copy | RGBA32_FLOAT | copy | - | XYZW |  |
| 39 | `k_32_AS_8` | 4x1 | 4 | R8_UNORM | split_block_to_texels | R8_SNORM | split_block_to_texels | - | XXXX | a 32-bit block of 4 (2) texels, X in the low byte after the swap (unverified) |
| 40 | `k_32_AS_8_8` | 2x1 | 4 | RG8_UNORM | split_block_to_texels | RG8_SNORM | split_block_to_texels | - | XYYY | a 32-bit block of 4 (2) texels, X in the low byte after the swap (unverified) |
| 41 | `k_16_MPEG` | 1x1 | 2 | R16_UNORM | copy | R16_SNORM | copy | - | XXXX | video formats: read as the plain format (no reference; the SDK has no support) |
| 42 | `k_16_16_MPEG` | 1x1 | 4 | RG16_UNORM | copy | RG16_SNORM | copy | - | XYYY | video formats: read as the plain format (no reference; the SDK has no support) |
| 43 | `k_8_INTERLACED` | 1x1 | 1 | R8_UNORM | copy | R8_SNORM | copy | - | XXXX | video formats: read as the plain format (no reference; the SDK has no support) |
| 44 | `k_32_AS_8_INTERLACED` | 4x1 | 4 | R8_UNORM | split_block_to_texels | R8_SNORM | split_block_to_texels | - | XXXX | a 32-bit block of 4 (2) texels, X in the low byte after the swap (unverified) |
| 45 | `k_32_AS_8_8_INTERLACED` | 1x1 | 2 | RG8_UNORM | copy | RG8_SNORM | copy | - | XYYY | the SDK's table gives it a 1x1 16-bit block; followed |
| 46 | `k_16_INTERLACED` | 1x1 | 2 | R16_UNORM | copy | R16_SNORM | copy | - | XXXX | video formats: read as the plain format (no reference; the SDK has no support) |
| 47 | `k_16_MPEG_INTERLACED` | 1x1 | 2 | R16_UNORM | copy | R16_SNORM | copy | - | XXXX | video formats: read as the plain format (no reference; the SDK has no support) |
| 48 | `k_16_16_MPEG_INTERLACED` | 1x1 | 4 | RG16_UNORM | copy | RG16_SNORM | copy | - | XYYY | video formats: read as the plain format (no reference; the SDK has no support) |
| 49 | `k_DXN` | 4x4 | 16 | BC5_UNORM | copy | BC5_SNORM | copy | RG8_UNORM (dxn_to_rg8), signed RG8_SNORM | XYYY | signed: BC5_SNORM (the SDK does not support signed DXN; unverified) |
| 50 | `k_8_8_8_8_AS_16_16_16_16` | 1x1 | 4 | RGBA8_UNORM | copy | RGBA8_SNORM | copy | - | XYZW |  |
| 51 | `k_DXT1_AS_16_16_16_16` | 4x4 | 8 | BC1_UNORM | copy | shader fix-up | - | RGBA8_UNORM (dxt1_to_rgba8) | XYZW | BC blocks after the 8in16 swap are byte-identical to the PC's; decoded to RGBA8 when level 0 is not a multiple of 4 |
| 52 | `k_DXT2_3_AS_16_16_16_16` | 4x4 | 16 | BC2_UNORM | copy | shader fix-up | - | RGBA8_UNORM (dxt3_to_rgba8) | XYZW | BC blocks after the 8in16 swap are byte-identical to the PC's; decoded to RGBA8 when level 0 is not a multiple of 4 |
| 53 | `k_DXT4_5_AS_16_16_16_16` | 4x4 | 16 | BC3_UNORM | copy | shader fix-up | - | RGBA8_UNORM (dxt5_to_rgba8) | XYZW | BC blocks after the 8in16 swap are byte-identical to the PC's; decoded to RGBA8 when level 0 is not a multiple of 4 |
| 54 | `k_2_10_10_10_AS_16_16_16_16` | 1x1 | 4 | R10G10B10A2_UNORM | copy | RGBA16_SNORM | 2_10_10_10_to_rgba16s | - | XYZW | signed: no host SNORM 10:10:10:2, widened to RGBA16_SNORM |
| 55 | `k_10_11_11_AS_16_16_16_16` | 1x1 | 4 | RGBA16_UNORM | 11_11_10_to_rgba16 | RGBA16_SNORM | 11_11_10_to_rgba16s | - | XYZZ | no host unorm / snorm 11:11:10; widened to 16 bits by bit replication |
| 56 | `k_11_11_10_AS_16_16_16_16` | 1x1 | 4 | RGBA16_UNORM | 10_11_11_to_rgba16 | RGBA16_SNORM | 10_11_11_to_rgba16s | - | XYZZ | no host unorm / snorm 11:11:10; widened to 16 bits by bit replication |
| 57 | `k_32_32_32_FLOAT` | 1x1 | 12 | RGBA32_FLOAT | rgb32_to_rgba32f | RGBA32_FLOAT | rgb32_to_rgba32f | - | XYZZ | no RGB32 texture format on every host; alpha 1 added |
| 58 | `k_DXT3A` | 4x4 | 8 | R8_UNORM | dxt3a_to_r8 | shader fix-up | - | - | XXXX | 4-bit values x 17 (R8 has no 4x4 alignment rule; as the SDK) |
| 59 | `k_DXT5A` | 4x4 | 8 | BC4_UNORM | copy | BC4_SNORM | copy | R8_UNORM (dxt5a_to_r8), signed R8_SNORM | XXXX | signed: BC4_SNORM (unverified, as DXN) |
| 60 | `k_CTX1` | 4x4 | 8 | RG8_UNORM | ctx1_to_rg8 | shader fix-up | - | - | XYYY | two 8:8 end points (G in the low byte), thirds truncated as the SDK's CPU decoder |
| 61 | `k_DXT3A_AS_1_1_1_1` | 4x4 | 8 | RGBA8_UNORM | dxt3a_as_1111_to_rgba8 | shader fix-up | - | - | XYZW | bit k of each 4-bit value -> component k (the SDK uses B4G4R4A4) |
| 62 | `k_8_8_8_8_GAMMA_EDRAM` | 1x1 | 4 | RGBA8_UNORM | copy | shader fix-up | - | - | XYZW | EDRAM gamma format: data as is, the shader degammas (kShaderGamma) |
| 63 | `k_2_10_10_10_FLOAT_EDRAM` | 1x1 | 4 | RGBA16_FLOAT | 7e3_to_rgba16f | shader fix-up | - | - | XYZW | 7e3 RGB (0..31.875) and 2-bit alpha to half floats |

### Choices that differ from the SDK

- **5:6:5, 1:5:5:5, 4:4:4:4 and 6:5:5 are widened to RGBA8** (the SDK uses B5G6R5, B5G5R5A1 and B4G4R4A4 on
  D3D12). NVRHI's Vulkan table maps `B5G6R5_UNORM` to `VK_FORMAT_B5G6R5_UNORM_PACK16` and `B5G5R5A1_UNORM` to
  `VK_FORMAT_B5G5R5A1_UNORM_PACK16`, whose bit layouts are not the DXGI ones (only `BGRA4_UNORM` is mapped to
  the matching `A4R4G4B4`), so the same bytes would read differently on the two backends. Widening by bit
  replication is exact at 0 and the maximum and within half a step elsewhere; it costs memory only for
  formats the game is not known to use much. Brief 04 should know about the NVRHI mapping either way.
- **Formats the SDK leaves unsupported get a conversion** (1 bpp, k_8_B, k_8_8_8_8_A, the EDRAM-only formats,
  the 32-bit integer formats, k_32_32_32_FLOAT, the split, MPEG and interlaced formats). These are marked
  unverified in the table; a census of the game's bound textures (below) tells which ones matter.
- **Signed DXN and DXT5A** use BC5_SNORM / BC4_SNORM (the SDK says it does not support signed compressed
  textures). Unverified: if the 360 decodes signed DXN end points differently, this shows as wrong normals.

### Depth, gamma, exponent

- Depth formats used as textures (`k_24_8`, `k_24_8_FLOAT`) become R32_FLOAT: 24-bit unorm as the SDK's
  n / 2^24 with the top bit added back (what a division by 2^24 - 1 rounds to), 20e4 floats exactly. Stencil is
  dropped, as in the SDK (shaders would need a second view).
- Gamma components keep their data; the shader applies the 360's piecewise-linear curve (not sRGB), as the
  SDK's translators do.
- exp_adjust (a power-of-two scale in the fetch constant) is a shader multiply.

## Vertex and index buffers

`kknr/buffers.h`. The 360 vertex declaration (D3DVERTEXELEMENT9 with the 360's D3DDECLTYPE words: format,
endian, signed, integer and swizzle fields; `MakeDeclType` builds them, `kDeclFloat3` and the other traced
constants are checked against it) drives the swaps:

- **Per element, at upload.** Each element is swapped with its own endian mode (FLOAT and D3DCOLOR elements are
  8in32, SHORT elements 8in16 in the same stream), bytes no element reads with the buffer's own mode
  (`PlanVertexSwap`, then `ConvertVertices`). After the swap every element is little-endian with X first, so
  the host input layout reads it directly (`GetHostVertexFormat`: FLOAT3 -> RGB32_FLOAT, SHORT4N ->
  RGBA16_SNORM, UBYTE4 -> RGBA8_UINT, D3DCOLOR -> RGBA8_UNORM ...). Formats with no host equivalent (10:11:11,
  11:11:10, signed or integer 2:10:10:10, 32-bit fixed) are fetched as raw R32_UINT words and unpacked by the
  shader (`needs_unpack`).
- **The declaration's swizzle is the shader's job.** D3DCOLOR's ZYXW (and FLOAT3's XYZ1) is applied by the
  translated vertex fetch (the XDK patches it into the shader's vfetch instructions at bind time), not by
  reordering bytes, so one converted buffer serves every declaration that reads it the same way.
- **One host buffer per (guest range, swap plan).** `VertexConversionId` identifies a plan; the buffer cache
  keys on it, so a buffer read through two declarations with different endian layouts gets two host copies.
  Two elements reading the same bytes with different modes cannot be served by one host buffer;
  `PlanVertexSwap` reports that as a conflict (the later element wins) so the renderer can log it.
- **Index buffers**: 16-bit indices with 8in16, 32-bit with 8in32 (bit 31 of the object's Common word);
  `ConvertIndices`. CPU addresses in the 0xE0000000 view are 4 KB ahead of physical memory
  (`CpuToPhysical`).

## Upload policy

`kknr/texture_cache.h` (tests: `test_cache.cpp`):

- A host texture is keyed by the fetch-constant fields that change its data (format, size, addresses, tiling,
  endian, mips, signs, number format); clamp, filter, LOD, border and swizzle are sampler / view state and
  share the entry.
- Created and uploaded on first bind. Later binds cost nothing unless the entry's guest ranges were written
  (`InvalidateRange`, fed by Unlock and by the SDK's physical-memory write watches) AND the content hash of the
  ranges changed: rewriting identical data does not re-upload. The ranges are exactly the layout's extents
  (tested to cover every block read).
- Resolve destinations are marked GPU-written: the host copy is the truth until the CPU writes there.
- Index and vertex buffers follow the same rules, keyed by (address, size, swap plan).
- Unused entries are trimmed after a number of frames; the renderer frees their host resources in a callback.

## Render targets

`kknr/render_targets.h` (tests: `test_render_targets.cpp`):

| 360 surface format | Host render target |
|---|---|
| k_8_8_8_8 (and _A, _AS_16_16_16_16, the gamma EDRAM variant) | RGBA8_UNORM (gamma applied in the shader) |
| k_2_10_10_10 (and _AS_16_16_16_16) | R10G10B10A2_UNORM |
| k_2_10_10_10_FLOAT_EDRAM, k_16_16_16_16_FLOAT | RGBA16_FLOAT (7e3 kept in half floats) |
| k_16_16_FLOAT | RG16_FLOAT |
| k_16_16_16_16 / k_16_16 (and the EDRAM variants) | RGBA16_SNORM / RG16_SNORM (the -32..32 range scaled by the shader, as the SDK) |
| k_32_FLOAT / k_32_32_FLOAT | R32_FLOAT / RG32_FLOAT |
| k_24_8 | D24S8 |
| k_24_8_FLOAT | D32S8 (20e4 depth kept in a float depth buffer) |

- **Pool.** The engine creates 12 transient surfaces per gameplay frame (832x832 R32F shadow maps, 320x180 /
  640x360 / 640x480 A8R8G8B8 and X8R8G8B8 post-effect targets) and releases them the same frame. Acquire
  returns a free host target with the same description and EDRAM base (the most recently released, so a
  surface re-created at the same EDRAM place sees what was drawn there, as on the 360), else a free one of the
  same size, format and sample count at another base, else asks the renderer to create one. After the first
  frame nothing is created (tested over five frames of the census's surface set). Unused targets are trimmed.
  The pool does not model EDRAM aliasing between surfaces of different formats placed over each other;
  the frame logs show no such reuse within a frame, to be confirmed with the census.
- **EDRAM size** of a surface (80x16-sample tiles, doubled for 64 bpp and for MSAA) is computed as the XDK
  does, for checking the engine's placements.
- **Resolve** (`PlanResolve`) becomes: a copy when source and destination share a host format and channel
  order (with the rectangle, destination point, level and slice; clipped to the destination level), a converting blit when they
  differ or R and B must be exchanged, a depth-to-float copy for depth sources (shadow maps resolved into R32F textures), or clears only.
  Its clear flags (0x100 colour, 0x200 depth / stencil) become clears of the pooled target after the copy.
  `swap_red_blue` is set when the destination's swizzle reads X as blue (an A8R8G8B8 texture: ZYXW). The
  host render target holds the shader's red in R, while the texture's data must hold blue in X (its view maps
  R to Z), so that copy has to exchange R and B: a blit, not a raw copy. Brief 04 decides how (a compute or
  draw blit; a typed copy cannot swap).

## Tools

| Tool | What it does |
|---|---|
| `kknr_texdump [--out DIR] [--csv FILE] [--no-images] FILE_OR_DIR...` | Converts KKTX dumps with the library: DDS in the host format (every level and layer), a PNG preview (level 0, layers or slices stacked, view swizzle applied), one summary line and a CSV row each, with an FNV-1a hash of the host data |
| `kknr_bfscan <KKTextures.bf> [--dump DIR] [--limit N] [--pack KEY] [--assume-tiled]` | Lists the texture records in the game's texture bigfile (size, D3DFORMAT, four unknown header words) and a count per format / endian / tiling; with `--dump` writes them as KKTX (base level only, fetch constant synthesized from the D3DFORMAT word) |
| `kknr_formats` | Prints the table above |
| `native-renderer/tools/texcompare.py OURS THEIRS [--tolerance N] [--html FILE]` | Compares our PNGs with textures exported from today's renderer: max / mean difference, PSNR, share of texels off by more than the tolerance (default 2 / 255) |

KKTX (written by `KK_DEV_TEX_DUMP` in the trace build and by `kknr_bfscan --dump`): "KKTX", version 1, the 6
fetch-constant words, base bytes, mip bytes (little-endian), then the base and mip regions exactly as stored.

## Validating on Windows

The cloud has no game data, so these steps are for the PC (paths as in `docs/briefs/README.md`):

1. Build and test the library: `native-renderer\resources\build.bat res-release` (configure, build, ctest; the
   NVRHI format test builds from `native-renderer\thirdparty\nvrhi`).
2. Census and dumps from the game (trace build generated by `tools/analysis/gen_d3d_trace.py`):
   `KK_DEV_TEX_CENSUS=F:/KK-native-renderer/analysis/census_<scene>.txt` and
   `KK_DEV_TEX_DUMP=F:/KK-native-renderer/analysis/texdump_<scene>,400` for the menus, a video, a Jack chapter
   and a Kong chapter (`KK_DEV_SCRIPT` / `KK_DEV_AUTOSKIP`), then
   `python native-renderer\tools\analysis\census_report.py analysis\census_*.txt > analysis\census_report.txt`
   for the set of (format, endian, tiling, dimension, signs, swizzle, mips) combinations the game binds.
3. Convert every dump: `kknr_texdump --out analysis\texout --csv analysis\texout.csv analysis\texdump_*`. Every
   line must say ok; any failure or any format marked unverified in the table above that shows up in the
   census is a finding.
4. Ten (or more) real textures against today's renderer: capture a frame of the same scene with RenderDoc on
   today's renderer (D3D12), save the matching textures (by base address: the KKTX name holds it) as PNG into
   `analysis\texref`, and run `python native-renderer\tools\texcompare.py analysis\texout analysis\texref --html
   analysis\texcompare.html`. Pick at least one of each format / tiling / endian combination the census lists,
   one cube map, one texture with packed mips, one resolve target (shadow map or post-effect texture).
5. Optionally, the bigfile path without running the game: `kknr_bfscan "<game>\KKTextures.bf" --dump
   analysis\bfdump --limit 200` (add `--assume-tiled` if the listing shows the tiling bit clear on textures
   that look scrambled) and `kknr_texdump --out analysis\bfout analysis\bfdump`. The front-end logo (pack
   FF80C45A, 512x256 8:8:8:8) must come out the same as the launcher's extraction.
