# ReXGlue SDK patches

The port ships the GPU plugin (`rexgpu-xenos.dll`) built from the ReXGlue SDK **v0.10.0** source (tag commit
`f5337cdc`) with the patches in this folder. `rexruntime.dll` is the plain v0.10.0 rebuild (no patches).

## 0001-d3d12-pipeline-wait.patch

Adds `async_shader_wait_ms`. With `async_shader_compilation`, a draw whose pipeline is still being created in
the background waits for it within a per-frame budget instead of being skipped at once. The launcher's
**Shader preparing: Balanced** (the default) uses it.

Also adds two testing aids, both environment variables that do nothing unless set:
`REX_DEV_SHADER_SALT` (makes the graphics driver's own shader cache treat every shader as new, to measure
first-time pipeline creation) and `REX_DEV_PIPELINE_STATS` (logs pipeline waits and skipped draws once a second).

## Building

```
git clone https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk
git checkout f5337cdc
git am <this folder>/0001-d3d12-pipeline-wait.patch
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config Release --target install --parallel
```

Use Visual Studio 2022 Build Tools with its Clang on the PATH. Copy `out/install/win-amd64/bin/rexgpu-xenos.dll`
over `tools/rexglue/win-amd64/bin/rexgpu-xenos.dll`.
