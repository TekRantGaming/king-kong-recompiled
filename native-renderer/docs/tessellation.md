# Experimental Ann tessellation

F8 has Tessellation (Off), Smoothness (0.75, range 0 to 1), Detail (4, range 1 to 16),
and the GPU time of the admitted draws. Launcher settings and automatic recommendations do not enable it.
The panel uses the same English labels as its existing rows; it has no translation lookup.

This prototype affects Ann's separate 63-index head-material piece only, using her face/hair atlas.
Its exact anatomical/pixel coverage was not isolated. Her observed main head, clothing, mouth and eye
meshes have depth permutations that fetch position and skinning inputs but no normal. They retain
their original geometry in every pass, as required by the missing-normal fallback. This is not a
smoothed face or body implementation. The panel reports when there is no eligible prepared Ann draw.

`backend/game_tessellation.cpp` admits two verified hair material passes, using their original vertex
and pixel microcode hashes plus content hashes of both bound textures. Canonical base-level compressed
blocks are hashed after untiling/endian conversion. Dimensions, host formats and normal availability
are checked too. Texture-cache uploads invalidate the cached identity. Resolved, aliased, dirty,
missing or changed textures fail closed. No guest allocation address identifies Ann.

`shaders/src/tessellation.cpp` exports the actual shaders' skinned position and normal at verified
instruction boundaries, in view space. Hull/control stages use a uniform integer factor. Domain/evaluation
stages project the Phong displacement and add it to the interpolated original clip position, retaining
the game's viewport, jitter and clip-plane conventions. Varyings keep the original material signature.
The motion variant evaluates current and previous skinning at identical domain coordinates; previous
smoothness is zero when the previous frame did not use the experimental geometry.

Both material pipelines and, with a temporal upscaler, their depth-writing pass's motion pipeline must
already be prepared before either pass switches. Live draws never compile a tessellation stage or
create a missing tessellation pipeline. Unsupported devices, missing packs, unprepared records and
unrecognised states keep the original path. Vulkan checks the SDK's enabled tessellation feature;
D3D12 has the corresponding hull/domain path.

The normal pack builder always includes both admitted permutations' six stage entries (ordinary and
motion VS/HS/DS) for its API. The pack stamp's tessellation revision invalidates older packs. Pipeline
prewarm adds the tessellated and motion descriptions from the original recorded descriptions.
Fresh or incomplete pipeline lists can therefore leave the effect unavailable instead of compiling
it during play.

Developer preparation: `kkshaders ann-tess <database> <base-pack> --out <pack> --dxc <folder>` extends
an inherited pack while preserving its index aliases. It compiles both APIs' tessellation stages,
the ordinary/motion vertex shaders and the two material pixel shaders. Generated packs and HLSL
are game-derived and must remain outside git.

`REX_DEV_FRAME_LOG` plus `REX_DEV_MATERIAL_DUMP=<folder>` logs canonical texture identities, index/vertex
content identities, skinning-palette hashes and the original shader identities for the selected frames.
It exports translated vertex HLSL and canonical texture blocks. This is expensive diagnostic work,
not a performance capture mode. `KK_DEV_TESS_BENCH=<folder>` is a developer-only Off/4/16 sequence through
the F8 change path; `REX_DEV_TESS_BENCH=1` also times admitted original draws for the Off baseline.

`nr_ann_tessellation_tests <prepared-pack> <dxc-folder> [d3d12]` renders the actual packed stages on
synthetic vertices. It checks alpha-zero coverage, curved silhouettes, original-material pipeline
linkage and depth-equal motion replay for both permutations at factors 4 and 16. `NR_BENCH=1` reports
GPU time per synthetic 21-triangle draw; this is not a game FPS prediction. Optional
`NR_TESS_PREWARM_LIST` and `NR_TESS_PREWARM_FILTERED` paths check production prewarm against recorded
material states. Reports and game captures for task 072 live in the coordinator's documentary folder.
