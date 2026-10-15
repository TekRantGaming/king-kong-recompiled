# Player-owned replacement texture packs

Texture packs default Off. Graphics has a Texture pack switch and a folder picker;
F8 has the same live switch. The renderer retains the original, so switching Off
does not read files, reconvert textures or wait for an upload. Nothing is downloaded.
Players supply their own extracted textures. No game art or example game pack is
part of the repository or release.

## Create a pack

Build the resource tools with `native-renderer/resources/build.bat res-release`.
The source for the extractor's Windows wrapper is `resources/tools/extract-textures.ps1`;
distribute it beside the extractor's `bin` directory. It also writes `pack-template`.
The existing `images/*.png` files are display previews with the guest swizzle applied.
Use the pack template, not those previews, when making replacements.

```
kknr_bfscan.exe X:/YourGame/KKTextures.bf --dump X:/Extracted/raw
kknr_texdump.exe --no-images --csv X:/Extracted/textures.csv --pack-template X:/Extracted/template X:/Extracted/raw
```

For upscalers that take PNG, add `--pack-png` to the second command. These PNGs
contain raw host channels, before the game's view swizzle. In particular, do not
swap red/blue, reconstruct normal Z into green, bake gamma, discard alpha, or
premultiply it. For signed normals the PNG's 0..255 encodes -1..1; the loader
converts back to signed RG. The ordinary preview PNGs remain unchanged.

Copy the template to a new folder, replace/upscale the images, and keep only the
manifest rows and images you want. Preserve the identity portion of each filename.
When changing DDS to PNG, change the row's filename extension too. Leave original
format and size columns unchanged. A pack is a folder; extract ZIP distributions
before selecting them. Subdirectories and images outside the folder are rejected.

```
kknr_packcheck.exe X:/MyPack X:/Extracted/template
```

The checker uses the template as the player's original catalog. It reports unknown
keys, unlisted images, changed original metadata, invalid containers, format family
mismatches and mip errors. Run it again after editing a pack. It does not verify
artistic quality or copyright permission.

## Version 1 format

`pack.txt` is UTF-8 without BOM, with LF or CRLF. First line: `KKTP 1`. Blank lines
and lines starting with `#` are ignored. Each subsequent row has six whitespace
separated fields:

```
# base_xxh3 bf_name original_host_format original_width original_height replacement_file
0123456789ABCDEF bf_12345678_0000000C BC3_UNORM 256 256 0123456789ABCDEF__bf_12345678_0000000C.png
```

This is a synthetic syntax example, not a game identity. Hashes have 16 hex digits;
BF names use the extractor's `bf_<8 hex pack>_<8 hex record offset>` convention.
The extractor adds `base_xxh3` and `pack_key` columns to CSV and uses that same key
in template filenames. Its old full-chain FNV `host_hash` column is retained for
existing diagnostics; it is not the replacement identity.

The content identity is XXH3-64 over converted, tightly packed base level bytes,
before the view swizzle, excluding other levels/layers and guest allocation padding.
This is the material census convention from tasks 072/077, not a cryptographic hash.
The runtime additionally matches original host format, width and height. Runtime
guest addresses are not identities. BF names provide extraction provenance and
filenames; the runtime finds their manifest entry by original content. Identical
BF records therefore share one canonical alias. The extractor chooses the first
sorted alias; the loader rejects ambiguous duplicate original content.

Limits: 32,768 manifest entries, 8 MiB manifest, 8192 texels per axis, one 2D layer,
128 MiB per stored image/decoded mip chain. Cube, volume, array, missing-base,
render-target and resolve-alias textures retain originals.

## Images and sampling

- DDS: BC1, BC3, BC5 UNORM/SNORM, BC7, raw RGBA8/RG8/R8 UNORM and RG8 SNORM.
  Legacy DXT1/DXT5/ATI2 and DX10 DDS headers are understood. DDS must be tightly
  packed single-image 2D. sRGB views and premultiplied alpha are rejected: the
  original game shader already handles gamma/signs and blending.
- BC base sizes must be multiples of four for D3D12. PNG and raw DDS can have any
  size, including non-power-of-two dimensions. Dimensions do not change UVs or
  the original sampler's addressing, filtering, LOD clamp or shader flags.
- PNG: Windows WIC, 8-bit, raw channels. Colour images require an explicit alpha
  channel (RGBA or grayscale plus alpha), including opaque images. It is not colour-managed. Colour/alpha
  remain RGBA; normal maps use RG, retaining signedness; scalar textures use R.
- Full DDS mip chains stay compressed and byte-identical. Incomplete BC1/BC3/BC5
  chains decode on workers and regenerate from the base. BC7 DDS requires authored
  complete mips. PNG/raw images generate missing mips. Generated colour/alpha
  mips average stored channels; RG normal mips reconstruct positive Z, average and
  renormalize before storing XY. Odd-sized images include all source rows/columns.
- A smooth-alpha original cannot use BC1, and a normal map cannot use a colour DDS.
  Wrong format, dimensions/metadata, missing files, unsupported API format, invalid
  version, truncated image and allocation failure all retain the original.

## Scheduling and memory

File work runs on a background dispatcher using the bounded texture conversion
worker executor. No manifest/image I/O or mip generation runs on a draw thread.
Requests are bounded to 256; at most four images are processed together; ready
decoded images have a 256 MiB limit. Folder changes invalidate pending generations.
New originals request their replacement after conversion, including task 107's
validated chapter-list preparations. Loading-screen service also publishes completed
replacement uploads before first binds where the recorded resources are available.
An unfamiliar chapter still renders originals until its replacements are ready.

GPU staging does one mip or strip per frame: 2 MiB in gameplay, 16 MiB while loading.
Large images become visible only after the whole chain is uploaded. BC NPOT mip
edges that cannot form legal strip copies use one whole-mip write, bounded by
16 MiB. This bounds work; it is not a guarantee of zero driver/allocation latency.
Originals are never replaced in place. Descriptors and GPU resources use the
renderer's existing retirement lifetime.

Replacement allocations are capped at 512 MiB, at most one eighth of a known GPU
budget, and available space after the normal memory reserve plus strip headroom.
Without a supported live budget query the conservative initial cap is 256 MiB.
Existing low-memory pressure handling drops replacements and keeps originals.
Switching Off parks replacements for quick comparison; clear the folder to free
them immediately (GPU retirement still applies). Actual resident bytes and ready
identities are in the renderer log. GPU-budget samples distinguish adapter usage
from process usage, as in `memory_budget.h`.

## Verification

Resource tests exercise canonical identity, raw channel handling, manifest/version
errors, ambiguous BF aliases, DDS bounds/formats/alpha/signedness, missing mips,
NPOT PNGs and asynchronous generation/budget behavior. `nr_texture_pack_tests` runs
the real renderer on Vulkan and D3D12: every pixel/alpha of a 1025x513 synthetic
replacement, incremental base upload, original-until-ready, descriptor reuse for
On/Off, unchanged original pixels and GPU-budget pressure fallback. No game data
is needed by these tests.

Game test evidence and measurement limits are in the external `codex/REPORT-126.md`.
