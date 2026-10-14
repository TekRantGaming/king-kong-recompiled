# Hardware recommendations (Windows)

On a first launch with no settings file, the port detects the high-performance
DXGI adapter's vendor, model and dedicated VRAM, Windows physical CPU core count,
usable system RAM and primary desktop resolution/refresh. It chooses a tier from
`kk/src/hardware_recommendation.h`. The launcher Graphics page's **Recommended**
button detects hardware again and updates defaults for settings the player has
not explicitly chosen. It is disabled under Original look and absent in-game.

| Tier | Conditions (first matching rule wins) | Render/upscaler mode | GTAO | Resolution | Samples |
|---|---|---|---|---|---|
| Low | Steam Deck; unknown vendor; less than 4 GiB dedicated VRAM (including integrated/shared-memory GPUs); fewer than 4 physical cores; less than 8 GiB usable RAM; recognized entry/old model | Performance | Off | Half if enabled manually | 1 |
| High | Recognized modern model, at least 10 GiB VRAM, 6 physical cores and 16 GiB usable RAM | Quality | On | Auto | 3 |
| Mid | All remaining dedicated hardware | Quality, Performance above 2560x1440 pixels | On | Half | 2 |

Entry/old model matches: GT 7xx/10xx, GTX 10xx/16xx, RTX 3050, RX 5xx,
RX 6400/6500 and Arc A380. High-eligible matches: RTX 30/40/50xx,
RX 67/68/69/77/78/79/90xx, Arc A770 and B580. These are conservative string
heuristics, not benchmark scores. An unrecognized model with large VRAM stays
Mid. Missing RAM/core/VRAM detection selects Low. Steam Deck is always Low.
Windows may report slightly less usable RAM than the amount installed.

Render modes use the existing render presets (Quality 1.5:1 and Performance
2:1 per side). Spatial rendering retains the port's integer 720p steps; temporal
rendering uses the SDK's mode. Output dimensions supplied by the launcher/game
determine the pixel-count adjustment; primary desktop refresh is diagnostic
only. Neither refresh nor tier changes the frame cap.

Upscaler defaults remain DLSS on NVIDIA, XeSS on Intel, and FSR on AMD, with
FSR 3.1 as the temporal fallback and FSR 1 when no temporal SDK runs. The existing
asynchronous native-plugin query tests SDK feature support for the selected API
and refines these defaults. Until its answer arrives (including a launcher-free
first launch), selection uses the vendor, and the renderer retains its existing
runtime fallback. FSR 4 remains available only on a supported AMD card when the
player explicitly chooses D3D12. Recommendations never select D3D12: Auto remains
Vulkan. Native device requirement checks still reject an unsupported device;
a Low tier does not make an unsupported Vulkan GPU supported.

The chosen `kk_hardware_tier` is saved with settings. Later launches reconstruct
that tier's defaults; a hardware change alone does not reclassify an install.
Existing installs with no marker are left alone until Recommended is selected.
Config, environment, command-line and runtime choices take precedence. Explicit
choices equal to the default are saved for the settings covered by this policy,
so they remain explicit after restarting. Custom render scales and Original look
are retained. Resetting a setting to its default opts it back into recommendations.

Recommendations leave frame cap (default 30), API, renderer and the new GI,
shadow, SSR and volumetric effects untouched. There is no frame generation or
interpolation. First-time Remastered setup uses this policy and keeps the cap;
it no longer forces a 60 FPS preset.

These starting points have fake-profile coverage, not certification on AMD,
Intel or integrated hardware. Task 012's memory/bandwidth and Vulkan binding
limits still require actual hardware testing. Task 044 benchmarks can refine
the policy later; this feature has no benchmark dependency.

With `KK_DEV_TOOLS=ON`, `kk-hardware-tests` links the actual settings code with
fake OS detection. Run it from the build directory or use CTest's
`hardware-recommendation` test. It covers NVIDIA/AMD/Intel Low/Mid/High,
integrated GPUs, Steam Deck, unknown hardware, CPU/RAM limits, 4K output,
all SDK support combinations and settings persistence/source precedence.
The test retains its small config evidence directory and never launches a game.
