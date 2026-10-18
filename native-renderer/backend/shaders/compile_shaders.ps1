# Windows twin of compile_shaders.sh: compiles the backend's shaders into C
# headers under generated/ with DXC (DXIL for D3D12; SPIR-V for Vulkan with
# NVRHI's binding offsets). Needs a DXC with the SPIR-V back end (the GitHub
# release, not the Windows SDK's dxc.exe, which has no -spirv).
# The headers are checked in so the build needs no shader compiler.
#
# Usage: compile_shaders.ps1 -Dxc <path\dxc.exe>
#        compile_shaders.ps1 -Dxc <path\dxc.exe> -Only upscale   (one group: base, ao, gtao, upscale, memory, cel, photo)
param([Parameter(Mandatory = $true)][string]$Dxc, [string]$Only = "")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here "generated"
New-Item -ItemType Directory -Force $out | Out-Null
function Compile-Entries([string]$src, [string[]]$specs, [bool]$shift = $true) {
    # $shift: NVRHI's default Vulkan binding offsets; without: the translated shaders' mapping (the
    # register is the binding), for a pixel shader drawn with a game vertex shader.
    $shifts = if ($shift) { @("-fvk-t-shift", "0", "0", "-fvk-s-shift", "128", "0", "-fvk-b-shift", "256", "0", "-fvk-u-shift", "384", "0") } else { @() }
    foreach ($spec in $specs) {
        $base, $stage, $entry = $spec.Split(":")
        & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $src
        if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $src $entry" }
        & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -spirv "-fspv-target-env=vulkan1.2" @shifts `
            -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $src
        if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $src $entry" }
    }
}
# Cel shading (cel.hlsl, docs/cel.md): one blob per entry point. -Only cel compiles just these.
if ($Only -eq "" -or $Only -eq "cel") {
    Compile-Entries (Join-Path $here "cel.hlsl") @("cel_exposure_cs:cs:cs_exposure", "cel_vs:vs:vs_main", "cel_ps:ps:ps_cel")
    if ($Only -eq "cel") { Write-Host "Shaders written to $out"; exit 0 }
}

# Photo mode (photo.hlsl, docs/photo-mode.md): one blob per entry point. -Only photo compiles just these.
if ($Only -eq "" -or $Only -eq "photo") {
    Compile-Entries (Join-Path $here "photo.hlsl") @("photo_vs:vs:vs_main", "photo_ps:ps:ps_photo")
    if ($Only -eq "photo") { Write-Host "Shaders written to $out"; exit 0 }
}

# Temporal upscalers (upscale.hlsl, docs/upscalers.md): one blob per entry point. The object motion
if ($Only -eq "" -or $Only -eq "memory") {
    Compile-Entries (Join-Path $here "memory_depth.hlsl") @("memory_depth_ps:ps:ps_main", "memory_probe_ps:ps:ps_probe")
    if ($Only -eq "memory") { exit 0 }
}
# pixel shader runs with the translated vertex shaders' motion variants: their binding mapping.
if ($Only -eq "" -or $Only -eq "upscale") {
    $up = Join-Path $here "upscale.hlsl"
    Compile-Entries $up @("up_vs:vs:vs_main", "up_camera_ps:ps:ps_camera_motion", "up_reactive_ps:ps:ps_reactive",
        "up_depth_ps:ps:ps_depth_upsample", "up_stretch_ps:ps:ps_stretch", "up_show_motion_ps:ps:ps_show_motion")
    Compile-Entries $up @("up_object_ps:ps:ps_object_motion") $false
}
if ($Only -ne "" -and $Only -ne "base") {
    if ($Only -eq "upscale") { Write-Host "Shaders written to $out"; exit 0 }
}
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
                    "gtao_smooth_normals_h_cs:cs:cs_smooth_normals_h", "gtao_smooth_normals_v_cs:cs:cs_smooth_normals_v",
                    "gtao_main_cs:cs:cs_main", "gtao_denoise_cs:cs:cs_denoise", "gtao_denoise_last_cs:cs:cs_denoise_last",
                    "gtao_main_low_cs:cs:cs_main_low", "gtao_main_high_cs:cs:cs_main_high",
                    "gtao_smooth_normals_h_cs:cs:cs_smooth_normals_h", "gtao_smooth_normals_v_cs:cs:cs_smooth_normals_v",
                    "gtao_output_cs:cs:cs_output", "gtao_temporal_cs:cs:cs_temporal", "gtao_vs:vs:vs_main", "gtao_apply_ps:ps:ps_apply",
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
                    "ssgi_gather_cs:cs:cs_gather", "ssgi_denoise_cs:cs:cs_denoise", "ssgi_temporal_cs:cs:cs_temporal", "ssgi_vs:vs:vs_main",
                    "ssgi_composite_ps:ps:ps_composite")) {
    $base, $stage, $entry = $spec.Split(":")
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $ssgi
    if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $ssgi $entry" }
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -spirv "-fspv-target-env=vulkan1.2" `
        -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 `
        -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $ssgi
    if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $ssgi $entry" }
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
# The volumetric light (lighting_volumetric.hlsl): renderer passes, the usual flags.
$vol = Join-Path $here "lighting_volumetric.hlsl"
foreach ($spec in @("lighting_volumetric_vs:vs:vs_main", "lighting_volumetric_ps:ps:ps_volumetric",
                    "lighting_volumetric_mask_cs:cs:cs_mask",
                    "lighting_volumetric_radial_cs:cs:cs_radial")) {
    $base, $stage, $entry = $spec.Split(":")
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -Fh (Join-Path $out "${base}_dxil.h") -Vn "k_${base}_dxil" $vol
    if ($LASTEXITCODE -ne 0) { throw "dxc (DXIL) failed on $vol $entry" }
    & $Dxc -nologo -T "${stage}_6_0" -E $entry -O3 -spirv "-fspv-target-env=vulkan1.2" `
        -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 `
        -Fh (Join-Path $out "${base}_spirv.h") -Vn "k_${base}_spirv" $vol
    if ($LASTEXITCODE -ne 0) { throw "dxc (SPIR-V) failed on $vol $entry" }
}
Write-Host "Shaders written to $out"
