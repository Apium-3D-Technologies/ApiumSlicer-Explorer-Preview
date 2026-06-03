param(
    [switch]$Loop,
    [int]$IntervalSeconds = 2
)

$ErrorActionPreference = "Stop"

function Get-RegDefaultValue {
    param([string]$Path)
    try {
        return (Get-ItemProperty -Path $Path -ErrorAction Stop)."(default)"
    } catch {
        return $null
    }
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
            if ($row -and $row.PID -and ([int]$row.PID -eq $ProcessId)) {
                return $true
            }
        }
    } catch {
    }
    return $false
}

function Resolve-GuidContext {
    param([string]$Guid)

    $clsidKey = "Registry::HKEY_CLASSES_ROOT\CLSID\$Guid"
    $appidKey = "Registry::HKEY_CLASSES_ROOT\AppID\$Guid"
    $classIndexRoot = "Registry::HKEY_CLASSES_ROOT\PackagedCom\ClassIndex\$Guid"

    $clsidName = Get-RegDefaultValue -Path $clsidKey
    $appIdName = Get-RegDefaultValue -Path $appidKey

    $packageRows = @()
    if (Test-Path -LiteralPath $classIndexRoot) {
        $packages = Get-ChildItem -Path $classIndexRoot -ErrorAction SilentlyContinue
        foreach ($pkg in $packages) {
            $pkgName = $pkg.PSChildName
            $classKey = "Registry::HKEY_CLASSES_ROOT\PackagedCom\Package\$pkgName\Class\$Guid"
            $serverId = $null
            $dllPath = $null
            try {
                $classProps = Get-ItemProperty -Path $classKey -ErrorAction Stop
                $serverId = $classProps.ServerId
                $dllPath = $classProps.DllPath
            } catch {
            }

            $serverKey = $null
            if ($null -ne $serverId) {
                $serverKey = "Registry::HKEY_CLASSES_ROOT\PackagedCom\Package\$pkgName\Server\$serverId"
            }

            $serverDisplay = $null
            $serverExecutable = $null
            $serverAppId = $null
            if ($serverKey) {
                try {
                    $serverProps = Get-ItemProperty -Path $serverKey -ErrorAction Stop
                    $serverDisplay = $serverProps.DisplayName
                    $serverExecutable = $serverProps.Executable
                    $serverAppId = $serverProps.ApplicationId
                } catch {
                }
            }

            $packageRows += [pscustomobject]@{
                PackageName      = $pkgName
                DllPath          = $dllPath
                ServerDisplay    = $serverDisplay
                ServerExecutable = $serverExecutable
                ServerAppId      = $serverAppId
            }
        }
    }

    return [pscustomobject]@{
        Guid       = $Guid
        ClsidName  = $clsidName
        AppIdName  = $appIdName
        Packages   = $packageRows
    }
}

function Guess-Viewer {
    param($Context)

    if ($Context.ClsidName -and $Context.ClsidName -like "*Apium*") {
        return "Apium Preview Handler"
    }
    if ($Context.ClsidName) {
        return $Context.ClsidName
    }
    return "Unknown"
}

function Print-Snapshot {
    Write-Host ""
    Write-Host ("Snapshot: {0}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
    Write-Host ""

    $hosts = Get-CimInstance Win32_Process -Filter "Name='prevhost.exe' OR Name='dllhost.exe' OR Name='explorer.exe'" |
        Select-Object Name, ProcessId, CommandLine

    if (-not $hosts) {
        Write-Host "No preview-relevant host processes found."
        return
    }

    $rows = @()
    foreach ($h in $hosts) {
        $procId = [int]$h.ProcessId
        $cmd = [string]$h.CommandLine
        $hasApium = Test-ProcessHasModule -ProcessId $procId -ModuleName "ApiumSlicerExplorerPreview.dll"

        $guids = @([regex]::Matches($cmd, '\{[0-9A-Fa-f\-]{36}\}') | ForEach-Object { $_.Value } | Select-Object -Unique)
        if (-not $guids -or $guids.Count -eq 0) {
            $rows += [pscustomobject]@{
                Process    = $h.Name
                PID        = $procId
                HasApium   = $hasApium
                Guid       = ""
                Viewer     = ""
                Package    = ""
                DllPath    = ""
            }
            continue
        }

        foreach ($g in $guids) {
            $ctx = Resolve-GuidContext -Guid $g
            $viewer = Guess-Viewer -Context $ctx
            if ($ctx.Packages -and $ctx.Packages.Count -gt 0) {
                foreach ($p in $ctx.Packages) {
                    $rows += [pscustomobject]@{
                        Process    = $h.Name
                        PID        = $procId
                        HasApium   = $hasApium
                        Guid       = $g
                        Viewer     = $viewer
                        Package    = $p.PackageName
                        DllPath    = $p.DllPath
                    }
                }
            } else {
                $rows += [pscustomobject]@{
                    Process    = $h.Name
                    PID        = $procId
                    HasApium   = $hasApium
                    Guid       = $g
                    Viewer     = $viewer
                    Package    = ""
                    DllPath    = ""
                }
            }
        }
    }

    $rows |
        Sort-Object Process, PID, Guid, Package -Unique |
        Format-Table -Wrap -AutoSize
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
