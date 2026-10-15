param([ValidateSet('tests','settings-tests','presenter-tests','all-tests','kk-beta','kk-dev-vulkan')][string]$Target='tests')
$ErrorActionPreference='Continue'
$taskEnv = cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" & set'
foreach ($line in $taskEnv) {
  if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$env:PATH='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;'+$env:PATH
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
$logs="$workspace/kk/out/task096"
New-Item -ItemType Directory -Path $logs -Force | Out-Null
if ($Target -eq 'tests') {
  cmake -S "$workspace/kk/tests/pacing-bench" -B "$workspace/kk/out/pacing-bench-tests" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release *> "$logs/tests-configure.log"
  if ($LASTEXITCODE) { throw 'Test configure failed' }
  cmake --build "$workspace/kk/out/pacing-bench-tests" -j 4 *> "$logs/tests-build.log"
  if ($LASTEXITCODE) { throw 'Test build failed' }
  ctest --test-dir "$workspace/kk/out/pacing-bench-tests" --output-on-failure *> "$logs/tests.log"
  if ($LASTEXITCODE) { throw 'Tests failed' }
} elseif ($Target -eq 'presenter-tests') {
  cmake -S "$workspace/native-renderer/tests/uipaint-069" -B "$workspace/kk/out/presenter-tests-096" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=F:/KK-native-renderer/sdk-vulkan-upscalers-038/win-amd64 *> "$logs/presenter-configure.log"
  if ($LASTEXITCODE) { throw 'Presenter-test configure failed' }
  cmake --build "$workspace/kk/out/presenter-tests-096" -j 2 *> "$logs/presenter-build.log"
  if ($LASTEXITCODE) { throw 'Presenter-test build failed' }
  Copy-Item "$workspace/kk/out/presentation-096/rexruntime.dll" "$workspace/kk/out/presenter-tests-096/" -Force
  & "$workspace/kk/out/presenter-tests-096/ui-paint-tests.exe" *> "$logs/presenter-tests.log"
  if ($LASTEXITCODE) { throw 'Presenter tests failed' }
} elseif ($Target -eq 'settings-tests') {
  Set-Location "$workspace/kk"
  cmake --preset kk-beta *> "$logs/settings-tests-configure.log"
  if ($LASTEXITCODE) { throw 'Settings-test configure failed' }
  cmake --build --preset kk-beta --target kk-vsync-tests -j 2 *> "$logs/settings-tests-build.log"
  if ($LASTEXITCODE) { throw 'Settings-test build failed' }
  ctest --test-dir "$workspace/kk/out/build/kk-beta" -R vsync-settings --output-on-failure *> "$logs/settings-tests.log"
  if ($LASTEXITCODE) { throw 'Settings tests failed' }
} elseif ($Target -eq 'all-tests') {
  Set-Location "$workspace/kk"
  cmake --preset kk-beta -DNR_BUILD_TESTS=ON -DPython3_EXECUTABLE=C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe -DNR_DXC_DIR=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64 -DNR_LIGHTING_DXC=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxc.exe -DNR_DXC_RUNTIME=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxcompiler.dll *> "$logs/all-tests-configure.log"
  if ($LASTEXITCODE) { throw 'All-test configure failed' }
  cmake --build --preset kk-beta -j 8 *> "$logs/all-tests-build.log"
  if ($LASTEXITCODE) { throw 'All-test build failed' }
  if(Test-Path "$workspace/kk/out/presentation-096/rexruntime.dll") {
    Copy-Item "$workspace/kk/out/presentation-096/rexruntime.dll" "$workspace/kk/out/build/kk-beta/rexruntime.dll" -Force
  }
  # The suite has synthetic GPU tests. Serialize them with game measurements,
  # including other tasks' priority reservations, to avoid contaminating either.
  $testGuard=$null
  $lockPath='F:/KK-native-renderer/runs/codex-game.lock'
  $priorityPath='F:/KK-native-renderer/runs/codex-game.priority'
  while(!$testGuard) {
    $state=(Get-Content $lockPath -Raw).Trim()
    if($state.StartsWith('user')) {throw 'User lock: GPU tests deferred'}
    $priority=if(Test-Path $priorityPath){(Get-Content $priorityPath -Raw).Trim()}else{''}
    if($state -eq 'free' -and (!$priority -or $priority -in '096','task096') -and
       !(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")) {
      try {
        $candidate=[IO.File]::Open($lockPath,'Open','ReadWrite','Read')
        $reader=New-Object IO.StreamReader($candidate,[Text.Encoding]::ASCII,$false,1024,$true)
        $fresh=$reader.ReadToEnd().Trim();$reader.Dispose()
        if($fresh -eq 'free' -and !(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")) {
          $testGuard=$candidate
          $testGuard.SetLength(0);$testGuard.Position=0
          $bytes=[Text.Encoding]::ASCII.GetBytes("task096-tests $(Get-Date -Format o)`r`n")
          $testGuard.Write($bytes,0,$bytes.Length);$testGuard.Flush()
        } else {$candidate.Dispose()}
      } catch {if($candidate -and !$testGuard){$candidate.Dispose()}}
    }
    if(!$testGuard){Write-Output "Waiting for GPU test slot: lock=$state priority=$priority";Start-Sleep -Seconds 30}
  }
  try {
    ctest --test-dir "$workspace/kk/out/build/kk-beta" --output-on-failure -j 1 *> "$logs/all-tests.log"
    if ($LASTEXITCODE) { throw 'CTest failed' }
  } finally {
    $testGuard.SetLength(0);$testGuard.Position=0
    $bytes=[Text.Encoding]::ASCII.GetBytes("free`r`n")
    $testGuard.Write($bytes,0,$bytes.Length);$testGuard.Flush();$testGuard.Dispose()
  }
} else {
  Set-Location "$workspace/kk"
  cmake --preset $Target *> "$logs/$Target-configure.log"
  if ($LASTEXITCODE) { throw 'Game configure failed' }
  cmake --build --preset $Target -j 8 *> "$logs/$Target-build.log"
  if ($LASTEXITCODE) { throw 'Game build failed' }
  if(Test-Path "$workspace/kk/out/presentation-096/rexruntime.dll") {
    Copy-Item "$workspace/kk/out/presentation-096/rexruntime.dll" "$workspace/kk/out/build/$Target/rexruntime.dll" -Force
  }
}
Write-Output "$Target passed. Logs: $logs"
