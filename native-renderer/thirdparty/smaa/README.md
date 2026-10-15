Official SMAA v2.8 release, tag c9ff6c0 (23 August 2013).
Source: https://github.com/iryoku/smaa/releases/tag/v2.8
Archive SHA-256: f6192b0759f3a9f52769f328832f61870849d2ba19dbae7718e0d76d3411befc

SMAA.h, AreaTex.h, SearchTex.h and LICENSE.txt come from the existing approved
task 122 download. The licence is the release's BSD-style licence, retained
here and in kk/THIRD-PARTY-NOTICES.txt, with its required binary credit.

Whitespace is normalized. The only shader changes are conversion of the source's Windows-1252 text to
UTF-8 and replacing the two FX sampler declarations with explicit s0/s1
bindings. The NVRHI samplers preserve their filter and clamp modes. Lookup
texel arrays are unmodified, with RG8_UNORM area and R8_UNORM search uploads.

The wrapper uses SMAA 1x, High, luma edges, zero subsample indices and no
temporal pass. As permitted by v2.8's integration instructions, neighborhood
blending uses the display image's gamma space, matching the existing spatial
AA path. Edges and weights use non-sRGB RGBA8 images cleared every frame.
Both API shader blobs and lookup images are built into the plugin; game shader
packs need no extra files. Renderer initialization and PipelinePrewarm create
the three pipelines before gameplay, including the native frame formats.
