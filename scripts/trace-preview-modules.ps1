param(
    [switch]$Loop,
    [int]$IntervalSeconds = 2
)

$ErrorActionPreference = "Stop"

function Get-ModuleUsers {
    param([string]$ModuleName)

    $rows = @()
    $output = & tasklist /m $ModuleName /fo csv /nh 2>$null
    foreach ($line in $output) {
        if (-not $line) {
            continue
        }
        if ($line -like "INFO:*") {
            continue
        }
        try {
            $csv = $line | ConvertFrom-Csv -Header "ImageName","PID","SessionName","SessionNum","MemUsage","Modules"
            if ($csv) {
                $rows += [pscustomobject]@{
                    ImageName = $csv.ImageName
                    PID       = $csv.PID
                    Module    = $ModuleName
                }
            }
        } catch {
        }
    }
    return $rows
}

function Test-ProcessHasModule {
    param(
        [int]$ProcessId,
        [string]$ModuleName
    )

    try {
        $lines = & tasklist /m $ModuleName /fi "PID eq $ProcessId" /fo csv /nh 2>$null
        foreach ($line in $lines) {
            if (-not $line -or $line -like "INFO:*") {
                continue
            }
            $row = $line | ConvertFrom-Csv -Header "ImageName","PID","SessionName","SessionNum","MemUsage","Modules"
            if ($row -and $row.PID) {
                if ([int]$row.PID -eq $ProcessId) {
                    return $true
                }
            }
        }
    } catch {
    }
    return $false
}

function Print-Snapshot {
    $targets = @(
        "ApiumSlicerExplorerPreview.dll"
    )

    Write-Host ""
    Write-Host ("Snapshot: {0}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
    Write-Host ""

    $all = @()
    foreach ($m in $targets) {
        $all += Get-ModuleUsers -ModuleName $m
    }

    if (-not $all -or $all.Count -eq 0) {
        Write-Host "No process currently reports loading Apium preview modules."
    } else {
        $all |
            Sort-Object Module, ImageName, PID -Unique |
            Format-Table -AutoSize
    }

    Write-Host ""
    Write-Host "Preview host processes (if running):"
    $hosts = Get-CimInstance Win32_Process -Filter "Name='prevhost.exe' OR Name='dllhost.exe' OR Name='explorer.exe' OR Name='PowerToys.Peek.UI.exe' OR Name='PowerToys.Peek.FilePreviewer.exe'" |
        Select-Object Name, ProcessId, CommandLine
    if ($hosts) {
        $hosts | Sort-Object Name, ProcessId | Format-Table -Wrap -AutoSize
        $activeHost = $hosts | Where-Object { $_.Name -in @('prevhost.exe','dllhost.exe','PowerToys.Peek.UI.exe','PowerToys.Peek.FilePreviewer.exe') }
        if ($activeHost) {
            Write-Host "Preview host active: YES"
            Write-Host ""
            Write-Host "Host module check:"
            $hostRows = @()
            foreach ($h in $hosts) {
                $procId = [int]$h.ProcessId
                $hasApium = Test-ProcessHasModule -ProcessId $procId -ModuleName "ApiumSlicerExplorerPreview.dll"
                $hostRows += [pscustomobject]@{
                    Name      = $h.Name
                    PID       = $procId
                    HasApium  = $hasApium
                }
            }
            $hostRows | Sort-Object Name, PID | Format-Table -AutoSize
        } else {
            Write-Host "Preview host active: NO (only Explorer is running)"
        }
    } else {
        Write-Host "No explorer/prevhost/dllhost process found."
    }
}

if ($IntervalSeconds -lt 1) {
    $IntervalSeconds = 1
}

if (-not $Loop) {
    Print-Snapshot
    exit 0
}

while ($true) {
    Print-Snapshot
    Start-Sleep -Seconds $IntervalSeconds
}
