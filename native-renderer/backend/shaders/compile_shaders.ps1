# Windows twin of compile_shaders.sh: compiles the backend's shaders into C
# headers under generated/ with DXC (DXIL for D3D12; SPIR-V for Vulkan with
# NVRHI's binding offsets). Needs a DXC with the SPIR-V back end (the GitHub
# release, not the Windows SDK's dxc.exe, which has no -spirv).
# The headers are checked in so the build needs no shader compiler.
#
# Usage: compile_shaders.ps1 -Dxc <path\dxc.exe>
param([Parameter(Mandatory = $true)][string]$Dxc)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here "generated"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($name in @("triangle", "placeholder", "blit", "clear")) {
    $src = Join-Path $here "$name.hlsl"
    foreach ($stage in @("vs", "ps")) {
        $base = "${name}_${stage}"
        & $Dxc -nologo -T "${stage}_6_0" -E "${stage}_main" -O3 -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $src
        if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $src $stage" }
        & $Dxc -nologo -T "${stage}_6_0" -E "${stage}_main" -O3 -spirv "-fspv-target-env=vulkan1.2" `
            -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 `
            -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $src
        if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $src $stage" }
    }
}
Write-Host "Shaders written to $out"
