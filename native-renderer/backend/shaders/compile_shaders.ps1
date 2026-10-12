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
# The lighting options (docs/lighting.md). The soft shadows replace the game's psshadowquad, so they are
# built like the game's translated shaders (kkshaders' compiler.cpp flags: HLSL 2021, -Gis, the DX layout,
# no binding shifts) with the translator's prelude: one blob per variant of it (lights, constant
# registers) and per quality (PCF or PCSS). Recompile when shaders/hlsl/kk_common.hlsli changes.
$shadow = Join-Path $here "lighting_shadow.hlsl"
foreach ($spec in @("1:5:4", "2:8:10", "3:12:15")) {
    $lights, $params, $color = $spec.Split(":")
    foreach ($q in @("pcf:0", "pcss:1")) {
        $qname, $pcss = $q.Split(":")
        $base = "lighting_shadow_${lights}_${qname}"
        $defs = @("-D", "KL_LIGHTS=$lights", "-D", "KL_PARAMS=$params", "-D", "KL_COLOR=$color", "-D", "KL_PCSS=$pcss")
        & $Dxc -nologo -T ps_6_0 -E main -HV 2021 -O3 -Gis -Qstrip_debug @defs -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $shadow
        if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $shadow $base" }
        & $Dxc -nologo -T ps_6_0 -E main -HV 2021 -O3 -Gis -Qstrip_debug @defs -spirv "-fspv-target-env=vulkan1.2" `
            -fvk-use-dx-layout -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $shadow
        if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $shadow $base" }
    }
}
# The water reflections' preparation (lighting_ssr.hlsl): renderer compute passes, the usual flags.
$ssr = Join-Path $here "lighting_ssr.hlsl"
foreach ($spec in @("lighting_ssr_depth_cs:cs:cs_depth", "lighting_ssr_depth_down_cs:cs:cs_depth_down",
                    "lighting_ssr_color_cs:cs:cs_color", "lighting_ssr_color_down_cs:cs:cs_color_down")) {
    $base, $stage, $entry = $spec.Split(":")
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $ssr
    if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $ssr $entry" }
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -spirv "-fspv-target-env=vulkan1.2" `
        -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 `
        -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $ssr
    if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $ssr $entry" }
}
# The volumetric light (lighting_volumetric.hlsl): a renderer pass, the usual flags.
$vol = Join-Path $here "lighting_volumetric.hlsl"
foreach ($spec in @("lighting_volumetric_vs:vs:vs_main", "lighting_volumetric_ps:ps:ps_volumetric")) {
    $base, $stage, $entry = $spec.Split(":")
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $vol
    if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $vol $entry" }
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -spirv "-fspv-target-env=vulkan1.2" `
        -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 `
        -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $vol
    if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $vol $entry" }
}
Write-Host "Shaders written to $out"
