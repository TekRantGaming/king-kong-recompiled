# Builds native-renderer/d3d_api.json (stream 01) from the curated knowledge below plus the Phase 0 machine
# outputs (device_tables.json, d3d_entry_points.json) and the trace logs (call counts per scene).
# Usage: python -I build_d3d_api.py <analysis dir> <out json>
import glob
import json
import os
import re
import sys

A = sys.argv[1]
OUT = sys.argv[2]
tables = json.load(open(os.path.join(A, 'device_tables.json')))
entry = {e['name']: e for e in json.load(open(os.path.join(A, 'd3d_entry_points.json')))}

# ---------------------------------------------------------------------------------------------------------
# Scenes traced: log file -> scene name. Per-frame counts come from the "frame N end:" histograms.
SCENES = [
    ('d3dtrace1.log', 'save_menu'),
    ('d3dtrace2.log', 'vrex_gameplay'),
    ('d3dtrace4.log', 'title_phase0'),
    ('d3dtrace5.log', 'vrex_gameplay_all_states'),
]
for path in sorted(glob.glob(os.path.join(A, '01', 'scene_*.log'))):
    SCENES.append((os.path.relpath(path, A).replace(os.sep, '/'), os.path.basename(path)[6:-4]))


def per_frame_counts():
    out = {}
    for log, scene in SCENES:
        path = os.path.join(A, log)
        if not os.path.exists(path):
            continue
        frames = []
        for line in open(path, encoding='utf-8', errors='replace'):
            if 'KK d3d: frame' in line and 'end:' in line:
                h = {}
                for m in re.finditer(r'(sub_[0-9A-F]{8})=(\d+)', line.split('end:', 1)[1]):
                    h[m.group(1)] = int(m.group(2))
                frames.append(h)
        if not frames:
            continue
        names = set().union(*frames)
        for n in names:
            out.setdefault(n, {})[scene] = max(f.get(n, 0) for f in frames)
    return out


COUNTS = per_frame_counts()


def traced_anywhere():
    seen = set()
    for log, _ in SCENES:
        path = os.path.join(A, log)
        if not os.path.exists(path):
            continue
        for line in open(path, encoding='utf-8', errors='replace'):
            if 'KK d3d' in line:
                seen.update(re.findall(r'sub_[0-9A-F]{8}', line))
    return seen


TRACED = traced_anywhere()

# ---------------------------------------------------------------------------------------------------------
# Function knowledge. (sub, name, group, renderer, args, returns, notes, status)
# renderer: replace = the native renderer implements it (the original emits GPU packets);
#           observe = keep the original and hook after it to learn about the object or the write;
#           keep    = keep the original, nothing for the renderer to do (state lands in the device struct);
#           ignore  = keep the original or make it a no-op, no visible effect.
# status: verified = read in the code and matched with traced values; code = read in the code only;
#         probable = role clear, exact XDK name a best guess.
D = 'pDevice'
F = []


def fn(sub, name, group, renderer, args, ret, notes, status='code'):
    F.append(dict(sub=sub, name=name, group=group, renderer=renderer, args=args, returns=ret, notes=notes,
                  status=status))


def a(reg, name, meaning=''):
    return {'reg': reg, 'name': name, 'meaning': meaning}


# Device, thread ownership
fn('sub_82108BC0', 'Direct3D_CreateDevice', 'device', 'observe',
   [a('r3', 'Adapter'), a('r4', 'DeviceType'), a('r5', 'hFocusWindow'), a('r6', 'BehaviorFlags'),
    a('r7', 'pPresentationParameters'), a('r8', 'ppReturnedDeviceInterface')], 'HRESULT',
   'Allocates the 20,608-byte device (128-aligned), sub_8211B838 sets up EDRAM, engines and the ring buffer. '
   'The engine stores the device at 0x82D62324 (GDI+4).', 'code')
fn('sub_82108A58', 'D3DDevice_AddRef', 'device', 'keep', [a('r3', D)], 'ULONG', 'Reference count at device+12.')
fn('sub_82108B18', 'D3DDevice_Release', 'device', 'observe', [a('r3', D)], 'ULONG',
   'Count at device+12; at 1 it tears the device down (sub_8211BA50) and frees it.')
fn('sub_82108B70', 'D3DDevice_AcquireThreadOwnership', 'device', 'keep', [a('r3', D)], 'void',
   'Owner thread id at device+10376.', 'verified')
fn('sub_82108BB0', 'D3DDevice_ReleaseThreadOwnership', 'device', 'keep', [a('r3', D)], 'void',
   'Clears device+10376.', 'verified')
fn('sub_8210C8B0', 'D3DDevice_GetDeviceCaps', 'device', 'keep', [a('r3', D), a('r4', 'pCaps', 'D3DCAPS9, 304 bytes')],
   'void', 'Copies a static 304-byte caps block from 0x820032E0.')
fn('sub_8210C918', 'Direct3D_CheckDepthStencilMatch', 'device', 'keep',
   [a('r3', 'Adapter'), a('r4', 'DeviceType', '1 = HAL'), a('r5', 'AdapterFormat'), a('r6', 'RenderTargetFormat'),
    a('r7', 'DepthStencilFormat', 'GPU format 22 or 23 = depth')], 'HRESULT',
   'Returns 0, D3DERR_NOTAVAILABLE (0x8876086B) or D3DERR_INVALIDCALL (0x8876086C).')
fn('sub_8210C990', 'D3DDevice_GetDisplayMode', 'device', 'keep',
   [a('r3', D), a('r4', 'SwapChain'), a('r5', 'pMode', 'D3DDISPLAYMODE {Width, Height, RefreshRate, Format}')],
   'void', 'Copies device+13720 (width), +13724 (height), +13768 (refresh), +13728 (format).')
fn('sub_8210EA88', 'unknown_FillDefaults', 'device', 'keep', [a('r3', 'pOut')], 'void',
   'Writes {DWORD 3, WORD 0} to pOut; called once from the engine device setup (sub_82797988).', 'probable')
fn('sub_82110D90', 'D3DDevice_SetShaderGPRAllocation', 'device', 'ignore',
   [a('r3', D), a('r4', 'Flags'), a('r5', 'VertexShaderCount'), a('r6', 'PixelShaderCount')], 'void',
   'Writes SQ_GPR_MANAGEMENT (0x0D00) into the ring (0,0 = default 64/64 split, value 0x40400 kept at '
   'device+10404). Register-file split only, no effect on the picture.', 'verified')
fn('sub_8210BF40', 'D3DDevice_SetGammaRamp', 'device', 'replace',
   [a('r3', D), a('r4', 'Flags'), a('r5', 'pRamp', 'D3DGAMMARAMP, 3 x 256 WORD = 1536 bytes')], 'void',
   'Keeps a copy at device+14060 and programs the display gamma (sub_8211C598) when it changed.')
fn('sub_8211ADE0', 'D3DDevice_InsertCallback', 'device', 'replace',
   [a('r3', D), a('r4', 'Type'), a('r5', 'pCallback'), a('r6', 'Context')], 'void',
   'Writes CALLBACK_ADDRESS / CALLBACK_CONTEXT (0x057C/0x057D) and an interrupt into the ring: the GPU calls '
   'pCallback(Context) when it gets there. Used after the movie upload (engine sub_82309FB0). Native: call it '
   'when the frame that contains it has been submitted.')
fn('sub_82109248', 'D3DResource_GetDevice', 'resource', 'keep', [a('r3', 'pResource'), a('r4', 'ppDevice')], 'void',
   'Returns the global device (AddRef).')

# Resource common
fn('sub_82108C48', 'D3DResource_AddRef', 'resource', 'keep', [a('r3', 'pResource')], 'ULONG',
   'Common word bits 0-7 = reference count; a surface (type 4) also AddRefs its parent at +44.', 'verified')
fn('sub_821091C8', 'D3DResource_Release', 'resource', 'observe', [a('r3', 'pResource')], 'ULONG',
   'At count 1 with no device binding (Common bits 19-23 zero) frees through sub_82108D60: memory release is '
   'deferred until the GPU passes the fence at +4. Native: drop the host copy here.', 'verified')
fn('sub_82108CB0', 'D3DResource_GetType', 'resource', 'keep', [a('r3', 'pResource')], 'D3DRESOURCETYPE',
   '1 SURFACE, 2 VOLUME, 3 TEXTURE, 4 VOLUMETEXTURE, 5 CUBETEXTURE, 6 VERTEXBUFFER, 7 INDEXBUFFER, '
   '8 ARRAYTEXTURE, from Common bits 16-18 and the fetch constant dimension.')

# Buffers
fn('sub_821092B0', 'D3DDevice_CreateVertexBuffer', 'buffer', 'observe',
   [a('r3', 'Length', 'bytes'), a('r4', 'Usage', 'bit 2 (4) = cached CPU memory')], 'D3DVertexBuffer*',
   '20-byte object, data in physical memory; see d3d-structs.md.', 'verified')
fn('sub_821093E0', 'D3DDevice_CreateIndexBuffer', 'buffer', 'observe',
   [a('r3', 'Length', 'bytes'), a('r4', 'Usage'), a('r5', 'Format', '1 = 32-bit indices, else 16-bit')],
   'D3DIndexBuffer*', '20-byte object; Common bit 31 = 32-bit indices.', 'verified')
fn('sub_82109360', 'D3DVertexBuffer_Lock', 'buffer', 'observe',
   [a('r3', 'pVB'), a('r4', 'OffsetToLock'), a('r5', 'SizeToLock', '0 = all'), a('r6', 'Flags')], 'void*',
   'Generic lock sub_82108EC0 (op 46): waits for the GPU if needed, lock count in Common bits 12-15.', 'verified')
fn('sub_821093D0', 'D3DVertexBuffer_Unlock', 'buffer', 'observe', [a('r3', 'pVB')], 'void',
   'sub_821090E0: lock count down, flushes the CPU cache range. Native: re-upload the buffer (or mark dirty).',
   'verified')
fn('sub_82109490', 'D3DIndexBuffer_Lock', 'buffer', 'observe',
   [a('r3', 'pIB'), a('r4', 'OffsetToLock'), a('r5', 'SizeToLock', '0 = all'), a('r6', 'Flags')], 'void*',
   'op 47.', 'verified')
fn('sub_821094F8', 'D3DIndexBuffer_Unlock', 'buffer', 'observe', [a('r3', 'pIB')], 'void', '', 'verified')
fn('sub_8210BD38', 'D3DDevice_SetStreamSource', 'state', 'keep',
   [a('r3', D), a('r4', 'StreamNumber'), a('r5', 'pStreamData', 'vertex buffer'), a('r6', 'OffsetInBytes'),
    a('r7', 'Stride', 'bytes')], 'void',
   'device+12556+8*stream = offset, +12560+8*stream = VB (binding count Common+0x80000, old one released with '
   'the current fence), +12688+stream = stride/4 (byte). Marks pending bit 10 of +48; the draw builds vertex '
   'fetch constant 95-stream from it.', 'verified')
fn('sub_8210BDD8', 'D3DDevice_GetStreamSource', 'state', 'keep',
   [a('r3', D), a('r4', 'StreamNumber'), a('r5', 'pOffsetInBytes'), a('r6', 'pStride')], 'D3DVertexBuffer*',
   'AddRefs the buffer.')
fn('sub_82118F78', 'D3DDevice_SetTexture', 'state', 'keep',
   [a('r3', D), a('r4', 'Sampler', '0-25'), a('r5', 'pTexture')], 'void',
   'Merges the texture fetch constant (texture+16..+36) with the sampler state already in the shadow '
   '(device+1152+24*sampler): word 0 keeps clamp bits 10-21, word 1 the address made physical plus bit 11, '
   'word 3 keeps filter bits 19-30, word 4 combines LOD bias / aniso walk / volume filters with '
   'mip_min_level = max(texture, MAXMIPLEVEL) and mip_max_level = min(texture, MINMIPLEVEL), word 5 keeps '
   'bits 0-8 (border colour, tri clamp, aniso bias). Texture pointer at device+12704+4*sampler (binding '
   'count, old one released with the fence). Pending bit 63-sampler of +32.', 'verified')
fn('sub_8210BE38', 'D3DDevice_SetIndices', 'state', 'keep', [a('r3', D), a('r4', 'pIndexData')], 'void',
   'device+12532.', 'verified')

# Textures and surfaces
fn('sub_82126E70', 'D3DXCreateTexture', 'texture', 'observe',
   [a('r3', D), a('r4', 'Width'), a('r5', 'Height'), a('r6', 'MipLevels'), a('r7', 'Usage', '-1 = 0'),
    a('r8', 'Format', 'D3DFORMAT'), a('r9', 'Pool'), a('r10', 'ppTexture')], 'HRESULT',
   'Adjusts the request (sub_82126A40, D3DXCheckTextureRequirements) then CreateTexture sub_82118A68.',
   'probable')
fn('sub_82118A68', 'D3DDevice_CreateTexture (inner)', 'texture', 'observe',
   [a('r3', 'Width'), a('r4', 'Height'), a('r5', 'Depth'), a('r6', 'Levels'), a('r7', 'Usage'),
    a('r8', 'Format', 'D3DFORMAT'), a('r9', 'Pool'), a('r10', 'ResourceType', '3 texture, 4 volume, 5 cube, 8 array')],
   'D3DBaseTexture*',
   '40-byte object; header from sub_82118558, base and mip memory from the physical allocator.', 'verified')
fn('sub_82118558', 'XGSetTextureHeader (inner)', 'texture', 'keep',
   [a('r3', 'ResourceType'), a('r4', 'Width'), a('r5', 'Height'), a('r6', 'Depth'), a('r7', 'Levels'),
    a('r8', 'Usage'), a('r9', 'Format'), a('r10', 'PackedMips', '2 = automatic')], 'void',
   'Fills the texture object (Common, fences, fetch constant) and returns the base and mip sizes.')
fn('sub_82118828', 'D3DBaseTexture_GetLevelCount', 'texture', 'keep', [a('r3', 'pTexture')], 'DWORD',
   'mip_max_level (fetch word 4 bits 6-9) + 1.')
fn('sub_82118838', 'D3DTexture_GetSurfaceLevel', 'texture', 'observe', [a('r3', 'pTexture'), a('r4', 'Level')],
   'D3DSurface*', '64-byte surface describing one level (Common bit 25 = belongs to a texture, parent at +44, '
   'AddRef on the parent).', 'verified')
fn('sub_82118900', 'D3DCubeTexture_GetCubeMapSurface', 'texture', 'observe',
   [a('r3', 'pCubeTexture'), a('r4', 'FaceType'), a('r5', 'Level')], 'D3DSurface*', '')
fn('sub_821189B8', 'D3DVolumeTexture_GetVolumeLevel', 'texture', 'observe', [a('r3', 'pVolumeTexture'), a('r4', 'Level')],
   'D3DVolume*', '48-byte object.')
fn('sub_821188F8', 'D3DTexture_GetLevelDesc', 'texture', 'keep',
   [a('r3', 'pTexture'), a('r4', 'Level'), a('r5', 'pDesc', 'D3DSURFACE_DESC')], 'void',
   'Desc = {Format, Type, Usage 0, Pool 0, MultiSampleType, MultiSampleQuality 0, Width, Height}.')
fn('sub_821189B0', 'D3DVolumeTexture_GetLevelDesc', 'texture', 'keep',
   [a('r3', 'pVolumeTexture'), a('r4', 'Level'), a('r5', 'pDesc', 'D3DVOLUME_DESC')], 'void', '')
fn('sub_821188E0', 'D3DTexture_LockRect', 'texture', 'observe',
   [a('r3', 'pTexture'), a('r4', 'Level'), a('r5', 'pLockedRect', '{Pitch, pBits}'), a('r6', 'pRect'),
    a('r7', 'Flags')], 'void', 'sub_82118420(texture, face 0, level, ...).', 'verified')
fn('sub_82116F98', 'D3DTexture_UnlockRect', 'texture', 'observe', [a('r3', 'pTexture'), a('r4', 'Level')], 'void',
   'Flushes the base and mip ranges (sub_821090E0). Native: re-upload the texture.', 'verified')
fn('sub_82118F58', 'D3DVolumeTexture_LockBox', 'texture', 'observe',
   [a('r3', 'pVolumeTexture'), a('r4', 'Level'), a('r5', 'pLockedVolume'), a('r6', 'pBox'), a('r7', 'Flags')], 'void',
   'sub_821184B0 returns {RowPitch, SlicePitch, pBits}.', 'probable')
fn('sub_82118F10', 'D3DVolume_GetDesc', 'texture', 'keep', [a('r3', 'pVolume'), a('r4', 'pDesc')], 'void', '',
   'probable')
fn('sub_82118E90', 'D3DSurface_GetDesc', 'surface', 'keep', [a('r3', 'pSurface'), a('r4', 'pDesc')], 'void',
   'Through the parent texture when the surface is a texture level.', 'code')
fn('sub_82118EE8', 'D3DSurface_LockRect', 'surface', 'observe',
   [a('r3', 'pSurface'), a('r4', 'pLockedRect'), a('r5', 'pRect'), a('r6', 'Flags')], 'void',
   'Locks the parent texture level (+44) through sub_82118420.')
fn('sub_82116FC0', 'D3DSurface_UnlockRect', 'surface', 'observe', [a('r3', 'pSurface')], 'void',
   'Flushes the parent texture ranges.')
fn('sub_82118B88', 'D3DDevice_CreateRenderTarget', 'surface', 'observe',
   [a('r3', 'Width'), a('r4', 'Height'), a('r5', 'Format', 'D3DFORMAT; GPU format 22/23 = depth (CreateDepthStencilSurface uses the same code)'),
    a('r6', 'MultiSample', '0 1x, 1 2x, 2 4x'), a('r7', 'pParameters', 'D3DSURFACE_PARAMETERS {Base, HierarchicalZBase, ColorExpBias} or NULL')],
   'D3DSurface*',
   'EDRAM surface: allocates tiles (5120 bytes each, 2048 in all) unless pParameters gives Base. 64-byte object.',
   'verified')
fn('sub_82118E90', 'D3DSurface_GetDesc', 'surface', 'keep', [a('r3', 'pSurface'), a('r4', 'pDesc')], 'void', '')
fn('sub_82116F00', 'XGGetBaseTextureDimensions', 'cpu_helper', 'keep',
   [a('r3', 'pTexture'), a('r4', 'pWidth'), a('r5', 'pHeight'), a('r6', 'pDepth')], 'void',
   'Base size from fetch word 2 (+1 and the border).', 'probable')
fn('sub_82117048', 'XGGetBlockDimensions', 'cpu_helper', 'keep',
   [a('r3', 'GpuFormat'), a('r4', 'pBlockWidth'), a('r5', 'pBlockHeight')], 'void',
   '4x4 for DXT and other block formats, 2x1 for the packed 4:2:2 formats, else 1x1.', 'probable')
fn('sub_821170E0', 'D3DBaseTexture_GetFormat', 'cpu_helper', 'keep', [a('r3', 'pTexture')], 'D3DFORMAT',
   'Rebuilds the D3DFORMAT from the fetch constant fields.', 'code')
fn('sub_821171F0', 'XGGetTextureLevelSize', 'cpu_helper', 'keep',
   [a('r3', 'pWidth'), a('r4', 'pHeight'), a('r5', 'pDepth'), a('r6', 'BitsPerPixel'), a('r7', 'GpuFormat'),
    a('r8', 'Dimension'), a('r9', 'Tiled')], 'DWORD',
   'Aligns the size to 32x32 tiles (or the 256-byte linear pitch) and returns the 4 KB-aligned size.', 'probable')
fn('sub_82117838', 'XGGetMipTailLevelOffset', 'cpu_helper', 'keep', [], 'DWORD',
   'Offset of a small level inside the packed mip tail.', 'probable')

# Shaders and declarations
fn('sub_82111CA0', 'D3DDevice_CreateVertexShader', 'shader', 'observe',
   [a('r3', 'pFunction', 'shader container {flags, virtualSize, physicalSize, ...}')], 'D3DVertexShader*',
   'Object = 52-byte header + copy of the container virtual part (at +52); microcode copied to physical '
   'memory (+12). Native: translate here (or at first use).', 'verified')
fn('sub_82111D90', 'D3DDevice_CreatePixelShader', 'shader', 'observe',
   [a('r3', 'pFunction', 'shader container')], 'D3DPixelShader*',
   'Object = 592-byte header + container virtual part (at +592); microcode in physical memory (+40); '
   'sub_82110A00 prepares it.', 'verified')
fn('sub_82110868', 'XGGetShaderSizes', 'shader', 'keep', [a('r3', 'pFunction'), a('r4', 'pOut')], 'void',
   'Out = {pFunction, object size (52 or 592 + virtual size), pMicrocode, microcode size}.', 'probable')
fn('sub_821106E0', 'D3DShader_AddRef', 'shader', 'keep', [a('r3', 'pShader')], 'ULONG', 'Count at +4 (shaders and declarations).')
fn('sub_821106F8', 'D3DVertexShader_Release', 'shader', 'observe', [a('r3', 'pShader')], 'ULONG',
   'Count at +4; frees microcode (+12) and object after the GPU fence (+8).', 'verified')
fn('sub_821107B0', 'D3DPixelShader_Release', 'shader', 'observe', [a('r3', 'pShader')], 'ULONG', '', 'verified')
fn('sub_821108B8', 'D3DDevice_SetVertexShader', 'shader', 'keep', [a('r3', D), a('r4', 'pShader')], 'void',
   'device+12944. Applies the shader block at shader+52+[shader+64]: ORs a 64-bit mask into the pending word '
   '+24, copies (offset, count) records into the shadow at device+1152+offset and applies AND/OR patches. '
   'Pending bits 8 and 5 of +48 (shader binding).', 'verified')
fn('sub_82110C28', 'D3DDevice_SetPixelShader', 'shader', 'keep', [a('r3', D), a('r4', 'pShader')], 'void',
   'device+20456 (the shader the engine set) and, through sub_82110AD0, device+12948 (the bound one); block at '
   'shader+592+[shader+604], mask into pending +16.', 'verified')
fn('sub_82110C90', 'D3DDevice_CreateVertexDeclaration', 'declaration', 'observe',
   [a('r3', 'pVertexElements', '12-byte D3DVERTEXELEMENT9, ended by Stream 0xFF')], 'D3DVertexDeclaration*',
   'Object = 40 bytes + elements; see d3d-structs.md.', 'verified')
fn('sub_82110D50', 'D3DVertexDeclaration_Release', 'declaration', 'observe', [a('r3', 'pDecl')], 'ULONG', '')
fn('sub_82111E68', 'D3DDevice_SetVertexDeclaration', 'declaration', 'keep', [a('r3', D), a('r4', 'pDecl')], 'void',
   'device+11408; pending bits 7 and 10 of +48.', 'verified')
fn('sub_821100C8', 'XGMicrocodeWalk', 'shader_assembler', 'keep', [], 'HRESULT',
   'Walks shader microcode instruction by instruction and calls a callback (r10). Startup only.', 'probable')

# Constants
fn('sub_82110300', 'D3DDevice_SetVertexShaderConstantF', 'constants', 'keep',
   [a('r3', D), a('r4', 'StartRegister'), a('r5', 'pConstantData', 'float4 array'), a('r6', 'Vector4fCount')],
   'void', 'Shadow device+1920+16*reg (ALU constants 0-255, register 0x4000); pending +16, one bit per 4 '
   'registers (bit 63 = c0-c3).', 'verified')
fn('sub_82110448', 'D3DDevice_SetPixelShaderConstantF', 'constants', 'keep',
   [a('r3', D), a('r4', 'StartRegister'), a('r5', 'pConstantData'), a('r6', 'Vector4fCount')], 'void',
   'Shadow device+6016+16*reg (ALU constants 256-511, register 0x4400); pending +24.', 'verified')
fn('sub_82110590', 'D3DDevice_SetVertexShaderConstantB', 'constants', 'keep',
   [a('r3', D), a('r4', 'StartRegister'), a('r5', 'pConstantData', 'BOOL array'), a('r6', 'BoolCount')], 'void',
   'Bits of the dword at device+10112+4*(reg/32) (bool constants 0x4900); pending bit 31 of +32.')
fn('sub_821105E8', 'D3DDevice_SetPixelShaderConstantB', 'constants', 'keep',
   [a('r3', D), a('r4', 'StartRegister'), a('r5', 'pConstantData'), a('r6', 'BoolCount')], 'void',
   'device+10128+4*(reg/32) (pixel bools are 128 after the vertex ones).')
fn('sub_82110640', 'D3DDevice_SetVertexShaderConstantI', 'constants', 'keep',
   [a('r3', D), a('r4', 'StartRegister'), a('r5', 'pConstantData', 'int4 array'), a('r6', 'Vector4iCount')], 'void',
   'Loop constant dword at device+10144+4*reg = x | y<<8 | z<<16 (count, start, step); register 0x4908.',
   'verified')
fn('sub_82110690', 'D3DDevice_SetPixelShaderConstantI', 'constants', 'keep',
   [a('r3', D), a('r4', 'StartRegister'), a('r5', 'pConstantData'), a('r6', 'Vector4iCount')], 'void',
   'device+10208+4*reg (pixel loop constants 16-31).')
fn('sub_8210C060', 'D3DDevice_SetClipPlane', 'state', 'replace',
   [a('r3', D), a('r4', 'Index', '0-5'), a('r5', 'pPlane', 'float4')], 'void',
   'Copy at device+12848+16*index, and PA_CL_UCP_n_X..W (0x2388+4n) written into the ring at once (with '
   'WAIT_UNTIL). Enabled by D3DRS_CLIPPLANEENABLE. Native: read the copy at the draw.', 'verified')

# Render targets, viewport, blend
fn('sub_8210C378', 'D3DDevice_SetRenderTarget', 'state', 'keep',
   [a('r3', D), a('r4', 'RenderTargetIndex', '0-3'), a('r5', 'pRenderTarget', 'surface or NULL')], 'void',
   'device+12536+4*index; RB_COLOR_INFO / RB_COLORn_INFO image from surface+52 (HIGHPRECISIONBLEND applied). '
   'Index 0 also writes RB_SURFACE_INFO (surface+48), PA_SC_AA_CONFIG, and resets scissor and viewport to the '
   'whole surface (sub_8210C2A0).', 'verified')
fn('sub_8210C6E0', 'D3DDevice_SetDepthStencilSurface', 'state', 'keep', [a('r3', D), a('r4', 'pZStencilSurface')],
   'void', 'device+12552; RB_DEPTH_INFO; re-applies ZENABLE / STENCILENABLE (forced off without a surface).',
   'verified')
fn('sub_8210BEB8', 'D3DDevice_GetRenderTarget', 'state', 'keep', [a('r3', D), a('r4', 'RenderTargetIndex')],
   'D3DSurface*', 'AddRef.', 'verified')
fn('sub_8210BF00', 'D3DDevice_GetDepthStencilSurface', 'state', 'keep', [a('r3', D)], 'D3DSurface*', 'AddRef.',
   'verified')
fn('sub_8210BAC8', 'D3DDevice_SetViewport', 'state', 'keep',
   [a('r3', D), a('r4', 'pViewport', 'D3DVIEWPORT9 {X, Y, Width, Height, MinZ, MaxZ}')], 'void',
   'Clamped to render target 0 (or the depth surface) and stored at device+12808; PA_CL_VPORT_XSCALE = W/2, '
   'XOFFSET = X+W/2, YSCALE = -H/2, YOFFSET = Y+H/2, ZSCALE = MaxZ-MinZ, ZOFFSET = MinZ (+11592..+11612); '
   'window scissor = viewport rect (intersected with the scissor rect when SCISSORTESTENABLE).', 'verified')
fn('sub_8210BD10', 'D3DDevice_GetViewport', 'state', 'keep', [a('r3', D), a('r4', 'pViewport')], 'void',
   'Copies 6 dwords from device+12808.', 'verified')
fn('sub_8210C130', 'D3DDevice_SetBlendState', 'state', 'keep',
   [a('r3', D), a('r4', 'RenderTargetIndex', '0-3'), a('r5', 'BlendState', 'packed RB_BLENDCONTROL')], 'void',
   'Index 0 writes RB_BLENDCONTROL0 (+11640), 1-3 RB_BLENDCONTROL1-3 (+11672..+11680). Does not touch the '
   'render-state blend cache (+11448/+11452). Typical value 0x07060706 = SRCALPHA, INVSRCALPHA, ADD.',
   'verified')

# Queries
fn('sub_8210C9B8', 'D3DDevice_BeginConditionalSurvey', 'query', 'replace',
   [a('r3', D), a('r4', 'Index', '0-63'), a('r5', 'Flags')], 'void',
   'PA_SC_VIZ_QUERY (0x2293) = 1 | index<<1 | ...; device+12160 = mask of surveys used.', 'verified')
fn('sub_8210CA70', 'D3DDevice_EndConditionalSurvey', 'query', 'replace', [a('r3', D), a('r4', 'Flags')], 'void', '',
   'verified')
fn('sub_8210CB50', 'D3DDevice_BeginConditionalRendering', 'query', 'keep', [a('r3', D), a('r4', 'Index')], 'void',
   'Pushes Index on the byte stack device+12234 (depth byte +12299). The draws inside are predicated on '
   'survey Index. First native version: draw everything.', 'verified')
fn('sub_8210CB70', 'D3DDevice_EndConditionalRendering', 'query', 'keep', [a('r3', D)], 'void', 'Pops.', 'verified')

# Draw, clear, resolve, present
fn('sub_82115708', 'D3DDevice_DrawIndexedVertices', 'draw', 'replace',
   [a('r3', D), a('r4', 'PrimitiveType', 'D3DPRIMITIVETYPE = Xenos prim type'),
    a('r5', 'BaseVertexIndex', '-> VGT_INDX_OFFSET (+11540)'), a('r6', 'StartIndex'), a('r7', 'IndexCount')],
   'void', 'Flushes pending state (sub_82121040), then DRAW_INDX with the index buffer at +12532 '
   '(address IB+12 + StartIndex*2 or *4). Splits counts above 65535.', 'verified')
fn('sub_821154C8', 'D3DDevice_DrawVertices', 'draw', 'replace',
   [a('r3', D), a('r4', 'PrimitiveType'), a('r5', 'StartVertex', '-> VGT_INDX_OFFSET'), a('r6', 'VertexCount')],
   'void', 'Auto-indexed draw.', 'verified')
fn('sub_82115418', 'D3DDevice_Clear', 'clear_resolve', 'replace',
   [a('r3', D), a('r4', 'Count'), a('r5', 'pRects', 'D3DRECT x1,y1,x2,y2 (16 bytes) or NULL'),
    a('r6', 'Flags', 'D3DCLEAR: 0xF targets 0-3 (bit per target), 0x10 Z, 0x20 stencil'),
    a('r7', 'Color', 'D3DCOLOR ARGB'), a('f1', 'Z', 'float; uses the r8 slot'), a('r9', 'Stencil')], 'void',
   'Per rect calls the clear path sub_82114D10 with the colour as float4.', 'verified')
fn('sub_82114D10', 'D3DDevice_ClearF (inner clear)', 'clear_resolve', 'replace',
   [a('r3', D), a('r4', 'Flags'), a('r5', 'pRect'), a('r6', 'pColor', 'float4'), a('f1', 'Z'), a('r8', 'Stencil')],
   'void', 'Draws or resolves the clear through the resolve path.', 'code')
fn('sub_82116178', 'D3DDevice_Resolve', 'clear_resolve', 'replace',
   [a('r3', D), a('r4', 'Flags', 'bits 0-2 source (0-3 target, 4 depth); 0x10-0x70 fragment select; '
                                    '0x100 clear target, 0x200 clear depth/stencil'),
    a('r5', 'pSourceRect', 'D3DRECT or NULL = whole'), a('r6', 'pDestTexture'),
    a('r7', 'pDestPoint', 'D3DPOINT or NULL'), a('r8', 'DestLevel'), a('r9', 'DestSliceOrFace'),
    a('r10', 'pClearColor', 'float4 or NULL'), a('f1', 'ClearZ'), a('stack', 'ClearStencil'),
    a('stack', 'pParameters')], 'void',
   'Copies EDRAM to the texture (resolve), with optional clears. Native: copy the host render target into the '
   'host texture.', 'verified')
fn('sub_821148F8', 'D3DDevice_Resolve (inner, EDRAM clear)', 'clear_resolve', 'replace', [], 'void',
   'Used by the clear and GPR paths.', 'code')
fn('sub_821147B8', 'D3DDevice_Present', 'present', 'replace', [a('r3', D)], 'void',
   'Sets render target 0 to the device back buffer (*(device+13964)), resolves it into the front buffer '
   'texture (*(device+13960)), restores viewport and scissor, then Swap sub_821141D8 (VdSwap). The engine '
   'calls it once per frame (sub_827977E0).', 'verified')
fn('sub_82114068', 'D3DDevice_PresentWait (internal)', 'present', 'ignore', [a('r3', D)], 'void',
   'Waits on the vblank per PRESENTINTERVAL (+13772) before the swap.', 'code')
fn('sub_821141D8', 'D3DDevice_Swap (internal)', 'present', 'replace', [a('r3', D), a('r4', 'pFrontBuffer'), a('r5', 'pSwapParameters')],
   'void', 'Calls VdSwap; hooked by the port for the frame counter.', 'verified')

# State blocks
fn('sub_82119490', 'D3DDevice_CreateStateBlock', 'stateblock', 'keep', [a('r3', D), a('r4', 'Type')],
   'D3DStateBlock*', "10,148-byte object, magic 'D3sb' (0x44337362), captures at creation (sub_821191A0).")
fn('sub_82119508', 'D3DStateBlock_Release', 'stateblock', 'keep', [a('r3', 'pStateBlock')], 'ULONG', '')
fn('sub_82119560', 'D3DStateBlock_Capture', 'stateblock', 'keep', [a('r3', 'pStateBlock')], 'void', '')
fn('sub_821195A8', 'D3DStateBlock_Apply', 'stateblock', 'keep', [a('r3', 'pStateBlock')], 'void',
   'Restores objects through the setters (viewport, scissor, streams, indices, clip planes, shaders) and '
   'copies register images back into the device. Used by the loading-screen thread (engine sub_82725xxx).')

# Shader microcode assembler (runtime shader building, CPU only)
for sub, nm in [('sub_821124E8', 'ShaderAssembler_Create (9,696-byte context)'),
                ('sub_821130B0', 'ShaderAssembler_Destroy'), ('sub_821130B8', 'ShaderAssembler_Link'),
                ('sub_82110F48', 'ShaderAssembler_SetCallbacks (stores two words at +36/+40)'),
                ('sub_82111EE8', 'ShaderAssembler_MarkRegister'), ('sub_82111EF0', 'ShaderAssembler_op (sub_82111058)'),
                ('sub_82111EF8', 'ShaderAssembler_GetCount (+12)'), ('sub_82111F00', 'ShaderAssembler_GetEntry'),
                ('sub_82111F28', 'ShaderAssembler_op (sub_82111118)'), ('sub_82111F30', 'ShaderAssembler_op (sub_82111278)'),
                ('sub_82111F38', 'ShaderAssembler_op (sub_821116D8)'), ('sub_821125C0', 'ShaderAssembler_SetConstantI'),
                ('sub_82112630', 'ShaderAssembler_op (sub_82112008)'), ('sub_82113A10', 'ShaderAssembler_Begin (sub_821130C0)'),
                ('sub_82113A18', 'ShaderAssembler_op (sub_82113270)'), ('sub_82113A20', 'ShaderAssembler_op (sub_821132D8)'),
                ('sub_82113A28', 'ShaderAssembler_EmitInstruction'), ('sub_82113A70', 'ShaderAssembler_op (sub_82113358)'),
                ('sub_82113CD8', 'ShaderAssembler_op (sub_82113A78)'), ('sub_82113CE0', 'ShaderAssembler_op (sub_82113AD0)'),
                ('sub_82113CE8', 'ShaderAssembler_op (sub_82113B28)'), ('sub_82113CF0', 'ShaderAssembler_op (sub_82113BA0)'),
                ('sub_82113E70', 'ShaderAssembler_op (sub_82113CF8)'), ('sub_82113E78', 'ShaderAssembler_EndBlock (sub_82113DE0)'),
                ('sub_82113E80', 'ShaderAssembler_End (returns count-1)')]:
    fn(sub, nm, 'shader_assembler', 'keep', [], '',
       'Part of the XDK shader microcode assembler (0x82110F48-0x82113F98) the engine uses at startup '
       '(sub_82228058, sub_82181xxx, sub_82211xxx) to build shaders, which then go through '
       'CreateVertexShader / CreatePixelShader. No device access.', 'probable')

# Misc library
fn('sub_82110E40', 'memcpy thunk', 'cpu_helper', 'keep', [], '', 'Tail call to the engine memcpy sub_823EC2F8.')
fn('sub_82110E48', 'memcpy thunk (swapped arguments)', 'cpu_helper', 'keep', [], '', 'Registered as a callback.')
fn('sub_821159F8', 'XMConvertToPWLGamma (probable)', 'xboxmath', 'keep', [a('v1', 'value')], 'v1',
   'VMX piecewise-linear conversion of a float4 with constant tables at 0x820036D0..; CPU only.', 'probable')

# xboxmath
for sub in ['sub_82123B08', 'sub_82123B48', 'sub_82123B88', 'sub_82123C38', 'sub_82123D08', 'sub_82123DC0',
            'sub_82124040', 'sub_821240E0', 'sub_82124388', 'sub_821244D8', 'sub_82124698', 'sub_821247F0',
            'sub_82124910', 'sub_82124A20', 'sub_82124BD8', 'sub_82124D18', 'sub_82124DF8', 'sub_82124EB0',
            'sub_82124FC0', 'sub_821250A0']:
    fn(sub, 'xboxmath VMX helper', 'xboxmath', 'keep', [], '', 'CPU math, no graphics state.', 'probable')
fn('sub_821241A8', 'XMMatrixMultiply', 'xboxmath', 'keep', [a('r3', 'pOut'), a('r4', 'pM1'), a('r5', 'pM2')],
   'void', 'About 5,000 calls per gameplay frame.', 'probable')

# D3DX (CPU texture loading)
fn('sub_82125B70', 'D3DXLoadSurfaceFromMemory (probable)', 'd3dx', 'keep', [], 'HRESULT',
   'Large converter (937 lines); writes texture memory on the CPU.', 'probable')
fn('sub_82127058', 'D3DXLoadSurfaceFromSurface', 'd3dx', 'keep',
   [a('r3', 'pDestSurface'), a('r4', 'pDestPalette'), a('r5', 'pDestRect'), a('r6', 'pSrcSurface'),
    a('r7', 'pSrcPalette'), a('r8', 'pSrcRect'), a('r9', 'Filter'), a('r10', 'ColorKey')], 'HRESULT', '', 'probable')
fn('sub_82127848', 'D3DXFilterTexture (probable)', 'd3dx', 'keep', [], 'HRESULT',
   'Walks the levels (GetLevelCount, GetSurfaceLevel, GetLevelDesc) and fills the smaller ones.', 'probable')
fn('sub_82127C78', 'D3DX texture fill / load (internal)', 'd3dx', 'keep', [], 'HRESULT', '', 'probable')
fn('sub_82127840', 'D3DX helper (sub_821272A8 with r7 = 0)', 'd3dx', 'keep', [], 'HRESULT', '', 'probable')
fn('sub_82125158', 'D3DX format table lookup', 'd3dx', 'keep', [a('r3', 'key')], 'entry*',
   '36-byte entries at 0x82004548.', 'probable')
fn('sub_82125258', 'D3DX format helper', 'd3dx', 'keep', [], '', '', 'probable')
fn('sub_82125318', 'D3DX FourCC to D3DFORMAT', 'd3dx', 'keep', [a('r3', 'FourCC')], 'D3DFORMAT', '', 'probable')
fn('sub_82125348', 'D3DXGetImageInfoFromFileInMemory (probable)', 'd3dx', 'keep',
   [a('r3', 'pSrcData'), a('r4', 'SrcDataSize'), a('r5', 'pSrcInfo')], 'HRESULT', '', 'probable')

# XAPI below the Direct3D library
for sub, nm, note in [
        ('sub_82108040', 'XAPI GetFileSizeEx (probable)', 'NtQueryInformationFile, returns a 64-bit size'),
        ('sub_821080A0', 'XAPI helper (sub_82107AE0)', ''),
        ('sub_82108120', 'XAPI path function (RtlInitAnsiString)', ''),
        ('sub_82108178', 'XAPI title check (XamGetExecutionId)', 'Returns 0 or ERROR_FUNCTION_FAILED (1627)'),
        ('sub_821081E0', 'XAPI helper (sub_820F5C18)', ''),
        ('sub_821083E0', 'XAPI helper (sub_82108920)', ''),
        ('sub_821083F0', 'XAPI time conversion (RtlTimeFieldsToTime)', ''),
        ('sub_82108648', 'XAPI config query (ExGetXConfigSetting)', '')]:
    fn(sub, nm, 'xapi', 'keep', [], '', 'Not Direct3D; in the hooked list because it sits just below the library. ' + note,
       'probable')

# ---------------------------------------------------------------------------------------------------------
# Render states: index -> (name, register, device offset, bits, encoding, extra)
RS = {
    10: ('D3DRS_ZENABLE', 'RB_DEPTHCONTROL', 11636, '1', 'BOOL', 'cache +11972; forced 0 when no depth surface'),
    11: ('D3DRS_ZFUNC', 'RB_DEPTHCONTROL', 11636, '4-6', 'D3DCMPFUNC', ''),
    12: ('D3DRS_ZWRITEENABLE', 'RB_DEPTHCONTROL', 11636, '2', 'BOOL', ''),
    13: ('D3DRS_FILLMODE', 'PA_SU_SC_MODE_CNTL', 11656, '3-10', 'D3DFILLMODE (SOLID 0, POINT 0x01, WIREFRAME 0x25)', 'poly_mode and front/back ptype'),
    14: ('D3DRS_CULLMODE', 'PA_SU_SC_MODE_CNTL', 11656, '0-2', 'D3DCULL (NONE 0, CW 2, CCW 6)', 'cull_front, cull_back, face'),
    15: ('D3DRS_ALPHABLENDENABLE', 'RB_BLENDCONTROL0-3', 11640, 'all', 'BOOL', 'cache +11452 bit 31; off writes 0x00010001 (ONE, ZERO, ADD) to all four'),
    16: ('D3DRS_SEPARATEALPHABLENDENABLE', 'RB_BLENDCONTROL0-3', 11640, 'all', 'BOOL', 'cache +11452 bit 30; off copies the colour fields into the alpha fields'),
    17: ('D3DRS_BLENDFACTOR', 'RB_BLEND_RED..ALPHA', 11552, 'float x4', 'D3DCOLOR ARGB -> R,G,B,A floats /255', ''),
    18: ('D3DRS_SRCBLEND', 'RB_BLENDCONTROL0-3', 11640, '0-4', 'D3DBLEND (Xenos BlendFactor)', 'cache +11448; written only while blending is on'),
    19: ('D3DRS_DESTBLEND', 'RB_BLENDCONTROL0-3', 11640, '8-12', 'D3DBLEND', 'cache +11448'),
    20: ('D3DRS_BLENDOP', 'RB_BLENDCONTROL0-3', 11640, '5-7', 'D3DBLENDOP (Xenos BlendOp)', 'cache +11448'),
    21: ('D3DRS_SRCBLENDALPHA', 'RB_BLENDCONTROL0-3', 11640, '16-20', 'D3DBLEND', 'cache +11448; only with separate alpha'),
    22: ('D3DRS_DESTBLENDALPHA', 'RB_BLENDCONTROL0-3', 11640, '24-28', 'D3DBLEND', 'cache +11448'),
    23: ('D3DRS_BLENDOPALPHA', 'RB_BLENDCONTROL0-3', 11640, '21-23', 'D3DBLENDOP', 'cache +11448'),
    24: ('D3DRS_ALPHATESTENABLE', 'RB_COLORCONTROL', 11644, '3', 'BOOL', ''),
    25: ('D3DRS_ALPHAREF', 'RB_ALPHA_REF', 11588, 'float', 'DWORD 0-255 -> value/255', ''),
    26: ('D3DRS_ALPHAFUNC', 'RB_COLORCONTROL', 11644, '0-2', 'D3DCMPFUNC', ''),
    27: ('D3DRS_STENCILENABLE', 'RB_DEPTHCONTROL', 11636, '0', 'BOOL', 'cache +11976; forced 0 when no depth surface'),
    28: ('D3DRS_TWOSIDEDSTENCILMODE', 'RB_DEPTHCONTROL', 11636, '7', 'BOOL', 'backface_enable'),
    29: ('D3DRS_STENCILFAIL', 'RB_DEPTHCONTROL', 11636, '11-13', 'D3DSTENCILOP (Xenos StencilOp)', ''),
    30: ('D3DRS_STENCILZFAIL', 'RB_DEPTHCONTROL', 11636, '17-19', 'D3DSTENCILOP', ''),
    31: ('D3DRS_STENCILPASS', 'RB_DEPTHCONTROL', 11636, '14-16', 'D3DSTENCILOP', ''),
    32: ('D3DRS_STENCILFUNC', 'RB_DEPTHCONTROL', 11636, '8-10', 'D3DCMPFUNC', ''),
    33: ('D3DRS_STENCILREF', 'RB_STENCILREFMASK', 11584, '0-7', 'BYTE', 'byte store at +11587'),
    34: ('D3DRS_STENCILMASK', 'RB_STENCILREFMASK', 11584, '8-15', 'BYTE', 'byte store at +11586'),
    35: ('D3DRS_STENCILWRITEMASK', 'RB_STENCILREFMASK', 11584, '16-23', 'BYTE', 'byte store at +11585'),
    36: ('D3DRS_CCW_STENCILFAIL', 'RB_DEPTHCONTROL', 11636, '23-25', 'D3DSTENCILOP', ''),
    37: ('D3DRS_CCW_STENCILZFAIL', 'RB_DEPTHCONTROL', 11636, '29-31', 'D3DSTENCILOP', ''),
    38: ('D3DRS_CCW_STENCILPASS', 'RB_DEPTHCONTROL', 11636, '26-28', 'D3DSTENCILOP', ''),
    39: ('D3DRS_CCW_STENCILFUNC', 'RB_DEPTHCONTROL', 11636, '20-22', 'D3DCMPFUNC', ''),
    40: ('D3DRS_CCW_STENCILREF', 'RB_STENCILREFMASK_BF', 11580, '0-7', 'BYTE', 'byte store at +11583'),
    41: ('D3DRS_CCW_STENCILMASK', 'RB_STENCILREFMASK_BF', 11580, '8-15', 'BYTE', 'byte store at +11582'),
    42: ('D3DRS_CCW_STENCILWRITEMASK', 'RB_STENCILREFMASK_BF', 11580, '16-23', 'BYTE', 'byte store at +11581'),
    43: ('D3DRS_CLIPPLANEENABLE', 'PA_CL_CLIP_CNTL', 11652, '0-5', 'mask of planes 0-5', 'planes from SetClipPlane'),
    44: ('D3DRS_POINTSIZE', 'PA_SU_POINT_SIZE', 11684, '0-15, 16-31', 'float x8 (radius in 12.4 fixed) into both halves', 'cache +11988'),
    45: ('D3DRS_POINTSIZE_MIN', 'PA_SU_POINT_MINMAX', 11688, '0-15', 'float x16 (12.4 fixed)', 'cache +11992'),
    46: ('D3DRS_POINTSPRITEENABLE', '(cache)', 11980, '', 'BOOL', 'stored only; read by the shader binding'),
    47: ('D3DRS_POINTSIZE_MAX', 'PA_SU_POINT_MINMAX', 11688, '16-31', 'float x16 (12.4 fixed)', 'cache +11996'),
    48: ('D3DRS_MULTISAMPLEANTIALIAS', 'PA_SU_SC_MODE_CNTL', 11656, '15', 'BOOL', 'msaa_enable'),
    49: ('D3DRS_MULTISAMPLEMASK', 'PA_SC_AA_MASK', 11840, '0-15', 'mask', ''),
    50: ('D3DRS_SCISSORTESTENABLE', '(cache) + PA_SC_WINDOW_SCISSOR', 11952, '', 'BOOL', 'recomputes the window scissor = viewport rect & scissor rect (+12832)'),
    51: ('D3DRS_SLOPESCALEDEPTHBIAS', 'PA_SU_POLY_OFFSET_FRONT/BACK_SCALE', 11920, 'float', 'float x16', 'also +11928; poly_offset_front/back_enable (PA_SU_SC_MODE_CNTL bits 11, 12) = scale or offset non-zero'),
    52: ('D3DRS_DEPTHBIAS', 'PA_SU_POLY_OFFSET_FRONT/BACK_OFFSET', 11924, 'float', 'float', 'also +11932; same enables'),
    53: ('D3DRS_COLORWRITEENABLE', 'RB_COLOR_MASK', 11548, '0-3', 'mask RGBA (bit 0 R)', 'cache +11956; forced 0 without render target 0'),
    54: ('D3DRS_COLORWRITEENABLE1', 'RB_COLOR_MASK', 11548, '4-7', 'mask', 'cache +11960; needs render target 1'),
    55: ('D3DRS_COLORWRITEENABLE2', 'RB_COLOR_MASK', 11548, '8-11', 'mask', 'cache +11964'),
    56: ('D3DRS_COLORWRITEENABLE3', 'RB_COLOR_MASK', 11548, '12-15', 'mask', 'cache +11968'),
    57: ('D3DRS_UNKNOWN_228', '(cache, never read)', 12124, '', 'DWORD', 'value stored at +12124, +12128, +12132, +12136; nothing reads them; no effect. The 2005 enum has one more state here than later XDKs.'),
    58: ('D3DRS_TESSELLATIONMODE', 'VGT_HOS_CNTL', 11704, '0-1', 'D3DTESSELLATIONMODE', ''),
    59: ('D3DRS_MINTESSELLATIONLEVEL', 'VGT_HOS_MIN_TESS_LEVEL', 11712, 'float', 'float', ''),
    60: ('D3DRS_MAXTESSELLATIONLEVEL', 'VGT_HOS_MAX_TESS_LEVEL', 11708, 'float', 'float', ''),
}
for i in range(16):
    RS[61 + i] = ('D3DRS_WRAP%d' % i, 'SQ_WRAPPING_%d' % (i // 8), 11628 + 4 * (i // 8), '%d-%d' % (4 * (i % 8), 4 * (i % 8) + 3),
                  'D3DWRAP mask (U 1, V 2, W 4, 4th coordinate 8)', 'getters for WRAP10-15 return 0 (stub sub_82304688)' if i >= 10 else '')
RS.update({
    77: ('D3DRS_VIEWPORTENABLE', 'PA_CL_VTE_CNTL + PA_CL_CLIP_CNTL', 11660, 'all', 'BOOL', 'on: VTE 0x43F (scale/offset enabled), clip on; off: VTE 0x400, clip_disable (PA_CL_CLIP_CNTL bit 16); cache +12140'),
    78: ('D3DRS_HIGHPRECISIONBLENDENABLE', 'RB_COLOR_INFO', 11460, '16-19', 'BOOL', 'switches 2_10_10_10(_FLOAT) render target 0 to its _AS_10_10_10_10 / _AS_16_16_16_16 form; cache +12144; also patches surface+52'),
    79: ('D3DRS_HIGHPRECISIONBLENDENABLE1', 'RB_COLOR1_INFO', 11468, '16-19', 'BOOL', 'cache +12148'),
    80: ('D3DRS_HIGHPRECISIONBLENDENABLE2', 'RB_COLOR2_INFO', 11472, '16-19', 'BOOL', 'cache +12152'),
    81: ('D3DRS_HIGHPRECISIONBLENDENABLE3', 'RB_COLOR3_INFO', 11476, '16-19', 'BOOL', 'cache +12156'),
    82: ('D3DRS_HALFPIXELOFFSET', 'PA_SU_VTX_CNTL', 11776, '0', 'BOOL', 'pix_center (0 = D3D9 half-pixel convention off)'),
    83: ('D3DRS_PRIMITIVERESETENABLE', 'PA_SU_SC_MODE_CNTL', 11656, '21', 'BOOL', 'multi_prim_ib_ena'),
    84: ('D3DRS_PRIMITIVERESETINDEX', 'VGT_MULTI_PRIM_IB_RESET_INDX', 11544, 'all', 'DWORD', ''),
    85: ('D3DRS_ALPHATOMASKENABLE', 'RB_COLORCONTROL', 11644, '4', 'BOOL', ''),
    86: ('D3DRS_ALPHATOMASKOFFSETS', 'RB_COLORCONTROL', 11644, '24-31', '4 x 2-bit offsets', ''),
    87: ('D3DRS_GUARDBAND_X', 'PA_CL_GB_HORZ_CLIP_ADJ', 11788, 'float', 'float', ''),
    88: ('D3DRS_GUARDBAND_Y', 'PA_CL_GB_VERT_CLIP_ADJ', 11780, 'float', 'float', ''),
    89: ('D3DRS_DISCARDBAND_X', 'PA_CL_GB_HORZ_DISC_ADJ', 11792, 'float', 'float', ''),
    90: ('D3DRS_DISCARDBAND_Y', 'PA_CL_GB_VERT_DISC_ADJ', 11784, 'float', 'float', ''),
    91: ('D3DRS_HISTENCILENABLE', 'RB_HIZCONTROL', 11648, '3', 'BOOL', 'hierarchical stencil (an optimisation)'),
    92: ('D3DRS_HISTENCILWRITEENABLE', 'RB_HIZCONTROL', 11648, '2', 'BOOL', ''),
    93: ('D3DRS_HISTENCILFUNC', 'RB_HIZCONTROL', 11648, '5', 'D3DHSCMPFUNC (0 EQUAL, 1 NOTEQUAL)', ''),
    94: ('D3DRS_HISTENCILREF', 'RB_HIZCONTROL', 11648, '8-15', 'BYTE', 'byte store at +11650'),
    95: ('D3DRS_PRESENTINTERVAL', '(cache)', 13772, '', 'D3DPRESENT_INTERVAL', 'the engine calls this setter and its getter through device+476 / +944'),
    96: ('D3DRS_PRESENTIMMEDIATETHRESHOLD', '(cache)', 11452, '23-29', '0-100', ''),
})

SS = {
    97: ('D3DSAMP_ADDRESSU', 0, '10-12', 'D3DTEXTUREADDRESS = Xenos ClampMode (WRAP 0, MIRROR 1, CLAMP 2, MIRRORONCE 3, BORDER_HALF 4, MIRRORONCE_BORDER_HALF 5, BORDER 6, MIRRORONCE_BORDER 7)', 'clamp_x'),
    98: ('D3DSAMP_ADDRESSV', 0, '13-15', 'D3DTEXTUREADDRESS', 'clamp_y'),
    99: ('D3DSAMP_ADDRESSW', 0, '16-18', 'D3DTEXTUREADDRESS', 'clamp_z'),
    100: ('D3DSAMP_BORDERCOLOR', 5, '0-1', 'D3DCOLOR: 0 -> black (0), anything else -> white (1)', 'border_color'),
    101: ('D3DSAMP_MAGFILTER', 3, '19-20 (+ 25-27, word 4 bit 10)', 'D3DTEXTUREFILTERTYPE (POINT 0, LINEAR 1, ANISOTROPIC 4)', 'mag_filter; value 4 sets mag_aniso_walk and aniso_filter from MAXANISOTROPY'),
    102: ('D3DSAMP_MINFILTER', 3, '21-22 (+ 25-27, word 4 bit 11)', 'D3DTEXTUREFILTERTYPE', 'min_filter; min_aniso_walk'),
    103: ('D3DSAMP_MIPFILTER', 3, '23-24', 'POINT 0, LINEAR 1, NONE 2 (base map)', 'mip_filter'),
    104: ('D3DSAMP_MIPMAPLODBIAS', 4, '12-21', 'float, stored x32 (5 fraction bits)', 'lod_bias'),
    105: ('D3DSAMP_MAXMIPLEVEL', 4, '2-5', 'DWORD; mip_min_level = max(texture, value)', 'mip_min_level; cache byte +12046+sampler'),
    106: ('D3DSAMP_MAXANISOTROPY', 3, '25-27', 'DWORD through table 0x82003298: 0-1 off, 2-3 -> 2:1, 4-6 -> 4:1, 7-12 -> 8:1, 13-16 -> 16:1', 'aniso_filter (only while an anisotropic filter is set); cache byte +12020+sampler'),
    107: ('D3DSAMP_MAGFILTERZ', None, '', 'D3DTEXTUREFILTERTYPE', 'cache byte +12098+sampler bit 0; SetTexture puts it in word 4 bit 0 (vol_mag_filter) when SEPARATEZFILTERENABLE'),
    108: ('D3DSAMP_MINFILTERZ', None, '', 'D3DTEXTUREFILTERTYPE', 'cache bit 1 -> word 4 bit 1 (vol_min_filter)'),
    109: ('D3DSAMP_SEPARATEZFILTERENABLE', None, '', 'BOOL', 'cache bit 2'),
    110: ('D3DSAMP_MINMIPLEVEL', 4, '6-9', 'DWORD; mip_max_level = min(texture, value)', 'mip_max_level; cache byte +12072+sampler'),
    111: ('D3DSAMP_TRILINEARTHRESHOLD', 5, '3-4', 'D3DTRILINEARTHRESHOLD', 'tri_clamp'),
    112: ('D3DSAMP_ANISOTROPYBIAS', 5, '5-8', 'float x -8', 'aniso_bias'),
    113: ('D3DSAMP_HGRADIENTEXPBIAS', 4, '22-26', 'int', 'grad_exp_adjust_h'),
    114: ('D3DSAMP_VGRADIENTEXPBIAS', 4, '27-31', 'int', 'grad_exp_adjust_v'),
    115: ('D3DSAMP_WHITEBORDERCOLORW', 5, '2', 'BOOL', 'force_bc_w_to_max'),
    116: ('D3DSAMP_POINTBORDERENABLE', 1, '11', 'BOOL; field = !value', 'nearest_clamp_policy'),
}

REGNUM = {}
for line in open(r'C:\rexsrc\include\rex\graphics\register_table.inc'):
    m = re.match(r'XE_GPU_REGISTER\((0x[0-9A-F]+), \w+, (\w+)\)', line)
    if m:
        REGNUM[m.group(2)] = m.group(1)


def trace_values(sub):
    vals = {}
    for log, scene in SCENES:
        path = os.path.join(A, log)
        if not os.path.exists(path):
            continue
        for line in open(path, encoding='utf-8', errors='replace'):
            if ('KK d3d: ' + sub + ' ') not in line:
                continue
            m = re.search(r'r4=([0-9A-F]{8}) r5=([0-9A-F]{8})', line)
            if m:
                vals.setdefault(m.group(1), set()).add(m.group(2))
    return vals


tab = {(t['table'], t['index']): t for t in tables}
render_states = []
for idx in range(97):
    t = tab[('A', idx)]
    g = tab.get(('B', idx - 10)) if idx >= 10 else None
    if idx < 10:
        render_states.append({'index': idx, 'value': idx * 4, 'name': None, 'setter': t['fn'],
                              'getter': 'sub_82304688', 'note': 'unsupported (empty setter, getter returns 0)'})
        continue
    name, reg, off, bits, enc, extra = RS[idx]
    regs = [r for r in re.findall(r'[A-Z][A-Z0-9_]+', reg) if r in REGNUM]
    vals = trace_values(t['fn'])
    render_states.append({'index': idx, 'value': idx * 4, 'name': name, 'setter': t['fn'], 'getter': g['fn'] if g else None,
                          'register': reg, 'register_number': [REGNUM[r] for r in regs], 'device_offset': off,
                          'bits': bits, 'encoding': enc, 'note': extra,
                          'traced_values': ['0x' + v.lstrip('0') if v.strip('0') else '0' for v in sorted(vals)][:12]})

sampler_states = []
for idx in range(97, 117):
    t = tab[('A', idx)]
    g = tab[('B', idx - 10)]
    name, word, bits, enc, note = SS[idx]
    vals = trace_values(t['fn'])
    sampler_states.append({'index': idx, 'value': (idx - 97) * 4, 'name': name, 'setter': t['fn'], 'getter': g['fn'],
                           'fetch_word': word, 'bits': bits, 'encoding': enc, 'note': note,
                           'traced_values': sorted({'0x' + v.lstrip('0') if v.strip('0') else '0'
                                                    for s in vals.values() for v in s})[:12]})

# ---------------------------------------------------------------------------------------------------------
# Register shadow: blocks the pending-state flush sub_82121040 sends (pending qword, bits MSB first).
shadow = [
    {'block': 'fetch constants', 'first': '0x4800', 'count': 192, 'offset': 1152, 'pending': '+32 bits 63-32 (bit 63 = fetch constant 0)',
     'note': '32 groups of 6 dwords: texture fetch constants 0-31; vertex fetch constant n (2 dwords) is group n/3 word 2*(n%3). Stream s uses vertex fetch constant 95-s.'},
    {'block': 'ALU constants (vertex)', 'first': '0x4000', 'count': 1024, 'offset': 1920, 'pending': '+16, one bit per 4 float4 (bit 63 = c0-c3)'},
    {'block': 'ALU constants (pixel)', 'first': '0x4400', 'count': 1024, 'offset': 6016, 'pending': '+24'},
    {'block': 'bool and loop constants', 'first': '0x4900', 'count': 40, 'offset': 10112, 'pending': '+32 bit 31',
     'note': '8 bool dwords (vertex bools 0-127 then pixel 128-255), 32 loop dwords (vertex 0-15 then pixel 16-31)'},
    {'block': 'destination packet', 'first': '0x2000', 'count': 16, 'offset': 11456, 'pending': '+32 bits 29-14',
     'note': 'RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, RB_COLOR1-3_INFO, COHER_DEST_BASE_0-7, PA_SC_SCREEN_SCISSOR_TL/BR'},
    {'block': 'window packet', 'first': '0x2080', 'count': 3, 'offset': 11520, 'pending': 'none (written to the ring at once by sub_82109508)',
     'note': 'PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL (+11524), _BR (+11528)'},
    {'block': 'values packet', 'first': '0x2100', 'count': 21, 'offset': 11532, 'pending': '+40 bits 63-43'},
    {'block': 'program packet', 'first': '0x2180', 'count': 5, 'offset': 11616, 'pending': '+40 bits 42-38',
     'note': 'SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, SQ_INTERPOLATOR_CNTL, SQ_WRAPPING_0/1 (written by the shader binding and WRAPn)'},
    {'block': 'control packet', 'first': '0x2200', 'count': 12, 'offset': 11636, 'pending': '+40 bits 37-26'},
    {'block': 'tessellator packet', 'first': '0x2280', 'count': 21, 'offset': 11684, 'pending': '+40 bits 25-5'},
    {'block': 'misc packet', 'first': '0x2300', 'count': 38, 'offset': 11768, 'pending': '+48 bits 63-26'},
    {'block': 'point packet', 'first': '0x2380', 'count': 8, 'offset': 11920, 'pending': '+48 bits 25-18'},
]
for b in shadow:
    first = int(b['first'], 16)
    names = {int(v, 16): k for k, v in REGNUM.items()}
    if first < 0x4000:
        b['registers'] = [{'offset': b['offset'] + 4 * i, 'reg': '0x%04X' % (first + i), 'name': names.get(first + i, '')}
                          for i in range(b['count'])]

pending_other = [
    {'qword': 48, 'bit': 10, 'meaning': 'streams or declaration changed: rebuild vertex fetch constants (sub_8211FF40)'},
    {'qword': 48, 'bits': '8-5', 'meaning': 'shader binding (sub_82120A70): shaders, declaration, alpha test / depth state that the binding depends on'},
    {'qword': 48, 'bits': '17-12', 'meaning': 'clip planes (already written to the ring by SetClipPlane)'},
]

objects_note = 'See native-renderer/docs/d3d-structs.md for the object layouts.'

funcs = []
seen_subs = set()
for f in F:
    if f['sub'] in seen_subs:
        continue
    seen_subs.add(f['sub'])
    e = entry.get(f['sub'])
    f['address'] = '0x' + f['sub'][4:]
    f['engine_call_sites'] = e['sites'] if e else 0
    f['traced'] = f['sub'] in TRACED
    f['per_frame'] = COUNTS.get(f['sub'], {})
    funcs.append(f)
# Table entries: point to the state lists.
for t in tables:
    if t['fn'] in seen_subs:
        continue
    seen_subs.add(t['fn'])
    idx = t['index'] if t['table'] == 'A' else t['index'] + 10
    if t['fn'] in ('sub_82148AB8', 'sub_82304688'):
        nm = 'empty setter (unsupported state)' if t['fn'] == 'sub_82148AB8' else 'getter stub (returns 0)'
        grp = 'render_state_setter' if t['table'] == 'A' else 'render_state_getter'
    elif idx < 97:
        nm = ('Set' if t['table'] == 'A' else 'Get') + 'RenderState(' + RS[idx][0] + ')'
        grp = 'render_state_setter' if t['table'] == 'A' else 'render_state_getter'
    else:
        nm = ('Set' if t['table'] == 'A' else 'Get') + 'SamplerState(' + SS[idx][0] + ')'
        grp = 'sampler_state_setter' if t['table'] == 'A' else 'sampler_state_getter'
    args = [a('r3', D), a('r4', 'Value')] if t['table'] == 'A' and idx < 97 else (
        [a('r3', D), a('r4', 'Sampler'), a('r5', 'Value')] if t['table'] == 'A' else (
            [a('r3', D)] if idx < 97 else [a('r3', D), a('r4', 'Sampler')]))
    funcs.append({'sub': t['fn'], 'address': '0x' + t['fn'][4:], 'name': nm, 'group': grp, 'renderer': 'keep',
                  'args': args, 'returns': 'void' if t['table'] == 'A' else 'DWORD',
                  'notes': 'Device table %s index %d (device+%d). Called by the engine as *(device+%d)(...).' % (
                      t['table'], t['index'], t['off'], t['off']),
                  'status': 'verified' if t['fn'] in TRACED else 'code', 'engine_call_sites': 0,
                  'traced': t['fn'] in TRACED, 'per_frame': COUNTS.get(t['fn'], {})})
funcs.sort(key=lambda f: f['address'])

missing = sorted(n for n in TRACED if n not in seen_subs)
out = {
    'about': 'The game\'s Xbox 360 XDK (2005) Direct3D library, as used by King Kong. Single source of names for '
             'the native renderer streams. Built by stream 01 (brief 01); see docs/d3d-api-map.md and '
             'docs/d3d-structs.md. Addresses are guest addresses; device offsets are bytes into the D3D device.',
    'conventions': {
        'abi': 'PowerPC 64 ABI: integer and pointer arguments in r3-r10 in order; a float argument goes in the next '
               'free f register AND uses up its r slot (D3DDevice_Clear: Color r7, Z f1, Stencil r9). Returns in r3 or f1.',
        'bits': 'Bit numbers are LSB = 0 unless the text says PowerPC (MSB = 0) numbering.',
        'renderer': {'replace': 'the native renderer implements it; the original writes GPU packets',
                     'observe': 'keep the original and hook after it (resource create / lock / unlock / release)',
                     'keep': 'keep the original; it only updates the device struct, read it at draw time',
                     'ignore': 'no visible effect'},
        'status': {'verified': 'code read and matched with traced values or dumps', 'code': 'code read',
                   'probable': 'role clear from the code, exact XDK name a best guess'},
        'scenes': {s: l for l, s in SCENES},
    },
    'device': {
        'pointer': '0x82D62324 (GDI+4)', 'size': 20608, 'setter_table': {'offset': 96, 'entries': 117,
        'index': 'D3DRENDERSTATETYPE / 4 for render states 0-96; 97 + D3DSAMPLERSTATETYPE / 4 for sampler states'},
        'getter_table': {'offset': 564, 'entries': 117},
        'pending': 'qwords at +16, +24, +32, +40, +48 (MSB first); flushed by sub_82121040 at each draw',
    },
    'functions': funcs,
    'render_states': render_states,
    'sampler_states': sampler_states,
    'register_shadow': shadow,
    'pending_other': pending_other,
    'traced_but_unnamed': missing,
}
json.dump(out, open(OUT, 'w', newline='\n'), indent=1)
print(len(funcs), 'functions,', len(render_states), 'render states,', len(sampler_states), 'sampler states')
print('traced but unnamed:', missing)
