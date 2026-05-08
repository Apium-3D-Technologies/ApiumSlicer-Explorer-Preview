[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet("VisualStudio", "Ninja")]
    [string]$Generator = "VisualStudio",

    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",

    [switch]$Clean,

    [switch]$Ninja,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Get-VsWherePath {
    $candidates = @()
    if (${env:ProgramFiles(x86)}) {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe")
    }
    if ($env:ProgramFiles) {
        $candidates += (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    }

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }
    return $null
}

function Get-VsDevCmdPath {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        throw "vswhere.exe not found. Install Visual Studio 2022 with C++ tools."
    }

    $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installPath) {
        throw "No Visual Studio installation with MSVC tools was found."
    }

    $installPath = ($installPath | Select-Object -First 1).Trim()
    $vsDevCmd = Join-Path $installPath "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmd)) {
        throw "VsDevCmd.bat not found at: $vsDevCmd"
    }

    return $vsDevCmd
}

function Invoke-InVsDevCmd {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CommandLine
    )

    $vsDevCmd = Get-VsDevCmdPath
    $wrapped = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 && $CommandLine"
    cmd.exe /c $wrapped
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

# Support Linux-style flag usage: --ninja
if ($RemainingArgs -and $RemainingArgs.Count -gt 0) {
    $allowed = @("--ninja")
    $unknown = @($RemainingArgs | Where-Object { $_ -notin $allowed })
    if ($unknown.Count -gt 0) {
        throw "Unknown argument(s): $($unknown -join ', ')"
    }

    if ($RemainingArgs -contains "--ninja") {
        $Ninja = $true
    }
}

if ($Ninja) {
    $Generator = "Ninja"
}

if ($Generator -eq "VisualStudio") {
    $buildDir = Join-Path $repoRoot "build"
    if ($Clean -and (Test-Path $buildDir)) {
        Write-Host "Cleaning build directory: $buildDir"
        Remove-Item -Recurse -Force $buildDir
    }

    Write-Host "Configuring (Visual Studio): $buildDir"
    cmake -S $repoRoot -B $buildDir -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "Building ($Config): $buildDir"
    cmake --build $buildDir --config $Config
    exit $LASTEXITCODE
}

$ninjaBuildDir = Join-Path $repoRoot "build-ninja"
if ($Clean -and (Test-Path $ninjaBuildDir)) {
    Write-Host "Cleaning build directory: $ninjaBuildDir"
    Remove-Item -Recurse -Force $ninjaBuildDir
}

Write-Host "Configuring (Ninja): $ninjaBuildDir"
$configureCmd = "cmake -S `"$repoRoot`" -B `"$ninjaBuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config"
$buildCmd = "cmake --build `"$ninjaBuildDir`""

$clAvailable = $null -ne (Get-Command cl.exe -ErrorAction SilentlyContinue)
if ($clAvailable) {
    cmake -S $repoRoot -B $ninjaBuildDir -G Ninja -DCMAKE_BUILD_TYPE=$Config
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "Building ($Config): $ninjaBuildDir"
    cmake --build $ninjaBuildDir
    exit $LASTEXITCODE
}

Write-Host "MSVC not found in this shell. Bootstrapping VS DevCmd."
Invoke-InVsDevCmd -CommandLine $configureCmd
Write-Host "Building ($Config): $ninjaBuildDir"
Invoke-InVsDevCmd -CommandLine $buildCmd
exit $LASTEXITCODE
