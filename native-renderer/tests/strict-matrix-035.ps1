param([string]$RuntimeDll='F:\KK-native-renderer\runs\task035-sdk-present-fix\rexruntime.dll',
      [switch]$SkipDlss8)
$ErrorActionPreference='Stop'
foreach ($chapter in @(8,0,27)) {
 foreach ($upscaler in @('dlss','fsr')) {
  if ($SkipDlss8 -and $chapter -eq 8 -and $upscaler -eq 'dlss') { continue }
  & "$PSScriptRoot\strict-035.ps1" -Chapter $chapter -Backend vulkan -Upscaler $upscaler -Gtao -Label fixed -RuntimeDll $RuntimeDll
 }
}
& "$PSScriptRoot\strict-035.ps1" -Chapter 8 -Backend warp -Label fixed -RuntimeDll $RuntimeDll
