<#
.SYNOPSIS
Unregister the shell extension or only its Explorer property schema.
.EXAMPLE
./scripts/unregister.ps1 -Soft
Unregister without clearing Explorer caches.
.EXAMPLE
./scripts/unregister.ps1 -MachineOnly
.EXAMPLE
./scripts/unregister.ps1 -SchemaOnly
Run elevated. Optionally pass -SchemaPath to select a schema file.
#>
param(
    [string]$DllPath = "",
    [switch]$Silent,
    [switch]$Soft,
    [switch]$ClearCache,
    [switch]$MachineOnly,
    [switch]$SchemaOnly,
    [string]$SchemaPath = "",
    [string[]]$Extensions = @(".gcode", ".a3d", ".stl", ".obj"),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"

$previewIid = "{8895B1C6-B41F-4C1C-A562-0D564250836F}"
$thumbnailIid = "{E357FCCD-A995-4576-B01F-234630154E96}"
$apiumPreviewClsid = "{B450D582-1E34-49E0-8AE7-AFF2D2CD2535}"
$apiumThumbnailClsid = "{8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}"
$apiumPropertyClsid = "{12D795E2-813D-4E87-93BB-DC5E6239BB42}"
$previewHostAppId = "{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}"
$legacyPreviewHostAppId = "{3B0FE91D-45E1-41E4-A6A7-6F8045AEF35A}"
$apiumProgId = "Apium.Slicer.1"
$apiumApplicationName = "Apium Slicer"
$apiumCapabilitiesSubPath = "Software\ApiumSlicerExplorerPreview\Capabilities"
$legacyRemovedExtensions = @(".3mf")

function Normalize-Extension {
    param([string]$Extension)
    $ext = $Extension.Trim()
    if (-not $ext.StartsWith(".")) {
        $ext = "." + $ext
    }
    return $ext.ToLowerInvariant()
}

if ($RemainingArgs -and $RemainingArgs.Count -gt 0) {
    $allowed = @("--soft", "--machine-only")
    $unknown = @($RemainingArgs | Where-Object { $_ -notin $allowed })
    if ($unknown.Count -gt 0) {
        throw "Unknown argument(s): $($unknown -join ', ')"
    }
    if ($RemainingArgs -contains "--soft") {
        $Soft = $true
    }
    if ($RemainingArgs -contains "--machine-only") {
        $MachineOnly = $true
    }
}

function Test-IsAdministrator {
    $principal = [Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Unregister-PropertySchema {
    param([string]$Path)

    if (-not (Test-IsAdministrator)) {
        throw "Property schema changes require an elevated PowerShell."
    }
    if ([string]::IsNullOrWhiteSpace($Path)) {
        $Path = Join-Path $PSScriptRoot "../resources/ApiumSlicer.propschema.propdesc"
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Schema file not found: $Path" }
    $resolvedSchema = (Resolve-Path -LiteralPath $Path).Path

    if (-not ([System.Management.Automation.PSTypeName]'ApiumPropertySchemaNative').Type) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class ApiumPropertySchemaNative {
    [DllImport("propsys.dll", CharSet = CharSet.Unicode)]
    public static extern int PSRegisterPropertySchema(string pszPath);

    [DllImport("propsys.dll", CharSet = CharSet.Unicode)]
    public static extern int PSUnregisterPropertySchema(string pszPath);

    [DllImport("propsys.dll")]
    public static extern int PSRefreshPropertySchema();
}
"@
    }
    $hr = [ApiumPropertySchemaNative]::PSUnregisterPropertySchema($resolvedSchema)
    if ($hr -ne 0) { throw ("PSUnregisterPropertySchema failed with HRESULT 0x{0:X8}" -f $hr) }
    $refreshHr = [ApiumPropertySchemaNative]::PSRefreshPropertySchema()
    if ($refreshHr -ne 0) { throw ("PSRefreshPropertySchema failed with HRESULT 0x{0:X8}" -f $refreshHr) }
    Write-Host "Unregistered property schema: $resolvedSchema"
}

# This mode replaces the standalone administrator schema script and does not
# require a compiled DLL or alter COM/file associations.
if ($SchemaOnly) {
    if ($MachineOnly -or $Soft) { throw "Use -SchemaOnly separately from -MachineOnly or -Soft." }
    Unregister-PropertySchema -Path $SchemaPath
    exit 0
}
if ($SchemaPath) { throw "Use -SchemaOnly with -SchemaPath." }

function Resolve-DllPath {
    param([string]$InputPath)

    if ($InputPath -and $InputPath.Trim().Length -gt 0) {
        return (Resolve-Path -LiteralPath $InputPath).Path
    }

    # Prefer the installed copy, even after the original build has been removed.
    $classesRoots = if ($MachineOnly) {
        @("HKEY_LOCAL_MACHINE\SOFTWARE\Classes")
    } else {
        @("HKEY_CURRENT_USER\Software\Classes", "HKEY_LOCAL_MACHINE\SOFTWARE\Classes")
    }
    foreach ($classesRoot in $classesRoots) {
        $key = "Registry::$classesRoot\CLSID\$apiumPreviewClsid\InprocServer32"
        $registeredDll = Get-DefaultValue -Path $key
        if ($registeredDll -and (Test-Path -LiteralPath $registeredDll -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $registeredDll).Path
        }
    }

    $repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
    $candidates = @(
        (Join-Path $repoRoot "build\Release\ApiumSlicerExplorerPreview.dll"),
        (Join-Path $repoRoot "build-ninja\ApiumSlicerExplorerPreview.dll")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "DLL not found. Pass -DllPath explicitly."
}

function Clear-PreviewCaches {
    Write-Host "Stopping Explorer to release preview cache handles..."
    Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 800

    $explorerCacheDir = Join-Path $env:LOCALAPPDATA "Microsoft\Windows\Explorer"
    $patterns = @(
        "thumbcache_*.db",
        "iconcache_*.db"
    )

    foreach ($pattern in $patterns) {
        Get-ChildItem -Path $explorerCacheDir -Filter $pattern -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }

    # Optional helper used by Windows to invalidate icon cache metadata.
    $ie4uinit = Join-Path $env:SystemRoot "System32\ie4uinit.exe"
    if (Test-Path $ie4uinit) {
        Start-Process -FilePath $ie4uinit -ArgumentList "-ClearIconCache" -WindowStyle Hidden -Wait
    }

    Write-Host "Restarting Explorer..."
    Start-Process -FilePath "explorer.exe"
}

function Remove-KeyIfExists {
    param([string]$Path)
    if (-not (Test-Path -Path $Path)) {
        return
    }

    try {
        Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        return
    } catch {
        $regPath = $Path
        if ($regPath.StartsWith("Registry::")) {
            $regPath = $regPath.Substring("Registry::".Length)
        }
        try {
            & reg.exe delete $regPath /f | Out-Null
            if ($LASTEXITCODE -eq 0) {
                return
            }
        } catch {
        }
        Write-Warning ("Could not remove registry key '{0}': {1}" -f $Path, $_.Exception.Message)
    }
}

function Remove-DefaultIfMatches {
    param(
        [string]$Path,
        [string]$ExpectedValue
    )

    $current = Get-DefaultValue -Path $Path
    if ($current -and $current.ToLowerInvariant() -eq $ExpectedValue.ToLowerInvariant()) {
        $regPath = $Path
        if ($regPath.StartsWith("Registry::")) {
            $regPath = $regPath.Substring("Registry::".Length)
        }
        & reg.exe delete $regPath /ve /f | Out-Null
    }
}

function Get-DefaultValue {
    param([string]$Path)
    try {
        return (Get-ItemProperty -Path $Path -ErrorAction Stop)."(default)"
    } catch {
        return $null
    }
}

function Remove-HandlerKeyIfMatches {
    param(
        [string]$Path,
        [string]$ExpectedClsid
    )

    $current = Get-DefaultValue -Path $Path
    if ($current -and $current.ToLowerInvariant() -eq $ExpectedClsid.ToLowerInvariant()) {
        Remove-KeyIfExists -Path $Path
    }
}

function Get-ValueNames {
    param([string]$Path)
    try {
        $item = Get-ItemProperty -Path $Path -ErrorAction Stop
        return @(
            $item.PSObject.Properties |
                Where-Object {
                    $_.Name -notlike "PS*" -and
                    $_.Name -ne "(default)"
                } |
                Select-Object -ExpandProperty Name
        )
    } catch {
        return @()
    }
}

function Resolve-ProgIds {
    param([string]$Extension)

    $progIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

    $userChoicePath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\UserChoice"
    try {
        $userChoiceProgId = (Get-ItemProperty -Path $userChoicePath -ErrorAction Stop).ProgId
        if ($userChoiceProgId) {
            $null = $progIds.Add($userChoiceProgId)
        }
    } catch {
    }

    $userChoiceLatestPath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\UserChoiceLatest\ProgId"
    try {
        $userChoiceLatestProgId = (Get-ItemProperty -Path $userChoiceLatestPath -ErrorAction Stop).ProgId
        if ($userChoiceLatestProgId) {
            $null = $progIds.Add($userChoiceLatestProgId)
        }
    } catch {
    }

    $defaultCandidates = @(
        "Registry::HKEY_CURRENT_USER\Software\Classes\$Extension",
        "Registry::HKEY_CLASSES_ROOT\$Extension"
    )
    foreach ($candidate in $defaultCandidates) {
        $value = Get-DefaultValue -Path $candidate
        if ($value) {
            $null = $progIds.Add($value)
        }
    }

    $openWithProgIdsPaths = @(
        "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\OpenWithProgids",
        "Registry::HKEY_CURRENT_USER\Software\Classes\$Extension\OpenWithProgids",
        "Registry::HKEY_CLASSES_ROOT\$Extension\OpenWithProgids"
    )
    foreach ($path in $openWithProgIdsPaths) {
        $valueNames = Get-ValueNames -Path $path
        foreach ($name in $valueNames) {
            if ($name) {
                $null = $progIds.Add($name)
            }
        }
    }

    return @($progIds)
}

function Remove-ApiumBindings {
    param(
        [string[]]$TargetExtensions,
        [string]$ClassesRoot = "Registry::HKEY_CURRENT_USER\Software\Classes"
    )

    foreach ($extRaw in $TargetExtensions) {
        $ext = Normalize-Extension -Extension $extRaw
        $extBase = "$ClassesRoot\$ext"
        Remove-HandlerKeyIfMatches -Path "$extBase\shellex\$previewIid" -ExpectedClsid $apiumPreviewClsid
        Remove-HandlerKeyIfMatches -Path "$extBase\shellex\$thumbnailIid" -ExpectedClsid $apiumThumbnailClsid
        Remove-HandlerKeyIfMatches -Path "$extBase\shellex\PropertyHandler" -ExpectedClsid $apiumPropertyClsid
        try {
            Remove-ItemProperty -Path "$extBase\OpenWithProgids" -Name $apiumProgId -ErrorAction SilentlyContinue
        } catch {
        }
        Remove-DefaultIfMatches -Path $extBase -ExpectedValue $apiumProgId

        $sfaBase = "$ClassesRoot\SystemFileAssociations\$ext"
        Remove-HandlerKeyIfMatches -Path "$sfaBase\shellex\$previewIid" -ExpectedClsid $apiumPreviewClsid
        Remove-HandlerKeyIfMatches -Path "$sfaBase\shellex\$thumbnailIid" -ExpectedClsid $apiumThumbnailClsid
        Remove-HandlerKeyIfMatches -Path "$sfaBase\shellex\PropertyHandler" -ExpectedClsid $apiumPropertyClsid

        $progIds = Resolve-ProgIds -Extension $ext
        foreach ($progId in $progIds) {
            if (-not $progId) {
                continue
            }
            $progBase = "$ClassesRoot\$progId"
            Remove-HandlerKeyIfMatches -Path "$progBase\shellex\$previewIid" -ExpectedClsid $apiumPreviewClsid
            Remove-HandlerKeyIfMatches -Path "$progBase\shellex\$thumbnailIid" -ExpectedClsid $apiumThumbnailClsid
            Remove-HandlerKeyIfMatches -Path "$progBase\shellex\PropertyHandler" -ExpectedClsid $apiumPropertyClsid
        }
    }

}

function Remove-PropertySystemBindings {
    param(
        [string[]]$TargetExtensions,
        [string]$PropertyHandlersPath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"
    )

    foreach ($extRaw in $TargetExtensions) {
        $ext = Normalize-Extension -Extension $extRaw
        Remove-HandlerKeyIfMatches -Path "$PropertyHandlersPath\$ext" -ExpectedClsid $apiumPropertyClsid
    }
}

function Get-SoftwareRootForClassesRoot {
    param([string]$ClassesRoot)
    if ($ClassesRoot -like "Registry::HKEY_LOCAL_MACHINE*") {
        return "Registry::HKEY_LOCAL_MACHINE\SOFTWARE"
    }
    return "Registry::HKEY_CURRENT_USER\Software"
}

function Remove-ApiumApplicationRegistration {
    param([string]$ClassesRoot)

    $softwareRoot = Get-SoftwareRootForClassesRoot -ClassesRoot $ClassesRoot
    Remove-KeyIfExists -Path "$ClassesRoot\$apiumProgId"
    Remove-KeyIfExists -Path "$softwareRoot\ApiumSlicerExplorerPreview\Capabilities"
    try {
        Remove-ItemProperty -Path "$softwareRoot\RegisteredApplications" -Name $apiumApplicationName -ErrorAction SilentlyContinue
    } catch {
    }
}

function Remove-UserApiumRegistration {
    $removalExtensions = @($Extensions + $legacyRemovedExtensions | Sort-Object -Unique)
    Remove-ApiumBindings -TargetExtensions $removalExtensions
    Remove-PropertySystemBindings -TargetExtensions $removalExtensions
    Remove-ApiumApplicationRegistration -ClassesRoot "Registry::HKEY_CURRENT_USER\Software\Classes"

    $previewHandlers = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers"
    try {
        Remove-ItemProperty -Path $previewHandlers -Name $apiumPreviewClsid -ErrorAction SilentlyContinue
    } catch {
    }

    Remove-KeyIfExists -Path "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$apiumPreviewClsid"
    Remove-KeyIfExists -Path "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$apiumThumbnailClsid"
    Remove-KeyIfExists -Path "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$apiumPropertyClsid"
    Remove-KeyIfExists -Path "Registry::HKEY_CURRENT_USER\Software\Classes\AppID\$legacyPreviewHostAppId"
}

if (-not ([System.Management.Automation.PSTypeName]'ShellNotify').Type) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class ShellNotify
{
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(int wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
}
"@
}

function Notify-AssociationChanged {
    [ShellNotify]::SHChangeNotify(0x08000000, 0x1000, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Remove-SystemApiumRegistration {
    if (-not (Test-IsAdministrator)) {
        Write-Warning "Skipping HKLM cleanup of Apium COM/PreviewHandlers (Run as Administrator for full cleanup)."
        return
    }

    $removalExtensions = @($Extensions + $legacyRemovedExtensions | Sort-Object -Unique)
    Remove-ApiumBindings `
        -TargetExtensions $removalExtensions `
        -ClassesRoot "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes"
    Remove-PropertySystemBindings `
        -TargetExtensions $removalExtensions `
        -PropertyHandlersPath "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"
    Remove-ApiumApplicationRegistration -ClassesRoot "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes"

    $previewHandlers64 = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PreviewHandlers"
    try {
        Remove-ItemProperty -Path $previewHandlers64 -Name $apiumPreviewClsid -ErrorAction SilentlyContinue
    } catch {
    }

    $previewHandlers32 = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\PreviewHandlers"
    try {
        Remove-ItemProperty -Path $previewHandlers32 -Name $apiumPreviewClsid -ErrorAction SilentlyContinue
    } catch {
    }

    Remove-KeyIfExists -Path "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\$apiumPreviewClsid"
    Remove-KeyIfExists -Path "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\$apiumThumbnailClsid"
    Remove-KeyIfExists -Path "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\$apiumPropertyClsid"
    Remove-KeyIfExists -Path "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\AppID\$legacyPreviewHostAppId"
    Write-Host "Removed HKLM COM + PreviewHandlers registration for Apium CLSIDs."
}

$resolvedDll = Resolve-DllPath -InputPath $DllPath

if ($MachineOnly) {
    Remove-SystemApiumRegistration
    Notify-AssociationChanged
    if ($ClearCache) {
        Clear-PreviewCaches
        Write-Host "Explorer thumbnail/preview cache cleared."
    } else {
        Write-Host "MachineOnly mode: skipped per-user unregister and cache clear."
    }
    exit 0
}

$args = @("/u", "/s")
$args += ('"{0}"' -f $resolvedDll)

$process = Start-Process -FilePath "regsvr32.exe" -ArgumentList $args -PassThru -Wait
if ($process.ExitCode -ne 0) {
    throw "regsvr32 /u failed with exit code $($process.ExitCode)."
}

Write-Host "Unregistered: $resolvedDll"

if (-not $Soft) {
    Remove-UserApiumRegistration
    Write-Host "Removed residual per-user Apium handler bindings."
    Remove-SystemApiumRegistration
}

Notify-AssociationChanged

if ($ClearCache -or (-not $Soft)) {
    Clear-PreviewCaches
    Write-Host "Explorer thumbnail/preview cache cleared."
} else {
    Write-Host "Soft mode enabled: skipped cache clear."
}
