param([Parameter(Mandatory=$true)][string]$CrashRun, [string]$LauncherRun='')
$ErrorActionPreference='Stop'
function Check($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Check-PrivateText([string]$Text) {
  foreach ($name in @($env:USERNAME, $env:COMPUTERNAME, 'PrivacyProbe')) {
    if ($name) { Check (!$Text.ToLowerInvariant().Contains($name.ToLowerInvariant())) "Private name leaked: $name" }
  }
  Check ($Text -notmatch '[A-Za-z]:[\\/]') 'Absolute Windows path leaked into report'
}
Check ($CrashRun.StartsWith('F:\KK-native-renderer\runs\task043-crash-')) 'Expected private task043 crash directory'
$dump=Get-ChildItem -LiteralPath "$CrashRun\userdata\crashes" -Filter 'crash-*.dmp' | Sort-Object LastWriteTime | Select-Object -Last 1
Check ($null -ne $dump -and $dump.Length -gt 1024) 'Missing or empty minidump'
$bytes=[IO.File]::ReadAllBytes($dump.FullName)
Check ([Text.Encoding]::ASCII.GetString($bytes,0,4) -eq 'MDMP') 'Invalid minidump signature'
$count=[BitConverter]::ToUInt32($bytes,8)
$directory=[BitConverter]::ToUInt32($bytes,12)
$exceptionCode=0
for ($i=0; $i -lt $count; $i++) {
  $entry=$directory+12*$i
  if ([BitConverter]::ToUInt32($bytes,$entry) -eq 6) {
    $rva=[BitConverter]::ToUInt32($bytes,$entry+8)
    $exceptionCode=[BitConverter]::ToUInt32($bytes,$rva+8)
  }
}
Check ($exceptionCode -eq [Convert]::ToUInt32('E0430001',16)) 'Dump does not contain the forced exception'
$textPath=[IO.Path]::ChangeExtension($dump.FullName,'.txt')
$report=[IO.File]::ReadAllText($textPath)
Check-PrivateText $report
foreach ($field in @('Version:', 'Windows:', 'CPU:', 'RAM:', 'GPU inventory', 'driver', 'dedicated VRAM',
    'kk_update_channel', 'kk_fullscreen_mode', 'native_backend = vulkan', 'native_upscaler', 'ao_mode',
    'Startup/runtime fallbacks', 'Last log errors', 'KK dev: forced crash after 120', '[path redacted]')) {
  Check ($report.Contains($field)) "Missing crash field: $field"
}
$results=@{dump=$dump.Name;dump_bytes=$dump.Length;exception_code=$exceptionCode.ToString('X8');
  streams=$count;privacy='pass';crash_fields='pass'}
if ($LauncherRun) {
  Check ($LauncherRun.StartsWith('F:\KK-native-renderer\runs\task043-launcher-')) 'Expected private task043 launcher directory'
  $clipboard=[IO.File]::ReadAllText("$LauncherRun\userdata\diagnostics-clipboard.txt")
  Check-PrivateText $clipboard
  Check ($clipboard.Contains('King Kong PC port diagnostics') -and $clipboard.Contains('native_backend = vulkan')) 'Invalid clipboard report'
  $log=[IO.File]::ReadAllText("$LauncherRun\run.log")
  Check ($log.Contains('KK dev: diagnostics clipboard action exercised; saved crash reports true')) 'Clipboard action did not run with saved crash'
  Check ($log.Contains('KK: offering saved local crash reports in the launcher')) 'Next-launch crash offer did not run'
  $results.clipboard='pass'
  $results.next_launch_offer='pass'
}
$results | ConvertTo-Json | Set-Content -LiteralPath "$CrashRun\verification.json"
$results | ConvertTo-Json
