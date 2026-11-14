[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",

    [string]$BuildDir = "",

    [string]$DllPath = "",

    [switch]$Clean,

    [switch]$SoftStop
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build"
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "==> $Name"
    & $Action
}

function Invoke-ChildPowerShellScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,

        [string[]]$Arguments = @()
    )

    $powershell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    $argumentList = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $ScriptPath
    ) + $Arguments

    & $powershell @argumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$ScriptPath failed with exit code $LASTEXITCODE."
    }
}

function Ensure-ExplorerRunning {
    $explorer = Get-Process -Name explorer -ErrorAction SilentlyContinue
    if ($explorer) {
        return
    }

    Write-Host ""
    Write-Host "Explorer is not running. Starting explorer.exe..."
    Start-Process -FilePath "explorer.exe" | Out-Null

    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 250
        if (Get-Process -Name explorer -ErrorAction SilentlyContinue) {
            Write-Host "Explorer restarted."
            return
        }
    }

    Write-Warning "explorer.exe was started, but no explorer process was detected yet."
}

try {
    if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
        Invoke-Step -Name "Clean build directory" -Action {
            Remove-Item -LiteralPath $BuildDir -Recurse -Force
        }
    }

    Invoke-Step -Name "Configure CMake" -Action {
        cmake -S $repoRoot -B $BuildDir -G "Visual Studio 17 2022" -A x64
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed with exit code $LASTEXITCODE."
        }
    }

    Invoke-Step -Name "Build $Config" -Action {
        cmake --build $BuildDir --config $Config
        if ($LASTEXITCODE -ne 0) {
            throw "CMake build failed with exit code $LASTEXITCODE."
        }
    }

    if ([string]::IsNullOrWhiteSpace($DllPath)) {
        $DllPath = Join-Path $BuildDir "$Config\ApiumSlicerExplorerPreview.dll"
    }
    $resolvedDll = (Resolve-Path -LiteralPath $DllPath).Path

    $stopArgs = @("-DllPath", $resolvedDll)
    if ($SoftStop) {
        $stopArgs += "-Soft"
    }
    Invoke-Step -Name "Stop preview host processes" -Action {
        Invoke-ChildPowerShellScript -ScriptPath (Join-Path $PSScriptRoot "stop-processes.ps1") -Arguments $stopArgs
    }
    Ensure-ExplorerRunning

    Invoke-Step -Name "Unregister extension" -Action {
        Invoke-ChildPowerShellScript `
            -ScriptPath (Join-Path $PSScriptRoot "unregister.ps1") `
            -Arguments @("-DllPath", $resolvedDll, "-Soft")
    }

    Invoke-Step -Name "Register extension" -Action {
        Invoke-ChildPowerShellScript `
            -ScriptPath (Join-Path $PSScriptRoot "register.ps1") `
            -Arguments @("-DllPath", $resolvedDll)
    }

    Write-Host ""
    Write-Host "Development registration completed."
}
finally {
    Ensure-ExplorerRunning
}
