# The game's Direct3D layer: API map

The full reference for the XDK (2005) Direct3D library inside King Kong (`0x82108A58`-`0x82122xxx`, plus D3DX
and xboxmath above it). Machine-readable twin: `native-renderer/d3d_api.json` (every function, render state,
sampler state and register block; the single source of names). Object layouts: `d3d-structs.md`.

Sources: the recompiled code (`kk/generated/default`; `tools/analysis/ppcfunc.py` prints a function's
PowerPC), the device setter / getter tables (`analysis/device_tables.json`), a device dump per traced scene,
and `KK_DEV_D3D_TRACE` traces of every scene below with object hex dumps (`tools/analysis/check_d3d_objects.py`
checks them against `d3d-structs.md`: every check passes on all 33,200 dumps). The device is `*(0x82D62324)`, 20,608 bytes;
offsets are bytes into it.

Conventions: arguments in r3-r10 in order; a float argument goes in f1, f2... and also uses up its r slot
(Clear: Color r7, Z f1, Stencil r9). Bit numbers are LSB = 0. "Native" says what the native renderer does
with the function: **replace** (the original writes GPU packets), **observe** (keep the original and hook
after it: resource creation, lock, unlock, release), **keep** (keep the original: it only writes the device
struct, which the renderer reads at draw time), **ignore** (no visible effect).

## How the native renderer should use this (decision, stream 01)

Every state setter only writes the device struct (register images, object pointers, caches) and sets
pending bits; GPU packets are written only by the draw, clear, resolve and present paths and a handful of
direct writers (SetClipPlane, InsertCallback, SetShaderGPRAllocation, conditional surveys, the window
scissor). So the cheapest correct design is: **keep the original library for all setters** (117 table
entries plus SetTexture, SetStreamSource, shaders, constants...), **replace** DrawIndexedVertices,
DrawVertices, Clear, Resolve, Present / Swap, the survey and callback writers, and at each draw read the state
from the device struct as the GPU would (below). The setters' side effects that matter (shader literal
constants copied into the constant shadow, sampler state merged into fetch constants, blend disabled by
writing ONE / ZERO, depth forced off without a depth surface, viewport clamped to the target) then come for
free. Reimplementing the setters instead is possible (this map has every bit) but must reproduce those side
effects.

### Draw-time state recipe

| What | Where in the device |
|---|---|
| primitive type, counts, base vertex | the draw's arguments; BaseVertexIndex is also VGT_INDX_OFFSET (+11540) |
| index buffer | +12532 (IB+12 address, Common bit 31 = 32-bit indices, big-endian) |
| vertex streams | +12556+8s offset, +12560+8s buffer, +12688+s stride/4; layout from the declaration +11408 |
| shaders | VS +12944, PS +12948 (as bound) |
| float constants | VS c0-c255 at +1920, PS c0-c255 at +6016 (16 bytes each, big-endian floats) |
| bool / loop constants | +10112 (8 dwords), +10144 (32 dwords) |
| textures and samplers | the merged fetch constant at +1152+24n for sampler n; texture object at +12704+4n |
| render targets | +12536..+12548 (surfaces), depth +12552; RB_COLOR_INFO etc. in the destination packet |
| viewport | +12808 (D3D values) or PA_CL_VPORT_* (+11592..+11612); scissor = PA_SC_WINDOW_SCISSOR (+11524, +11528) |
| depth / stencil | RB_DEPTHCONTROL +11636, RB_STENCILREFMASK +11584, _BF +11580 |
| blend | RB_BLENDCONTROL0 +11640, 1-3 +11672..+11680, RB_BLEND_RGBA +11552, RB_COLOR_MASK +11548 |
| alpha test | RB_COLORCONTROL +11644 (func bits 0-2, enable bit 3, alpha to mask bit 4), RB_ALPHA_REF +11588 |
| rasteriser | PA_SU_SC_MODE_CNTL +11656 (cull, fill, poly offset enables, MSAA), PA_SU_POLY_OFFSET_* +11920.., PA_CL_CLIP_CNTL +11652 with planes at +12848 |
| conditional rendering | stack +12234, depth byte +12299 (ignore in the first version) |

## Scenes traced

Per-frame counts (the larger of two consecutive frames; "Functions" = distinct library functions called in the frame). Every function that appeared in any scene (308 in
all, including the Phase 0 traces) is named in `d3d_api.json`; no new library function appeared in the new
scenes.

| Scene | DrawIndexed | DrawVertices | Resolve | Clear | SetTexture | CreateRT | LockRect | D3DX load | Functions |
|---|---|---|---|---|---|---|---|---|---|
| title screen ("Press START") | 1 | 5 | 2 | 2 | 4 | 0 | 0 | 0 | 30 |
| save menu | 104 | 12 | 3 | 1 | 89 | 0 | 0 | 0 | 54 |
| main menu | 104 | 13 | 3 | 1 | 90 | 0 | 0 | 0 | 54 |
| chapter select | 111 | 12 | 3 | 1 | 96 | 0 | 0 | 0 | 54 |
| startup movie (Ubisoft logo) | 0 | 1 | 1 | 2 | 3 | 0 | 3 | 0 | 30 |
| loading screen (Kong to the Rescue) | 2 | 1 | 1 | 3 | 31 | 0 | 3 | 24 | 222 |
| Jack gameplay (V-Rex chapter) | 651 | 264 | 29 | 8 | 1171 | 17 | 0 | 0 | 76 |
| Kong gameplay (Kong to the Rescue, vs V-Rex) | 741 | 159 | 19 | 10 | 1183 | 16 | 0 | 0 | 83 |
| pause menu (over gameplay) | 641 | 261 | 28 | 8 | 1166 | 17 | 0 | 0 | 76 |

What each scene adds:

- Title screen: a handful of 2D draws into the back buffer; the 6-12 SetClipPlane calls per frame happen in
  every scene (plane values from the engine's camera code).
- Menus: about 110 draws per frame over the animated title backdrop; drawn into the back buffer.
- Startup movie: per frame the engine locks three textures (Y 1280x720 and U, V 640x360, format k_8,
  `LockRect` flags 0x2000), copies the decoded frame in, unlocks, sets them on samplers 0-2 and draws one
  quad with `DrawVertices`, then `InsertCallback` (engine `sub_82309FB0`). Native: upload the three planes
  at UnlockRect and convert YUV in the pixel shader the engine already provides.
- Loading screen: D3DX texture loading on the loader thread (`D3DXCreateTexture`, `D3DXLoadSurfaceFromMemory`,
  `D3DXLoadSurfaceFromSurface`, `D3DXFilterTexture`, GetSurfaceLevel / LockRect / UnlockRect on surfaces), and
  the loading-screen draw wrapped in `D3DStateBlock_Capture` / `Apply`.
- Gameplay (Jack and Kong alike): 650-750 indexed draws, 160-260 non-indexed (quad lists, strips), 16-17
  render-to-texture passes (CreateRenderTarget + GetSurfaceLevel + Resolve, released the same frame), 64
  conditional surveys, 8-10 clears. Kong uses the same functions; only the counts differ.
- Pause menu: the gameplay frame plus the menu; no new function.

Objects seen (from the dumps): only 2D textures (formats DXT1 18, DXT4_5 20, DXN 49, k_8 2, 8888 6,
k_32_FLOAT 36, k_24_8 22 as a texture), only 16-bit index buffers, EDRAM targets 1280x720 colour (tile 0) and
depth (tile 720), 832x832 R32F (shadow map), 640x480, 640x360, 320x180, all 1x MSAA; 17 distinct vertex
declarations (usages POSITION 0, BLENDWEIGHT 1, BLENDINDICES 2, NORMAL 3, TEXCOORD 5, TANGENT 6, COLOR 10);
all vertex shader containers start 0x102A0E00 and all pixel shader containers 0x102A0E01 (`design.md` had
these two swapped). The engine passes {0, 0, 0x7FFFFFFF, 0x7FFFFFFF} to SetViewport for "whole target" and
relies on the clamp.

## Functions

### Device

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82108A58` | D3DDevice_AddRef | pDevice (r3) | ULONG | keep |  | Reference count at device+12. |
| `sub_82108B18` | D3DDevice_Release | pDevice (r3) | ULONG | observe |  | Count at device+12; at 1 it tears the device down (sub_8211BA50) and frees it. |
| `sub_82108B70` | D3DDevice_AcquireThreadOwnership | pDevice (r3) | void | keep | 2 | Owner thread id at device+10376. |
| `sub_82108BB0` | D3DDevice_ReleaseThreadOwnership | pDevice (r3) | void | keep | 2 | Clears device+10376. |
| `sub_82108BC0` | Direct3D_CreateDevice | Adapter (r3), DeviceType (r4), hFocusWindow (r5), BehaviorFlags (r6), pPresentationParameters (r7), ppReturnedDeviceInterface (r8) | HRESULT | observe |  | Allocates the 20,608-byte device (128-aligned), sub_8211B838 sets up EDRAM, engines and the ring buffer. The engine stores the device at 0x82D62324 (GDI+4). |
| `sub_8210BF40` | D3DDevice_SetGammaRamp | pDevice (r3), Flags (r4), pRamp (r5) | void | replace |  | Keeps a copy at device+14060 and programs the display gamma (sub_8211C598) when it changed. |
| `sub_8210C8B0` | D3DDevice_GetDeviceCaps | pDevice (r3), pCaps (r4) | void | keep |  | Copies a static 304-byte caps block from 0x820032E0. |
| `sub_8210C918` | Direct3D_CheckDepthStencilMatch | Adapter (r3), DeviceType (r4), AdapterFormat (r5), RenderTargetFormat (r6), DepthStencilFormat (r7) | HRESULT | keep |  | Returns 0, D3DERR_NOTAVAILABLE (0x8876086B) or D3DERR_INVALIDCALL (0x8876086C). |
| `sub_8210C990` | D3DDevice_GetDisplayMode | pDevice (r3), SwapChain (r4), pMode (r5) | void | keep |  | Copies device+13720 (width), +13724 (height), +13768 (refresh), +13728 (format). |
| `sub_8210EA88` | unknown_FillDefaults | pOut (r3) | void | keep |  | Writes {DWORD 3, WORD 0} to pOut; called once from the engine device setup (sub_82797988). |
| `sub_82110D90` | D3DDevice_SetShaderGPRAllocation | pDevice (r3), Flags (r4), VertexShaderCount (r5), PixelShaderCount (r6) | void | ignore | 2 | Writes SQ_GPR_MANAGEMENT (0x0D00) into the ring (0,0 = default 64/64 split, value 0x40400 kept at device+10404). Register-file split only, no effect on the picture. |
| `sub_8211ADE0` | D3DDevice_InsertCallback | pDevice (r3), Type (r4), pCallback (r5), Context (r6) | void | replace |  | Writes CALLBACK_ADDRESS / CALLBACK_CONTEXT (0x057C/0x057D) and an interrupt into the ring: the GPU calls pCallback(Context) when it gets there. Used after the movie upload (engine sub_82309FB0). Native: call it when the frame that contains it has been submitted. |

### Resources (common)

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82108C48` | D3DResource_AddRef | pResource (r3) | ULONG | keep | 51 | Common word bits 0-7 = reference count; a surface (type 4) also AddRefs its parent at +44. |
| `sub_82108CB0` | D3DResource_GetType | pResource (r3) | D3DRESOURCETYPE | keep | 51 | 1 SURFACE, 2 VOLUME, 3 TEXTURE, 4 VOLUMETEXTURE, 5 CUBETEXTURE, 6 VERTEXBUFFER, 7 INDEXBUFFER, 8 ARRAYTEXTURE, from Common bits 16-18 and the fetch constant dimension. |
| `sub_821091C8` | D3DResource_Release | pResource (r3) | ULONG | observe | 85 | At count 1 with no device binding (Common bits 19-23 zero) frees through sub_82108D60: memory release is deferred until the GPU passes the fence at +4. Native: drop the host copy here. |
| `sub_82109248` | D3DResource_GetDevice | pResource (r3), ppDevice (r4) | void | keep |  | Returns the global device (AddRef). |

### Vertex and index buffers

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_821092B0` | D3DDevice_CreateVertexBuffer | Length (r3), Usage (r4) | D3DVertexBuffer* | observe |  | 20-byte object, data in physical memory; see d3d-structs.md. |
| `sub_82109360` | D3DVertexBuffer_Lock | pVB (r3), OffsetToLock (r4), SizeToLock (r5), Flags (r6) | void* | observe | 1 | Generic lock sub_82108EC0 (op 46): waits for the GPU if needed, lock count in Common bits 12-15. |
| `sub_821093D0` | D3DVertexBuffer_Unlock | pVB (r3) | void | observe | 1 | sub_821090E0: lock count down, flushes the CPU cache range. Native: re-upload the buffer (or mark dirty). |
| `sub_821093E0` | D3DDevice_CreateIndexBuffer | Length (r3), Usage (r4), Format (r5) | D3DIndexBuffer* | observe |  | 20-byte object; Common bit 31 = 32-bit indices. |
| `sub_82109490` | D3DIndexBuffer_Lock | pIB (r3), OffsetToLock (r4), SizeToLock (r5), Flags (r6) | void* | observe |  | op 47. |
| `sub_821094F8` | D3DIndexBuffer_Unlock | pIB (r3) | void | observe |  |  |

### Textures

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82116F98` | D3DTexture_UnlockRect | pTexture (r3), Level (r4) | void | observe |  | Flushes the base and mip ranges (sub_821090E0). Native: re-upload the texture. |
| `sub_82118558` | XGSetTextureHeader (inner) | ResourceType (r3), Width (r4), Height (r5), Depth (r6), Levels (r7), Usage (r8), Format (r9), PackedMips (r10) | void | keep |  | Fills the texture object (Common, fences, fetch constant) and returns the base and mip sizes. |
| `sub_82118828` | D3DBaseTexture_GetLevelCount | pTexture (r3) | DWORD | keep |  | mip_max_level (fetch word 4 bits 6-9) + 1. |
| `sub_82118838` | D3DTexture_GetSurfaceLevel | pTexture (r3), Level (r4) | D3DSurface* | observe | 17 | 64-byte surface describing one level (Common bit 25 = belongs to a texture, parent at +44, AddRef on the parent). |
| `sub_821188E0` | D3DTexture_LockRect | pTexture (r3), Level (r4), pLockedRect (r5), pRect (r6), Flags (r7) | void | observe |  | sub_82118420(texture, face 0, level, ...). |
| `sub_821188F8` | D3DTexture_GetLevelDesc | pTexture (r3), Level (r4), pDesc (r5) | void | keep |  | Desc = {Format, Type, Usage 0, Pool 0, MultiSampleType, MultiSampleQuality 0, Width, Height}. |
| `sub_82118900` | D3DCubeTexture_GetCubeMapSurface | pCubeTexture (r3), FaceType (r4), Level (r5) | D3DSurface* | observe |  |  |
| `sub_821189B0` | D3DVolumeTexture_GetLevelDesc | pVolumeTexture (r3), Level (r4), pDesc (r5) | void | keep |  |  |
| `sub_821189B8` | D3DVolumeTexture_GetVolumeLevel | pVolumeTexture (r3), Level (r4) | D3DVolume* | observe |  | 48-byte object. |
| `sub_82118A68` | D3DDevice_CreateTexture (inner) | Width (r3), Height (r4), Depth (r5), Levels (r6), Usage (r7), Format (r8), Pool (r9), ResourceType (r10) | D3DBaseTexture* | observe |  | 40-byte object; header from sub_82118558, base and mip memory from the physical allocator. |
| `sub_82118F10` | D3DVolume_GetDesc | pVolume (r3), pDesc (r4) | void | keep |  |  |
| `sub_82118F58` | D3DVolumeTexture_LockBox | pVolumeTexture (r3), Level (r4), pLockedVolume (r5), pBox (r6), Flags (r7) | void | observe |  | sub_821184B0 returns {RowPitch, SlicePitch, pBits}. |
| `sub_82126E70` | D3DXCreateTexture | pDevice (r3), Width (r4), Height (r5), MipLevels (r6), Usage (r7), Format (r8), Pool (r9), ppTexture (r10) | HRESULT | observe |  | Adjusts the request (sub_82126A40, D3DXCheckTextureRequirements) then CreateTexture sub_82118A68. |

### Surfaces

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82116FC0` | D3DSurface_UnlockRect | pSurface (r3) | void | observe |  | Flushes the parent texture ranges. |
| `sub_82118B88` | D3DDevice_CreateRenderTarget | Width (r3), Height (r4), Format (r5), MultiSample (r6), pParameters (r7) | D3DSurface* | observe | 17 | EDRAM surface: allocates tiles (5120 bytes each, 2048 in all) unless pParameters gives Base. 64-byte object. |
| `sub_82118E90` | D3DSurface_GetDesc | pSurface (r3), pDesc (r4) | void | keep | 17 | Through the parent texture when the surface is a texture level. |
| `sub_82118EE8` | D3DSurface_LockRect | pSurface (r3), pLockedRect (r4), pRect (r5), Flags (r6) | void | observe |  | Locks the parent texture level (+44) through sub_82118420. |

### Shaders

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_821106E0` | D3DShader_AddRef | pShader (r3) | ULONG | keep |  | Count at +4 (shaders and declarations). |
| `sub_821106F8` | D3DVertexShader_Release | pShader (r3) | ULONG | observe | 212 | Count at +4; frees microcode (+12) and object after the GPU fence (+8). |
| `sub_821107B0` | D3DPixelShader_Release | pShader (r3) | ULONG | observe | 712 |  |
| `sub_82110868` | XGGetShaderSizes | pFunction (r3), pOut (r4) | void | keep |  | Out = {pFunction, object size (52 or 592 + virtual size), pMicrocode, microcode size}. |
| `sub_821108B8` | D3DDevice_SetVertexShader | pDevice (r3), pShader (r4) | void | keep | 228 | device+12944. Applies the shader block at shader+52+[shader+64]: ORs a 64-bit mask into the pending word +24, copies (offset, count) records into the shadow at device+1152+offset and applies AND/OR patches. Pending bits 8 and 5 of +48 (shader binding). |
| `sub_82110C28` | D3DDevice_SetPixelShader | pDevice (r3), pShader (r4) | void | keep | 356 | device+20456 (the shader the engine set) and, through sub_82110AD0, device+12948 (the bound one); block at shader+592+[shader+604], mask into pending +16. |
| `sub_82111CA0` | D3DDevice_CreateVertexShader | pFunction (r3) | D3DVertexShader* | observe |  | Object = 52-byte header + copy of the container virtual part (at +52); microcode copied to physical memory (+12). Native: translate here (or at first use). |
| `sub_82111D90` | D3DDevice_CreatePixelShader | pFunction (r3) | D3DPixelShader* | observe |  | Object = 592-byte header + container virtual part (at +592); microcode in physical memory (+40); sub_82110A00 prepares it. |

### Vertex declarations

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82110C90` | D3DDevice_CreateVertexDeclaration | pVertexElements (r3) | D3DVertexDeclaration* | observe |  | Object = 40 bytes + elements; see d3d-structs.md. |
| `sub_82110D50` | D3DVertexDeclaration_Release | pDecl (r3) | ULONG | observe |  |  |
| `sub_82111E68` | D3DDevice_SetVertexDeclaration | pDevice (r3), pDecl (r4) | void | keep | 83 | device+11408; pending bits 7 and 10 of +48. |

### Shader constants

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82110300` | D3DDevice_SetVertexShaderConstantF | pDevice (r3), StartRegister (r4), pConstantData (r5), Vector4fCount (r6) | void | keep | 1040 | Shadow device+1920+16*reg (ALU constants 0-255, register 0x4000); pending +16, one bit per 4 registers (bit 63 = c0-c3). |
| `sub_82110448` | D3DDevice_SetPixelShaderConstantF | pDevice (r3), StartRegister (r4), pConstantData (r5), Vector4fCount (r6) | void | keep | 321 | Shadow device+6016+16*reg (ALU constants 256-511, register 0x4400); pending +24. |
| `sub_82110590` | D3DDevice_SetVertexShaderConstantB | pDevice (r3), StartRegister (r4), pConstantData (r5), BoolCount (r6) | void | keep |  | Bits of the dword at device+10112+4*(reg/32) (bool constants 0x4900); pending bit 31 of +32. |
| `sub_821105E8` | D3DDevice_SetPixelShaderConstantB | pDevice (r3), StartRegister (r4), pConstantData (r5), BoolCount (r6) | void | keep |  | device+10128+4*(reg/32) (pixel bools are 128 after the vertex ones). |
| `sub_82110640` | D3DDevice_SetVertexShaderConstantI | pDevice (r3), StartRegister (r4), pConstantData (r5), Vector4iCount (r6) | void | keep | 35 | Loop constant dword at device+10144+4*reg = x \| y<<8 \| z<<16 (count, start, step); register 0x4908. |
| `sub_82110690` | D3DDevice_SetPixelShaderConstantI | pDevice (r3), StartRegister (r4), pConstantData (r5), Vector4iCount (r6) | void | keep |  | device+10208+4*reg (pixel loop constants 16-31). |

### Other state

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_8210BAC8` | D3DDevice_SetViewport | pDevice (r3), pViewport (r4) | void | keep | 64 | Clamped to render target 0 (or the depth surface) and stored at device+12808; PA_CL_VPORT_XSCALE = W/2, XOFFSET = X+W/2, YSCALE = -H/2, YOFFSET = Y+H/2, ZSCALE = MaxZ-MinZ, ZOFFSET = MinZ (+11592..+11612); window scissor = viewport rect (intersected with the scissor rect when SCISSORTESTENABLE). |
| `sub_8210BD10` | D3DDevice_GetViewport | pDevice (r3), pViewport (r4) | void | keep | 17 | Copies 6 dwords from device+12808. |
| `sub_8210BD38` | D3DDevice_SetStreamSource | pDevice (r3), StreamNumber (r4), pStreamData (r5), OffsetInBytes (r6), Stride (r7) | void | keep | 396 | device+12556+8*stream = offset, +12560+8*stream = VB (binding count Common+0x80000, old one released with the current fence), +12688+stream = stride/4 (byte). Marks pending bit 10 of +48; the draw builds vertex fetch constant 95-stream from it. |
| `sub_8210BDD8` | D3DDevice_GetStreamSource | pDevice (r3), StreamNumber (r4), pOffsetInBytes (r5), pStride (r6) | D3DVertexBuffer* | keep |  | AddRefs the buffer. |
| `sub_8210BE38` | D3DDevice_SetIndices | pDevice (r3), pIndexData (r4) | void | keep | 657 | device+12532. |
| `sub_8210BEB8` | D3DDevice_GetRenderTarget | pDevice (r3), RenderTargetIndex (r4) | D3DSurface* | keep | 17 | AddRef. |
| `sub_8210BF00` | D3DDevice_GetDepthStencilSurface | pDevice (r3) | D3DSurface* | keep | 17 | AddRef. |
| `sub_8210C060` | D3DDevice_SetClipPlane | pDevice (r3), Index (r4), pPlane (r5) | void | replace | 7 | Copy at device+12848+16*index, and PA_CL_UCP_n_X..W (0x2388+4n) written into the ring at once (with WAIT_UNTIL). Enabled by D3DRS_CLIPPLANEENABLE. Native: read the copy at the draw. |
| `sub_8210C130` | D3DDevice_SetBlendState | pDevice (r3), RenderTargetIndex (r4), BlendState (r5) | void | keep | 25 | Index 0 writes RB_BLENDCONTROL0 (+11640), 1-3 RB_BLENDCONTROL1-3 (+11672..+11680). Does not touch the render-state blend cache (+11448/+11452). Typical value 0x07060706 = SRCALPHA, INVSRCALPHA, ADD. |
| `sub_8210C378` | D3DDevice_SetRenderTarget | pDevice (r3), RenderTargetIndex (r4), pRenderTarget (r5) | void | keep | 29 | device+12536+4*index; RB_COLOR_INFO / RB_COLORn_INFO image from surface+52 (HIGHPRECISIONBLEND applied). Index 0 also writes RB_SURFACE_INFO (surface+48), PA_SC_AA_CONFIG, and resets scissor and viewport to the whole surface (sub_8210C2A0). |
| `sub_8210C6E0` | D3DDevice_SetDepthStencilSurface | pDevice (r3), pZStencilSurface (r4) | void | keep | 19 | device+12552; RB_DEPTH_INFO; re-applies ZENABLE / STENCILENABLE (forced off without a surface). |
| `sub_82118F78` | D3DDevice_SetTexture | pDevice (r3), Sampler (r4), pTexture (r5) | void | keep | 1171 | Merges the texture fetch constant (texture+16..+36) with the sampler state already in the shadow (device+1152+24*sampler): word 0 keeps clamp bits 10-21, word 1 the address made physical plus bit 11, word 3 keeps filter bits 19-30, word 4 combines LOD bias / aniso walk / volume filters with mip_min_level = max(texture, MAXMIPLEVEL) and mip_max_level = min(texture, MINMIPLEVEL), word 5 keeps bits 0-8 (border colour, tri clamp, aniso bias). Texture pointer at device+12704+4*sampler (binding count, old one released with the fence). Pending bit 63-sampler of +32. |

### Occlusion queries and conditional rendering

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_8210C9B8` | D3DDevice_BeginConditionalSurvey | pDevice (r3), Index (r4), Flags (r5) | void | replace | 64 | PA_SC_VIZ_QUERY (0x2293) = 1 \| index<<1 \| ...; device+12160 = mask of surveys used. |
| `sub_8210CA70` | D3DDevice_EndConditionalSurvey | pDevice (r3), Flags (r4) | void | replace | 64 |  |
| `sub_8210CB50` | D3DDevice_BeginConditionalRendering | pDevice (r3), Index (r4) | void | keep | 64 | Pushes Index on the byte stack device+12234 (depth byte +12299). The draws inside are predicated on survey Index. First native version: draw everything. |
| `sub_8210CB70` | D3DDevice_EndConditionalRendering | pDevice (r3) | void | keep | 64 | Pops. |

### Draw

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_821154C8` | D3DDevice_DrawVertices | pDevice (r3), PrimitiveType (r4), StartVertex (r5), VertexCount (r6) | void | replace | 264 | Auto-indexed draw. |
| `sub_82115708` | D3DDevice_DrawIndexedVertices | pDevice (r3), PrimitiveType (r4), BaseVertexIndex (r5), StartIndex (r6), IndexCount (r7) | void | replace | 651 | Flushes pending state (sub_82121040), then DRAW_INDX with the index buffer at +12532 (address IB+12 + StartIndex*2 or *4). Splits counts above 65535. |

### Clear and resolve

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_821148F8` | D3DDevice_Resolve (inner, EDRAM clear) |  | void | replace |  | Used by the clear and GPR paths. |
| `sub_82114D10` | D3DDevice_ClearF (inner clear) | pDevice (r3), Flags (r4), pRect (r5), pColor (r6), Z (f1), Stencil (r8) | void | replace | 8 | Draws or resolves the clear through the resolve path. |
| `sub_82115418` | D3DDevice_Clear | pDevice (r3), Count (r4), pRects (r5), Flags (r6), Color (r7), Z (f1), Stencil (r9) | void | replace | 8 | Per rect calls the clear path sub_82114D10 with the colour as float4. |
| `sub_82116178` | D3DDevice_Resolve | pDevice (r3), Flags (r4), pSourceRect (r5), pDestTexture (r6), pDestPoint (r7), DestLevel (r8), DestSliceOrFace (r9), pClearColor (r10), ClearZ (f1), ClearStencil (stack), pParameters (stack) | void | replace | 29 | Copies EDRAM to the texture (resolve), with optional clears. Native: copy the host render target into the host texture. |

### Present

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82114068` | D3DDevice_PresentWait (internal) | pDevice (r3) | void | ignore |  | Waits on the vblank per PRESENTINTERVAL (+13772) before the swap. |
| `sub_821141D8` | D3DDevice_Swap (internal) | pDevice (r3), pFrontBuffer (r4), pSwapParameters (r5) | void | replace |  | Calls VdSwap; hooked by the port for the frame counter. |
| `sub_821147B8` | D3DDevice_Present | pDevice (r3) | void | replace | 1 | Sets render target 0 to the device back buffer (*(device+13964)), resolves it into the front buffer texture (*(device+13960)), restores viewport and scissor, then Swap sub_821141D8 (VdSwap). The engine calls it once per frame (sub_827977E0). |

### State blocks

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82119490` | D3DDevice_CreateStateBlock | pDevice (r3), Type (r4) | D3DStateBlock* | keep |  | 10,148-byte object, magic 'D3sb' (0x44337362), captures at creation (sub_821191A0). |
| `sub_82119508` | D3DStateBlock_Release | pStateBlock (r3) | ULONG | keep |  |  |
| `sub_82119560` | D3DStateBlock_Capture | pStateBlock (r3) | void | keep |  |  |
| `sub_821195A8` | D3DStateBlock_Apply | pStateBlock (r3) | void | keep |  | Restores objects through the setters (viewport, scissor, streams, indices, clip planes, shaders) and copies register images back into the device. Used by the loading-screen thread (engine sub_82725xxx). |

### CPU helpers (keep)

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82110E40` | memcpy thunk |  |  | keep |  | Tail call to the engine memcpy sub_823EC2F8. |
| `sub_82110E48` | memcpy thunk (swapped arguments) |  |  | keep |  | Registered as a callback. |
| `sub_82116F00` | XGGetBaseTextureDimensions | pTexture (r3), pWidth (r4), pHeight (r5), pDepth (r6) | void | keep | 34 | Base size from fetch word 2 (+1 and the border). |
| `sub_82117048` | XGGetBlockDimensions | GpuFormat (r3), pBlockWidth (r4), pBlockHeight (r5) | void | keep | 34 | 4x4 for DXT and other block formats, 2x1 for the packed 4:2:2 formats, else 1x1. |
| `sub_821170E0` | D3DBaseTexture_GetFormat | pTexture (r3) | D3DFORMAT | keep | 17 | Rebuilds the D3DFORMAT from the fetch constant fields. |
| `sub_821171F0` | XGGetTextureLevelSize | pWidth (r3), pHeight (r4), pDepth (r5), BitsPerPixel (r6), GpuFormat (r7), Dimension (r8), Tiled (r9) | DWORD | keep | 34 | Aligns the size to 32x32 tiles (or the 256-byte linear pitch) and returns the 4 KB-aligned size. |
| `sub_82117838` | XGGetMipTailLevelOffset |  | DWORD | keep |  | Offset of a small level inside the packed mip tail. |

### Shader microcode assembler (keep)

`sub_821100C8` XGMicrocodeWalk, `sub_82110F48` ShaderAssembler_SetCallbacks (stores two words at +36/+40), `sub_82111EE8` ShaderAssembler_MarkRegister, `sub_82111EF0` ShaderAssembler_op (sub_82111058), `sub_82111EF8` ShaderAssembler_GetCount (+12), `sub_82111F00` ShaderAssembler_GetEntry, `sub_82111F28` ShaderAssembler_op (sub_82111118), `sub_82111F30` ShaderAssembler_op (sub_82111278), `sub_82111F38` ShaderAssembler_op (sub_821116D8), `sub_821124E8` ShaderAssembler_Create (9,696-byte context), `sub_821125C0` ShaderAssembler_SetConstantI, `sub_82112630` ShaderAssembler_op (sub_82112008), `sub_821130B0` ShaderAssembler_Destroy, `sub_821130B8` ShaderAssembler_Link, `sub_82113A10` ShaderAssembler_Begin (sub_821130C0), `sub_82113A18` ShaderAssembler_op (sub_82113270), `sub_82113A20` ShaderAssembler_op (sub_821132D8), `sub_82113A28` ShaderAssembler_EmitInstruction, `sub_82113A70` ShaderAssembler_op (sub_82113358), `sub_82113CD8` ShaderAssembler_op (sub_82113A78), `sub_82113CE0` ShaderAssembler_op (sub_82113AD0), `sub_82113CE8` ShaderAssembler_op (sub_82113B28), `sub_82113CF0` ShaderAssembler_op (sub_82113BA0), `sub_82113E70` ShaderAssembler_op (sub_82113CF8), `sub_82113E78` ShaderAssembler_EndBlock (sub_82113DE0), `sub_82113E80` ShaderAssembler_End (returns count-1).

### D3DX (keep)

| Function | Name | Arguments | Returns | Native | Per frame | Notes |
|---|---|---|---|---|---|---|
| `sub_82125158` | D3DX format table lookup | key (r3) | entry* | keep |  | 36-byte entries at 0x82004548. |
| `sub_82125258` | D3DX format helper |  |  | keep |  |  |
| `sub_82125318` | D3DX FourCC to D3DFORMAT | FourCC (r3) | D3DFORMAT | keep |  |  |
| `sub_82125348` | D3DXGetImageInfoFromFileInMemory (probable) | pSrcData (r3), SrcDataSize (r4), pSrcInfo (r5) | HRESULT | keep |  |  |
| `sub_82125B70` | D3DXLoadSurfaceFromMemory (probable) |  | HRESULT | keep |  | Large converter (937 lines); writes texture memory on the CPU. |
| `sub_82127058` | D3DXLoadSurfaceFromSurface | pDestSurface (r3), pDestPalette (r4), pDestRect (r5), pSrcSurface (r6), pSrcPalette (r7), pSrcRect (r8), Filter (r9), ColorKey (r10) | HRESULT | keep |  |  |
| `sub_82127840` | D3DX helper (sub_821272A8 with r7 = 0) |  | HRESULT | keep |  |  |
| `sub_82127848` | D3DXFilterTexture (probable) |  | HRESULT | keep |  | Walks the levels (GetLevelCount, GetSurfaceLevel, GetLevelDesc) and fills the smaller ones. |
| `sub_82127C78` | D3DX texture fill / load (internal) |  | HRESULT | keep |  |  |

### xboxmath (keep)

`sub_821159F8` XMConvertToPWLGamma (probable), `sub_82123B08` xboxmath VMX helper, `sub_82123B48` xboxmath VMX helper, `sub_82123B88` xboxmath VMX helper, `sub_82123C38` xboxmath VMX helper, `sub_82123D08` xboxmath VMX helper, `sub_82123DC0` xboxmath VMX helper, `sub_82124040` xboxmath VMX helper, `sub_821240E0` xboxmath VMX helper, `sub_821241A8` XMMatrixMultiply, `sub_82124388` xboxmath VMX helper, `sub_821244D8` xboxmath VMX helper, `sub_82124698` xboxmath VMX helper, `sub_821247F0` xboxmath VMX helper, `sub_82124910` xboxmath VMX helper, `sub_82124A20` xboxmath VMX helper, `sub_82124BD8` xboxmath VMX helper, `sub_82124D18` xboxmath VMX helper, `sub_82124DF8` xboxmath VMX helper, `sub_82124EB0` xboxmath VMX helper, `sub_82124FC0` xboxmath VMX helper, `sub_821250A0` xboxmath VMX helper.

### XAPI, not Direct3D (keep)

`sub_82108040` XAPI GetFileSizeEx (probable), `sub_821080A0` XAPI helper (sub_82107AE0), `sub_82108120` XAPI path function (RtlInitAnsiString), `sub_82108178` XAPI title check (XamGetExecutionId), `sub_821081E0` XAPI helper (sub_820F5C18), `sub_821083E0` XAPI helper (sub_82108920), `sub_821083F0` XAPI time conversion (RtlTimeFieldsToTime), `sub_82108648` XAPI config query (ExGetXConfigSetting).


## Render states and sampler states

The device holds 117 setter pointers at +96 and the matching getters at +564. The engine calls
`*(device + 96 + state)(device, value)` for SetRenderState and `*(device + 96 + 388 + type)(device, sampler,
value)` for SetSamplerState: the 2005 XDK's D3DRENDERSTATETYPE / D3DSAMPLERSTATETYPE values are byte offsets
into the table (render states 0-384 = index 0-96, 0-36 unsupported; sampler states 0-76 = index 97-116).
Every setter was read; the names follow the XDK enums and each one matches the register field it writes.
Compared with later XDKs, the 2005 enum has one extra state at value 228 (index 57, stored and never read),
which shifts every later value by 4. Setters that write a register image also set its pending bit; a few keep
a D3D-level cache that the getter returns. "Traced" lists the values the engine set in the scenes above.

D3D enums are the Xenos encodings: D3DCMPFUNC = `CompareFunction` (NEVER 0 ... ALWAYS 7), D3DBLEND =
`BlendFactor` (ZERO 0, ONE 1, SRCCOLOR 4, INVSRCCOLOR 5, SRCALPHA 6, INVSRCALPHA 7, DESTCOLOR 8, INVDESTCOLOR 9,
DESTALPHA 10, INVDESTALPHA 11, BLENDFACTOR 12, INVBLENDFACTOR 13, CONSTANTALPHA 14, INVCONSTANTALPHA 15,
SRCALPHASAT 16), D3DBLENDOP = `BlendOp` (ADD 0, SUBTRACT 1, MIN 2, MAX 3, REVSUBTRACT 4), D3DSTENCILOP =
`StencilOp`, D3DPRIMITIVETYPE = `PrimitiveType` (TRIANGLELIST 4, TRIANGLESTRIP 6, QUADLIST 13 seen).

### Render states

| Index | Value | Name | Setter / getter | Register (device offset) | Bits | Values | Traced |
|---|---|---|---|---|---|---|---|
| 10 | 40 | ZENABLE | `sub_82109E78` / `sub_82109EC0` | RB_DEPTHCONTROL (+11636) | 1 | BOOL; cache +11972; forced 0 when no depth surface | 0, 0x1 |
| 11 | 44 | ZFUNC | `sub_82109F00` / `sub_82109F38` | RB_DEPTHCONTROL (+11636) | 4-6 | D3DCMPFUNC | 0x3, 0x4, 0x7 |
| 12 | 48 | ZWRITEENABLE | `sub_82109EC8` / `sub_82109EF0` | RB_DEPTHCONTROL (+11636) | 2 | BOOL | 0, 0x1 |
| 13 | 52 | FILLMODE | `sub_821097C0` / `sub_821097E8` | PA_SU_SC_MODE_CNTL (+11656) | 3-10 | D3DFILLMODE (SOLID 0, POINT 0x01, WIREFRAME 0x25); poly_mode and front/back ptype | 0 |
| 14 | 56 | CULLMODE | `sub_82109788` / `sub_821097B0` | PA_SU_SC_MODE_CNTL (+11656) | 0-2 | D3DCULL (NONE 0, CW 2, CCW 6); cull_front, cull_back, face | 0, 0x2, 0x6 |
| 15 | 60 | ALPHABLENDENABLE | `sub_82109840` / `sub_821098D0` | RB_BLENDCONTROL0-3 (+11640) | all | BOOL; cache +11452 bit 31; off writes 0x00010001 (ONE, ZERO, ADD) to all four | 0 |
| 16 | 64 | SEPARATEALPHABLENDENABLE | `sub_82109C10` / `sub_82109CA0` | RB_BLENDCONTROL0-3 (+11640) | all | BOOL; cache +11452 bit 30; off copies the colour fields into the alpha fields | 0 |
| 17 | 68 | BLENDFACTOR | `sub_82109D58` / `sub_82109DF8` | RB_BLEND_RED..ALPHA (+11552) | float x4 | D3DCOLOR ARGB -> R,G,B,A floats /255 | 0xFFFFFFFF |
| 18 | 72 | SRCBLEND | `sub_82109978` / `sub_82109A00` | RB_BLENDCONTROL0-3 (+11640) | 0-4 | D3DBLEND (Xenos BlendFactor); cache +11448; written only while blending is on | 0x1 |
| 19 | 76 | DESTBLEND | `sub_82109A10` / `sub_82109A98` | RB_BLENDCONTROL0-3 (+11640) | 8-12 | D3DBLEND; cache +11448 | 0 |
| 20 | 80 | BLENDOP | `sub_821098E0` / `sub_82109968` | RB_BLENDCONTROL0-3 (+11640) | 5-7 | D3DBLENDOP (Xenos BlendOp); cache +11448 | 0 |
| 21 | 84 | SRCBLENDALPHA | `sub_82109B20` / `sub_82109B88` | RB_BLENDCONTROL0-3 (+11640) | 16-20 | D3DBLEND; cache +11448; only with separate alpha | 0x1 |
| 22 | 88 | DESTBLENDALPHA | `sub_82109B98` / `sub_82109C00` | RB_BLENDCONTROL0-3 (+11640) | 24-28 | D3DBLEND; cache +11448 | 0 |
| 23 | 92 | BLENDOPALPHA | `sub_82109AA8` / `sub_82109B10` | RB_BLENDCONTROL0-3 (+11640) | 21-23 | D3DBLENDOP; cache +11448 | 0 |
| 24 | 96 | ALPHATESTENABLE | `sub_821097F8` / `sub_82109830` | RB_COLORCONTROL (+11644) | 3 | BOOL | 0, 0x1 |
| 25 | 100 | ALPHAREF | `sub_82109CB0` / `sub_82109CF0` | RB_ALPHA_REF (+11588) | float | DWORD 0-255 -> value/255 | 0, 0x1, 0x4, 0xB, 0xC, 0x10, 0x11, 0x15, 0x17, 0x18, 0x19, 0x22 |
| 26 | 104 | ALPHAFUNC | `sub_82109D20` / `sub_82109D48` | RB_COLORCONTROL (+11644) | 0-2 | D3DCMPFUNC | 0x4, 0x6 |
| 27 | 108 | STENCILENABLE | `sub_82109F48` / `sub_82109F90` | RB_DEPTHCONTROL (+11636) | 0 | BOOL; cache +11976; forced 0 when no depth surface | 0, 0x1 |
| 28 | 112 | TWOSIDEDSTENCILMODE | `sub_82109F98` / `sub_82109FD0` | RB_DEPTHCONTROL (+11636) | 7 | BOOL; backface_enable | 0 |
| 29 | 116 | STENCILFAIL | `sub_8210A018` / `sub_8210A050` | RB_DEPTHCONTROL (+11636) | 11-13 | D3DSTENCILOP (Xenos StencilOp) | 0 |
| 30 | 120 | STENCILZFAIL | `sub_8210A060` / `sub_8210A098` | RB_DEPTHCONTROL (+11636) | 17-19 | D3DSTENCILOP | 0 |
| 31 | 124 | STENCILPASS | `sub_8210A0A8` / `sub_8210A0D0` | RB_DEPTHCONTROL (+11636) | 14-16 | D3DSTENCILOP | 0x2 |
| 32 | 128 | STENCILFUNC | `sub_82109FE0` / `sub_8210A008` | RB_DEPTHCONTROL (+11636) | 8-10 | D3DCMPFUNC | 0x7 |
| 33 | 132 | STENCILREF | `sub_8210A1E0` / `sub_8210A200` | RB_STENCILREFMASK (+11584) | 0-7 | BYTE; byte store at +11587 | 0, 0xFF |
| 34 | 136 | STENCILMASK | `sub_8210A208` / `sub_8210A228` | RB_STENCILREFMASK (+11584) | 8-15 | BYTE; byte store at +11586 | 0xFF |
| 35 | 140 | STENCILWRITEMASK | `sub_8210A230` / `sub_8210A250` | RB_STENCILREFMASK (+11584) | 16-23 | BYTE; byte store at +11585 | 0xFF |
| 36 | 144 | CCW_STENCILFAIL | `sub_8210A118` / `sub_8210A150` | RB_DEPTHCONTROL (+11636) | 23-25 | D3DSTENCILOP | 0 |
| 37 | 148 | CCW_STENCILZFAIL | `sub_8210A160` / `sub_8210A198` | RB_DEPTHCONTROL (+11636) | 29-31 | D3DSTENCILOP | 0 |
| 38 | 152 | CCW_STENCILPASS | `sub_8210A1A8` / `sub_8210A1D0` | RB_DEPTHCONTROL (+11636) | 26-28 | D3DSTENCILOP | 0 |
| 39 | 156 | CCW_STENCILFUNC | `sub_8210A0E0` / `sub_8210A108` | RB_DEPTHCONTROL (+11636) | 20-22 | D3DCMPFUNC | 0x7 |
| 40 | 160 | CCW_STENCILREF | `sub_8210A258` / `sub_8210A278` | RB_STENCILREFMASK_BF (+11580) | 0-7 | BYTE; byte store at +11583 |  |
| 41 | 164 | CCW_STENCILMASK | `sub_8210A280` / `sub_8210A2A0` | RB_STENCILREFMASK_BF (+11580) | 8-15 | BYTE; byte store at +11582 |  |
| 42 | 168 | CCW_STENCILWRITEMASK | `sub_8210A2A8` / `sub_8210A2C8` | RB_STENCILREFMASK_BF (+11580) | 16-23 | BYTE; byte store at +11581 |  |
| 43 | 172 | CLIPPLANEENABLE | `sub_8210A2D0` / `sub_8210A300` | PA_CL_CLIP_CNTL (+11652) | 0-5 | mask of planes 0-5; planes from SetClipPlane | 0, 0x1, 0x3F |
| 44 | 176 | POINTSIZE | `sub_8210A618` / `sub_8210A660` | PA_SU_POINT_SIZE (+11684) | 0-15, 16-31 | float x8 (radius in 12.4 fixed) into both halves; cache +11988 | 0 |
| 45 | 180 | POINTSIZE_MIN | `sub_8210A670` / `sub_8210A6B0` | PA_SU_POINT_MINMAX (+11688) | 0-15 | float x16 (12.4 fixed); cache +11992 | 0 |
| 46 | 184 | POINTSPRITEENABLE | `sub_8210A608` / `sub_8210A610` | (cache) (+11980) |  | BOOL; stored only; read by the shader binding | 0 |
| 47 | 188 | POINTSIZE_MAX | `sub_8210A6C0` / `sub_8210A700` | PA_SU_POINT_MINMAX (+11688) | 16-31 | float x16 (12.4 fixed); cache +11996 | 0x3F800000 |
| 48 | 192 | MULTISAMPLEANTIALIAS | `sub_8210A480` / `sub_8210A4A8` | PA_SU_SC_MODE_CNTL (+11656) | 15 | BOOL; msaa_enable | 0x1 |
| 49 | 196 | MULTISAMPLEMASK | `sub_8210A4B8` / `sub_8210A4E0` | PA_SC_AA_MASK (+11840) | 0-15 | mask | 0xFFFF |
| 50 | 200 | SCISSORTESTENABLE | `sub_8210C368` / `sub_8210A310` | (cache) + PA_SC_WINDOW_SCISSOR (+11952) |  | BOOL; recomputes the window scissor = viewport rect & scissor rect (+12832) | 0 |
| 51 | 204 | SLOPESCALEDEPTHBIAS | `sub_8210A318` / `sub_8210A3B8` | PA_SU_POLY_OFFSET_FRONT/BACK_SCALE (+11920) | float | float x16; also +11928; poly_offset_front/back_enable (PA_SU_SC_MODE_CNTL bits 11, 12) = scale or offset non-zero | 0 |
| 52 | 208 | DEPTHBIAS | `sub_8210A3D8` / `sub_8210A470` | PA_SU_POLY_OFFSET_FRONT/BACK_OFFSET (+11924) | float | float; also +11932; same enables | 0 |
| 53 | 212 | COLORWRITEENABLE | `sub_8210A4E8` / `sub_8210A528` | RB_COLOR_MASK (+11548) | 0-3 | mask RGBA (bit 0 R); cache +11956; forced 0 without render target 0 | 0, 0x7, 0x8, 0xF |
| 54 | 216 | COLORWRITEENABLE1 | `sub_8210A530` / `sub_8210A570` | RB_COLOR_MASK (+11548) | 4-7 | mask; cache +11960; needs render target 1 | 0xF |
| 55 | 220 | COLORWRITEENABLE2 | `sub_8210A578` / `sub_8210A5B8` | RB_COLOR_MASK (+11548) | 8-11 | mask; cache +11964 | 0xF |
| 56 | 224 | COLORWRITEENABLE3 | `sub_8210A5C0` / `sub_8210A600` | RB_COLOR_MASK (+11548) | 12-15 | mask; cache +11968 | 0xF |
| 57 | 228 | UNKNOWN_228 | `sub_8210AAA0` / `sub_8210AAB8` | (cache, never read) (+12124) |  | DWORD; value stored at +12124, +12128, +12132, +12136; nothing reads them; no effect. The 2005 enum has one more state here than later XDKs. | 0 |
| 58 | 232 | TESSELLATIONMODE | `sub_8210AE60` / `sub_8210AE80` | VGT_HOS_CNTL (+11704) | 0-1 | D3DTESSELLATIONMODE |  |
| 59 | 236 | MINTESSELLATIONLEVEL | `sub_8210AE00` / `sub_8210AE20` | VGT_HOS_MIN_TESS_LEVEL (+11712) | float | float | 0x3F800000 |
| 60 | 240 | MAXTESSELLATIONLEVEL | `sub_8210AE30` / `sub_8210AE50` | VGT_HOS_MAX_TESS_LEVEL (+11708) | float | float | 0x3F800000 |
| 61 | 244 | WRAP0 | `sub_8210A710` / `sub_8210AA00` | SQ_WRAPPING_0 (+11628) | 0-3 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 62 | 248 | WRAP1 | `sub_8210A740` / `sub_8210AA10` | SQ_WRAPPING_0 (+11628) | 4-7 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 63 | 252 | WRAP2 | `sub_8210A770` / `sub_8210AA20` | SQ_WRAPPING_0 (+11628) | 8-11 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 64 | 256 | WRAP3 | `sub_8210A7A0` / `sub_8210AA30` | SQ_WRAPPING_0 (+11628) | 12-15 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 65 | 260 | WRAP4 | `sub_8210A7D0` / `sub_8210AA40` | SQ_WRAPPING_0 (+11628) | 16-19 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 66 | 264 | WRAP5 | `sub_8210A800` / `sub_8210AA50` | SQ_WRAPPING_0 (+11628) | 20-23 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 67 | 268 | WRAP6 | `sub_8210A830` / `sub_8210AA60` | SQ_WRAPPING_0 (+11628) | 24-27 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 68 | 272 | WRAP7 | `sub_8210A860` / `sub_8210AA70` | SQ_WRAPPING_0 (+11628) | 28-31 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 69 | 276 | WRAP8 | `sub_8210A888` / `sub_8210AA80` | SQ_WRAPPING_1 (+11632) | 0-3 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 70 | 280 | WRAP9 | `sub_8210A8B8` / `sub_8210AA90` | SQ_WRAPPING_1 (+11632) | 4-7 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8) | 0 |
| 71 | 284 | WRAP10 | `sub_8210A8E8` / `sub_82304688` | SQ_WRAPPING_1 (+11632) | 8-11 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8); getters for WRAP10-15 return 0 (stub sub_82304688) | 0 |
| 72 | 288 | WRAP11 | `sub_8210A918` / `sub_82304688` | SQ_WRAPPING_1 (+11632) | 12-15 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8); getters for WRAP10-15 return 0 (stub sub_82304688) | 0 |
| 73 | 292 | WRAP12 | `sub_8210A948` / `sub_82304688` | SQ_WRAPPING_1 (+11632) | 16-19 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8); getters for WRAP10-15 return 0 (stub sub_82304688) | 0 |
| 74 | 296 | WRAP13 | `sub_8210A978` / `sub_82304688` | SQ_WRAPPING_1 (+11632) | 20-23 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8); getters for WRAP10-15 return 0 (stub sub_82304688) | 0 |
| 75 | 300 | WRAP14 | `sub_8210A9A8` / `sub_82304688` | SQ_WRAPPING_1 (+11632) | 24-27 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8); getters for WRAP10-15 return 0 (stub sub_82304688) | 0 |
| 76 | 304 | WRAP15 | `sub_8210A9D8` / `sub_82304688` | SQ_WRAPPING_1 (+11632) | 28-31 | D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8); getters for WRAP10-15 return 0 (stub sub_82304688) | 0 |
| 77 | 308 | VIEWPORTENABLE | `sub_8210AAC0` / `sub_8210AB10` | PA_CL_VTE_CNTL + PA_CL_CLIP_CNTL (+11660) | all | BOOL; on: VTE 0x43F (scale/offset enabled), clip on; off: VTE 0x400, clip_disable (PA_CL_CLIP_CNTL bit 16); cache +12140 | 0, 0x1 |
| 78 | 312 | HIGHPRECISIONBLENDENABLE | `sub_8210AB20` / `sub_8210ABD0` | RB_COLOR_INFO (+11460) | 16-19 | BOOL; switches 2_10_10_10(_FLOAT) render target 0 to its _AS_10_10_10_10 / _AS_16_16_16_16 form; cache +12144; also patches surface+52 | 0 |
| 79 | 316 | HIGHPRECISIONBLENDENABLE1 | `sub_8210ABD8` / `sub_8210AC88` | RB_COLOR1_INFO (+11468) | 16-19 | BOOL; cache +12148 | 0 |
| 80 | 320 | HIGHPRECISIONBLENDENABLE2 | `sub_8210AC90` / `sub_8210AD40` | RB_COLOR2_INFO (+11472) | 16-19 | BOOL; cache +12152 | 0 |
| 81 | 324 | HIGHPRECISIONBLENDENABLE3 | `sub_8210AD48` / `sub_8210ADF8` | RB_COLOR3_INFO (+11476) | 16-19 | BOOL; cache +12156 | 0 |
| 82 | 328 | HALFPIXELOFFSET | `sub_8210AE90` / `sub_8210AEB8` | PA_SU_VTX_CNTL (+11776) | 0 | BOOL; pix_center (0 = D3D9 half-pixel convention off) |  |
| 83 | 332 | PRIMITIVERESETENABLE | `sub_8210AEC8` / `sub_8210AEF0` | PA_SU_SC_MODE_CNTL (+11656) | 21 | BOOL; multi_prim_ib_ena |  |
| 84 | 336 | PRIMITIVERESETINDEX | `sub_8210AF00` / `sub_8210AF20` | VGT_MULTI_PRIM_IB_RESET_INDX (+11544) | all | DWORD |  |
| 85 | 340 | ALPHATOMASKENABLE | `sub_8210AF28` / `sub_8210AF50` | RB_COLORCONTROL (+11644) | 4 | BOOL | 0 |
| 86 | 344 | ALPHATOMASKOFFSETS | `sub_8210AF60` / `sub_8210AF88` | RB_COLORCONTROL (+11644) | 24-31 | 4 x 2-bit offsets | 0x87 |
| 87 | 348 | GUARDBAND_X | `sub_8210AFA0` / `sub_8210AFC8` | PA_CL_GB_HORZ_CLIP_ADJ (+11788) | float | float | 0x40000000 |
| 88 | 352 | GUARDBAND_Y | `sub_8210AFD0` / `sub_8210AFF8` | PA_CL_GB_VERT_CLIP_ADJ (+11780) | float | float | 0x40000000 |
| 89 | 356 | DISCARDBAND_X | `sub_8210B000` / `sub_8210B028` | PA_CL_GB_HORZ_DISC_ADJ (+11792) | float | float | 0x3F800000 |
| 90 | 360 | DISCARDBAND_Y | `sub_8210B030` / `sub_8210B058` | PA_CL_GB_VERT_DISC_ADJ (+11784) | float | float | 0x3F800000 |
| 91 | 364 | HISTENCILENABLE | `sub_8210B060` / `sub_8210B088` | RB_HIZCONTROL (+11648) | 3 | BOOL; hierarchical stencil (an optimisation) | 0 |
| 92 | 368 | HISTENCILWRITEENABLE | `sub_8210B098` / `sub_8210B0C0` | RB_HIZCONTROL (+11648) | 2 | BOOL | 0 |
| 93 | 372 | HISTENCILFUNC | `sub_8210B0D0` / `sub_8210B0F8` | RB_HIZCONTROL (+11648) | 5 | D3DHSCMPFUNC (0 EQUAL, 1 NOTEQUAL) | 0 |
| 94 | 376 | HISTENCILREF | `sub_8210B108` / `sub_8210B128` | RB_HIZCONTROL (+11648) | 8-15 | BYTE; byte store at +11650 | 0 |
| 95 | 380 | PRESENTINTERVAL | `sub_8210B130` / `sub_8210B138` | (cache) (+13772) |  | D3DPRESENT_INTERVAL; the engine calls this setter and its getter through device+476 / +944 |  |
| 96 | 384 | PRESENTIMMEDIATETHRESHOLD | `sub_8210B140` / `sub_8210B150` | (cache) (+11452) | 23-29 | 0-100 |  |

### Sampler states

Fetch word n is the dword at device+1152+24*sampler+4n (and texture+16+4n).

| Index | Value | Name | Setter / getter | Fetch word | Bits | Values | Traced |
|---|---|---|---|---|---|---|---|
| 97 | 0 | ADDRESSU | `sub_8210B750` / `sub_8210B788` | 0 | 10-12 | D3DTEXTUREADDRESS = Xenos ClampMode (WRAP 0, MIRROR 1, CLAMP 2, MIRRORONCE 3, BORDER_HALF 4, MIRRORONCE_BORDER_HALF 5, BORDER 6, MIRRORONCE_BORDER 7); clamp_x | 0, 0x2, 0x6 |
| 98 | 4 | ADDRESSV | `sub_8210B7A0` / `sub_8210B7D8` | 0 | 13-15 | D3DTEXTUREADDRESS; clamp_y | 0, 0x2, 0x6 |
| 99 | 8 | ADDRESSW | `sub_8210B7F0` / `sub_8210B828` | 0 | 16-18 | D3DTEXTUREADDRESS; clamp_z | 0, 0x2 |
| 100 | 12 | BORDERCOLOR | `sub_8210B6E0` / `sub_8210B730` | 5 | 0-1 | D3DCOLOR: 0 -> black (0), anything else -> white (1); border_color | 0, 0xFFFFFFFF |
| 101 | 16 | MAGFILTER | `sub_8210B270` / `sub_8210B2F8` | 3 | 19-20 (+ 25-27, word 4 bit 10) | D3DTEXTUREFILTERTYPE (POINT 0, LINEAR 1, ANISOTROPIC 4); mag_filter; value 4 sets mag_aniso_walk and aniso_filter from MAXANISOTROPY | 0, 0x1 |
| 102 | 20 | MINFILTER | `sub_8210B160` / `sub_8210B1E8` | 3 | 21-22 (+ 25-27, word 4 bit 11) | D3DTEXTUREFILTERTYPE; min_filter; min_aniso_walk | 0, 0x1 |
| 103 | 24 | MIPFILTER | `sub_8210B378` / `sub_8210B3B8` | 3 | 23-24 | POINT 0, LINEAR 1, NONE 2 (base map); mip_filter | 0, 0x1, 0x2 |
| 104 | 28 | MIPMAPLODBIAS | `sub_8210B540` / `sub_8210B5A0` | 4 | 12-21 | float, stored x32 (5 fraction bits); lod_bias | 0, 0x3F000000, 0x3F800000, 0xBF800000, 0xBFC00000, 0xC0000000 |
| 105 | 32 | MAXMIPLEVEL | `sub_8210B5E0` / `sub_8210B650` | 4 | 2-5 | DWORD; mip_min_level = max(texture, value); mip_min_level; cache byte +12046+sampler | 0 |
| 106 | 36 | MAXANISOTROPY | `sub_8210B420` / `sub_8210B488` | 3 | 25-27 | DWORD through table 0x82003298: 0-1 off, 2-3 -> 2:1, 4-6 -> 4:1, 7-12 -> 8:1, 13-16 -> 16:1; aniso_filter (only while an anisotropic filter is set); cache byte +12020+sampler | 0x1 |
| 107 | 40 | MAGFILTERZ | `sub_8210B330` / `sub_8210B368` |  |  | D3DTEXTUREFILTERTYPE; cache byte +12098+sampler bit 0; SetTexture puts it in word 4 bit 0 (vol_mag_filter) when SEPARATEZFILTERENABLE |  |
| 108 | 44 | MINFILTERZ | `sub_8210B220` / `sub_8210B260` |  |  | D3DTEXTUREFILTERTYPE; cache bit 1 -> word 4 bit 1 (vol_min_filter) |  |
| 109 | 48 | SEPARATEZFILTERENABLE | `sub_8210B3D0` / `sub_8210B410` |  |  | BOOL; cache bit 2 |  |
| 110 | 52 | MINMIPLEVEL | `sub_8210B660` / `sub_8210B6D0` | 4 | 6-9 | DWORD; mip_max_level = min(texture, value); mip_max_level; cache byte +12072+sampler |  |
| 111 | 56 | TRILINEARTHRESHOLD | `sub_8210B840` / `sub_8210B880` | 5 | 3-4 | D3DTRILINEARTHRESHOLD; tri_clamp |  |
| 112 | 60 | ANISOTROPYBIAS | `sub_8210B498` / `sub_8210B4F8` | 5 | 5-8 | float x -8; aniso_bias |  |
| 113 | 64 | HGRADIENTEXPBIAS | `sub_8210B898` / `sub_8210B8D8` | 4 | 22-26 | int; grad_exp_adjust_h |  |
| 114 | 68 | VGRADIENTEXPBIAS | `sub_8210B8F0` / `sub_8210B930` | 4 | 27-31 | int; grad_exp_adjust_v |  |
| 115 | 72 | WHITEBORDERCOLORW | `sub_8210B948` / `sub_8210B988` | 5 | 2 | BOOL; force_bc_w_to_max |  |
| 116 | 76 | POINTBORDERENABLE | `sub_8210B9A0` / `sub_8210B9E8` | 1 | 11 | BOOL; field = !value; nearest_clamp_policy |  |

## The register shadow and pending bits

The setters write packed Xenos register values into images of contiguous register ranges; the draw path
(`sub_82121040`, called by both draws) walks five 64-bit pending words (MSB first) and sends each dirty run
with `sub_8211FC68` (constants with `sub_8211FE18` / `sub_8211FD28`). Register numbers from
`C:\rexsrc\include\rex\graphics\register_table.inc`; every block below was checked against the device dumps
(defaults such as VGT_MAX_VTX_INDX 0xFFFFFF, the 1280x720 viewport scales, guard bands 2.0 / 1.0,
SQ_VS_CONST 0xFF000 / SQ_PS_CONST 0xFF100 sit exactly where the map puts them).

| Device offset | Registers | Block | Pending bits |
|---|---|---|---|
| +1152 .. +1919 | 0x4800 .. | fetch constants 0-31 (6 dwords each); vertex fetch constant n is group n/3, dwords 2(n%3), 2(n%3)+1; stream s uses vertex fetch constant 95-s (`sub_8211FF40` builds it at draw time from the stream and the buffer: address = buffer+12 + offset, size = buffer+16 - offset) | +32 bits 63-32 |
| +1920 .. +6015 | 0x4000 .. | ALU constants 0-255 (vertex c0-c255) | +16, bit 63 = c0-c3 |
| +6016 .. +10111 | 0x4400 .. | ALU constants 256-511 (pixel c0-c255) | +24 |
| +10112 .. +10271 | 0x4900 .. | 8 bool dwords, 32 loop dwords | +32 bit 31 |
| +11456 .. +11519 | 0x2000 - 0x200F | destination packet: RB_SURFACE_INFO, RB_COLOR_INFO (+11460), RB_DEPTH_INFO (+11464), RB_COLOR1-3_INFO, COHER_DEST_BASE_0-7, PA_SC_SCREEN_SCISSOR_TL/BR | +32 bits 29-14 |
| +11520 .. +11531 | 0x2080 - 0x2082 | window packet: PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL (+11524), _BR (+11528) | none: written to the ring at once (`sub_82109508`) |
| +11532 .. +11615 | 0x2100 - 0x2114 | values packet: VGT_MAX/MIN_VTX_INDX, VGT_INDX_OFFSET (+11540), VGT_MULTI_PRIM_IB_RESET_INDX (+11544), RB_COLOR_MASK (+11548), RB_BLEND_RED..ALPHA (+11552), RB_FOG_COLOR, RB_STENCILREFMASK_BF (+11580), RB_STENCILREFMASK (+11584), RB_ALPHA_REF (+11588), PA_CL_VPORT_XSCALE..ZOFFSET (+11592..+11612) | +40 bits 63-43 |
| +11616 .. +11635 | 0x2180 - 0x2184 | program packet: SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, SQ_INTERPOLATOR_CNTL, SQ_WRAPPING_0 (+11628), SQ_WRAPPING_1 (+11632) | +40 bits 42-38 |
| +11636 .. +11683 | 0x2200 - 0x220B | control packet: RB_DEPTHCONTROL, RB_BLENDCONTROL0 (+11640), RB_COLORCONTROL (+11644), RB_HIZCONTROL (+11648), PA_CL_CLIP_CNTL (+11652), PA_SU_SC_MODE_CNTL (+11656), PA_CL_VTE_CNTL (+11660), VGT_CURRENT_BIN_ID_MIN, RB_MODECONTROL (+11668), RB_BLENDCONTROL1-3 (+11672..+11680) | +40 bits 37-26 |
| +11684 .. +11767 | 0x2280 - 0x2294 | tessellator packet: PA_SU_POINT_SIZE (+11684), PA_SU_POINT_MINMAX (+11688), ..., VGT_HOS_CNTL (+11704), VGT_HOS_MAX/MIN_TESS_LEVEL (+11708, +11712), ... | +40 bits 25-5 |
| +11768 .. +11919 | 0x2300 - 0x2325 | misc packet: PA_SC_LINE_CNTL, PA_SC_AA_CONFIG (+11772), PA_SU_VTX_CNTL (+11776), PA_CL_GB_VERT_CLIP_ADJ (+11780), VERT_DISC_ADJ (+11784), HORZ_CLIP_ADJ (+11788), HORZ_DISC_ADJ (+11792), SQ_VS_CONST, SQ_PS_CONST, ..., PA_SC_AA_MASK (+11840), ..., RB_COPY_* | +48 bits 63-26 |
| +11920 .. +11951 | 0x2380 - 0x2387 | point packet: PA_SU_POLY_OFFSET_FRONT_SCALE / OFFSET, BACK_SCALE / OFFSET (+11920..+11932), PA_CL_POINT_* | +48 bits 25-18 |

Other pending bits in +48: bit 10 vertex streams or declaration changed (rebuild vertex fetch constants),
bits 8-5 shader binding (`sub_82120A70`: VS and PS linkage, vertex fetch patching for the declaration and
strides, SQ_PROGRAM_CNTL / SQ_CONTEXT_MISC / SQ_INTERPOLATOR_CNTL), bits 17-12 clip planes (already in the
ring). Shaders also carry a block of literal constants that SetVertexShader / SetPixelShader copy into the
constant shadow (see `d3d-structs.md`).

## Engine-side flow (per draw)

From the executor at about `0x82862170`-`0x828622E4` (draws issued from `0x82793BE0` / `0x82794BFC`):

```
BeginConditionalRendering(id)
SetIndices(IB)
[XMMatrixMultiply]                      -> SetVertexShaderConstantF(0, world, 4)
SetStreamSource(0, VB, 0, stride)
[SetVertexDeclaration] [SetTexture(s)] [SetPixelShader] [SetVertexShader] [SetBlendState]
[SetRenderState / SetSamplerState through the device table]
DrawIndexedVertices(TRIANGLELIST, 0, 0, n)
EndConditionalRendering()
```

Render-to-texture (shadow map 832x832 R32F, the screen-space shadow mask, post effects; 16-17 per frame):
GetViewport, GetRenderTarget / GetDepthStencilSurface, CreateRenderTarget + GetSurfaceLevel,
SetRenderTarget / SetDepthStencilSurface, SetViewport, draw, Resolve to the texture, Release the surfaces,
restore. Present: the engine (`sub_827977E0`) calls `D3DDevice_Present`, which makes the device back buffer
render target 0, resolves it into the front buffer texture and swaps.

## Corrections to the Phase 0 map

- Found in the game by stream 04 (Windows, 10 October 2026; details in `backend.md`): the shader functions are
  swapped. `sub_821108B8` is D3DDevice_SetPixelShader and `sub_82110C28` D3DDevice_SetVertexShader (the object
  passed to `sub_82110C28` has the 592-byte header and a vertex container and is never null; the one passed to
  `sub_821108B8` has the 52-byte header and is null in the depth pre-pass), and with the shader stream's finding
  `sub_82111D90` is CreateVertexShader and `sub_82111CA0` CreatePixelShader. The rows below still carry the old
  names. Clear's flags are D3DCLEAR_TARGET0-3 = bits 0-3, ZBUFFER 0x10, STENCIL 0x20 (`sub_82114D10`), and its
  stencil is r9. The vertex buffer's fetch-constant address is a CPU physical-view address (the 0xE0000000 view
  is 4 KB ahead), like the index buffer's.
- SetStreamSource: +12556+8s is the offset and +12560+8s the buffer (not the reverse).
- `sub_8210C060` is SetClipPlane (PA_CL_UCP), not a pixel-shader constant writer.
- `sub_8210C130` is SetBlendState (packed D3DBLENDSTATE = RB_BLENDCONTROL); it bypasses the render-state blend cache.
- The misc packet (0x2300) starts at +11768, not +11776: +11776 is PA_SU_VTX_CNTL (HALFPIXELOFFSET) and the
  guard band registers are VERT_CLIP +11780, VERT_DISC +11784, HORZ_CLIP +11788, HORZ_DISC +11792
  (GUARDBAND_X = +11788, GUARDBAND_Y = +11780, DISCARDBAND_X = +11792, DISCARDBAND_Y = +11784).
- The getter table starts at +564 (index 0-9 are stubs), so a getter is at +564 + state, its setter at +96 + state.
- `sub_82110D90` is SetShaderGPRAllocation (SQ_GPR_MANAGEMENT), not a viewport or scissor writer.
- `sub_82109360` / `sub_821093D0` are the vertex buffer Lock / Unlock; `sub_82109490` / `sub_821094F8` the
  index buffer ones.
- Sampler state names past MIPMAPLODBIAS are confirmed (MAXMIPLEVEL, MAXANISOTROPY, MAGFILTERZ, MINFILTERZ,
  SEPARATEZFILTERENABLE, MINMIPLEVEL, TRILINEARTHRESHOLD, ANISOTROPYBIAS, H/VGRADIENTEXPBIAS,
  WHITEBORDERCOLORW, POINTBORDERENABLE).

## Open points

- Index 57 (value 228) is an unnamed 2005 render state; nothing reads it.
- `sub_8210EA88` (writes {3, 0}) is unnamed; called once at device setup, no device access.
- The D3DX and shader-assembler names are role names, not confirmed XDK names (all CPU-only, keep).
- No cube, volume or array texture, MSAA target or 32-bit index buffer appeared in the traced scenes; their
  layouts are from the code only.
