# Third-party code

Vendored copies (exported with `git archive`, no `.git` data), each with its own licence file.

| Folder | Upstream | Commit | Licence |
|---|---|---|---|
| `nvrhi/` | https://github.com/NVIDIA-RTX/NVRHI | 6b96fb03e07539f08327aea76c56d55f1de9d906 (2026-10-05) | MIT (`nvrhi/LICENSE.txt`; `ThirdPartyLicenses.txt` for what it bundles) |
| `DirectX-Headers/` | https://github.com/microsoft/DirectX-Headers | v1.717.0-preview, d873b344dc540898868697245f100c2a67fe68d9 (2025-05-28): only `include/directx/*.h`, `include/dxguids`, `include/wsl`, `src/dxguids.cpp` | MIT (`DirectX-Headers/LICENSE`) |
| `XenosRecomp/` | https://github.com/hedge-dev/XenosRecomp | 990d03b28a27b50277ee5d8d942e1c5f873869d1 (2025-08-03) | MIT (`XenosRecomp/LICENSE.md`) |

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
- `XenosRecomp/XenosRecomp/shader_recompiler.cpp` (Phase 2, differential tests; translator version 4): cube
  fetches apply the instruction's offsets and unnormalized coordinates; the vector half's a0 / p0 changes
  take effect before the scalar half reads its operands and a scalar write replaces them; aL is clamped to
  [-256, 256]; loop repeat keeps the current aL; not-equal goes through `kk_Ne` (unordered); in instruction
  mode `vfetch_mini` uses the binding and stride of the `vfetch_full` that ran last; bool literals are
  inlined (the prelude's multiply rule changed too, in `native-renderer/shaders`). Listed with the reasons in
  `docs/shaders.md`.
