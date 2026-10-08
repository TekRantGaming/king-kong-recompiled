# ReXGlue SDK patches

The port ships the GPU plugin (`rexgpu-xenos.dll`) built from the ReXGlue SDK **v0.10.0** source (tag commit
`f5337cdc`) with the patches in this folder (applied in order), and `rexruntime.dll` from the same build (it has
the upscalers of 0008; before that it was the plain v0.10.0 rebuild).

## 0001: let a frame wait briefly for pipelines created in the background

Adds `async_shader_wait_ms`. With `async_shader_compilation`, a draw whose pipeline is still being created in
the background waits for it within a per-frame budget instead of being skipped at once. The launcher's
**Shader preparing: Balanced** (the default) uses it.

It and 0002 add testing aids, environment variables that do nothing unless set:
`REX_DEV_SHADER_SALT` (makes the graphics driver's own shader cache treat every shader as new, to measure
first-time pipeline creation) and `REX_DEV_PIPELINE_STATS` (logs pipeline waits, skipped draws and, with 0002, pipeline creation times once a
second).

The port sets the wait to half a frame at the player's frame rate cap (16 ms at 30 FPS, 8 at 60, 4 from 120).
Tried and dropped, as they made no measurable difference: creating waited-for pipelines first, a total wait
limit per pipeline, lower-priority creation threads, and more creation threads.

## 0003: frame log (testing)

`REX_DEV_FRAME_LOG=<seconds>[,<frames>]` logs every draw and copy of a frame (render targets, depth, the game shader
each draw uses), to see how the game builds its frames. Does nothing unless set.

## 0004 to 0007: ambient occlusion

Screen-space AO, off by default (`ao_mode` 0 off, 1 on, 2 show the AO only; `ao_strength` 1 to 3), set from the
launcher's Graphics page. King Kong draws its scene depth first and copies it out of the console's graphics memory
(EDRAM) before lighting. The plugin notices that copy, and at the next full-screen color copy in that frame (and any
later copy to the same place):

1. once a frame, at half resolution: reads the depth from its EDRAM buffer, works out the occlusion (10 samples around
   a normal rebuilt from the depth), blurs it with a depth-aware 9-tap blur each way, then scales it up to full
   resolution, edge-aware, fading it out with distance;
2. at each such copy: multiplies it into the scene color in the EDRAM buffer just before the copy.

The final copy of the post-processed image goes elsewhere, so the HUD isn't touched. The game draws its distance
fog before that scene copy, so the plugin notes the constants of the game's fog pass (its two AfterEffects fog shaders:
c0 fog color, c1 `g_vFogParams`, c2 `g_vFogNormalizing`), works out the same fog amount per pixel, and applies
`color * ao + fog color * fog * (1 - ao)`: only the scene under the fog is darkened, and fogged walls don't show through. Only with host render targets
(NVIDIA and AMD) for now: the ROV path, which Intel GPUs use, is untested.

The shaders are `src/graphics/shaders/ambient_occlusion.cs.hlsl`, compiled into `bytecode/d3d12_5_1/ao_*_cs.h` with
`fxc /T cs_5_1 /O3 /E <entry> /Fh <file> /Vn <name>`. EDRAM layout they rely on: tiles of 80 x 16 pixels times the
resolution scale, row-major inside a tile; depth tiles have their left and right halves swapped; depth is 24-bit in the
top bits; color is 8:8:8:8 with red in the low byte.

GPU time at 3x (3840 x 2160) in V-Rex at strength 1 on an RTX 4070 Ti: about 0.7 to 1.0 ms a frame.

Testing switches (do nothing unless set): `REX_DEV_AO=<HLSL file>` compiles that source instead of the built-in
shaders; `REX_DEV_AO_STATS=1` logs GPU time per pass every 2 seconds; `REX_DEV_AO_TOGGLE=<seconds>` switches AO on and
off; `REX_DEV_AO_CYCLE=<seconds>` steps through views 0 to 7 (off, depth, AO only 1-3, applied 1-3), and an 8th
`REX_DEV_AO_PARAMS` value above 0 stamps the view number; `REX_DEV_AO_TRACE=<count>` logs the full-screen copies seen.
Developer builds of the port also have F8 (off / on / AO only) and F9 (strength).

## 0008: AMD FSR 1 and NVIDIA Image Scaling upscalers

Changes `rexruntime.dll` (the presenter that scales the game's picture to the window). The spatial FidelityFX effects
(FSR 1 EASU and RCAS, CAS) were only built when the AMD FidelityFX SDK was found, though their shaders are built in
and don't need it; they're now always built (`present_effect` `fsr` and `cas`). The temporal FSR 2/3 path still needs
the SDK.

Adds NVIDIA Image Scaling (`present_effect` `nis`, D3D12 only, FSR elsewhere; `present_nis_sharpness` 0 to 1,
default 0.5) from the [NVIDIA Image Scaling SDK](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling) v1.0.3 (MIT,
`thirdparty/nis`). NVScaler upscales by 1x to 2x along each axis, so for bigger factors it runs again (like the FSR
EASU passes), sharpening only in the last pass; without upscaling, NVSharpen. Both are compute shaders
(`src/ui/shaders/guest_output_nis.cs.hlsl`, the filter banks as constant arrays instead of textures) writing to an
intermediate image, followed by a 1:1 (or, for a supersampled picture, downscaling) bilinear pass. Also fixes the
size of the presenter's RTV heap, which only had room for 2 intermediate images (FSR from 720p to 8K needs 3).

## Building

```
git clone https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk
git checkout f5337cdc
git am <this folder>/*.patch
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config Release --target install --parallel
```

Use Visual Studio 2022 Build Tools with its Clang on the PATH. Copy `out/install/win-amd64/bin/rexgpu-xenos.dll`
and `rexruntime.dll` over the ones in `tools/rexglue/win-amd64/bin/`.
