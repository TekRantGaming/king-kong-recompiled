$ErrorActionPreference = 'Stop'
$dxc = 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxc.exe'
$folder = (Resolve-Path "$PSScriptRoot\..\backend\shaders").Path
foreach ($spec in @('prepare:cs:cs_prepare','prefilter:cs:cs_prefilter','normals:cs:cs_normals','main:cs:cs_main',
    'denoise:cs:cs_denoise','denoise_last:cs:cs_denoise_last','output:cs:cs_output','temporal:cs:cs_temporal',
    'vs:vs:vs_main','apply:ps:ps_apply','apply_resolve:ps:ps_apply_resolve')) {
  $name,$stage,$entry = $spec.Split(':')
  $base = if ($name -eq 'vs') {'gtao_vs'} else {"gtao_${name}_${stage}"}
  & $dxc -nologo -T "${stage}_6_0" -E $entry -O3 -Fh "$folder\generated\${base}_dxil.h" -Vn "k_${base}_dxil" "$folder\gtao.hlsl"
  if ($LASTEXITCODE) { throw "DXIL $entry failed" }
  & $dxc -nologo -T "${stage}_6_0" -E $entry -O3 -spirv '-fspv-target-env=vulkan1.2' `
    -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 `
    -Fh "$folder\generated\${base}_spirv.h" -Vn "k_${base}_spirv" "$folder\gtao.hlsl"
  if ($LASTEXITCODE) { throw "SPIR-V $entry failed" }
  foreach ($format in @('dxil','spirv')) {
    $path = "$folder\generated\${base}_${format}.h"
    $lines = [IO.File]::ReadAllLines($path) | ForEach-Object { $_.TrimEnd() }
    [IO.File]::WriteAllText($path, ($lines -join "`n") + "`n", (New-Object Text.UTF8Encoding $false))
  }
  Write-Output "Compiled $base"
}
