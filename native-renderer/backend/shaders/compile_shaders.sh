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
for name in triangle placeholder; do
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
echo "Shaders written to $out"
