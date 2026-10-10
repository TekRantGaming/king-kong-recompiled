# The game's Direct3D objects: layouts

Byte offsets, big-endian guest memory. Bit numbers are LSB = 0 (the PowerPC listings number bits from the
MSB; converted here). "Code" means read in the recompiled library (`kk/generated/default`, function named);
"dump" means seen in memory (`analysis/device_dump.bin`, or the `KK d3d obj:` / `KK d3d ret:` hex lines the
trace writes, see `d3d-api-map.md`); a layout counts as confirmed when both agree. Machine-readable names are
in `native-renderer/d3d_api.json`.

## The common resource header

Every resource (vertex buffer, index buffer, texture, surface) starts with:

| Offset | Field | Meaning |
|---|---|---|
| +0 | Common | bits 0-7 reference count (AddRef `sub_82108C48`, Release `sub_821091C8`); bits 12-15 lock count (Lock +0x1000, Unlock `sub_821090E0` -0x1000); bits 16-18 type: 1 vertex buffer, 2 index buffer, 3 texture, 4 surface; bits 19-23 device binding count (each Set* of the resource adds 0x80000, replacing it subtracts; Release frees only when this and the reference count are zero); bit 24 created by D3D; bit 25 memory not owned (a surface that is a texture level: Release does not free memory); bit 30 cached CPU memory (D3DUSAGE 4 at creation); bit 31 index buffer: 32-bit indices / surface: EDRAM allocated by D3D |
| +4 | Fence | GPU fence (device+10396) when the resource was last unbound; Release defers freeing until the GPU passes it |
| +8 | dirty range | for Unlock's cache flush: (first 128-byte line << 16) \| last line; 0xFFFF0000 = empty |

Code: `sub_82108C48`, `sub_821091C8`, `sub_82108D60`, `sub_821090E0`, `sub_8210BD38`. Dump: every buffer and
texture in the traces has the type and count fields as described (checked by `check_d3d_objects.py`).

## Vertex buffer (20 bytes) and index buffer (20 bytes)

| Offset | Vertex buffer | Index buffer |
|---|---|---|
| +0 | Common = 0x01010001 (\| 0x40000000 cached) | Common = 0x01020001 (\| 0x80000000 32-bit indices, \| 0x40000000 cached) |
| +4 | Fence | Fence |
| +8 | dirty range | dirty range |
| +12 | vertex fetch constant word 0: physical address \| 3 (type = vertex) | physical address of the indices |
| +16 | vertex fetch constant word 1: size in dwords << 2 \| endian 2 (8in32) | size in bytes |

Created by `sub_821092B0` (length, usage) and `sub_821093E0` (length, usage, format), memory from the physical
allocator `sub_820F2108`. The guest address of the data is `+12 & ~3` (vertex) or `+12` (index), both in the
physical views (0xA0000000 / 0xC0000000 / 0xE0000000; the GPU address is the low 29 bits, plus 0x1000 for the
0xE0000000 view). Index data is big-endian 16 or 32-bit.

## Texture (40 bytes)

| Offset | Field |
|---|---|
| +0 | Common = 0x01030001 (\| 0x40000000 cached) |
| +4 | Fence |
| +8 | base dirty range |
| +12 | mip dirty range |
| +16 .. +36 | the GPU texture fetch constant, 6 dwords (below) |

Created by `sub_82118A68` (inner CreateTexture; the engine calls `D3DXCreateTexture` `sub_82126E70` or the inner
one directly); the header is filled by `sub_82118558` from a D3DFORMAT; base memory (word 1) and mip memory
(word 5) are separate physical allocations, 4 KB aligned. The resource type passed to the creator picks the
dimension: 3 texture (2D), 4 volume (3D), 5 cube, 8 array (stacked 2D).

### Fetch constant (texture+16, and the device's per-sampler copy at device+1152+24*sampler)

The Xenos `xe_gpu_texture_fetch_t` (`C:\rexsrc\include\rex\graphics\xenos.h`). Who writes what:

| Word | Bits | Field | Set by |
|---|---|---|---|
| 0 | 0-1 | type = 2 (texture) | creation |
| 0 | 2-9 | sign x, y, z, w (2 bits each) | creation, from the D3DFORMAT |
| 0 | 10-12, 13-15, 16-18 | clamp x, y, z | ADDRESSU / V / W (sampler) |
| 0 | 22-30 | pitch in texels >> 5 | creation |
| 0 | 31 | tiled | creation (D3DFORMAT bit 8) |
| 1 | 0-5 | GPU texture format (`TextureFormat`) | creation |
| 1 | 6-7 | endian | creation |
| 1 | 8-9 | request size | |
| 1 | 10 | stacked (array texture) | creation |
| 1 | 11 | nearest clamp policy | POINTBORDERENABLE (sampler, inverted) |
| 1 | 12-31 | base address >> 12 | creation (SetTexture converts it to the GPU physical address) |
| 2 | | size - 1: 1D width 24 bits; 2D/cube width 13, height 13, stack depth 6; 3D 11, 11, 10 | creation |
| 3 | 0 | number format (fraction / integer) | creation |
| 3 | 1-12 | swizzle (x, y, z, w; 3 bits each) | creation |
| 3 | 13-18 | exponent adjust | |
| 3 | 19-20, 21-22, 23-24 | mag, min, mip filter | MAGFILTER, MINFILTER, MIPFILTER |
| 3 | 25-27 | anisotropic filter | MAXANISOTROPY (while a filter is ANISOTROPIC) |
| 3 | 31 | border size | |
| 4 | 0, 1 | volume mag / min filter | MAGFILTERZ / MINFILTERZ, applied by SetTexture |
| 4 | 2-5 | mip min level (the most detailed level used) | MAXMIPLEVEL, max with the texture's own |
| 4 | 6-9 | mip max level (= levels - 1) | creation; MINMIPLEVEL, min with the texture's own |
| 4 | 10, 11 | mag / min anisotropic walk | MAGFILTER / MINFILTER = ANISOTROPIC |
| 4 | 12-21 | LOD bias, signed, 5 fraction bits | MIPMAPLODBIAS |
| 4 | 22-26, 27-31 | gradient exponent adjust H, V | HGRADIENTEXPBIAS, VGRADIENTEXPBIAS |
| 5 | 0-1 | border colour (0 black, 1 white) | BORDERCOLOR |
| 5 | 2 | force border alpha to max | WHITEBORDERCOLORW |
| 5 | 3-4 | trilinear clamp | TRILINEARTHRESHOLD |
| 5 | 5-8 | anisotropy bias | ANISOTROPYBIAS |
| 5 | 9-10 | dimension: 0 1D, 1 2D or stacked, 2 3D, 3 cube | creation |
| 5 | 11 | packed mips (mip tail) | creation |
| 5 | 12-31 | mip address >> 12 | creation |

SetTexture (`sub_82118F78`) builds the device copy from the texture's words and the sampler-state bits
already in the device copy, so at draw time device+1152+24*sampler is exactly what the GPU would fetch; the
texture object pointer is at device+12704+4*sampler.

### D3DFORMAT (360 encoding, decoded by `sub_82118558`, rebuilt by `sub_821170E0`)

| Bits | Field |
|---|---|
| 0-5 | GPU texture format |
| 6-7 | endian |
| 8 | tiled |
| 9-10, 11-12, 13-14, 15-16 | sign x, y, z, w |
| 17 | number format |
| 18-20, 21-23, 24-26, 27-29 | swizzle x, y, z, w |

Examples from the traces: 0x18280186 (A8R8G8B8: format 6 k_8_8_8_8, endian 2, tiled, swizzle; also the
display mode format), 0x28280186 (format 6 with another swizzle), 0x2DA2ABA4 (format 36 k_32_FLOAT, the
render-to-texture targets of the shadow passes).

## Surface (64 bytes)

Two kinds: EDRAM render targets (CreateRenderTarget `sub_82118B88`, also used for depth / stencil) and
texture levels (GetSurfaceLevel `sub_82118838` / GetCubeMapSurface `sub_82118900`, through `sub_82117678`).

| Offset | Render target / depth stencil | Texture level |
|---|---|---|
| +0 | Common = 0x01040001, \| 0x80000000 when D3D allocated the EDRAM | Common = 0x03040001 (bit 25: memory belongs to the parent) \| cached bit of the parent |
| +4 | Fence | Fence |
| +8 | 0xFFFF0000 | 0xFFFF0000 |
| +16 .. +36 | a fetch-constant image describing the surface as a texture: word 1 format and endian, word 2 width-1 / height-1, word 3 swizzle, word 5 dimension 2D | copy of the parent's fetch constant with the pitch (word 0), size (word 2, face in bits 26-31), level (word 4: min = max = level) and dimension 2D |
| +40 | EDRAM size in bytes (tiles * 5120) | offset of the level |
| +44 | | parent texture (AddRef'd; Release releases it) |
| +48 | RB_SURFACE_INFO image: pitch in pixels (80-pixel tiles at 1x/2x, 40 at 4x) bits 0-13, MSAA samples bits 16-17, HiZ pitch bits 18-31 | |
| +52 | RB_COLOR_INFO (EDRAM base in tiles bits 0-11, colour format bits 16-19, exponent bias bits 20-25) or RB_DEPTH_INFO (base bits 0-11, depth format bit 16: 0 D24S8, 1 D24FS8) | |
| +56 | depth only: HiZ enable / base | |

EDRAM is 2048 tiles of 5120 bytes (10 MB); `sub_8211BF18` allocates, `sub_8211BEC8` frees. A 1280x720 1x
32-bit target is 720 tiles (the title-screen dump has the colour target at tile 0 and depth at tile 720,
RB_DEPTH_INFO 0x2D0). SetRenderTarget copies +52 into RB_COLOR_INFO / RB_COLORn_INFO and, for target 0,
+48 into RB_SURFACE_INFO.

For the native renderer: an EDRAM surface becomes a host render target keyed by (base tile, format, size,
MSAA); a texture-level surface writes into its parent texture level.

## Volume (48 bytes)

From GetVolumeLevel `sub_821189B8` (`sub_82117758`): the 3D counterpart of a texture-level surface. Not seen in
the traces.

## Vertex shader and pixel shader

| Offset | Vertex shader (52-byte header) | Pixel shader (592-byte header) |
|---|---|---|
| +4 | reference count (AddRef `sub_821106E0`, Release `sub_821106F8` / `sub_821107B0`) | reference count |
| +8 | Fence | Fence |
| +12 | physical address of the microcode | |
| +28 | flags: bit 31 microcode owned, bit 30 object owned | |
| +40 | | physical address of the microcode |
| +588 | | flags (bit 31 bound, bits 29-30 owned) |
| +52 / +592 | copy of the shader container's virtual part (header, constant table, input semantics, shader block) | same |

The container passed to CreateVertexShader / CreatePixelShader (`sub_82111CA0` / `sub_82111D90`) is the
`xeshaders.bin` / compiler output: word 0 flags (low 7 bits non-zero = pixel shader), word 1 virtual size,
word 2 physical (microcode) size, word 3 offset of the "shader block". The object is header + a copy of the
first virtual-size bytes; the microcode (physical part, which follows the virtual part in the container) is
copied to its own physical allocation.

The shader block (at container + word 3, i.e. shader+52+[shader+64] or shader+592+[shader+604]) is applied
by SetVertexShader / SetPixelShader: a 64-bit mask OR'ed into a pending-constant word, a second 64-bit word
(non-zero: mark the bool/loop constants dirty), a byte count, then records `{u16 offset from device+1152,
u16 dword count, data}` that copy literal constants into the device shadow, and records `{offset, count,
(and mask, or mask) pairs}` that patch bits. This is how a shader's literal constants (`def`) reach the
constant file: the renderer must treat the device's constant shadow after SetVertexShader / SetPixelShader as
the truth, not only the SetXxxShaderConstant calls.

The pixel shader's 592-byte header also caches the linkage with the vertex shader (interpolator patching,
`sub_82120A70`), and the vertex shader object caches its microcode patched for a vertex declaration and
stride set (`sub_82120028` rewrites each vertex fetch instruction with the fetch constant 95-stream, offset,
stride and format of the declaration element that matches its usage). A translated vertex shader therefore
depends on (shader, declaration, strides); bind vertex inputs by usage / usage index from the declaration.

## Vertex declaration (40 + 12 * elements bytes)

| Offset | Field |
|---|---|
| +4 | reference count |
| +8 | element count (not counting the end marker) |
| +12 | highest stream number used |
| +16 .. +31 | 16 bytes, one per stream: 0xFF if the stream is used |
| +32 | id, given on first use from a global counter (0x828E4420) and used to key the patched shaders |
| +36 | the elements, copied |

Element (D3DVERTEXELEMENT9, 360 form, 12 bytes): +0 WORD Stream (0xFF = end), +2 WORD Offset (bytes),
+4 DWORD Type (D3DDECLTYPE: bits 0-5 are the Xenos `VertexFormat`, e.g. 0x2A23B9 FLOAT3 = 57 k_32_32_32_FLOAT,
0x1A23A6 FLOAT4 = 38, 0x2C23A5 FLOAT2 = 37, 0x182886 D3DCOLOR = 6 k_8_8_8_8; the other bits carry endian,
sign, number format and swizzle like D3DFORMAT), +8 BYTE Method, +9 BYTE Usage, +10 BYTE UsageIndex, +11 pad.
Created by `sub_82110C90`, released by `sub_82110D50`, set by `sub_82111E68`.

## State block (10,148 bytes)

`sub_82119490` creates it: magic 'D3sb' (0x44337362) at +0, type +4, device +8, reference count +12,
textures +16 (26), stream sources +124 (16 x {offset, buffer}), index buffer +264, pixel shader +268,
declaration +272, vertex shader +276, then copies of the device's register images. Used by the
loading-screen thread to save and restore device state around its own drawing.

## The device (20,608 bytes)

At *(0x82D62324). Fields by offset (the register images are listed in `d3d-api-map.md`):

| Offset | Field |
|---|---|
| +0, +4, +8 | ring buffer: current write position (last word written), segment start, segment limit (a write past it kicks the segment, `sub_8211AB20`) |
| +12 | reference count |
| +16 .. +55 | pending (dirty) state, five 64-bit words, MSB first: +16 vertex constants (bit per 4 float4), +24 pixel constants, +32 fetch constants (bits 63-32) / bool and loop constants (bit 31) / destination packet (bits 29-14), +40 values, program, control, tessellator packets, +48 misc and point packets plus derived state (bit 10 vertex streams, bits 5-8 shader binding) |
| +96 .. +563 | render-state and sampler-state setter table (117 function pointers) |
| +564 .. +1031 | the matching getters (index 0-9 return 0) |
| +1152 .. +1919 | fetch constants 0-31 (6 dwords each) |
| +1920 .. +6015 | vertex shader float constants c0-c255 |
| +6016 .. +10111 | pixel shader float constants c0-c255 |
| +10112 .. +10143 | bool constants (vertex 0-127, pixel 128-255) |
| +10144 .. +10271 | loop constants (vertex 0-15, pixel 16-31; x \| y << 8 \| z << 16) |
| +10376 | owner thread (Acquire / ReleaseThreadOwnership) |
| +10384 | address the GPU writes completed fences to |
| +10396 | current fence number |
| +10404 | SQ_GPR_MANAGEMENT value |
| +10432 .. +10434 | flag bytes (+10432 bit 4: predicated tiling active; bit 2: draws go through the secondary buffer at +13536) |
| +11408 | vertex declaration |
| +11424, +11432 | strides at the last shader binding |
| +11448, +11452 | blend render-state cache (packed blend fields; bit 31 ALPHABLENDENABLE, bit 30 SEPARATEALPHABLENDENABLE, bits 23-29 PRESENTIMMEDIATETHRESHOLD) |
| +11456 .. +11951 | register images (`d3d-api-map.md`, "The register shadow") |
| +11952 .. +12156 | render-state caches (SCISSORTESTENABLE, COLORWRITEENABLE0-3, ZENABLE, STENCILENABLE, POINTSPRITEENABLE, point sizes, per-sampler bytes for MAXANISOTROPY +12020, MAXMIPLEVEL +12046, MINMIPLEVEL +12072, Z filters +12098, state 57 +12124, VIEWPORTENABLE +12140, HIGHPRECISIONBLENDENABLE0-3 +12144) |
| +12160 | mask of conditional surveys in use (64 bits) |
| +12234, +12299 | conditional rendering stack and depth |
| +12532 | index buffer |
| +12536 .. +12548 | render targets 0-3 |
| +12552 | depth stencil surface |
| +12556 + 8s, +12560 + 8s | stream s: offset in bytes, vertex buffer (16 streams) |
| +12688 + s | stream s stride / 4 (byte) |
| +12704 + 4n | texture on sampler n (26 samplers) |
| +12808 | D3DVIEWPORT9 (X, Y, Width, Height as DWORD, MinZ, MaxZ as float), after clamping to the target |
| +12832 | scissor rect (left, top, right, bottom; default 0, 0, 0x7FFFFFFF, 0x7FFFFFFF) |
| +12848 + 16n | clip plane n (6) |
| +12944 | vertex shader |
| +12948 | pixel shader as bound |
| +12972 .. +13480 | predicated tiling: targets, tile count +12992, tile rects +12996 |
| +13720, +13724, +13728, +13768 | display mode: width, height, format, refresh rate |
| +13772 | PRESENTINTERVAL |
| +13816 | swap parameters |
| +13960 | front buffer texture (the resolve destination at Present) |
| +13964 | back buffer surface (render target 0 at Present) |
| +14060 .. +15595 | gamma ramp copy |
| +20456 | pixel shader as set by the engine |

Confirmed against the title-screen dump: the register images hold the expected defaults (VGT_MAX_VTX_INDX
0xFFFFFF, RB_COLOR_MASK 0xF, the viewport 640 / 640 / -360 / 360 / 1 / 0 for 1280x720, guard bands 2.0 / 1.0,
SQ_VS_CONST 0xFF000 and SQ_PS_CONST 0xFF100, RB_SURFACE_INFO pitch 1280, RB_DEPTH_INFO base 720 tiles,
window scissor 1280x720, PA_CL_VTE_CNTL 0x43F), render target 0 is the back buffer (+12536 = +13964), the
display mode is 1280 x 720 A8R8G8B8.
