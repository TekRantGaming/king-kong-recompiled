# Shared immutable run artifacts. Never link directly to a mutable build output.
# Logs, userdata, pipeline caches, shader build caches and glyphs stay private.
function New-RunArtifactLink([string]$Path, [string]$Target) {
  New-Item -ItemType HardLink -Path $Path -Target $Target -ErrorAction Stop | Out-Null
}

function Initialize-RunBuild {
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$Run,
        [string[]]$ShaderPacks = @(), [string]$Dxc = '', [string]$NativeDll = '',
        [string]$Glyphs = '', [string]$StageRoot = 'F:\KK-native-renderer\runs\.build-staging')
  $files = @{}
  Get-ChildItem -LiteralPath $Build -File | Where-Object { $_.Extension -in '.exe','.dll','.pack' } |
    ForEach-Object { $files[$_.Name] = $_.FullName }
  foreach ($pack in $ShaderPacks) { $files[[IO.Path]::GetFileName($pack)] = $pack }
  if ($Dxc) { $files['dxcompiler.dll'] = $Dxc }
  if ($NativeDll) { $files['rexgpu-native.dll'] = $NativeDll }
  if (!$files.ContainsKey('king_kong.exe')) { throw 'Build has no king_kong.exe' }
  $entries = @($files.Keys | Sort-Object | ForEach-Object {
    $src = Get-Item -LiteralPath $files[$_]
    [pscustomobject]@{name=$_; source=$src.FullName; bytes=$src.Length; sha256=(Get-FileHash -LiteralPath $src.FullName -Algorithm SHA256).Hash}
  })
  $canonical = ($entries | ForEach-Object { "$($_.name.ToLowerInvariant()) $($_.sha256)" }) -join "`n"
  $sha = [Security.Cryptography.SHA256]::Create()
  try { $hash = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($canonical)))).Replace('-','').ToLowerInvariant() }
  finally { $sha.Dispose() }
  $stage = Join-Path $StageRoot $hash
  New-Item -ItemType Directory -Path $stage,$Run -Force | Out-Null
  # Serialise publication of a build across independent harness processes.
  $guard = $null
  $deadline = (Get-Date).AddSeconds(30)
  while (!$guard) {
    try { $guard = [IO.File]::Open((Join-Path $stage '.stage.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch [IO.IOException] {
      if ((Get-Date) -gt $deadline) { throw 'Timed out waiting for staging lock' }
      Start-Sleep -Milliseconds 200
    }
  }
  try {
    foreach ($e in $entries) {
      $target = Join-Path $stage $e.name
      if (!(Test-Path -LiteralPath $target)) {
        # This is the only full copy for this build hash. A source changing during copying fails closed.
        [IO.File]::Copy($e.source, $target, $false)
      }
      if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $e.sha256) {
        throw "Staged artifact hash mismatch: $target. Move the invalid stage aside before retrying."
      }
      [IO.File]::SetAttributes($target, ([IO.File]::GetAttributes($target) -bor [IO.FileAttributes]::ReadOnly))
    }
  } finally { $guard.Dispose() }
  $linked = 0L; $copied = 0L
  $manifest = @()
  foreach ($e in $entries) {
    $dest = Join-Path $Run $e.name
    if (Test-Path -LiteralPath $dest) { throw "Refusing to overwrite a run artifact: $dest" }
    $target = Join-Path $stage $e.name
    $mode = 'hardlink'
    try { New-RunArtifactLink $dest $target; $linked += $e.bytes }
    catch {
      # Other volumes and filesystems may not support NTFS links. Copy without overwriting anything.
      if (Test-Path -LiteralPath $dest) { throw }
      [IO.File]::Copy($target, $dest, $false)
      $mode = 'copy'; $copied += $e.bytes
    }
    [IO.File]::SetAttributes($dest, ([IO.File]::GetAttributes($dest) -bor [IO.FileAttributes]::ReadOnly))
    $manifest += [pscustomobject]@{name=$e.name; sha256=$e.sha256; bytes=$e.bytes; mode=$mode; staged=$target}
  }
  if (!$Glyphs) { $Glyphs = Join-Path $Build 'glyphs' }
  if (Test-Path -LiteralPath $Glyphs) { Copy-Item -LiteralPath $Glyphs -Destination (Join-Path $Run 'glyphs') -Recurse }
  [pscustomobject]@{build_hash=$hash; stage=$stage; linked_bytes=$linked; copied_bytes=$copied; files=$manifest} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Run 'build-staging.json') -Encoding UTF8
  Write-Output "Build $hash : $linked bytes linked, $copied bytes copied"
}

function Assert-RunBuildUnchanged([string]$Run) {
  $manifest = Get-Content -LiteralPath (Join-Path $Run 'build-staging.json') -Raw | ConvertFrom-Json
  foreach ($e in $manifest.files) {
    foreach ($path in @((Join-Path $Run $e.name), $e.staged)) {
      if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $e.sha256) { throw "Run modified immutable artifact: $path" }
      if (!((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReadOnly)) { throw "Run cleared read-only protection: $path" }
    }
  }
}

function Complete-RunCaptures([string]$Run, [string]$Python = 'C:\Users\TekRa\AppData\Local\Programs\Python\Python312\python.exe') {
  & $Python -I (Join-Path $PSScriptRoot 'finalize-captures.py') $Run
  if ($LASTEXITCODE -ne 0) { throw "Capture finalization failed for $Run" }
}
