$ErrorActionPreference='Stop'
$lock='F:\KK-native-renderer\runs\codex-game.lock'
while ($true) {
  $file=$null; $claimed=$false
  try {
    $file=[IO.File]::Open($lock,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $bytes=New-Object byte[] ([int]$file.Length); $null=$file.Read($bytes,0,$bytes.Length)
    $state=[Text.Encoding]::UTF8.GetString($bytes).Trim([char]0xFEFF).Trim()
    if ($state.StartsWith('user')) { throw 'USER_PAUSE: GPU validation deferred' }
    if (($state -eq 'free' -or !$state) -and !(Get-Process king_kong -ErrorAction SilentlyContinue)) {
      $claim=[Text.Encoding]::UTF8.GetBytes("task036-gpu $(Get-Date -Format o)")
      $file.SetLength(0); $file.Write($claim,0,$claim.Length); $file.Flush(); $claimed=$true
    }
  } catch [IO.IOException] {} finally { if ($file) { $file.Dispose() } }
  if ($claimed) { break }
  Write-Output "WAIT $(Get-Date -Format o) GPU validation queued"
  Start-Sleep -Seconds 30
}
try {
  $layer='F:\KK-native-renderer\downloads\vulkan-sdk-latest\sdk\Bin'
  if (Test-Path "$layer\VkLayer_khronos_validation.json") { $env:VK_LAYER_PATH=$layer }
  $exe=(Resolve-Path "$PSScriptRoot\..\..\out\task036-tests\backend\tests\nr_memory_migration_tests.exe").Path
  $info=New-Object Diagnostics.ProcessStartInfo $exe
  $info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.WindowStyle='Hidden'
  $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
  $test=New-Object Diagnostics.Process; $test.StartInfo=$info; $null=$test.Start()
  $stdout=$test.StandardOutput.ReadToEndAsync(); $stderr=$test.StandardError.ReadToEndAsync()
  if (!$test.WaitForExit(120000)) { Stop-Process -Id $test.Id; throw 'GPU validation exceeded two minutes' }
  $stdout.Result | Set-Content 'F:\KK-native-renderer\runs\task036-gpu-test.log'
  $stderr.Result | Set-Content 'F:\KK-native-renderer\runs\task036-gpu-test-errors.log'
  if ($test.ExitCode -ne 0) { throw "GPU validation failed: $($test.ExitCode)" }
  Write-Output 'GPU validation passed'
} finally {
  if ((Get-Content $lock -Raw).Trim().StartsWith('task036-gpu ')) { Set-Content $lock 'free' }
}
