#!/bin/sh
# Compiles the backend's shaders into C headers under generated/ with DXC
# (the Linux release from github.com/microsoft/DirectXShaderCompiler has both
# back ends and libdxil.so, so the DXIL comes out signed):
#   DXIL, shader model 6.0, for D3D12
#   SPIR-V (Vulkan 1.2) with NVRHI's Vulkan binding offsets: shader resources
#   at 0, samplers at 128, constant buffers at 256, unordered access at 384
# The headers are checked in so the build needs no shader compiler.
#
# Usage: compile_shaders.sh [path/to/dxc]   (default: dxc on PATH)
set -eu
DXC=${1:-dxc}
here=$(cd "$(dirname "$0")" && pwd)
out="$here/generated"
mkdir -p "$out"
for name in triangle placeholder blit clear; do
  src="$here/$name.hlsl"
  for stage in vs ps; do
    entry="${stage}_main"
    profile="${stage}_6_0"
    base="${name}_${stage}"
    "$DXC" -nologo -T "$profile" -E "$entry" -O3 -Fh "$out/${base}_dxil.h" -Vn "k_${base}_dxil" "$src"
    "$DXC" -nologo -T "$profile" -E "$entry" -O3 -spirv -fspv-target-env=vulkan1.2 \
      -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 \
      -Fh "$out/${base}_spirv.h" -Vn "k_${base}_spirv" "$src"
  done
done
# Ambient occlusion: one source, one blob per entry point (output name, stage, entry).
src="$here/ambient_occlusion.hlsl"
for spec in ao_vs:vs:vs_main ao_copy_ps:ps:ps_copy ao_depth_ps:ps:ps_depth ao_ao_ps:ps:ps_ao \
            ao_blur_ps:ps:ps_blur ao_upsample_ps:ps:ps_upsample ao_apply_ps:ps:ps_apply; do
  base=${spec%%:*}; rest=${spec#*:}; stage=${rest%%:*}; entry=${rest#*:}
  "$DXC" -nologo -T "${stage}_6_0" -E "$entry" -O3 -Fh "$out/${base}_dxil.h" -Vn "k_${base}_dxil" "$src"
  "$DXC" -nologo -T "${stage}_6_0" -E "$entry" -O3 -spirv -fspv-target-env=vulkan1.2 \
    -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 \
    -Fh "$out/${base}_spirv.h" -Vn "k_${base}_spirv" "$src"
done
# GTAO (gtao.hlsl, docs/ao.md): compute passes and the apply passes, one blob per entry point.
src="$here/gtao.hlsl"
for spec in gtao_prepare_cs:cs:cs_prepare gtao_prefilter_cs:cs:cs_prefilter gtao_normals_cs:cs:cs_normals \
            gtao_main_cs:cs:cs_main gtao_denoise_cs:cs:cs_denoise gtao_denoise_last_cs:cs:cs_denoise_last \
            gtao_main_low_cs:cs:cs_main_low gtao_main_high_cs:cs:cs_main_high \
            gtao_smooth_normals_h_cs:cs:cs_smooth_normals_h gtao_smooth_normals_v_cs:cs:cs_smooth_normals_v \
            gtao_output_cs:cs:cs_output gtao_temporal_cs:cs:cs_temporal gtao_vs:vs:vs_main gtao_apply_ps:ps:ps_apply \
            gtao_apply_resolve_ps:ps:ps_apply_resolve; do
  base=${spec%%:*}; rest=${spec#*:}; stage=${rest%%:*}; entry=${rest#*:}
  "$DXC" -nologo -T "${stage}_6_0" -E "$entry" -O3 -Fh "$out/${base}_dxil.h" -Vn "k_${base}_dxil" "$src"
  "$DXC" -nologo -T "${stage}_6_0" -E "$entry" -O3 -spirv -fspv-target-env=vulkan1.2 \
    -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 \
    -Fh "$out/${base}_spirv.h" -Vn "k_${base}_spirv" "$src"
done
# Screen-space GI (ssgi.hlsl, docs/gi.md): one blob per entry point.
src="$here/ssgi.hlsl"
for spec in ssgi_radiance_cs:cs:cs_radiance ssgi_radiance_down_cs:cs:cs_radiance_down ssgi_gather_cs:cs:cs_gather             ssgi_denoise_cs:cs:cs_denoise ssgi_temporal_cs:cs:cs_temporal ssgi_vs:vs:vs_main ssgi_composite_ps:ps:ps_composite; do
  base=${spec%%:*}; rest=${spec#*:}; stage=${rest%%:*}; entry=${rest#*:}
  "$DXC" -nologo -T "${stage}_6_0" -E "$entry" -O3 -Fh "$out/${base}_dxil.h" -Vn "k_${base}_dxil" "$src"
  "$DXC" -nologo -T "${stage}_6_0" -E "$entry" -O3 -spirv -fspv-target-env=vulkan1.2     -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0     -Fh "$out/${base}_spirv.h" -Vn "k_${base}_spirv" "$src"
done
# Official SMAA v2.8 1x (both lookup textures are embedded by smaa.cpp).
src="$here/smaa.hlsl"
for part in edge weight blend; do
  for stage in vs ps; do
    base="smaa_${part}_${stage}"
    "$DXC" -nologo -T "${stage}_6_0" -E "${stage}_${part}" -O3 -Fh "$out/${base}_dxil.h" -Vn "k_${base}_dxil" "$src"
    sed -i 's/[[:blank:]]*$//' "$out/${base}_dxil.h"
    "$DXC" -nologo -T "${stage}_6_0" -E "${stage}_${part}" -O3 -spirv -fspv-target-env=vulkan1.2 \
      -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 \
      -Fh "$out/${base}_spirv.h" -Vn "k_${base}_spirv" "$src"
  done
done
echo "Shaders written to $out"
