# Third-party code

Vendored copies (exported with `git archive`, no `.git` data), each with its own licence file.

| Folder | Upstream | Commit | Licence |
|---|---|---|---|
| `nvrhi/` | https://github.com/NVIDIA-RTX/NVRHI | 6b96fb03e07539f08327aea76c56d55f1de9d906 (2026-10-05) | MIT (`nvrhi/LICENSE.txt`; `ThirdPartyLicenses.txt` for what it bundles) |
| `XenosRecomp/` | https://github.com/hedge-dev/XenosRecomp | 990d03b28a27b50277ee5d8d942e1c5f873869d1 (2025-08-03) | MIT (`XenosRecomp/LICENSE.md`) |

Not vendored:

- NVRHI fetches `Vulkan-Headers` (v1.4.352) and `DirectX-Headers` (v1.717.0-preview) with CMake FetchContent
  unless `Vulkan::Headers` / `Microsoft::DirectX-Headers` targets already exist. Point it at the SDK's copies
  (brief 04) rather than letting it download.
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
