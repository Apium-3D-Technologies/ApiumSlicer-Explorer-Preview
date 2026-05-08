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
cmake -S $repoRoot -B $ninjaBuildDir -G Ninja -DCMAKE_BUILD_TYPE=$Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Building ($Config): $ninjaBuildDir"
cmake --build $ninjaBuildDir
exit $LASTEXITCODE
