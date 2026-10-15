param([ValidateSet('upscale','renderer-affected','shader-control','shader-hw')][string]$Test = 'upscale', [int]$Count = 20,
      [string]$Filter = '', [string]$Hlsl = '', [string]$Label = '')
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot/..").Path
$out = "$workspace/out/task103"
$env:PATH = 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64;F:\KK-native-renderer\sdk-vulkan-upscalers-038\win-amd64\bin;' + $env:PATH
$lockPath = 'F:/KK-native-renderer/runs/codex-game.lock'
$priorityPath = 'F:/KK-native-renderer/runs/codex-game.priority'
$summary = @()
if (!$Label) { $Label = $Test }
for ($i = 1; $i -le $Count; ++$i) {
    # These are offscreen test programs, not game launches. Use the shared slot
    # anyway so they cannot contaminate another task's GPU measurements.
    $guard = $null
    while (!$guard) {
        $state = (Get-Content -LiteralPath $lockPath -Raw).Trim()
        if ($state.StartsWith('user')) { throw 'User lock: synthetic GPU tests deferred' }
        $priority = if (Test-Path $priorityPath) { (Get-Content $priorityPath -Raw).Trim() } else { '' }
        if ($state -eq 'free' -and (!$priority -or $priority -in '103', 'task103') -and
            !(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")) {
            $candidate = $null
            try {
                $candidate = [IO.File]::Open($lockPath, 'Open', 'ReadWrite', 'Read')
                $reader = New-Object IO.StreamReader($candidate, [Text.Encoding]::ASCII, $false, 1024, $true)
                $fresh = $reader.ReadToEnd().Trim()
                $reader.Dispose()
                if ($fresh -eq 'free' -and !(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")) {
                    $guard = $candidate
                    $guard.SetLength(0)
                    $guard.Position = 0
                    $bytes = [Text.Encoding]::ASCII.GetBytes("task103-tests $(Get-Date -Format o)`r`n")
                    $guard.Write($bytes, 0, $bytes.Length)
                    $guard.Flush()
                } else { $candidate.Dispose() }
            } catch { if ($candidate -and !$guard) { $candidate.Dispose() } }
        }
        if (!$guard) {
            Write-Output "Waiting for synthetic GPU slot: lock=$state priority=$priority"
            Start-Sleep -Seconds 30
        }
    }
    $log = "$out/$Label-$('{0:D2}' -f $i).log"
    if (Test-Path $log) { Move-Item -LiteralPath $log -Destination ($log + '.previous-' + (Get-Date -Format HHmmssfff)) }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        $ErrorActionPreference = 'Continue'
        if ($Test -eq 'upscale') {
            if ($Filter) {
                & 'C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe' -I "$workspace/native-renderer/backend/tests/check_validation.py" "$out/renderer/backend/tests/nr_game_upscale_tests.exe" $Filter *> $log
            } else {
                ctest --test-dir "$out/renderer" -R '^nr_game_upscale_tests$' --timeout 120 --output-on-failure *> $log
            }
        } elseif ($Test -eq 'renderer-affected') {
            ctest --test-dir "$out/renderer" -R '^nr_(render|ao|gtao|game|game_ao|game_gi|game_upscale|upscaler_sdk)_tests$' --timeout 120 --output-on-failure *> $log
        } else {
            $build = if ($Test -eq 'shader-control') { "$out/shader-control" } else { 'F:/KK-native-renderer/wt/hw-beta6/native-renderer/shaders/out/build/task095' }
            if ($Filter) {
                $arguments = @('gpu', '--dxc', 'F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64',
                               '--filter', $Filter, '--out', "$out/$Label-out", '--trace', '1')
                if ($Hlsl) { $arguments += @('--hlsl', $Hlsl) }
                & "$build/kkshaders_gpu_tests.exe" @arguments *> $log
            } else {
                ctest --test-dir $build -R '^(interpreter|gpu)$' --output-on-failure *> $log
            }
        }
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
    } finally {
        $guard.SetLength(0)
        $guard.Position = 0
        $bytes = [Text.Encoding]::ASCII.GetBytes("free`r`n")
        $guard.Write($bytes, 0, $bytes.Length)
        $guard.Flush()
        $guard.Dispose()
    }
    if (!$Filter) {
        $build = if ($Test -in 'upscale','renderer-affected') { "$out/renderer" } elseif ($Test -eq 'shader-control') { "$out/shader-control" } else { 'F:/KK-native-renderer/wt/hw-beta6/native-renderer/shaders/out/build/task095' }
        Copy-Item -LiteralPath "$build/Testing/Temporary/LastTest.log" -Destination "$out/$Label-$('{0:D2}' -f $i)-ctest.log"
    }
    $row = [pscustomobject]@{ test = $Test; iteration = $i; exit = $code; seconds = $timer.Elapsed.TotalSeconds; log = $log }
    $summary += $row
    $summary | ConvertTo-Json | Set-Content "$out/$Label-summary.json"
    Write-Output "$Test $i/$Count exit=$code seconds=$([Math]::Round($timer.Elapsed.TotalSeconds, 2)) log=$log"
}
if ($summary.Where({ $_.exit -ne 0 }).Count) { exit 1 }
