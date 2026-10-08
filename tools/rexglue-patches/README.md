# ReXGlue SDK patches

The port ships the GPU plugin (`rexgpu-xenos.dll`) built from the ReXGlue SDK **v0.10.0** source (tag commit
`f5337cdc`) with the patches in this folder. `rexruntime.dll` is the plain v0.10.0 rebuild (no patches).

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
over `tools/rexglue/win-amd64/bin/rexgpu-xenos.dll`.
