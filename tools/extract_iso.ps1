param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [Parameter(Mandatory = $true)][string]$OutDir
)
Add-Type -Path (Join-Path $PSScriptRoot 'XisoExtract.cs')
[XisoExtract]::Run((Resolve-Path -LiteralPath $Iso).Path, [IO.Path]::GetFullPath($OutDir)) | Out-Null
