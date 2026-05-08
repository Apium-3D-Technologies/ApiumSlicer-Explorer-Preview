param(
    [string]$DllPath = "",
    [switch]$Silent
)

$ErrorActionPreference = "Stop"

function Resolve-DllPath {
    param([string]$InputPath)

    if ($InputPath -and $InputPath.Trim().Length -gt 0) {
        return (Resolve-Path $InputPath).Path
    }

    $repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
    $candidates = @(
        (Join-Path $repoRoot "build\Release\ApiumSlicerExplorerPreview.dll"),
        (Join-Path $repoRoot "build-ninja\ApiumSlicerExplorerPreview.dll")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "DLL not found. Pass -DllPath explicitly."
}

$resolvedDll = Resolve-DllPath -InputPath $DllPath
$args = @("/u")
if ($Silent) { $args += "/s" }
$args += $resolvedDll

$process = Start-Process -FilePath "regsvr32.exe" -ArgumentList $args -PassThru -Wait
if ($process.ExitCode -ne 0) {
    throw "regsvr32 /u failed with exit code $($process.ExitCode)."
}

Write-Host "Unregistered: $resolvedDll"
