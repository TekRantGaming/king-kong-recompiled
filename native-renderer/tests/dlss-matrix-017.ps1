param([string]$Stage='before', [string]$NativeDll='', [string]$PipelineList='F:\KK-native-renderer\analysis\tour\pipelines\tour-all.list')
$ErrorActionPreference='Stop'
foreach ($api in @('vulkan','d3d12')) {
 foreach ($size in @('2560x1440','1920x1080')) {
  foreach ($chapter in @(8,0,27)) {
   & "$PSScriptRoot\dlss-017.ps1" -Label "$Stage-$api-$size-dlss" -Chapter $chapter -Backend $api -Scale $size -Upscaler dlss -NativeDll $NativeDll -PipelineList $PipelineList
  }
  & "$PSScriptRoot\dlss-017.ps1" -Label "$Stage-$api-$size-cutscene" -Chapter 27 -Backend $api -Scale $size -Upscaler dlss -Cutscene -NativeDll $NativeDll -PipelineList $PipelineList
 }
}
