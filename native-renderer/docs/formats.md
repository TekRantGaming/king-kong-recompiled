# Guest formats and host formats (brief 03)

How the game's Xbox 360 resources become host resources: every guest texture format with its host format and
conversion, how tiling, mips and endianness are undone, how vertex and index buffers are swapped, and how
EDRAM render targets map to pooled host targets. The code is `native-renderer/resources` (library
`kknr_resources`, namespace `kknr`); the references are the SDK's `include/rex/graphics/xenos.h`,
`src/graphics/pipeline/texture/` and the D3D12 / Vulkan texture caches (rexglue-sdk v0.10.0, Xenia-derived,
BSD), read but not copied.

Status: implemented, unit-tested with synthetic data (`ctest`: 59 converter, tiling, buffer, cache and
render-target tests plus 3 NVRHI format-table tests, passing on Linux and on Windows with Visual Studio 2022
clang and Ninja, release and debug), and checked on the game's own data: every texture shape the game binds
converts byte for byte as today's renderer converts it (the census and the results are in "What the game
binds" and "Checked against today's renderer" below). Choices marked "unverified" in the table concern
formats the game does not bind; they have no hardware or SDK reference and are the first to check if a
future use shows them wrong.

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
  above 0 drops the base; a mip address with max level 0 means no mips, and so does a mip address of 0
  whatever the max level (as the SDK's `GetSubresourcesFromFetchConstant`; the game binds its 8x8 and 16x16
  textures whose whole chain sits in the base's tail that way, see the findings below).

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
  formats the game does not bind at all (census below). Brief 04 should know about the NVRHI mapping either way.
- **Formats the SDK leaves unsupported get a conversion** (1 bpp, k_8_B, k_8_8_8_8_A, the EDRAM-only formats,
  the 32-bit integer formats, k_32_32_32_FLOAT, the split, MPEG and interlaced formats). These are marked
  unverified in the table; the census (below) finds none of them bound.
- **Signed DXN and DXT5A** use BC5_SNORM / BC4_SNORM (the SDK says it does not support signed compressed
  textures). Unverified: if the 360 decodes signed DXN end points differently, this shows as wrong normals.
  The game binds DXN unsigned only, and no DXT5A.

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

## What the game binds (census)

Sources: stream 01's object dumps (the 40-byte texture object passed to every SetTexture, `KK d3d obj:
sub_82118F78` lines, in `analysis\01`: the title screen, startup movie, save menu, main menu, chapter select,
loading screen, Jack gameplay in the V-Rex chapter, the pause menu, Kong gameplay and a Kong chapter with
Jack, plus every other window of the four runs) and this stream's resource census run (`KK_DEV_TEX_CENSUS`,
from launch into the V-Rex chapter: every distinct texture bound). A texture is one distinct fetch constant as
the object holds it (no sampler state). `tools/analysis/tex_census.py` builds the table; 441 distinct textures.

| Format | Endian | Tiling | Dim | Textures | Sizes (levels) | Seen in |
|---|---|---|---|---|---|---|
| k_DXN | 8in16 | tiled | 2D | 102 | 32x32 to 1024x1024, all with full mip chains and packed tails | menus, gameplay |
| k_DXT4_5 | 8in16 | tiled | 2D | 100 | 32x32 to 1024x1024 with mips; 1024x64, 512x64, 128x64, 256x128, 512x512 without | everywhere but the movie |
| k_DXT1 | 8in16 | tiled | 2D | 71 | 64x64 to 1024x1024 with mips; 16x16 levels 0-4 with the whole chain in the base's tail | menus, gameplay |
| k_8_8_8_8 | 8in32 | tiled | 2D | 67 | 64x64 to 512x512 with mips; 8x8 and 16x16 chains in the base's tail; resolve targets 1280x720, 640x480, 640x360, 320x180; 16x16, 64x32, 256x128, 256x256 without mips | everywhere |
| k_8 | none | tiled | 2D | 55 | 128x512 to 1024x1024 with mips (masks); 1280x720 resolve targets | menus, gameplay |
| k_8 | none | linear | 2D | 30 | 1280x720 and 640x360 (the video's Y, U and V planes) | movie, loading |
| k_8_8_8_8 | 8in32 | linear | 2D | 6 | 512x32 | loading |
| k_32_FLOAT | 8in32 | tiled | 2D | 6 | 832x832 (shadow maps, resolved; number format "integer", signs SSSS) | gameplay |
| k_24_8 | 8in32 | tiled | 2D | 2 | 1280x720, 320x180 (depth resolved into a texture) | menus, gameplay |
| k_8_8 | 8in16 | tiled | 2D | 1 | 64x64, signed (SSSS) | Jack gameplay |
| k_DXT1 | 8in16 | linear | cube | 1 | 128x128, levels 0-7, packed tail | Jack gameplay |

Every texture has signs uuuu except the k_32_FLOAT and k_8_8 ones (SSSS); swizzles are XYZW (block formats),
ZYXW / ZYX1 (A8R8G8B8 / X8R8G8B8), 000X (k_8 masks), XXX1 (video planes), X111 (shadow maps) and XY11 (depth,
k_8_8). No exp_adjust, no 3D or stacked textures, and no format the table marks unverified.

KKTextures.bf holds 11,940 texture records (`kknr_bfscan`): k_DXN 4,134, k_DXT4_5 3,284, k_DXT1 2,495, k_8 1,798
and k_8_8_8_8 229, all tiled; KKMaps.bf holds none. The bound textures not from the bigfile are made at run
time: render-target resolves, the video planes (written by the CPU through LockRect every frame), the cube
map, the 512x32 linear and the small 8:8:8:8 and signed 8:8 textures.

Render targets (CreateRenderTarget, census run): 832x832 k_32_FLOAT (0x2DA2ABA4), 320x180 / 640x360 / 1280x720
A8R8G8B8 (0x18280186), 640x480 X8R8G8B8 (0x28280186), 1280x720 D24S8 (0x2D200196), no MSAA; all map to the
pool's formats. Resolves go into k_32_FLOAT, k_8_8_8_8 (8in32, and once endian none ZYX1), k_8 (endian none,
000X) and k_24_8 textures, with clear flags 0x100, 0x200 and 0x300.

Vertex and index buffers (stream 01's SetVertexDeclaration dumps and the census run): 20 distinct
declarations, up to 8 elements on streams 0 and 1, using six element types only: FLOAT3 0x002A23B9 (position,
normal), FLOAT2 0x002C23A5 (texture coordinates), FLOAT4 0x001A23A6, D3DCOLOR 0x00182886 (colour), SHORT4N
0x001A215A (8in16, normals and tangents) and UBYTE4 0x001A2286 (blend indices). Streams mix 8in32 and 8in16
elements (FLOAT3, D3DCOLOR, FLOAT2, SHORT4N and UBYTE4 in one vertex), which is what the per-element swap plan is
for; all six map to host input formats without shader unpacking (`test_buffers.cpp` checks each). Every vertex
buffer has fetch type 3 and endian 8in32; every index buffer is 16-bit.

## Checked against today's renderer

The reference is the SDK's own texture code rather than a capture: `kknr_sdkref` compiles the SDK's
`texture_util` (which levels exist, the guest layout, tiled addressing, packed mip offsets) from `C:\rexsrc`
and emulates `D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl` on the CPU, and with `--gpu` runs the
SDK's compiled load shaders (the same bytecode the game's renderer dispatches) through D3D12 with the cache's
constants, dispatch sizes, copy-buffer layout and CopyTextureRegion boxes. This needs no game run (stream 04
has the game) and compares exact bytes per subresource, which a RenderDoc capture would also give but only for
the textures of one captured frame. Both references must match ConvertTexture's output block for block, and
the view swizzle must equal the SRV swizzle the SDK binds. A one-block change in the reference is caught
(checked by a temporary mutation).

| Data | Textures | Result |
|---|---|---|
| Memory dumps of bound textures, V-Rex run (every combination in the census, version 1 dumps corrected) | 45 | all match, CPU and GPU |
| KKTextures.bf records, up to 10 per shape (all 77 shapes: sizes, level counts, packed tails, the 5 formats) | 680 | all match, CPU and GPU |
| Every bound fetch constant (441) with random texel data | 441 | all match, CPU and GPU |
| Resolve-target dumps (stale memory in the dumps) refilled with random data: k_24_8 depth conversion, k_32_FLOAT, k_8 | 7 | all match bit for bit, CPU and GPU |
| bf record sizes (level count, packed level, data size) against the library's layout | 11,940 | all equal |
| Fetch constants synthesized from bf records against the bound ones of the same shape | 341 | all fields equal (5 with mips allocated apart from the base) |
| The front-end logo (512x256 8:8:8:8) against the launcher's own untiling (`kk/src/launcher_art.cpp`) | 1 | identical |

PNG previews (`kknr_texdump`, and `<name>.ref.png` from `kknr_sdkref --png`) were looked at for every
combination: textures come out whole (Kong's normal map, the environment cube map, the logo, the masks);
`texcompare.py` over 265 pairs finds no texel difference.

No converter change was needed: the cloud's conversions and layout match today's renderer on all of it.
`roundtrip_census_fetch_constants` keeps one fetch constant of each bound combination (25 in all: the cube map
whose mips are allocated before its base, the chains kept in the base's tail, the video planes, the resolve
targets) in the unit tests, through the tiling round trip and the host plan.

Findings:
- The census dump (version 1) read every texture one 4 KB page early (fixed, see KKTX below).
- Small textures whose whole chain sits in the base's packed tail (8x8 and 16x16 8:8:8:8, 16x16 DXT1) are bound
  with max level 3 or 4 and mip address 0. The SDK then loads level 0 only, and so does this library, to match
  today's renderer; the hardware probably reads levels 1+ from the base's tail. Visible only when such a
  texture is minified. Open.
- The shadow maps' D3DFORMAT (0x2DA2ABA4) sets the integer number format on a float format; ignored, as the
  SDK does (kShaderInteger is only set for fixed-point formats).
- The resolve into a k_8 texture (from an 8:8:8:8 target) and the endian-none 8:8:8:8 resolve are the two
  unusual resolves; their contents can only be checked on frames (stream 04).
- Resolve targets in guest memory are stale in dumps (today's renderer keeps resolves on the GPU), so their
  contents were checked with random data instead; the cache must treat them as GPU-written (it does).

## Tools

| Tool | What it does |
|---|---|
| `kknr_texdump [--out DIR] [--csv FILE] [--no-images] FILE_OR_DIR...` | Converts KKTX dumps with the library: DDS in the host format (every level and layer), a PNG preview (level 0, layers or slices stacked, view swizzle applied), one summary line and a CSV row each, with an FNV-1a hash of the host data |
| `kknr_sdkref [--out DIR] [--csv FILE] [--png] [--quiet] [--gpu] FILE_OR_DIR...` | Checks ConvertTexture against the SDK's texture code (CPU emulation; `--gpu`: also the SDK's load shaders on D3D12), block for block, plus the view swizzle; `--png` writes the reference's level 0 as `<name>.ref.png`. Built when `KKNR_SDK_SOURCE` (default `C:/rexsrc`) holds the SDK source |
| `kknr_bfscan <KKTextures.bf> [--dump DIR] [--limit N] [--pack KEY] [--per-shape K] [--assume-tiled]` | Lists the bigfile's texture records (size, D3DFORMAT, level count, first packed level, data size), checks each record's size against the layout, counts per format; `--dump` writes KKTX files with the whole mip chain (`--per-shape K`: at most K per shape) |
| `kknr_formats` | Prints the table above |
| `tools/analysis/tex_census.py [--bf LISTING] [--csv FILE] LABEL=FILE...` | The census table from census files and stream 01 logs (and a bfscan listing) |
| `tools/analysis/census_synth.py CENSUS_CSV OUT_DIR` | One KKTX per bound fetch constant with random data, for kknr_sdkref |
| `tools/analysis/check_bf_fetch.py CENSUS_CSV BFDUMP_DIR` | Synthesized bigfile fetch constants against the bound ones |
| `native-renderer/tools/texcompare.py OURS THEIRS [--tolerance N] [--html FILE]` | Compares PNGs in pairs (ours against `.ref.png` or RenderDoc exports): max / mean difference, PSNR, share of texels off by more than the tolerance |

KKTX (`tools/kktx.h`): "KKTX", a version, the 6 fetch-constant words as the object holds them, base bytes, mip
bytes (little-endian), then the base and mip regions exactly as stored. Version 1 trace dumps took the low 29
bits of the object's CPU address as the physical address; for the 0xE0000000 view the GPU address is a page
further, so those dumps start one page early and miss their last page. The readers move version 1 regions up a
page (last page zero). The trace writes version 2 with the right page; kknr_bfscan writes version 2.

## Reproducing the validation

On the PC (outputs under `F:\KK-native-renderer\analysis\03`, never in git):

1. `native-renderer\resources\build.bat res-release` (and `res-debug`): library, tools, tests.
2. Census: `python -I native-renderer\tools\analysis\tex_census.py --bf analysis\03\bfscan_list.txt --csv
   analysis\03\tex_census.csv title=analysis\01\scene_title.log ... census=analysis\03\census_vrex.txt` (one
   LABEL=FILE per stream 01 scene log or run log and per census file; the bfscan listing from step 3).
3. Bigfile: `kknr_bfscan kk\assets\KKTextures.bf > analysis\03\bfscan_list.txt` (size check of every record),
   then `kknr_bfscan kk\assets\KKTextures.bf --dump analysis\03\bfdump10 --per-shape 10`.
4. `kknr_sdkref --gpu --quiet --csv analysis\03\sdkref_bf10_gpu.csv analysis\03\bfdump10`, the same on the
   census dumps (`analysis\03\dumps_vrex`) and on `census_synth.py`'s output; every line must say match.
5. More dumps from the game, when it is free: the trace build (`kk\build.bat kk-dev`) with
   `KK_DEV_TEX_CENSUS=<file>` and `KK_DEV_TEX_DUMP=<dir>,400`, then steps 2 and 4 on them.
