# HDR output (task 127)

`present_hdr` defaults to Off. When requested, the Windows presenter checks the
monitor containing its own HWND with `IDXGIOutput6::GetDesc1`. HDR is eligible
only when Windows reports `RGB_FULL_G2084_NONE_P2020` for that monitor. The
request is retained when the monitor or OS cannot provide HDR.

The launcher and pause menu expose HDR under Settings > Display. Turning HDR
on shows Peak brightness and Paper white controls. These settings apply live;
the launcher Display reset also resets all three HDR values. F8 retains its
temporary HDR controls.

Both APIs use linear scRGB with BT.709 primaries and FP16 RGBA pixels:

| API | Swapchain format | Color space |
| --- | --- | --- |
| Vulkan | `R16G16B16A16_SFLOAT` | `EXTENDED_SRGB_LINEAR_EXT` |
| DirectX 12 | `R16G16B16A16_FLOAT` | `RGB_FULL_G10_NONE_P709` |

Vulkan optionally enables `VK_EXT_swapchain_colorspace` and requires the exact
advertised format/color-space pair. DirectX 12 checks presentation support and
sets the color space explicitly. Failure to allocate or connect HDR retries SDR
without changing the player's request. There is no HDR10/PQ path or HDR metadata
for this scRGB implementation. One scRGB unit represents 80 cd/m2.

References: [Microsoft Advanced Color](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range),
[Vulkan color spaces](https://docs.vulkan.org/refpages/latest/refpages/source/VkColorSpaceKHR.html).

## Order and appearance

The native renderer keeps its existing SDR output. Its temporal upscale and
game UI precede `GameRenderer::Present`; final blit/gamma ramp and FXAA (or the
SMAA branch's anti-aliasing), followed by the CAS branch's sharpening, finish
before the SDK presenter receives that image. Spatial presenter effects also
finish before HDR conversion. No temporal SDK flags, exposure, motion vectors
or sharpening inputs change for HDR.

In HDR only, the presenter paints the game into BGRA8, and its UI into a
separate transparent BGRA8 layer. Its final shader decodes sRGB once, expands
bright chromatic pixels with a smooth inverse Reinhard shoulder, scales RGB
together to retain hue, and caps the largest component at the chosen peak.
Black and neutral grayscale retain the SDR transfer curve at paper-white
exposure. In particular, white guest subtitles and neutral fog do not become
peak-bright white. Host UI is unpremultiplied, decoded, and composited in linear
light at paper white; its alpha is ordinary Porter-Duff source-over.

The source is already tone-mapped SDR. This cannot recover clipped scene
radiance, infer materials, or distinguish a colored guest HUD pixel from a
colored highlight. Colored skies can also receive highlight expansion. No
claims of native scene HDR or wide-gamut reconstruction are made. The separate
host UI is protected independently of that reconstruction.

`present_hdr_peak` is 80-10000 nits (default 1000), limited by the display's
reported maximum. `present_hdr_paper_white` is 80-500 nits (default 200), limited
by the effective peak. Non-finite values are sanitized before upload. Peak is
a ceiling, so most SDR pixels will remain below it. Saturation is unchanged.

An On/Off change, monitor migration or OS HDR-state change reconnects the
swapchain. Display checks run at most once per second. Resize and disconnect
drain submissions before destroying HDR layers; D3D12 retains the -041 pacing
and Vulkan retains its per-image present semaphores. No extra queued frames or
frame generation are introduced.

## Building and verification

Patch `0043-scrgb-hdr-output.patch` applies cleanly to SDK source -041. This task
continues its previously prepared -043 copy; historical 0009 remains reserved. Build a
new SDK install, and compile the game/plugin against its matching headers and
import library. Do not replace the shared -041 SDK. The task's isolated copy is
`F:/KK-native-renderer/sdk-vulkan-upscalers-043`; `codex/build-hdr-127.ps1`
rebuilds the private objects using read-only -041 dependencies, and its `game`
mode configures the matching SDK and all CTests. Generated shader headers are
included in the patch; the builder regenerates both APIs from the one HLSL
source using DXC and validates SPIR-V.

With `NR_HDR_SDK_SOURCE` pointing at the patched SDK source,
`nr_hdr_output_tests` runs its actual embedded DXIL/SPIR-V into FP16 textures
on both APIs without a window or game data. It checks known scRGB values, UI
alpha, black, neutral whites, grayscale monotonicity, highlight reconstruction,
format-pair selection and peak/finite clamps. Optional input/output arguments
read an SDR RGBA image and capture the converted GPU buffer as floats.
`native-renderer/tests/hdr-evidence-127.py` exports this readback as a small
linear BT.709 FLOAT EXR (1 = 80 nits, preserving the FP16 values exactly) and an explicitly SDR preview. Float output
is captured even if Windows HDR is off, without forcing an HDR swapchain on an
SDR display. Live game runs use `codex/run-hdr-127.ps1`, the shared game guard,
private user data, windowed mode and the background helper, with a four-run cap.

For actual D3D12 swapchain evidence, `REX_DEV_HDR_CAPTURE=<owned path>` records
one final FP16 image after 1800 HDR presents, including host UI. This explicitly
requested developer capture waits for its GPU copy and writes width, height
and tightly packed HALF RGBA pixels. Normal play allocates no readback buffer
and adds no fence wait. The evidence tool's `capture` mode exports it to EXR.

## Highlight expansion (task 262 prototype, SDK patch 0044, off by default)

The game renders only 8-bit 0..1 colour targets (frame logs of V-Rex, chapters 4/12, Kong, swamps: every colour
draw is `k_8_8_8_8`, the only other target is `k_32_FLOAT` depth copies; no 7e3/FP16 targets, no exposure pass),
so there is no scene radiance above 1.0 to keep. `present_hdr_highlights` (0..1, default 0) instead expands
the SDR picture's highlights in the presenter's HDR pass (`guest_output_hdr.hlsl`, `MapHighlights`):

- level = luminance lifted halfway toward the largest component (fire and coloured lights count too);
- at or below `present_hdr_knee` (linear, default 0.5 = sRGB byte 187) a pixel is exactly `c * paper`;
- above it the gain rises in log2 as t^2 (C1 at the knee) to ceiling / paper at SDR white; RGB scaled together;
- ceiling = peak, moved toward `present_hdr_full_frame` when the surroundings (12 taps on rings at 3% / 8% of
  the height) are bright too, so skies, white fades and bright sea stay at the display's full-frame brightness;
- `present_hdr_black` (the display's black, nits) lifts black with a BT.2390-style toe that fades by paper white;
- one code value of triangular dither in the expanded range (not on clipped white) against 8-bit banding.

At 0 the task 127 path runs unchanged (byte-identical FP16 output, checked on both APIs). The game sets
`present_hdr_black` / `present_hdr_full_frame` from the Windows HDR Calibration profile, else DXGI, else 0
(`kk::hdr::AutoRange`); peak and paper white still come from `kk_hdr_levels` (task 219). F8 shows the two new
sliders only with an SDK that has the cvars. Build: `native-renderer/tests/build-sdk-262.py` (private
`sdk-src-real-hdr-262` -> `sdk-vulkan-real-hdr-262`); evidence: `native-renderer/tests/hdr-highlights-262.py`.
Known limit: the guest HUD/subtitles are inside the game image, so pure white text also expands toward peak.
