param(
    [string[]]$Extensions = @(".gcode", ".a3d", ".stl", ".obj"),
    [switch]$Remove,
    [switch]$AllLoadedUsers,
    [switch]$RemoveApiumFileAssociations
)

$ErrorActionPreference = "Stop"

$previewIid = "{8895B1C6-B41F-4C1C-A562-0D564250836F}"
$thumbnailIid = "{E357FCCD-A995-4576-B01F-234630154E96}"
$apiumPreviewClsid = "{B450D582-1E34-49E0-8AE7-AFF2D2CD2535}"
$apiumThumbnailClsid = "{8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}"
$apiumPropertyClsid = "{12D795E2-813D-4E87-93BB-DC5E6239BB42}"
$legacyPreviewHostAppId = "{3B0FE91D-45E1-41E4-A6A7-6F8045AEF35A}"

$apiumClsids = @(
    $apiumPreviewClsid,
    $apiumThumbnailClsid,
    $apiumPropertyClsid
)

$knownApiumProgIds = @(
    "Apium.Slicer",
    "Apium.Slicer.1",
    "Apium.Slicer.Project.1",
    "ApiumSlicer.GCodeViewer.1"
)

$metadataValues = @(
    "PreviewDetails",
    "FullDetails",
    "InfoTip"
)

function Normalize-Extension {
    param([string]$Extension)
    $ext = $Extension.Trim()
    if (-not $ext.StartsWith(".")) {
        $ext = "." + $ext
    }
    return $ext.ToLowerInvariant()
}

function Test-Administrator {
    $principal = [Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-RegDefaultValue {
    param([string]$Path)
    try {
        return (Get-Item -LiteralPath $Path -ErrorAction Stop).GetValue("")
    } catch {
        return $null
    }
}

function Get-RegValue {
    param(
        [string]$Path,
        [string]$Name
    )
    try {
        return (Get-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction Stop).$Name
    } catch {
        return $null
    }
}

function Remove-RegKey {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
}

function Remove-RegValue {
    param(
        [string]$Path,
        [string]$Name
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    if ($Name -eq "(default)" -or $Name -eq "") {
        $regPath = $Path
        if ($regPath.StartsWith("Registry::")) {
            $regPath = $regPath.Substring("Registry::".Length)
        }
        & reg.exe delete $regPath /ve /f | Out-Null
        return
    }
    Remove-ItemProperty -LiteralPath $Path -Name $Name -Force -ErrorAction SilentlyContinue
}

function Add-Finding {
    param(
        [System.Collections.Generic.List[object]]$Findings,
        [string]$Scope,
        [string]$Kind,
        [string]$Path,
        [string]$Name = "",
        [string]$Value = "",
        [bool]$Removed = $false
    )
    $Findings.Add([pscustomobject]@{
        Scope   = $Scope
        Kind    = $Kind
        Path    = $Path
        Name    = $Name
        Value   = $Value
        Removed = $Removed
    }) | Out-Null
}

function Test-ApiumClsid {
    param([string]$Value)
    if (-not $Value) {
        return $false
    }
    foreach ($clsid in $apiumClsids) {
        if ($Value.ToLowerInvariant() -eq $clsid.ToLowerInvariant()) {
            return $true
        }
    }
    return $false
}

function Test-ApiumProgId {
    param([string]$Value)
    if (-not $Value) {
        return $false
    }
    foreach ($progId in $knownApiumProgIds) {
        if ($Value.ToLowerInvariant() -eq $progId.ToLowerInvariant()) {
            return $true
        }
    }
    return $false
}

function Test-ApiumMetadata {
    param([object]$Value)
    if (-not $Value) {
        return $false
    }
    return ([string]$Value) -like "*Apium.ApiumSlicer*"
}

function Check-HandlerKey {
    param(
        [System.Collections.Generic.List[object]]$Findings,
        [string]$Scope,
        [string]$Kind,
        [string]$Path
    )
    $value = Get-RegDefaultValue -Path $Path
    if (-not (Test-ApiumClsid -Value $value)) {
        return
    }

    $removed = $false
    if ($Remove) {
        Remove-RegKey -Path $Path
        $removed = $true
    }
    Add-Finding -Findings $Findings -Scope $Scope -Kind $Kind -Path $Path -Name "(default)" -Value $value -Removed $removed
}

function Check-MetadataValues {
    param(
        [System.Collections.Generic.List[object]]$Findings,
        [string]$Scope,
        [string]$Path
    )
    foreach ($name in $metadataValues) {
        $value = Get-RegValue -Path $Path -Name $name
        if (-not (Test-ApiumMetadata -Value $value)) {
            continue
        }

        $removed = $false
        if ($Remove) {
            Remove-RegValue -Path $Path -Name $name
            $removed = $true
        }
        Add-Finding -Findings $Findings -Scope $Scope -Kind "Metadata" -Path $Path -Name $name -Value ([string]$value) -Removed $removed
    }
}

function Check-ClassRoot {
    param(
        [System.Collections.Generic.List[object]]$Findings,
        [string]$Scope,
        [string]$ClassesRoot
    )
    if (-not (Test-Path -LiteralPath $ClassesRoot)) {
        return
    }

    foreach ($extRaw in $Extensions) {
        $ext = Normalize-Extension -Extension $extRaw
        $extBase = "$ClassesRoot\$ext"
        $sfaBase = "$ClassesRoot\SystemFileAssociations\$ext"

        Check-HandlerKey -Findings $Findings -Scope $Scope -Kind "PreviewHandler" -Path "$extBase\shellex\$previewIid"
        Check-HandlerKey -Findings $Findings -Scope $Scope -Kind "ThumbnailProvider" -Path "$extBase\shellex\$thumbnailIid"
        Check-HandlerKey -Findings $Findings -Scope $Scope -Kind "PropertyHandler" -Path "$extBase\shellex\PropertyHandler"
        Check-HandlerKey -Findings $Findings -Scope "$Scope/SystemFileAssociations" -Kind "PreviewHandler" -Path "$sfaBase\shellex\$previewIid"
        Check-HandlerKey -Findings $Findings -Scope "$Scope/SystemFileAssociations" -Kind "ThumbnailProvider" -Path "$sfaBase\shellex\$thumbnailIid"
        Check-HandlerKey -Findings $Findings -Scope "$Scope/SystemFileAssociations" -Kind "PropertyHandler" -Path "$sfaBase\shellex\PropertyHandler"

        Check-MetadataValues -Findings $Findings -Scope $Scope -Path $extBase

        $defaultProgId = Get-RegDefaultValue -Path $extBase
        if (Test-ApiumProgId -Value $defaultProgId) {
            $removed = $false
            if ($Remove -and $RemoveApiumFileAssociations) {
                Remove-RegValue -Path $extBase -Name "(default)"
                $removed = $true
            }
            Add-Finding -Findings $Findings -Scope $Scope -Kind "FileAssociation" -Path $extBase -Name "(default)" -Value $defaultProgId -Removed $removed
        }

    }

    foreach ($progId in $knownApiumProgIds) {
        $progBase = "$ClassesRoot\$progId"
        Check-HandlerKey -Findings $Findings -Scope "$Scope/ProgID:$progId" -Kind "PreviewHandler" -Path "$progBase\shellex\$previewIid"
        Check-HandlerKey -Findings $Findings -Scope "$Scope/ProgID:$progId" -Kind "ThumbnailProvider" -Path "$progBase\shellex\$thumbnailIid"
        Check-HandlerKey -Findings $Findings -Scope "$Scope/ProgID:$progId" -Kind "PropertyHandler" -Path "$progBase\shellex\PropertyHandler"
        Check-MetadataValues -Findings $Findings -Scope "$Scope/ProgID:$progId" -Path $progBase
    }

    foreach ($clsid in $apiumClsids) {
        $path = "$ClassesRoot\CLSID\$clsid"
        if (Test-Path -LiteralPath $path) {
            $removed = $false
            if ($Remove) {
                Remove-RegKey -Path $path
                $removed = $true
            }
            Add-Finding -Findings $Findings -Scope $Scope -Kind "COMClass" -Path $path -Name "" -Value $clsid -Removed $removed
        }
    }

    $appIdPath = "$ClassesRoot\AppID\$legacyPreviewHostAppId"
    if (Test-Path -LiteralPath $appIdPath) {
        $removed = $false
        if ($Remove) {
            Remove-RegKey -Path $appIdPath
            $removed = $true
        }
        Add-Finding -Findings $Findings -Scope $Scope -Kind "AppID" -Path $appIdPath -Name "" -Value $legacyPreviewHostAppId -Removed $removed
    }
}

function Check-PreviewHandlersList {
    param(
        [System.Collections.Generic.List[object]]$Findings,
        [string]$Scope,
        [string]$Path
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    foreach ($clsid in @($apiumPreviewClsid)) {
        $value = Get-RegValue -Path $Path -Name $clsid
        if (-not $value) {
            continue
        }
        $removed = $false
        if ($Remove) {
            Remove-RegValue -Path $Path -Name $clsid
            $removed = $true
        }
        Add-Finding -Findings $Findings -Scope $Scope -Kind "PreviewHandlersList" -Path $Path -Name $clsid -Value ([string]$value) -Removed $removed
    }
}

function Check-PropertyHandlersRoot {
    param(
        [System.Collections.Generic.List[object]]$Findings,
        [string]$Scope,
        [string]$Root
    )
    foreach ($extRaw in $Extensions) {
        $ext = Normalize-Extension -Extension $extRaw
        Check-HandlerKey -Findings $Findings -Scope $Scope -Kind "PropertySystemHandler" -Path "$Root\$ext"
    }
}

function Get-LoadedUserHives {
    $rows = @()
    $hkuRoot = "Registry::HKEY_USERS"
    if (-not (Test-Path -LiteralPath $hkuRoot)) {
        return $rows
    }

    foreach ($key in Get-ChildItem -LiteralPath $hkuRoot -ErrorAction SilentlyContinue) {
        $name = $key.PSChildName
        if ($name -match '^S-\d-\d+-(\d+-){1,}\d+$') {
            $rows += [pscustomobject]@{
                Sid = $name
                ClassesRoot = "$hkuRoot\$name`_Classes"
                UserRoot = "$hkuRoot\$name"
            }
        }
    }
    return $rows
}

$findings = [System.Collections.Generic.List[object]]::new()

Write-Host ("Identity: {0}" -f ([Security.Principal.WindowsIdentity]::GetCurrent().Name))
Write-Host ("Admin:    {0}" -f (Test-Administrator))
Write-Host ""

Check-ClassRoot -Findings $findings -Scope "HKCU.Classes" -ClassesRoot "Registry::HKEY_CURRENT_USER\Software\Classes"
Check-PreviewHandlersList -Findings $findings -Scope "HKCU" -Path "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers"
Check-PropertyHandlersRoot -Findings $findings -Scope "HKCU" -Root "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"

Check-ClassRoot -Findings $findings -Scope "HKLM.Classes" -ClassesRoot "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes"
Check-ClassRoot -Findings $findings -Scope "HKLM.WOW6432.Classes" -ClassesRoot "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Classes"
Check-PreviewHandlersList -Findings $findings -Scope "HKLM" -Path "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PreviewHandlers"
Check-PreviewHandlersList -Findings $findings -Scope "HKLM.WOW6432" -Path "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\PreviewHandlers"
Check-PropertyHandlersRoot -Findings $findings -Scope "HKLM" -Root "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"

if ($AllLoadedUsers) {
    foreach ($userHive in Get-LoadedUserHives) {
        Check-ClassRoot -Findings $findings -Scope "HKU:$($userHive.Sid)_Classes" -ClassesRoot $userHive.ClassesRoot
        Check-PreviewHandlersList -Findings $findings -Scope "HKU:$($userHive.Sid)" -Path "$($userHive.UserRoot)\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers"
        Check-PropertyHandlersRoot -Findings $findings -Scope "HKU:$($userHive.Sid)" -Root "$($userHive.UserRoot)\Software\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"
    }
}

if ($findings.Count -eq 0) {
    Write-Host "No Apium shell residue found in checked scopes."
} else {
    $findings |
        Sort-Object Scope, Kind, Path, Name -Unique |
        Format-Table Scope, Kind, Name, Value, Removed, Path -Wrap -AutoSize
}

Write-Host ""
if ($Remove) {
    Write-Host "Removal pass completed. Restart Explorer or sign out/in before retesting Explorer previews."
} else {
    Write-Host "Audit only. Re-run with -Remove to delete Apium preview/thumbnail/property residue."
}

if (-not $AllLoadedUsers) {
    Write-Host "Use -AllLoadedUsers from an elevated shell to inspect loaded user hives beyond the current HKCU."
}
