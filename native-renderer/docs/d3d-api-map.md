# The game's Direct3D layer: API map

From static analysis of the recompiled code plus three traces (`analysis/d3dtrace1.log`: the save menu,
`analysis/d3dtrace2.log`: V-Rex gameplay, 3 frames; `analysis/d3dtrace5.log`: V-Rex, 2 frames, with the
render-state and sampler-state setters hooked too; `KK_DEV_D3D_TRACE`). Names are the XDK Direct3D functions
these behave like; "per frame" is the gameplay count. The device is `*(0x82D62324)` (0x4006A580 in these runs),
20,608 bytes. Offsets are into the device unless said otherwise.

## Draw and frame

| Function | Name | Arguments | Per frame | Notes |
|---|---|---|---|---|
| `sub_82115708` | DrawIndexedVertices | dev, primType, baseVertex, startIndex, indexCount | 641 | primType 4 = triangle list always; from `0x82793BE0` / `0x82794BFC` |
| `sub_821154C8` | DrawVertices | dev, primType, startVertex, vertexCount | 259 | primType 13 (quad list) ×131, 6 (strip) ×105, 4 ×20 |
| `sub_82116178` | Resolve | dev, flags, srcRect, destTexture, destPoint, slice, level, clear values... | 25 | flags 0, 0x100, 0x200, 0x300 (clear RT / DS), 0x14; ~2000 lines; calls the clear path `sub_82114D10` |
| `sub_82115418` | Clear | dev, count, rects, flags, color, z (f1), stencil | 8 | flags 1, 0xF, 0x3F, 0x30; implemented through `sub_82114D10` |
| `sub_821147B8` | Present | dev, ... | 1 | calls Swap `sub_821141D8` (VdSwap) and a resolve of the frontbuffer |
| `sub_82110D90` | SetViewport-or-scissor packet writer | dev, 0, 0, 0, packed 0x000A0340... | 1 | from the present path; emits 8 register packets then `sub_821148F8` |

## State

| Function | Name | Arguments | Per frame | Device field |
|---|---|---|---|---|
| `sub_82118F78` | SetTexture | dev, sampler (0-6 seen), texture | 580 | copies the texture's 6-word fetch constant (texture+16..+36) into the sampler's fetch-constant image at +1152+sampler*24, keeping the sampler-state bits already there (word 0 clamp / sign bits, word 3 filter bits, word 4 LOD bits); texture pointer at +12704+sampler*4 (AddRef, old one released); dirty bit 63-sampler in the qword at +32 |
| `sub_8210BE38` | SetIndices | dev, IB | 648 | +12532 |
| `sub_8210BD38` | SetStreamSource | dev, stream (0-1 seen), VB, offsetInBytes, stride | 377 | VB pointer at +12556+stream*8 (+4 = previous, released), stride/4 as a byte at +12688+stream; dirty bit 10 of the qword at +48. The GPU vertex fetch constants are built at draw time from these plus the vertex declaration |
| `sub_82110C28` | SetPixelShader | dev, ps | 352 | +20456; `sub_821107B0` = pixel shader Release |
| `sub_821108B8` | SetVertexShader | dev, vs | 228 | +12944; `sub_821106F8` = vertex shader Release; reads vs+52.. (bindings) |
| `sub_82111E68` | SetVertexDeclaration | dev, decl | 80 | +11408 |
| `sub_82110300` | SetVertexShaderConstantF | dev, reg, float4*, count | 1019 | shadow at +1920+reg*16 (256 regs to +6016) |
| `sub_82110448` | SetPixelShaderConstantF | dev, reg, float4*, count | 303 | shadow at +6016+reg*16 |
| `sub_82110640` | SetVertexShaderConstantI (loop constants) | dev, reg, int4*, count | 35 | packs x,y,z bytes into +10144+reg*4 |
| `sub_8210C130` | Set blend control (per render target) | dev, rtIndex, packed RB_BLENDCONTROL (e.g. 0x07060706) | 25 | index 0 writes +11640 (RB_BLENDCONTROL0), index 1-3 write +11668+idx*4 (RB_BLENDCONTROL1-3); dirty bits at +16.. |
| `sub_8210C378` | SetRenderTarget | dev, index, surface | 29 | +12536+index*4; calls the viewport apply `sub_8210BAC8` |
| `sub_8210C6E0` | SetDepthStencilSurface | dev, surface | 19 | +12552 |
| `sub_8210BAC8` | SetViewport (probable) | dev, D3DVIEWPORT* (6 dwords; a static 0x8308FE00 or stack) | 50 | viewport at +12808 |
| `sub_8210BD10` | GetViewport | dev, out | 17 | copies 6 dwords from +12808 |
| `sub_8210BEB8` | GetRenderTarget | dev, index, out | 17 | AddRef |
| `sub_8210BF00` | GetDepthStencilSurface | dev, out | 17 | AddRef |
| `sub_8210C9B8` / `sub_8210CA70` | Begin / EndConditionalSurvey | dev, id (0-63), flags / dev | 64 each | occlusion queries: the renderer's "viz query" log lines |
| `sub_8210CB50` / `sub_8210CB70` | Begin / EndConditionalRendering | dev, id / dev | 64 each | nesting stack at +12234, depth at +12299 |
| `sub_82108B70` / `sub_82108BB0` | Acquire / ReleaseThreadOwnership | dev | 2 each | owner thread id at +10376 |
| `sub_82109360` / `sub_821093D0` | Lock / Unlock (resource op 46) | resource, ... | 1 each | per-frame dynamic buffer |

Not seen per frame but called: `sub_8210C060` (a single PS-side float4 at +12848+reg*16, internal), the
texture / surface creation below, and the XDK's other setters among the 152 entry points
(`analysis/d3d_entry_points.json`), to be named as they appear in more scenes (menus with text, videos, Kong
chapters, the pause menu).

## Render states and sampler states (the device's setter table)

The device holds a table of 117 function pointers at +96..+560 and the engine calls
`*(dev + 96 + state)(dev, value)` for SetRenderState and `*(dev + 96 + 388 + type)(dev, sampler, value)` for
SetSamplerState: this is the 2005 XDK's `D3DRENDERSTATETYPE` / `D3DSAMPLERSTATETYPE`, whose values are byte
offsets into that table (render states 0..384 = table index 0..96, with 0..36 unsupported stubs; sampler states
0..76 = table index 97..116). The getters at +604..+1028 mirror them. Verified by reading each setter and
matching the traced values (`analysis/d3dtrace5.log`, two V-Rex frames; `analysis/device_tables.json` has every
entry with the device offsets it writes):

| Index | D3DRS value | Setter | Name | Writes | Traced values |
|---|---|---|---|---|---|
| 10 | 40 | `sub_82109E78` | ZENABLE | RB_DEPTHCONTROL bit 1 (+11636), cache +11972; forced 0 when no depth surface is set | 0, 1 |
| 11 | 44 | `sub_82109F00` | ZFUNC | RB_DEPTHCONTROL zfunc | 3 LESSEQUAL, 7 ALWAYS, 4 GREATER |
| 12 | 48 | `sub_82109EC8` | ZWRITEENABLE | RB_DEPTHCONTROL bit 2 | 0, 1 |
| 13 | 52 | | FILLMODE | PA_SU_SC_MODE_CNTL | not seen |
| 14 | 56 | `sub_82109788` | CULLMODE | PA_SU_SC_MODE_CNTL (+11656) | 0 NONE, 2 CW, 6 CCW |
| 15-23 | 60-92 | | ALPHABLENDENABLE, SEPARATEALPHABLENDENABLE, BLENDFACTOR, SRCBLEND, DESTBLEND, BLENDOP, SRCBLENDALPHA, DESTBLENDALPHA, BLENDOPALPHA | RB_BLENDCONTROL0-3, RB_BLEND_* | the engine sets blending through `sub_8210C130` instead |
| 24 | 96 | `sub_821097F8` | ALPHATESTENABLE | RB_COLORCONTROL (+11644) bit 3 | 0, 1 |
| 25 | 100 | `sub_82109CB0` | ALPHAREF | RB_ALPHA_REF (+11588) = value / 255 as float | 0x79, 0x4C, 0xCA, 0x69, 0x85, 4 |
| 26 | 104 | `sub_82109D20` | ALPHAFUNC | RB_COLORCONTROL bits 0-2 | 4 GREATER, 6 GREATEREQUAL |
| 27 | 108 | `sub_82109F48` | STENCILENABLE | RB_DEPTHCONTROL bit 0, cache +11976 | 0, 1 |
| 28-32 | 112-128 | | TWOSIDEDSTENCILMODE, STENCILFAIL, STENCILZFAIL, STENCILPASS, STENCILFUNC | RB_DEPTHCONTROL | not seen |
| 33 | 132 | `sub_8210A1E0` | STENCILREF | RB_STENCILREFMASK (+11584) low byte | 0xFF, 0 |
| 34-42 | 136-168 | | STENCILMASK, STENCILWRITEMASK, CCW_STENCIL* | RB_STENCILREFMASK, RB_STENCILREFMASK_BF | not seen |
| 43 | 172 | `sub_8210A2D0` | CLIPPLANEENABLE | PA_CL_CLIP_CNTL (+11652) | 0, 1 |
| 53 | 212 | `sub_8210A4E8` | COLORWRITEENABLE | RB_COLOR_MASK (+11548) bits 0-3, cache +11956; forced 0 when no render target 0 | 0xF, 7, 8, 0 |
| 87-90 | 348-360 | `sub_8210AFA0`, `sub_8210AFD0`, `sub_8210B000`, `sub_8210B030` | guard band / discard band | PA_CL_GB_VERT_CLIP_ADJ (+11788) = 2.0, HORZ_CLIP_ADJ (+11796) = 2.0, VERT_DISC_ADJ (+11792) = 1.0, HORZ_DISC_ADJ (+11800) = 1.0 | once per frame, from `0x82797840` |
| 95 | 380 | `sub_8210B130` | (the +476 pointer the engine calls) | | |
| 97 | sampler 0 | `sub_8210B750` | ADDRESSU | fetch word 0 bits 10-12 (clamp_x) | 0 WRAP, 2 CLAMP, 6 |
| 98 | 4 | `sub_8210B7A0` | ADDRESSV | fetch word 0 bits 13-15 | 0, 2, 6 |
| 99 | 8 | `sub_8210B7F0` | ADDRESSW | fetch word 0 bits 16-18 | 0, 2 |
| 100 | 12 | `sub_8210B6E0` | BORDERCOLOR | fetch word 5 bits 0-1 (border colour select: 0 black, 1 white) | 0xFFFFFFFF, 0 |
| 101 | 16 | `sub_8210B270` | MAGFILTER | fetch word 3 bits 19-20 (mag filter) and 25-27 (aniso, from a table keyed by the per-sampler byte at +12020+sampler), word 4 bit 10 (mag aniso walk) | 0 POINT, 1 LINEAR |
| 102 | 20 | `sub_8210B160` | MINFILTER | the min filter fields next to those | 0, 1 |
| 103 | 24 | `sub_8210B378` | MIPFILTER | fetch word 3 bits 23-24 | 0 POINT, 1 LINEAR, 2 NONE |
| 104 | 28 | `sub_8210B540` | MIPMAPLODBIAS | fetch word 4 bits 12-21 = float * 32 (fixed point) | -2.0, -1.0 |
| 105-116 | 32-76 | | MAXMIPLEVEL, MAXANISOTROPY, MAGFILTERZ, MINFILTERZ, SEPARATEZFILTERENABLE, MINMIPLEVEL, ... | fetch words 3-4 | not seen |

Names for the indices that were not traced follow the XDK's 2008 enum order and need confirming when they
appear (the 2005 enum has 97 entries; the 2008 one has 101, so names above index 53 may be shifted by one).

### The register shadow

The setters write packed Xenos register values into images of contiguous register ranges inside the device;
the draw path sends the dirty ones. Verified anchors (register numbers from the SDK's `register_table.inc`):

| Device offset | Registers | Contents |
|---|---|---|
| +1152 .. +1920 | fetch constants | 32 texture fetch constants, 6 words each (SetTexture copies the texture's words in, SetSamplerState patches bits) |
| +1920 .. +6016 | ALU constants 0-255 | vertex shader float constants |
| +6016 .. +9600 | ALU constants 256-479 | pixel shader float constants |
| +10144 | loop / integer constants | SetVertexShaderConstantI packs them |
| +11548 .. +11616 | 0x2104 .. 0x2114 | RB_COLOR_MASK, RB_BLEND_RED..ALPHA, RB_FOG_COLOR_*, RB_STENCILREFMASK_BF (+11580), RB_STENCILREFMASK (+11584), RB_ALPHA_REF (+11588), PA_CL_VPORT_XSCALE..ZOFFSET (+11592 .. +11612) |
| +11636 .. +11684 | 0x2200 .. 0x220B | RB_DEPTHCONTROL, RB_BLENDCONTROL0 (+11640), RB_COLORCONTROL (+11644), RB_HIZCONTROL, PA_CL_CLIP_CNTL (+11652), PA_SU_SC_MODE_CNTL (+11656), PA_CL_VTE_CNTL, VGT_CURRENT_BIN_ID_MIN, RB_MODECONTROL (+11668), RB_BLENDCONTROL1-3 (+11672 .. +11680) |
| +11776 .. | 0x2300 .. | PA_SC_LINE_CNTL, PA_SC_AA_CONFIG, PA_SU_VTX_CNTL, PA_CL_GB_VERT_CLIP_ADJ (+11788), VERT_DISC_ADJ, HORZ_CLIP_ADJ, HORZ_DISC_ADJ |
| +12532 .. +12768 | D3D objects | index buffer (+12532), render targets 0-3 (+12536), depth stencil (+12552), stream sources (+12556, 8 bytes each), strides (+12688), textures (+12704) |
| +12808 | D3DVIEWPORT | 6 dwords (X, Y, Width, Height, MinZ, MaxZ) |
| +16 .. +88 | dirty masks | 64-bit words; the draw path walks them to decide which register ranges to emit |

So a replacement renderer never needs the GPU registers at all: at each draw it has the D3D-level state
(the objects above), the register images for depth / blend / cull / alpha test / viewport / colour mask, and
the fetch constants for textures and samplers. That is exactly the information a D3D11-style pipeline state
needs.

## Resources and objects

| Function | Name | Notes |
|---|---|---|
| `sub_82108BC0` | Direct3D_CreateDevice | allocates the 20,608-byte device (128-aligned), `sub_8211B838` initialises EDRAM / engines / ring buffer |
| `sub_82108C48` | AddRef | refcount = low 8 bits of word 0; type = bits 13-15 (XDK D3DCOMMON: 1 VB, 2 IB, 3 texture, 4 surface) |
| `sub_821091C8` | Release | frees through `sub_82108D60`; a surface (type 4) releases its parent at +44 |
| `sub_82108CB0` | resource type dispatch (format info) | |
| `sub_82126E70` | CreateTexture (public, returns D3DERR_INVALIDCALL 0x8876086C) | inner `sub_82118A68`: 40-byte D3DTexture = 16-byte header (Common, RefCount, Fence, ReadFence) + the 6-word GPU texture fetch constant at +16..+36 (word 1 at +20 holds the base address, word 5 at +36 the mip address); texel memory from the physical allocator (`sub_820F2108`, attributes 0x8C80....) |
| `sub_82118838` | GetSurfaceLevel-like (texture, level) | 64-byte surface object, 12 per frame (render-to-texture setup), released the same frame |
| `sub_82118B88` | CreateRenderTarget / surface | width, height, packed format (0x2DA2ABA4 / 0x28280186 / 0x18280186), ..., params*; 12 per frame |
| `sub_82118E90` | surface attach (texture, surface) | 12 per frame |
| `sub_82111CA0` | CreateVertexShader | container {flags, virtualSize, physicalSize, ...} = XenosRecomp's `ShaderContainer`; object = 52 bytes + virtual part; microcode copied to physical memory (attributes 0xB580....) |
| `sub_82111D90` | CreatePixelShader | object = 592 bytes + virtual part; `sub_8211C090` after the copy, `sub_82110A00` binds |
| `sub_82117048` | format -> block size / bytes lookup | internal |
| `sub_82110E48` | thunk to engine `sub_823EC2F8` | the engine's memcpy, registered as a callback |

Objects' layout matches the XDK's: every resource starts with the 16-byte header (Common with the type in
bits 13-15 and the reference count in the upper bits, RefCount, Fence, ReadFence); a texture's fetch constant
follows at +16; vertex shader bindings at +52. The device's own function pointers are the render-state and
sampler-state setter / getter tables described below (+476 and +944 are entries in them).

## Engine-side flow (per draw)

From the executor at about `0x82862170`-`0x828622E4` (draws issued from `0x82793BE0` / `0x82794BFC`):

```
BeginConditionalRendering(id)
SetIndices(IB)
[XMMatrixMultiply]                      -> SetVertexShaderConstantF(0, world, 4)
SetStreamSource(0, VB, 0)
[SetVertexDeclaration] [SetTexture(s)] [SetPixelShader] [SetVertexShader] [blend control]
DrawIndexedVertices(TRIANGLELIST, 0, 0, n)
EndConditionalRendering()
```

Render-to-texture (shadow maps, the screen-space shadow mask, post effects): GetViewport, GetRenderTarget /
GetDepthStencilSurface, CreateRenderTarget + GetSurfaceLevel, SetRenderTarget / SetDepthStencilSurface,
SetViewport, draw, Resolve to the texture, Release the surfaces, restore. 12 such switches per gameplay frame.

## Not graphics

`sub_821241A8` (XMMatrixMultiply, 4970 per frame), `sub_82123C38`, `sub_821244D8`, `sub_82124040`,
`sub_82124910`, `sub_82123B88`, `sub_82124388`, `sub_82124A20`, `sub_82124BD8`, `sub_82123DC0`: xboxmath VMX
helpers that share the library's address range. The Direct3D library proper ends at about `0x82122xxx`.
