param(
    [string]$DllPath,
    [switch]$Soft,
    [switch]$IncludeKnownHosts = $true,
    [int]$MaxAttempts = 6,
    [int]$RetryDelayMs = 300
)

$ErrorActionPreference = "Stop"

function Resolve-RepoDllPath {
    param(
        [string]$RepoRoot
    )

    $candidates = @(
        (Join-Path $RepoRoot "build\Release\ApiumSlicerExplorerPreview.dll"),
        (Join-Path $RepoRoot "build-ninja\ApiumSlicerExplorerPreview.dll")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "No ApiumSlicerExplorerPreview.dll found in build outputs. Pass -DllPath explicitly."
}

function Get-LockingPidsFromRestartManager {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath
    )

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
"@ -ErrorAction SilentlyContinue

    $sessionHandle = 0
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

$repoRoot = Split-Path -Parent $PSScriptRoot
$resolvedDll = if ($DllPath) { (Resolve-Path -LiteralPath $DllPath).Path } else { Resolve-RepoDllPath -RepoRoot $repoRoot }

Write-Host "Target DLL: $resolvedDll"

$knownHosts = @(
    "prevhost",
    "dllhost",
    "explorer",
    "PowerToys.Peek.UI",
    "PowerToys.Peek"
)

for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
    $rmPids = Get-LockingPidsFromRestartManager -FilePath $resolvedDll
    $modulePids = Get-LockingPidsFromProcessModules -ResolvedDllPath $resolvedDll
    $allPids = @($rmPids + $modulePids | Sort-Object -Unique)

    if ($allPids.Count -gt 0) {
        Write-Host ("Attempt {0}: detected locking PIDs: {1}" -f $attempt, (($allPids | ForEach-Object { $_.ToString() }) -join ", "))
        Stop-PidList -Pids $allPids -SoftStop:$Soft
    } else {
        Write-Host ("Attempt {0}: no locking PIDs detected." -f $attempt)
    }

    if ($IncludeKnownHosts) {
        foreach ($name in $knownHosts) {
            $procs = Get-Process -Name $name -ErrorAction SilentlyContinue
            foreach ($proc in $procs) {
                try {
                    if ($Soft) {
                        Stop-Process -Id $proc.Id -ErrorAction Stop
                    }
                    else {
                        Stop-Process -Id $proc.Id -Force -ErrorAction Stop
                    }
                    Write-Host ("Stopped host process {0} (PID {1})" -f $proc.ProcessName, $proc.Id)
                }
                catch {
                    Write-Warning ("Could not stop host process {0} (PID {1}): {2}" -f $proc.ProcessName, $proc.Id, $_.Exception.Message)
                }
            }
        }
    }

    Start-Sleep -Milliseconds $RetryDelayMs

    if (Test-ExclusiveWriteOpen -Path $resolvedDll) {
        Write-Host ("DLL is now writable after attempt {0}." -f $attempt)
        Write-Host "Done."
        exit 0
    }
}

Write-Warning "DLL is still locked after all attempts. Close Explorer windows showing .gcode/.a3d files and rerun."
exit 1
