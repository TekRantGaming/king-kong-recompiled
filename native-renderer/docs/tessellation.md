# Experimental Ann tessellation

F8 has Tessellation (Off), Smoothness (0.75, range 0 to 1), Detail (4, range 1 to 16),
and the GPU time of the admitted draws. Launcher settings and automatic recommendations do not enable it.
The panel uses the same English labels as its existing rows; it has no translation lookup.

The selector admits Ann's verified 5298-index clothing/body, 3024/2901-index head/skin,
7779-index main face/head, and the original 63-index head-material piece. Mouth/eye materials
shared with other actors remain original. No whole-character shader family is an actor identity.

`backend/game_tessellation.cpp` joins the main depth and lit draws by the exact consumed
index/vertex content hash used by the material census. Lit admission additionally requires the
verified VS/PS pair and both canonical texture hashes, dimensions and formats. Depth learns
NORMAL's declaration, buffer source, stream offset, stride, format and endian from that paired
lit draw; it binds this source at reserved vertex-fetch slot 15, even when depth has no NORMAL
in its declaration or its stream 1 binding belongs to another draw. The first observed frame
stays original. A changed normal source's content, unknown geometry, unavailable normal, or
unprepared pipeline keeps the part original. Buffer addresses locate the learned source;
content hashes and paired material keys establish identity.

Only referenced skinning-palette rows are compared across depth and lit. Unused registers may
contain stale lighting/projection constants, so hashing the entire declared palette would
incorrectly reject the same skinned mesh. The supported main layouts use integer UBYTE4 bone
indices in the hashed stream 0. Different layouts fail closed. Chapter 1 uses only the first
main lighting pass; chapter 2 uses both. Every observed lit pass and the current depth pass,
plus depth motion when upscaling, must have prepared tessellation pipelines.

`shaders/src/tessellation.cpp` exports the actual shaders' skinned position and normal at verified
instruction boundaries, in view space for the original small head piece. Main pieces instead capture the translated
palette blend's skinned model position and transform the artist normal by those identical
blended rows. Depth adds this normal read without changing its original skinning or constants;
all main variants project the same model-space Phong displacement through their own verified WVP
registers. Hull/control stages use a uniform integer factor. Domain/evaluation
stages project the Phong displacement and add it to the interpolated original clip position, retaining
the game's viewport, jitter and clip-plane conventions. Varyings keep the original material signature.
The motion variant evaluates current and previous skinning at identical domain coordinates; previous
smoothness is tracked per admitted main geometry, including first admission and the transition
back to Off (one zero-smoothness replay frame).

Both material pipelines and, with a temporal upscaler, their depth-writing pass's motion pipeline must
already be prepared before either pass switches. Live draws never compile a tessellation stage or
create a missing tessellation pipeline. Unsupported devices, missing packs, unprepared records and
unrecognised states keep the original path. Vulkan checks the SDK's enabled tessellation feature;
D3D12 has the corresponding hull/domain path.

The normal pack builder always includes all eight admitted shader hashes' six stage entries (ordinary and
motion VS/HS/DS) for its API. The pack stamp's tessellation revision invalidates older packs. Pipeline
prewarm adds the tessellated and motion descriptions from the original recorded descriptions.
Fresh or incomplete pipeline lists can therefore leave the effect unavailable instead of compiling
it during play.

Developer preparation: `kkshaders ann-tess <database> <base-pack> --out <pack> --dxc <folder>` extends
an inherited pack while preserving its index aliases. It compiles both APIs' tessellation stages for all translation inputs,
the ordinary/motion vertex shaders and the verified material pixel shaders. Generated packs and HLSL
are game-derived and must remain outside git.

`REX_DEV_FRAME_LOG` plus `REX_DEV_MATERIAL_DUMP=<folder>` logs canonical texture identities, index/vertex
content identities, skinning-palette hashes and the original shader identities for the selected frames.
It exports translated vertex HLSL and canonical texture blocks. This is expensive diagnostic work,
not a performance capture mode. `KK_DEV_TESS_BENCH=<folder>` is a developer-only Off/4/16 sequence through
the F8 change path; `REX_DEV_TESS_BENCH=1` also times admitted original draws for the Off baseline.

`nr_ann_tessellation_tests <prepared-pack> <dxc-folder> [d3d12]` renders the actual packed stages on
synthetic vertices. It checks alpha-zero coverage, curved silhouettes, original-material pipeline
linkage and depth-equal motion replay for all eight shader hashes at factors 4 and 16. Separate depth/lit replay checks require zero missing pixels and matching motion under blended skinning. `NR_BENCH=1` reports
GPU time per synthetic 21-triangle draw; this is not a game FPS prediction. Optional
`NR_TESS_PREWARM_LIST` and `NR_TESS_PREWARM_FILTERED` paths check production prewarm against recorded
material states. Reports and game captures live in the coordinator's documentary/tess and documentary/tess2 folders. Task 077's pack stamp revision is 3.
