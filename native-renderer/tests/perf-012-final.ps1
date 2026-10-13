$ErrorActionPreference='Stop'
$runner=Join-Path $PSScriptRoot 'perf-012.ps1'
foreach($chapter in @(0,8,27)) {
 foreach($backend in @('vulkan','d3d12')) {
  & $runner -Label "ticket-$backend-1440-off" -Chapter $chapter -Backend $backend -Scale 2560x1440
 }
}
& $runner -Label 'gunfire-after-ticket-vulkan-4k' -Chapter 8 -Backend vulkan -Scale 3840x2160 -Fire
& $runner -Label 'validation-final-vulkan' -Chapter 8 -Backend vulkan -Scale 2560x1440 -Validation
