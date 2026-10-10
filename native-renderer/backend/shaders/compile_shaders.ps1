# Compiles the backend's test shaders into C headers under generated/:
#   DXIL (shader model 6.0) with the Windows SDK's dxc.exe, for D3D12
#   SPIR-V with glslangValidator (its HLSL front end), for Vulkan
# The generated headers are checked in so the build needs neither tool.
#
# Usage: compile_shaders.ps1 [-Dxc <path\dxc.exe>] [-Glslang <path\glslangValidator.exe>]
# glslangValidator can be built from the SDK's sources (C:\rexsrc\thirdparty\glslang,
# cmake with ENABLE_HLSL=ON); dxc.exe is in the Windows SDK's bin folder.
param(
    [string]$Dxc = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe",
    [string]$Glslang = ""
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here "generated"
New-Item -ItemType Directory -Force $out | Out-Null

$shaders = @(
    @{ File = "triangle.hlsl";    Name = "triangle" },
    @{ File = "placeholder.hlsl"; Name = "placeholder" }
)
$stages = @(
    @{ Entry = "vs_main"; Profile = "vs_6_0"; Glsl = "vert"; Suffix = "vs" },
    @{ Entry = "ps_main"; Profile = "ps_6_0"; Glsl = "frag"; Suffix = "ps" }
)

foreach ($s in $shaders) {
    $src = Join-Path $here $s.File
    foreach ($st in $stages) {
        $base = "$($s.Name)_$($st.Suffix)"
        # DXIL: dxc writes a header with a byte array named by -Vn.
        $dxil = Join-Path $out "${base}_dxil.h"
        & $Dxc -T $st.Profile -E $st.Entry -O3 -Fh $dxil -Vn "k_$($base)_dxil" $src
        if ($LASTEXITCODE -ne 0) { throw "dxc failed on $src $($st.Entry)" }
        # SPIR-V: glslang's HLSL front end, with NVRHI's Vulkan binding layout
        # (shader resources at 0, samplers at 128, constant buffers at 256,
        # unordered access at 384).
        if ($Glslang -ne "") {
            $spv = Join-Path $out "${base}_spirv.h"
            & $Glslang -V -D -e $st.Entry -S $st.Glsl --auto-map-locations --auto-map-bindings `
                --shift-sampler-binding 128 --shift-cbuffer-binding 256 --shift-uav-binding 384 `
                --vn "k_$($base)_spirv" -o $spv $src
            if ($LASTEXITCODE -ne 0) { throw "glslang failed on $src $($st.Entry)" }
        }
    }
}
Write-Host "Shaders written to $out"
