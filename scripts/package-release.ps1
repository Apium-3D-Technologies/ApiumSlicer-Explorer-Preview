[CmdletBinding()]
param(
    [string]$BuildDir = "build",

    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",

    [string]$OutputDir = "dist"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# Resolve relative paths against the repository, independent of the caller's cwd.
if (-not [IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}
if (-not [IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir = Join-Path $repoRoot $OutputDir
}

$dllName = "ApiumSlicerExplorerPreview.dll"
$dllSource = Join-Path $BuildDir "$Config/$dllName"
$packageFiles = @(
    $dllSource
    (Join-Path $repoRoot "resources/ApiumSlicer.propschema.propdesc")
    (Join-Path $repoRoot "LICENSE")
    (Join-Path $repoRoot "NOTICES.md")
)
foreach ($path in $packageFiles) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Release input not found: $path"
    }
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$dllPath = Join-Path $OutputDir $dllName
$zipPath = Join-Path $OutputDir "ApiumSlicerExplorerPreview-windows-x64.zip"
Copy-Item -LiteralPath $dllSource -Destination $dllPath -Force
# Explicit inputs prevent stale build files or debug binaries from entering the ZIP.
Compress-Archive -LiteralPath $packageFiles -DestinationPath $zipPath -CompressionLevel Optimal -Force

$checksums = foreach ($path in @($dllPath, $zipPath)) {
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    "{0}  {1}" -f $hash, [IO.Path]::GetFileName($path)
}
Set-Content -LiteralPath (Join-Path $OutputDir "SHA256SUMS.txt") -Value $checksums -Encoding ascii
Write-Host "Release assets written to: $OutputDir"
