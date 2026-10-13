param([switch]$SkipHandsBaseline, [switch]$SkipBaselines)
$ErrorActionPreference = 'Stop'
$baseline = (Resolve-Path "$PSScriptRoot\..\..\kk\out\task016-baseline").Path
$run = "$PSScriptRoot\task016-run.ps1"
$off = @('--native_upscaler=off','--kk_original_look=true','--kk_frame_rate=30')
$sdk = @('--kk_original_look=true','--kk_frame_rate=30','--kk_original_look_keep=ao_mode,ao_strength,native_upscaler',
         '--native_upscaler_quality=native','--native_upscaler_debug=16')
if (!$SkipBaselines) {
  if (!$SkipHandsBaseline) { & $run -Label before-hands -Entries @(8) -Build $baseline -Hands -ExtraArgs $off }
  & $run -Label before-dlss -Entries @(2,8) -Build $baseline -Seconds 90 -ExtraArgs ($sdk + '--native_upscaler=dlss')
}
& $run -Label after -ExtraArgs $off
foreach ($quality in @(@('low',1),@('medium',2),@('high',3))) {
  & $run -Label "$($quality[0])-hands" -Entries @(8) -Hands -ExtraArgs ($off + "--native_ao_samples=$($quality[1])")
}
foreach ($name in @('dlss','fsr','xess')) {
  $kind = if ($name -eq 'fsr') { 'fsr3' } else { $name }
  & $run -Label "$name-hands" -Entries @(8) -Hands -Seconds 90 -ExtraArgs ($sdk + "--native_upscaler=$kind")
}
& $run -Label after-dlss -Entries @(2,8) -Seconds 90 -ExtraArgs ($sdk + '--native_upscaler=dlss')
