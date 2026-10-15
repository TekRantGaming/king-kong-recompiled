# A control executable with Beta 5's limiter and the new benchmark recorder.
# Recompile only frame_stats.cpp, relink existing objects into a private output.
$ErrorActionPreference='Continue'
$workspace=(Resolve-Path "$PSScriptRoot/..").Path.Replace('\','/')
$build="$workspace/kk/out/build/kk-beta"
$output="$workspace/kk/out/baseline-096"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$taskEnv = cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" & set'
foreach ($line in $taskEnv) {
  if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$ninja=(Get-Content "$build/CMakeCache.txt" | Select-String '^CMAKE_MAKE_PROGRAM:FILEPATH=').Line.Split('=',2)[1]
$commands = & $ninja -C $build -t commands king_kong.exe
$source=(git -C $workspace show e890ece:kk/src/frame_stats.cpp) -join "`n"
if ($LASTEXITCODE) { throw 'Cannot read Beta 5 baseline' }
$source=$source.Replace('#include "frame_stats.h"',"#include `"frame_stats.h`"`n#include `"benchmark.h`"")
$source=$source.Replace('void OnGuestSwap() {',"void OnGuestSwap() {`n  benchmark::GuestSwap();")
[IO.File]::WriteAllText("$output/frame_stats.cpp",$source)
$cmd=@($commands | Where-Object {$_.EndsWith(" -c $workspace/kk/src/frame_stats.cpp")})
if($cmd.Count -ne 1) {throw 'Expected one frame_stats compile command'}
$cmd=$cmd[0].Replace("-c $workspace/kk/src/frame_stats.cpp","-c $output/frame_stats.cpp")
$cmd=$cmd.Replace('-o CMakeFiles/king_kong.dir/src/frame_stats.cpp.obj',"-o $output/frame_stats.cpp.obj")
$cmd=[regex]::Replace($cmd,' -MD -MT .*? -MF .*? -o ',' -o ')
[IO.File]::WriteAllText("$output/compile.cmd","@echo off`r`n$cmd`r`n")
Push-Location $build
& "$output/compile.cmd" *> "$output/compile.log"
$code=$LASTEXITCODE
Pop-Location
if($code) {throw 'Baseline compile failed'}
$lines=Get-Content "$build/build.ninja"
$at=-1
for($i=0;$i -lt $lines.Count;$i++) {if($lines[$i] -match '^build king_kong.exe:') {$at=$i;break}}
if($at -lt 0) {throw 'Game link rule absent'}
$objects=([regex]::Match($lines[$at],': CXX_EXECUTABLE_LINKER__king_kong_Release (.*?)( \||$)')).Groups[1].Value
$objects=$objects.Replace('CMakeFiles/king_kong.dir/src/frame_stats.cpp.obj',"$output/frame_stats.cpp.obj")
$libraries=($lines[($at+1)..($at+14)] | Where-Object {$_ -match '^  LINK_LIBRARIES = '}).Substring(19)
$flags=($lines[($at+1)..($at+14)] | Where-Object {$_ -match '^  LINK_FLAGS = '}).Substring(15)
$flags=$flags.Replace('-Xlinker /MAP ',"-Xlinker /MAP:$output/king_kong.map ")
if(!$objects -or !$libraries -or !$flags) {throw 'Game link inputs absent'}
[IO.File]::WriteAllText("$output/link.rsp","$objects $libraries")
$compiler='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang++.exe'
$cmd="`"$compiler`" -nostartfiles -nostdlib -O3 -DNDEBUG -D_DLL -D_MT -Xclang --dependent-lib=msvcrt $flags -o $output/king_kong.exe @$output/link.rsp"
[IO.File]::WriteAllText("$output/link.cmd","@echo off`r`n$cmd`r`n")
Push-Location $build
& "$output/link.cmd" *> "$output/link.log"
$code=$LASTEXITCODE
Pop-Location
if($code) {throw 'Baseline link failed'}
Write-Output "Baseline executable: $output/king_kong.exe"
