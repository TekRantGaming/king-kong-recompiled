# Ambient occlusion prototype (#24)

Not in any release. A test of screen-space ambient occlusion in the GPU plugin.

## How it works

Every gameplay frame, King Kong draws its scene depth first (a depth-only pass) and copies the full-screen depth out of
the console's graphics memory (EDRAM) before lighting. The patched plugin notices that copy. At the next full-screen
color copy in that frame (the scene going to an effect such as water refraction, or to post-processing), and at any
later copy to the same place, it:

1. once a frame, at half resolution: reads the depth from its EDRAM buffer (`PrepareDepth`), works out the occlusion
   (`ComputeAO`: 10 samples around a normal rebuilt from the depth), blurs it with a depth-aware 9-tap blur each way
   (`BlurH`, `BlurV`), then scales it up to full resolution, edge-aware, fading it out with distance (`Upsample`);
2. at each such copy: multiplies it into the scene color in the EDRAM buffer just before the copy (`ApplyAO`).

The final copy of the post-processed image goes elsewhere, so the AO isn't applied twice and the HUD isn't touched.
The AO is squared, which matches the look first tested (when it was applied twice by mistake).

EDRAM layout used by the shader: tiles of 80 x 16 pixels times the resolution scale, row-major inside a tile; depth
tiles have their left and right halves swapped; depth is 24-bit in the top bits; color is 8:8:8:8 with red in the low
byte.

## Cost

GPU time at 3x (3840 x 2160), V-Rex, strength 1, RTX 4070 Ti, from `REX_DEV_AO_STATS`:

| | ms a frame |
| --- | ---: |
| depth to half resolution | 0.03 |
| AO | 0.25 - 0.45 |
| blur | 0.08 |
| upsample | 0.10 - 0.12 |
| applying (3 scene copies in V-Rex) | 0.23 - 0.35 |
| total | about 0.7 - 1.0 |

The first version (full resolution, 32-bit images, blur at every copy) took about 6 ms.

## Trying it

1. Apply `rexglue-ao-prototype.patch` (also adds `REX_DEV_FRAME_LOG`) to the SDK source on top of `../rexglue-patches`
   and build the plugin (see `../rexglue-patches/README.md`). Use the result in a test copy only.
2. Run a developer build (`kk-dev`) with `REX_DEV_AO=<path to ao.hlsl>`. The shader is compiled when the game starts.
3. In game: **F8** cycles off / on / AO only, **F9** cycles strength sets 1 to 3 (`ao_mode`, `ao_strength`).

Testing switches: `REX_DEV_AO_STATS=1` logs GPU time per pass every 2 seconds; `REX_DEV_AO_TOGGLE=<seconds>` switches
AO on and off to compare; `REX_DEV_AO_CYCLE=<seconds>` steps through views 0 to 7 (off, depth, AO only 1-3, applied
1-3), and an 8th `REX_DEV_AO_PARAMS` value above 0 stamps the view number (view + 1 squares, top right);
`REX_DEV_AO_TRACE=<count>` logs the full-screen copies it sees.
