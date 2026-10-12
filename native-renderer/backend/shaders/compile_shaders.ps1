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
# Ambient occlusion: one source, one blob per entry point.
$ao = Join-Path $here "ambient_occlusion.hlsl"
foreach ($spec in @("ao_vs:vs:vs_main", "ao_copy_ps:ps:ps_copy", "ao_depth_ps:ps:ps_depth", "ao_ao_ps:ps:ps_ao",
                    "ao_blur_ps:ps:ps_blur", "ao_upsample_ps:ps:ps_upsample", "ao_apply_ps:ps:ps_apply")) {
    $base, $stage, $entry = $spec.Split(":")
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $ao
    if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $ao $entry" }
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -spirv "-fspv-target-env=vulkan1.2" `
        -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 `
        -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $ao
    if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $ao $entry" }
}
# GTAO (gtao.hlsl, docs/ao.md): compute passes and the apply passes, one blob per entry point.
$gtao = Join-Path $here "gtao.hlsl"
foreach ($spec in @("gtao_prepare_cs:cs:cs_prepare", "gtao_prefilter_cs:cs:cs_prefilter", "gtao_normals_cs:cs:cs_normals",
                    "gtao_main_cs:cs:cs_main", "gtao_denoise_cs:cs:cs_denoise", "gtao_denoise_last_cs:cs:cs_denoise_last",
                    "gtao_output_cs:cs:cs_output", "gtao_vs:vs:vs_main", "gtao_apply_ps:ps:ps_apply",
                    "gtao_apply_resolve_ps:ps:ps_apply_resolve")) {
    $base, $stage, $entry = $spec.Split(":")
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $gtao
    if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $gtao $entry" }
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -spirv "-fspv-target-env=vulkan1.2" `
        -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 `
        -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $gtao
    if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $gtao $entry" }
}
# Screen-space GI (ssgi.hlsl, docs/gi.md): one blob per entry point.
$ssgi = Join-Path $here "ssgi.hlsl"
foreach ($spec in @("ssgi_radiance_cs:cs:cs_radiance", "ssgi_radiance_down_cs:cs:cs_radiance_down",
                    "ssgi_gather_cs:cs:cs_gather", "ssgi_denoise_cs:cs:cs_denoise", "ssgi_vs:vs:vs_main",
                    "ssgi_composite_ps:ps:ps_composite")) {
    $base, $stage, $entry = $spec.Split(":")
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $ssgi
    if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $ssgi $entry" }
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -spirv "-fspv-target-env=vulkan1.2" `
        -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 `
        -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $ssgi
    if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $ssgi $entry" }
}
Write-Host "Shaders written to $out"
