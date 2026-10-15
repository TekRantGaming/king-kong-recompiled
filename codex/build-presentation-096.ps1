# Build the changed SDK objects and their header consumers, then relink against the existing
# SDK -038 objects/libraries. Nothing in the shared SDK source/build/install is
# written. Patch 0039 is the source change to carry into a normal SDK build.
$ErrorActionPreference='Continue'
$workspace=(Resolve-Path "$PSScriptRoot/..").Path.Replace('\','/')
$output="$workspace/kk/out/presentation-096"
$sdk='F:/KK-native-renderer/sdk-src-upscalers-038'
$sdkBuild="$sdk/out/build/win-amd64"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$taskEnv = cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" & set'
foreach ($line in $taskEnv) {
  if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$ninja=(Get-Content "$sdkBuild/CMakeCache.txt" | Select-String '^CMAKE_MAKE_PROGRAM:FILEPATH=').Line.Split('=',2)[1]
$commands = & $ninja -C $sdkBuild -f build-Release.ninja -t commands rexruntime
if ($LASTEXITCODE) { throw 'Cannot read SDK commands' }
$header='include/rex/ui/d3d12/d3d12_presenter.h'
$headerText=[IO.File]::ReadAllText("$sdk/$header")
$anchor='    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;'
if(!$headerText.Contains($anchor)){throw 'Swapchain header anchor changed'}
$headerText=$headerText.Replace($anchor,$anchor+"`n    HANDLE frame_latency_waitable = nullptr;`n    UINT swap_chain_flags = 0;")
New-Item -ItemType Directory -Path "$output/include/rex/ui/d3d12" -Force | Out-Null
[IO.File]::WriteAllText("$output/$header",$headerText)
$files=@('src/ui/presenter.cpp','src/ui/d3d12/d3d12_presenter.cpp',
  'src/ui/d3d12/d3d12_provider.cpp','src/ui/d3d12/d3d12_immediate_drawer.cpp')
$objects=@{}
foreach ($file in $files) {
  $source=[IO.File]::ReadAllText("$sdk/$file")
  $name=[IO.Path]::GetFileName($file)
  $privateSource="$output/$name"
  if ($name -eq 'presenter.cpp') {
    $anchor='#include <rex/ui/window.h>'
    $insert=@'


// Optional observer, no Presenter layout/ABI change. Called only after a
// successful host present, never for a skipped or failed paint. The caller
// keeps its function alive until process exit and does no IO in the callback.
namespace {
std::atomic<void (*)()> present_observer_096{nullptr};
}
#if defined(_WIN32)
extern "C" __declspec(dllexport)
#else
extern "C" __attribute__((visibility("default")))
#endif
void RexSetPresentObserver096(void (*observer)()) {
  present_observer_096.store(observer, std::memory_order_release);
}
'@
    if (!$source.Contains($anchor)) { throw 'Observer include anchor changed' }
    $source=$source.Replace($anchor,$anchor+$insert)
    $anchor='  PaintResult result = PaintAndPresentImpl(execute_ui_drawers);'
    $insert=@'

  if (result == PaintResult::kPresented || result == PaintResult::kPresentedSuboptimal) {
    if (auto observer = present_observer_096.load(std::memory_order_acquire))
      observer();
  }
'@
    if (!$source.Contains($anchor)) { throw 'Observer paint anchor changed' }
    $source=$source.Replace($anchor,$anchor+$insert)
  } elseif ($name -eq 'd3d12_presenter.cpp') {
    $anchor='  HRESULT present_result = paint_context_.swap_chain->Present('
    if (!$source.Contains($anchor)) { throw 'D3D present anchor changed' }
    $source=$source.Replace($anchor,"  // Honor VSync with a synchronized present.`n  const bool sync = !REXCVAR_GET(d3d12_allow_variable_refresh_rate_and_tearing);`n"+$anchor)
    $source=$source.Replace('      0, DXGI_PRESENT_RESTART |','      sync ? 1 : 0, sync ? 0 : DXGI_PRESENT_RESTART |')
    $source=$source.Replace('    swap_chain_desc.Flags = 0;', '    swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;')
    $old=@'
        if (FAILED(dxgi_factory->CreateSwapChainForHwnd(
                direct_queue, surface_hwnd, &swap_chain_desc, nullptr, nullptr, &swap_chain_1))) {
'@
    $new=@'
        auto create_swapchain = [&] {
          return dxgi_factory->CreateSwapChainForHwnd(direct_queue, surface_hwnd,
              &swap_chain_desc, nullptr, nullptr, &swap_chain_1);
        };
        HRESULT created = create_swapchain();
        if (FAILED(created)) {
          // Old/unsupported presentation paths keep their previous behavior.
          swap_chain_desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
          created = create_swapchain();
          REXLOG_WARN("D3D12Presenter: frame latency waitable swapchain unavailable");
        }
        if (FAILED(created)) {
'@
    $source=$source.Replace("`r`n","`n")
    if(!$source.Contains($old)){throw 'Swapchain creation anchor changed'}
    $source=$source.Replace($old,$new)
    $anchor='    paint_context_.swap_chain_width = new_swap_chain_width;'
    $new=@'
    paint_context_.swap_chain_flags = swap_chain_desc.Flags;
    if (swap_chain_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
      if (SUCCEEDED(paint_context_.swap_chain->SetMaximumFrameLatency(1)))
        paint_context_.frame_latency_waitable = paint_context_.swap_chain->GetFrameLatencyWaitableObject();
      if (!paint_context_.frame_latency_waitable)
        REXLOG_WARN("D3D12Presenter: could not enable maximum frame latency 1");
      else
        REXLOG_INFO("D3D12Presenter: maximum frame latency 1, waitable swapchain");
    }
'@
    # Only the creation block, not the earlier successful-resize assignment.
    $anchor="    // before returning.`n"+$anchor
    if(!$source.Contains($anchor)){throw 'Swapchain init anchor changed'}
    $source=$source.Replace($anchor,"    // before returning.`n"+$new+"`n    paint_context_.swap_chain_width = new_swap_chain_width;")
    $source=$source.Replace('        paint_context_.swap_chain_allows_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));',
      '        paint_context_.swap_chain_flags));')
    $source=$source.Replace('  swap_chain.Reset();',"  if (frame_latency_waitable) {`n    CloseHandle(frame_latency_waitable);`n    frame_latency_waitable = nullptr;`n  }`n  swap_chain_flags = 0;`n  swap_chain.Reset();")
    $anchor='Presenter::PaintResult D3D12Presenter::PaintAndPresentImpl(bool execute_ui_drawers) {'
    $new=@'

  // Wait before consuming the latest guest output, so backpressure does not
  // leave an older image queued behind the display. No extra swapchain buffers.
  if (paint_context_.frame_latency_waitable) {
    const DWORD waited = WaitForSingleObject(paint_context_.frame_latency_waitable, 1000);
    if (waited == WAIT_TIMEOUT) return PaintResult::kNotPresented;
    if (waited != WAIT_OBJECT_0) {
      REXLOG_WARN("D3D12Presenter: frame latency wait failed, using normal presentation");
      CloseHandle(paint_context_.frame_latency_waitable);
      paint_context_.frame_latency_waitable = nullptr;
    }
  }
'@
    $source=$source.Replace($anchor,$anchor+$new)
    $source=$source.Replace('is_vsync_implicit_out = false;', 'is_vsync_implicit_out = !REXCVAR_GET(d3d12_allow_variable_refresh_rate_and_tearing);')
  }
  [IO.File]::WriteAllText($privateSource,$source)
  $cmd=@($commands | Where-Object { $_.EndsWith(" -c $sdk/$file") })
  if ($cmd.Count -ne 1) { throw "Expected one compile command for $file" }
  $cmd=$cmd[0]
  $firstInclude=$cmd.IndexOf(' -I')
  $cmd=$cmd.Insert($firstInclude," -I$output/include")
  $oldObj=[regex]::Match($cmd,' -o ([^ ]+) -c ').Groups[1].Value
  $newObj="$output/$name.obj"
  $objects[$oldObj]=$newObj
  $cmd=$cmd.Replace("-o $oldObj ","-o $newObj ")
  if($name -in 'presenter.cpp','d3d12_presenter.cpp'){$cmd=$cmd.Replace("-c $sdk/$file","-c $privateSource")}
  $cmd=[regex]::Replace($cmd,' -MD -MT .*? -MF .*? -o ',' -o ')
  $commandFile="$output/compile-$name.cmd"
  [IO.File]::WriteAllText($commandFile,"@echo off`r`n$cmd`r`n")
  Push-Location $sdkBuild
  & $commandFile *> "$output/compile-$name.log"
  $code=$LASTEXITCODE
  Pop-Location
  if ($code) { throw "Compile failed for $file" }
}
$lines=Get-Content "$sdkBuild/CMakeFiles/impl-Release.ninja"
$at=-1
for($i=0;$i -lt $lines.Count;$i++) {
  if ($lines[$i] -match '^build .*rexruntime.dll .*CXX_SHARED_LIBRARY_LINKER') { $at=$i; break }
}
if ($at -lt 0) { throw 'SDK link rule absent' }
$linkObjects=([regex]::Match($lines[$at],': CXX_SHARED_LIBRARY_LINKER__rexruntime_Release (.*?)( \||$)')).Groups[1].Value
foreach($old in $objects.Keys) { $linkObjects=$linkObjects.Replace($old,$objects[$old]) }
$libraries=($lines[($at+1)..($at+20)] | Where-Object {$_ -match '^  LINK_LIBRARIES = '}).Substring(19)
if (!$linkObjects -or !$libraries) { throw 'SDK link inputs absent' }
[IO.File]::WriteAllText("$output/runtime.rsp", "$linkObjects $libraries")
$compiler='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang++.exe'
Push-Location $sdkBuild
& $compiler -nostartfiles -nostdlib -march=x86-64-v2 -O3 -DNDEBUG -D_DLL -D_MT -Xclang --dependent-lib=msvcrt -shared -fuse-ld=lld -Xlinker /DEF:src/system/CMakeFiles/rexruntime.dir/Release/exports.def -o "$output/rexruntime.dll" -Xlinker /MANIFEST:EMBED -Xlinker "/implib:$output/rexruntime.lib" -Xlinker "/pdb:$output/rexruntime.pdb" "@$output/runtime.rsp" *> "$output/link.log"
$code=$LASTEXITCODE
Pop-Location
if ($code) { throw 'Private SDK runtime link failed' }
Write-Output "Private SDK runtime: $output/rexruntime.dll"
