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
  DXC binaries). Its own code is `XenosRecomp/XenosRecomp/` (ten files). `native-renderer/shaders` builds
  only `shader_recompiler.cpp` from it, against the SDK's fmt and xxHash (header-only); DXC is loaded at
  run time by kkshaders; zstd and smol-v are not used. XenosRecomp's own `main.cpp`, `dxc_compiler.*`,
  `shader_common.h` and CMake files are kept unchanged for reference but not built.

Changes we make to vendored code are kept as small as possible and listed here:

- XenosRecomp `pch.h`: trimmed to what the translator core needs (no Windows.h, dxcapi.h, smol-v, zstd,
  xxHash, `<execution>`).
- XenosRecomp `shader_recompiler.h` / `.cpp`: reworked for this game (the brief asks for it; the diff
  against upstream 990d03b2 is large). The class, the StringBuffer printing and the per-instruction
  structure stay; what changed:
  - Input: a `RecompilerInput` (microcode, constant table, literals, vertex elements, interpolators)
    filled by the kkshaders container adapter instead of XenosRecomp's own (2010-era) container format.
  - Output: the kkshaders binding model (register-file constant buffers, bindless textures, samplers and
    vertex buffers) instead of Unleashed Recompiled's; no specialisation constants or DXIL linking.
  - Fixes: conditional execs (cexec / cexec pred) were executed unconditionally; conditional ends;
    co-issued vector and scalar operations now read their sources before either writes; setp_*_push,
    kill*, maxa and the scalar operand components follow ucode.h; Direct3D 9 multiplication; clamping
    variants of rcp / rsq / log; relative constant addressing per operand (const_0 / const_1, a0 / aL);
    loop constants with start and step, nested loops, predicated breaks, calls / returns (a general pc /
    switch form when the control flow is not structured); 64 temporaries and relative temporaries;
    exports to every register (point size, memory export ignored).
  - New: vertex fetch by pulling from guest memory (binding mode for the game's template fetches,
    instruction mode for patched microcode, every vertex format, mini fetches); texture fetch modes (LOD
    bias, register LOD, register gradients, offsets, unnormalised coordinates, filter overrides as sampler
    bindings, 1D / 3D / stacked / cube, getCompTexLOD, getWeights, getGradients, setTexLOD / gradients);
    exact cube map handling.
