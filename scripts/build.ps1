<#
.SYNOPSIS
Build the extension and optionally refresh local registration.
.DESCRIPTION
Use -Register to release DLL locks before building, then unregister and
register the new DLL. Use -StopProcesses to release locks without registering.
Only processes holding the target DLL are stopped; Explorer is restored if needed.
.EXAMPLE
./scripts/build.ps1 -Register
.EXAMPLE
./scripts/build.ps1 -Ninja -Config Release
.EXAMPLE
./scripts/build.ps1 -BuildDir build-review -Clean -StopProcesses
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet("VisualStudio", "Ninja")]
    [string]$Generator = "VisualStudio",

    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",

    [string]$BuildDir = "",
    [switch]$Clean,
    [switch]$Ninja,
    [switch]$Register,
    [switch]$StopProcesses,
    [switch]$SoftStop,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$script:restartExplorer = $false

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

function Get-LockingPidsFromRestartManager {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath
    )

    if (-not ([System.Management.Automation.PSTypeName]'RestartManagerNative').Type) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class RestartManagerNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RM_UNIQUE_PROCESS {
        public int dwProcessId;
        public System.Runtime.InteropServices.ComTypes.FILETIME ProcessStartTime;
    }

    public enum RM_APP_TYPE {
        RmUnknownApp = 0,
        RmMainWindow = 1,
        RmOtherWindow = 2,
        RmService = 3,
        RmExplorer = 4,
        RmConsole = 5,
        RmCritical = 1000
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct RM_PROCESS_INFO {
        public RM_UNIQUE_PROCESS Process;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string strAppName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string strServiceShortName;
        public RM_APP_TYPE ApplicationType;
        public uint AppStatus;
        public uint TSSessionId;
        [MarshalAs(UnmanagedType.Bool)]
        public bool bRestartable;
    }

    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    public static extern int RmStartSession(out uint pSessionHandle, int dwSessionFlags, string strSessionKey);

    [DllImport("rstrtmgr.dll")]
    public static extern int RmEndSession(uint pSessionHandle);

    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    public static extern int RmRegisterResources(
        uint pSessionHandle,
        uint nFiles,
        string[] rgsFilenames,
        uint nApplications,
        IntPtr rgApplications,
        uint nServices,
        string[] rgsServiceNames
    );

    [DllImport("rstrtmgr.dll")]
    public static extern int RmGetList(
        uint dwSessionHandle,
        out uint pnProcInfoNeeded,
        ref uint pnProcInfo,
        [In, Out] RM_PROCESS_INFO[] rgAffectedApps,
        ref uint lpdwRebootReasons
    );
}
"@
    }

    [uint32]$sessionHandle = 0
    $sessionKey = [Guid]::NewGuid().ToString()
    $start = [RestartManagerNative]::RmStartSession([ref]$sessionHandle, 0, $sessionKey)
    if ($start -ne 0) {
        return @()
    }

    try {
        $register = [RestartManagerNative]::RmRegisterResources(
            $sessionHandle,
            1,
            @($FilePath),
            0,
            [IntPtr]::Zero,
            0,
            $null
        )
        if ($register -ne 0) {
            return @()
        }

        [uint32]$needed = 0
        [uint32]$count = 0
        [uint32]$reasons = 0
        $first = [RestartManagerNative]::RmGetList($sessionHandle, [ref]$needed, [ref]$count, $null, [ref]$reasons)

        $ERROR_MORE_DATA = 234
        if ($first -ne 0 -and $first -ne $ERROR_MORE_DATA) {
            return @()
        }
        if ($needed -eq 0) {
            return @()
        }

        $apps = New-Object RestartManagerNative+RM_PROCESS_INFO[] $needed
        $count = $needed
        $second = [RestartManagerNative]::RmGetList($sessionHandle, [ref]$needed, [ref]$count, $apps, [ref]$reasons)
        if ($second -ne 0) {
            return @()
        }

        $pids = @()
        for ($i = 0; $i -lt [int]$count; $i++) {
            $pids += $apps[$i].Process.dwProcessId
        }
        return $pids
    }
    finally {
        [void][RestartManagerNative]::RmEndSession($sessionHandle)
    }
}

function Get-LockingPidsFromProcessModules {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ResolvedDllPath
    )

    $wanted = $ResolvedDllPath.ToLowerInvariant()
    $pids = New-Object System.Collections.Generic.HashSet[int]

    foreach ($proc in Get-Process -ErrorAction SilentlyContinue) {
        try {
            foreach ($module in $proc.Modules) {
                if ($null -eq $module.FileName) {
                    continue
                }
                if ($module.FileName.ToLowerInvariant() -eq $wanted) {
                    [void]$pids.Add($proc.Id)
                    break
                }
            }
        }
        catch {
            continue
        }
    }

    return @($pids)
}

function Stop-PidList {
    param(
        [int[]]$Pids,
        [switch]$SoftStop
    )

    $currentPid = $PID
    foreach ($pidValue in ($Pids | Sort-Object -Unique)) {
        if ($pidValue -eq $currentPid) {
            continue
        }

        $proc = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
        if (-not $proc) {
            continue
        }

        try {
            if ($SoftStop) {
                Stop-Process -Id $pidValue -ErrorAction Stop
            }
            else {
                Stop-Process -Id $pidValue -Force -ErrorAction Stop
            }
            if ($proc.ProcessName -eq "explorer") { $script:restartExplorer = $true }
            Write-Host ("Stopped PID {0} ({1})" -f $proc.Id, $proc.ProcessName)
        }
        catch {
            Write-Warning ("Could not stop PID {0} ({1}): {2}" -f $proc.Id, $proc.ProcessName, $_.Exception.Message)
        }
    }
}

function Test-ExclusiveWriteOpen {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        $fs.Close()
        return $true
    }
    catch {
        return $false
    }
}

function Import-VsDevEnvironment {
    $vsDevCmd = Get-VsDevCmdPath
    # Only the trusted VS installation path enters cmd.exe. Build arguments are
    # passed directly to CMake so paths with spaces remain individual arguments.
    $environment = cmd.exe /d /c "`"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "VS DevCmd failed with exit code $LASTEXITCODE." }
    foreach ($line in $environment) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

function Release-DllLocks {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    for ($attempt = 1; $attempt -le 6; $attempt++) {
        if (Test-ExclusiveWriteOpen -Path $Path) { return }
        $rmPids = @(Get-LockingPidsFromRestartManager -FilePath $Path)
        $modulePids = @(Get-LockingPidsFromProcessModules -ResolvedDllPath $Path)
        $lockingPids = @($rmPids + $modulePids | Sort-Object -Unique)
        if ($lockingPids.Count -gt 0) {
            Stop-PidList -Pids $lockingPids -SoftStop:$SoftStop
        }
        Start-Sleep -Milliseconds 300
    }
    if (-not (Test-ExclusiveWriteOpen -Path $Path)) {
        throw "DLL is still locked. Close previews of .gcode/.a3d/.stl/.obj files and retry: $Path"
    }
}

function Invoke-RegistrationScript {
    param([string]$Name, [string[]]$Arguments)

    $powershell = Join-Path $env:SystemRoot "System32/WindowsPowerShell/v1.0/powershell.exe"
    & $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $Name) @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE." }
}

function Restore-Explorer {
    if ($script:restartExplorer -and -not (Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
        Start-Process -FilePath "explorer.exe" -WindowStyle Hidden | Out-Null
    }
}

if ($RemainingArgs -and $RemainingArgs.Count -gt 0) {
    $unknown = @($RemainingArgs | Where-Object { $_ -ne "--ninja" })
    if ($unknown.Count -gt 0) { throw "Unknown argument(s): $($unknown -join ', ')" }
    if ($RemainingArgs -contains "--ninja") { $Ninja = $true }
}
if ($Ninja) { $Generator = "Ninja" }
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = if ($Generator -eq "Ninja") { "build-ninja" } else { "build" }
}
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repoRoot $BuildDir }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$dllPath = if ($Generator -eq "Ninja") {
    Join-Path $BuildDir "ApiumSlicerExplorerPreview.dll"
} else {
    Join-Path $BuildDir "$Config/ApiumSlicerExplorerPreview.dll"
}

try {
    # Release the old binary before deleting it or linking a replacement.
    if ($Register -or $StopProcesses) { Release-DllLocks -Path $dllPath }
    if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
        $workspacePrefix = $repoRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        if (-not $BuildDir.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath (Join-Path $BuildDir "CMakeCache.txt") -PathType Leaf)) {
            throw "Refusing to clean a directory that is not a CMake build directory inside the repository: $BuildDir"
        }
        Write-Host "Cleaning build directory: $BuildDir"
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }

    Write-Host "Configuring ($Generator): $BuildDir"
    if ($Generator -eq "VisualStudio") {
        cmake -S $repoRoot -B $BuildDir -G "Visual Studio 17 2022" -A x64
    } else {
        if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Import-VsDevEnvironment }
        cmake -S $repoRoot -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
    }
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

    Write-Host "Building ($Config): $BuildDir"
    cmake --build $BuildDir --config $Config
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE." }

    if ($Register) {
        $resolvedDll = (Resolve-Path -LiteralPath $dllPath).Path
        Invoke-RegistrationScript -Name "unregister.ps1" -Arguments @("-DllPath", $resolvedDll, "-Soft")
        Invoke-RegistrationScript -Name "register.ps1" -Arguments @("-DllPath", $resolvedDll)
        Write-Host "Development registration completed."
    }
} finally {
    Restore-Explorer
}
