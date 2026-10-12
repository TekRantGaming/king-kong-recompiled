# Third-party code

Vendored copies (exported with `git archive`, no `.git` data), each with its own licence file.

| Folder | Upstream | Commit | Licence |
|---|---|---|---|
| `nvrhi/` | https://github.com/NVIDIA-RTX/NVRHI | 6b96fb03e07539f08327aea76c56d55f1de9d906 (2026-10-05) | MIT (`nvrhi/LICENSE.txt`; `ThirdPartyLicenses.txt` for what it bundles) |
| `DirectX-Headers/` | https://github.com/microsoft/DirectX-Headers | v1.717.0-preview, d873b344dc540898868697245f100c2a67fe68d9 (2025-05-28): only `include/directx/*.h`, `include/dxguids`, `include/wsl`, `src/dxguids.cpp` | MIT (`DirectX-Headers/LICENSE`) |
| `XenosRecomp/` | https://github.com/hedge-dev/XenosRecomp | 990d03b28a27b50277ee5d8d942e1c5f873869d1 (2025-08-03) | MIT (`XenosRecomp/LICENSE.md`) |
| `XeGTAO/` | https://github.com/GameTechDev/XeGTAO | a5b1686c7ea37788eeb3576b5be47f7c03db532c (2024-04-22), XeGTAO 1.30: only `LICENSE` and `Source/Rendering/Shaders/XeGTAO.h`, `XeGTAO.hlsli`, `vaGTAO.hlsl` (the sample's entry points, kept for reference, not compiled) | MIT (`XeGTAO/LICENSE`) |
| `FidelityFX-API/` | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK | v1.1.4 (2025-05-08), FSR 3.1.4: only the FidelityFX API headers `ffx-api/include/ffx_api/` (`ffx_api.h`, `ffx_api_types.h`, `ffx_upscale.h`, `ffx_api_loader.h`, `vk/ffx_api_vk.h`, `dx12/ffx_api_dx12.h`) and `sdk/LICENSE.txt`. The AMD-signed DLLs are not in git: the build copies them beside the game from the SDK (`cmake/upscalers.cmake`, docs/upscalers.md) | MIT (`FidelityFX-API/LICENSE.txt`) |

Not vendored:

- NVRHI fetches `Vulkan-Headers` (v1.4.352) with CMake FetchContent unless a `Vulkan::Headers` target already
  exists; `cmake/nvrhi.cmake` points it at the SDK's copy and never lets it download. `DirectX-Headers` is vendored
  (above) because NVRHI's D3D12 backend uses preview names (`ID3D12DevicePreview`, the linear algebra barrier
  sync) that the retail Windows SDK 10.0.26100 `d3d12.h` lacks, although it defines `D3D12_PREVIEW_SDK_VERSION`
  as 717. The first Windows build failed on exactly that.
- XenosRecomp's submodules are not included: `thirdparty/fmt` (873670ba), `thirdparty/xxHash` (2bf8313b),
  `thirdparty/zstd` (f7a8bb12), `thirdparty/smol-v` (9dd54c37) and `thirdparty/dxc-bin` (737ac9f5, prebuilt
  DXC binaries). Its own code is `XenosRecomp/XenosRecomp/` (ten files). Build it against the libraries the
  SDK already ships (DXC, fmt, xxHash, zstd) and drop smol-v (SPIR-V compression for its cache, which we do
  not use); do not download the binaries.

Changes we make to vendored code are kept as small as possible and listed here:

- `XenosRecomp/XenosRecomp/shader_recompiler.cpp`, `shader_recompiler.h`, `pch.h`: reworked for King Kong
  by the shader translator (stream 02): the container reading moved to `native-renderer/shaders`
  (`RecompilerInput`), bindless resources, vertex pulling from guest memory, the full constant and
  control-flow model, literal constants served in the shader (`kkConstRel`), one `if` block per run of
  instructions with the same predicate, and branch-free `select` forms for scalar conditionals (the
  SPIR-V back end keeps short-circuit `?:` as branches). The submodules and `main.cpp` /
  `dxc_compiler.cpp` are not built.
- `XenosRecomp/XenosRecomp/pch.h`: includes `<iterator>` (Phase 2: with the SDK's fmt, which no longer pulls it
  in, `shader_recompiler.h` did not compile in the plugin build).
- `nvrhi/src/validation/validation-device.cpp` (Phase 2, Linux): the validation layer refused a sampler in an
  immutable bindless layout ("Bindless samplers are not implemented"), so the game renderer's sampler table
  (set 5) failed to initialise whenever NVRHI validation was on. The Vulkan backend makes such a table like any
  other descriptor array and the game renderer's tests draw through it under the Khronos layer, so the check now
  lets it through on Vulkan. It still refuses it on D3D12, where it is right to: that backend binds an immutable
  table from the CBV/SRV/UAV heap (`DescriptorTable::isSamplerTable()` is true only for `MutableSampler`
  layouts), which cannot hold samplers.
- `XenosRecomp/XenosRecomp/shader_recompiler.cpp` (Phase 2, differential tests; translator version 4): cube
  fetches apply the instruction's offsets and unnormalized coordinates; the vector half's a0 / p0 changes
  take effect before the scalar half reads its operands and a scalar write replaces them; aL is clamped to
  [-256, 256]; loop repeat keeps the current aL; not-equal goes through `kk_Ne` (unordered); in instruction
  mode `vfetch_mini` uses the binding and stride of the `vfetch_full` that ran last; bool literals are
  inlined (the prelude's multiply rule changed too, in `native-renderer/shaders`). Listed with the reasons in
  `docs/shaders.md`.
- `XenosRecomp/XenosRecomp/shader_recompiler.{h,cpp}` (Phase 2b, render scale): `RecompilerInput::renderScaleAware`
  (off: byte-identical output). On: the pixel position register is `(iPos.xy * kk_RenderScale.zw - 0.5)`, each
  texture fetch's `GetDimensions` size goes through `kk_GuestSize(size, fetch constant)`, and the
  `KKScaleConstants` buffer (b3) and `kk_GuestSize` are declared. Reasons and checks in `docs/shaders.md`,
  "Render scale".
- `nvrhi/src/d3d12/d3d12-resource-bindings.cpp` (Phase 2, Windows): `DescriptorTable::isSamplerTable()` is also
  true for an immutable bindless layout whose register spaces are all samplers, and descriptor tables are bound
  from their own heap (`getDescriptorHeap()`) instead of always the CBV/SRV/UAV heap, so the game renderer's
  sampler table (set 5) lives in and binds from the sampler heap on D3D12 as well.
- `nvrhi/include/nvrhi/vulkan.h`, `nvrhi/src/vulkan/vulkan-device.cpp`, `nvrhi/src/vulkan/vulkan-backend.h` (Phase 2c,
  the driver's pipeline cache): `nvrhi::vulkan::DeviceDesc::pipelineCache`. When the application gives a
  `VkPipelineCache`, the device creates every pipeline with it instead of the empty cache it made for itself, and
  does not destroy it (`Device::m_OwnsPipelineCache`). NVRHI had no way to fill its cache from saved data or to read
  it back; the native renderer's `--native_vk_pipeline_cache` keeps it in a file (`backend/vk_pipeline_cache.*`,
  docs/backend.md, "The driver's pipeline cache"). Without the field (VK_NULL_HANDLE, the default) the device does
  what it always did.
- `nvrhi/src/d3d12/d3d12-backend.h`, `nvrhi/src/d3d12/d3d12-resource-bindings.cpp` (Phase 2c, the render thread): the
  root signature cache (`DeviceResources::rootsigCache`) is locked (`rootsigCacheMutex`, recursive) in
  `Device::getRootSignature` and `RootSignature::~RootSignature`, so D3D12 pipelines can be made on several
  threads (the game renderer's pipeline workers) as on Vulkan. Before it, the renderer made D3D12 pipelines on the
  draw thread.
- `XenosRecomp/XenosRecomp/shader_recompiler.{h,cpp}` (Phase 3, ambient-only AO, `docs/ao.md`):
  `RecompilerInput::ambientOcclusion` (kind None: byte-identical output, checked on the 1,557 corpus
  translations). Set, a pixel shader declares the `KKAmbientOcclusion` buffer (b4) and `kk_AmbientOcclusion`,
  reads the occlusion once (`kkAoMul`) and multiplies one term by it: its colour output, every direct read of a
  constant, an interpolated input, the result of one fetch constant's texture fetches, or a register after one
  instruction. `RecompilerInput::annotateAddresses` (the tool's `translate --addresses` only) writes each
  instruction's address as a comment.
- `XeGTAO/` (Phase 3, ambient occlusion, `docs/ao.md`): not changed. `backend/shaders/gtao.hlsl` includes
  `XeGTAO.hlsli` as it is and adapts it from outside: `XE_GTAO_USE_HALF_FLOAT_PRECISION 0` and `XE_GTAO_FP32_DEPTHS`
  (32-bit depth everywhere), `VA_SATURATE` defined as `saturate`, and `unorm` defined away before the include, so
  the edges image is a plain `float` image (R32_FLOAT) on both APIs. R32 formats are the ones every Vulkan device
  can store to without `shaderStorageImageExtendedFormats`. The C++ side uses only `XeGTAO.h`'s `GTAOConstants`
  struct for the constant buffer layout.

## SDKs used but not vendored

The temporal upscalers' SDKs whose licences do not allow their files in this repository (docs/upscalers.md):
the build finds them under `tools/upscaler-sdks/` (git-ignored) and copies their runtime files beside the game.

| SDK | Upstream | Version | What the build uses | Licence |
|---|---|---|---|---|
| NVIDIA DLSS SDK | https://github.com/NVIDIA/DLSS | v310.9.1 (`tools/upscaler-sdks/dlss`) | `include/` (NGX headers), `lib/Windows_x86_64/x64/nvsdk_ngx_d.lib` (the NGX loader, linked into rexgpu-native.dll), `lib/Windows_x86_64/rel/nvngx_dlss.dll` (beside the game) | NVIDIA RTX SDKs License (the SDK's `LICENSE.txt`): object code only, attribution with the NVIDIA marks in the credits and about box, NVIDIA to be notified before a commercial release |
