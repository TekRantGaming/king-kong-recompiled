param([ValidateSet('basic','effects','gunfire','affinity','all')][string]$Group='all')
$ErrorActionPreference='Stop'
$runner=Join-Path $PSScriptRoot 'perf-012.ps1'
$jobs=@()
if ($Group -in 'basic','all') {
 $jobs+=@{Label='final-vk-1080-off';Chapter=8;Scale='1920x1080';Backend='vulkan'}
 $jobs+=@{Label='final-vk-1440-off';Chapter=8;Scale='2560x1440';Backend='vulkan'}
 foreach($scale in @('1920x1080','2560x1440')) {
  $jobs+=@{Label="final-dx12-$scale-off";Chapter=8;Scale=$scale;Backend='d3d12'}
 }
 foreach($chapter in @(0,27)) {
  foreach($backend in @('vulkan','d3d12')) {
   $jobs+=@{Label="final-$backend-1440-off";Chapter=$chapter;Scale='2560x1440';Backend=$backend;PassTimers=$true}
  }
 }
}
if ($Group -in 'gunfire','all') {
 $before='F:\KK-native-renderer\runs\task012-before-vk-1440-ch8-20261013-153744\rexgpu-native.dll'
 foreach($backend in @('vulkan','d3d12')) {
  $jobs+=@{Label="gunfire-before-$backend-4k";Chapter=8;Scale='3840x2160';Backend=$backend;Fire=$true;NativeDll=$before}
  $jobs+=@{Label="gunfire-after-$backend-4k";Chapter=8;Scale='3840x2160';Backend=$backend;Fire=$true}
 }
}
if ($Group -in 'affinity','all') {
 foreach($cores in @(4,6)) {
  $jobs+=@{Label="affinity-vk-$cores-cores";Chapter=8;Scale='1920x1080';Backend='vulkan';Cores=$cores}
 }
}
if ($Group -in 'effects','all') {
 foreach($backend in @('vulkan','d3d12')) {
  foreach($scale in @('1920x1080','2560x1440')) {
   $jobs+=@{Label="effects-$backend-$scale-dlss";Chapter=8;Scale=$scale;Backend=$backend;Upscaler='dlss'}
   $jobs+=@{Label="effects-$backend-$scale-gtao";Chapter=8;Scale=$scale;Backend=$backend;Gtao=$true}
   $jobs+=@{Label="effects-$backend-$scale-gtao-dlss";Chapter=8;Scale=$scale;Backend=$backend;Gtao=$true;Upscaler='dlss'}
  }
 }
}
foreach($job in $jobs) {
 Write-Output ("CASE " + ($job | ConvertTo-Json -Compress))
 & $runner @job
}
