# Ambient occlusion prototype (#24)

Not in any release. A test of screen-space ambient occlusion in the GPU plugin.

## How it works

Every gameplay frame, King Kong draws its scene depth first (a depth-only pass) and copies the full-screen depth out of
the console's graphics memory (EDRAM) before lighting. The patched plugin notices that copy, then, at each full-screen
color copy later in the same frame (the scene going to an effect such as water refraction, or to post-processing):

1. `ComputeAO` reads the depth from the plugin's EDRAM buffer and works out the occlusion (12 samples a pixel around
   a normal rebuilt from the depth, faded with distance).
2. `ApplyAO` blurs it (depth-aware) and multiplies it into the scene color in the EDRAM buffer, just before the copy.

EDRAM layout used by the shader: tiles of 80 x 16 pixels times the resolution scale, row-major inside a tile; depth
tiles have their left and right halves swapped; depth is 24-bit in the top bits; color is 8:8:8:8 with red in the low
byte.

## Trying it

1. Apply `rexglue-ao-prototype.patch` (also adds `REX_DEV_FRAME_LOG`) to the SDK source on top of `../rexglue-patches`
   and build the plugin (see `../rexglue-patches/README.md`). Use the result in a test copy only.
2. Run a developer build (`kk-dev`) with `REX_DEV_AO=<path to ao.hlsl>`. The shader is compiled when the game starts.
3. In game: **F8** cycles off / on / AO only, **F9** cycles strength sets 1 to 3 (`ao_mode`, `ao_strength`).

For comparison captures, `REX_DEV_AO_CYCLE=<seconds>` steps through views 0 to 7 (off, depth, AO only 1-3, applied
1-3), and an 8th `REX_DEV_AO_PARAMS` value above 0 stamps the view number (view + 1 squares, top right).
