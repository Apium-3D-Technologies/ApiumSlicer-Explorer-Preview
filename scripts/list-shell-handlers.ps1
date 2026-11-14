param(
    [string[]]$Extensions = @(".gcode", ".a3d", ".stl", ".obj")
)

$ErrorActionPreference = "Stop"

$previewIid = "{8895B1C6-B41F-4C1C-A562-0D564250836F}"
$thumbnailIid = "{E357FCCD-A995-4576-B01F-234630154E96}"
$apiumPreviewClsid = "{B450D582-1E34-49E0-8AE7-AFF2D2CD2535}"
$apiumThumbnailClsid = "{8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}"
$apiumPropertyClsid = "{12D795E2-813D-4E87-93BB-DC5E6239BB42}"
$propertyExtensions = @(".gcode", ".a3d")

function Normalize-Extension {
    param([string]$Extension)
    $ext = $Extension.Trim()
    if (-not $ext.StartsWith(".")) {
        $ext = "." + $ext
    }
    return $ext.ToLowerInvariant()
}

function Get-RegDefaultValue {
    param([string]$Path)
    try {
        $item = Get-ItemProperty -Path $Path -ErrorAction Stop
        return $item.'(default)'
    } catch {
        return $null
    }
}

function Get-UserChoiceProgId {
    param([string]$Extension)
    $path = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\UserChoice"
    try {
        return (Get-ItemProperty -Path $path -ErrorAction Stop).ProgId
    } catch {
        return $null
    }
}

function Get-UserChoiceLatestProgId {
    param([string]$Extension)
    $path = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\UserChoiceLatest\ProgId"
    try {
        return (Get-ItemProperty -Path $path -ErrorAction Stop).ProgId
    } catch {
        return $null
    }
}

function Resolve-InprocDll {
    param([string]$Clsid)
    if (-not $Clsid) {
        return $null
    }

    $candidates = @(
        "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$Clsid\InprocServer32",
        "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$Clsid\InprocServer32",
        "Registry::HKEY_CLASSES_ROOT\CLSID\$Clsid\InprocServer32"
    )

    foreach ($path in $candidates) {
        $dll = Get-RegDefaultValue -Path $path
        if ($dll) {
            return $dll
        }
    }
    return $null
}

function Add-HandlerResult {
    param(
        [ref]$Results,
        [string]$Extension,
        [string]$Scope,
        [string]$HandlerType,
        [string]$RegistryPath,
        [string]$Clsid
    )

    $dll = Resolve-InprocDll -Clsid $Clsid
    $Results.Value += [pscustomobject]@{
        Extension    = $Extension
        Scope        = $Scope
        HandlerType  = $HandlerType
        Clsid        = $Clsid
        DllPath      = $dll
        RegistryPath = $RegistryPath
    }
}

function Get-HandlerSubkeyName {
    param([string]$HandlerType)
    switch ($HandlerType) {
        "PreviewHandler" { return $previewIid }
        "ThumbnailProvider" { return $thumbnailIid }
        "PropertyHandler" { return "PropertyHandler" }
        default { throw "Unknown handler type: $HandlerType" }
    }
}

function Resolve-EffectiveHandler {
    param(
        [string]$Extension,
        [string]$HandlerType
    )

    $subkey = Get-HandlerSubkeyName -HandlerType $HandlerType
    $progId = Get-RegDefaultValue -Path "Registry::HKEY_CLASSES_ROOT\$Extension"
    $userChoiceProgId = Get-UserChoiceProgId -Extension $Extension
    $userChoiceLatestProgId = Get-UserChoiceLatestProgId -Extension $Extension

    $candidatePaths = @(
        "Registry::HKEY_CLASSES_ROOT\$Extension\shellex\$subkey"
    )
    if ($userChoiceProgId) {
        $candidatePaths = @("Registry::HKEY_CLASSES_ROOT\$userChoiceProgId\shellex\$subkey") + $candidatePaths
    }
    if ($userChoiceLatestProgId) {
        $candidatePaths = @("Registry::HKEY_CLASSES_ROOT\$userChoiceLatestProgId\shellex\$subkey") + $candidatePaths
    }
    if ($progId) {
        $candidatePaths += "Registry::HKEY_CLASSES_ROOT\$progId\shellex\$subkey"
    }
    $candidatePaths += "Registry::HKEY_CLASSES_ROOT\SystemFileAssociations\$Extension\shellex\$subkey"

    foreach ($path in $candidatePaths) {
        $clsid = Get-RegDefaultValue -Path $path
        if ($clsid) {
            return [pscustomobject]@{
                Extension    = $Extension
                HandlerType  = $HandlerType
                Clsid        = $clsid
                DllPath      = Resolve-InprocDll -Clsid $clsid
                RegistryPath = $path
            }
        }
    }

    return [pscustomobject]@{
        Extension    = $Extension
        HandlerType  = $HandlerType
        Clsid        = $null
        DllPath      = $null
        RegistryPath = $null
    }
}

$normalizedExtensions = @($Extensions | ForEach-Object { Normalize-Extension -Extension $_ } | Sort-Object -Unique)
$results = @()

foreach ($ext in $normalizedExtensions) {
    $roots = @(
        @{ Scope = "HKCU.Classes"; Prefix = "Registry::HKEY_CURRENT_USER\Software\Classes" },
        @{ Scope = "HKCR"; Prefix = "Registry::HKEY_CLASSES_ROOT" }
    )

    foreach ($root in $roots) {
        $prefix = $root.Prefix
        $scope = $root.Scope

        $previewKey = "$prefix\$ext\shellex\$previewIid"
        $previewClsid = Get-RegDefaultValue -Path $previewKey
        if ($previewClsid) {
            Add-HandlerResult -Results ([ref]$results) -Extension $ext -Scope $scope -HandlerType "PreviewHandler" -RegistryPath $previewKey -Clsid $previewClsid
        }

        $thumbKey = "$prefix\$ext\shellex\$thumbnailIid"
        $thumbClsid = Get-RegDefaultValue -Path $thumbKey
        if ($thumbClsid) {
            Add-HandlerResult -Results ([ref]$results) -Extension $ext -Scope $scope -HandlerType "ThumbnailProvider" -RegistryPath $thumbKey -Clsid $thumbClsid
        }

        $propertyKey = "$prefix\$ext\shellex\PropertyHandler"
        $propertyClsid = Get-RegDefaultValue -Path $propertyKey
        if ($propertyClsid) {
            Add-HandlerResult -Results ([ref]$results) -Extension $ext -Scope $scope -HandlerType "PropertyHandler" -RegistryPath $propertyKey -Clsid $propertyClsid
        }

        $assocPreviewKey = "$prefix\SystemFileAssociations\$ext\shellex\$previewIid"
        $assocPreviewClsid = Get-RegDefaultValue -Path $assocPreviewKey
        if ($assocPreviewClsid) {
            Add-HandlerResult -Results ([ref]$results) -Extension $ext -Scope "$scope/SystemFileAssociations" -HandlerType "PreviewHandler" -RegistryPath $assocPreviewKey -Clsid $assocPreviewClsid
        }

        $assocThumbKey = "$prefix\SystemFileAssociations\$ext\shellex\$thumbnailIid"
        $assocThumbClsid = Get-RegDefaultValue -Path $assocThumbKey
        if ($assocThumbClsid) {
            Add-HandlerResult -Results ([ref]$results) -Extension $ext -Scope "$scope/SystemFileAssociations" -HandlerType "ThumbnailProvider" -RegistryPath $assocThumbKey -Clsid $assocThumbClsid
        }

        $assocPropKey = "$prefix\SystemFileAssociations\$ext\shellex\PropertyHandler"
        $assocPropClsid = Get-RegDefaultValue -Path $assocPropKey
        if ($assocPropClsid) {
            Add-HandlerResult -Results ([ref]$results) -Extension $ext -Scope "$scope/SystemFileAssociations" -HandlerType "PropertyHandler" -RegistryPath $assocPropKey -Clsid $assocPropClsid
        }

        $progId = Get-RegDefaultValue -Path "$prefix\$ext"
        if ($progId) {
            $progPreviewKey = "$prefix\$progId\shellex\$previewIid"
            $progPreviewClsid = Get-RegDefaultValue -Path $progPreviewKey
            if ($progPreviewClsid) {
                Add-HandlerResult -Results ([ref]$results) -Extension $ext -Scope "$scope/ProgID:$progId" -HandlerType "PreviewHandler" -RegistryPath $progPreviewKey -Clsid $progPreviewClsid
            }

            $progThumbKey = "$prefix\$progId\shellex\$thumbnailIid"
            $progThumbClsid = Get-RegDefaultValue -Path $progThumbKey
            if ($progThumbClsid) {
                Add-HandlerResult -Results ([ref]$results) -Extension $ext -Scope "$scope/ProgID:$progId" -HandlerType "ThumbnailProvider" -RegistryPath $progThumbKey -Clsid $progThumbClsid
            }

            $progPropKey = "$prefix\$progId\shellex\PropertyHandler"
            $progPropClsid = Get-RegDefaultValue -Path $progPropKey
            if ($progPropClsid) {
                Add-HandlerResult -Results ([ref]$results) -Extension $ext -Scope "$scope/ProgID:$progId" -HandlerType "PropertyHandler" -RegistryPath $progPropKey -Clsid $progPropClsid
            }
        }
    }
}

Write-Host ""
Write-Host "Resolved handlers for: $($normalizedExtensions -join ', ')"
Write-Host ""

Write-Host "UserChoice ProgID (HKCU FileExts):"
foreach ($ext in $normalizedExtensions) {
    $uc = Get-UserChoiceProgId -Extension $ext
    $ucl = Get-UserChoiceLatestProgId -Extension $ext
    if ($uc) {
        Write-Host (" - {0}: {1}" -f $ext, $uc)
    } else {
        Write-Host (" - {0}: (none)" -f $ext)
    }
    if ($ucl) {
        Write-Host ("   UserChoiceLatest: {0}" -f $ucl)
    }
}
Write-Host ""

if (-not $results -or $results.Count -eq 0) {
    Write-Host "No shell handlers found."
} else {
    $results = $results | Sort-Object Extension, HandlerType, Scope, Clsid, DllPath -Unique
    $results | Format-Table -AutoSize
}

$effective = @()
foreach ($ext in $normalizedExtensions) {
    $effective += Resolve-EffectiveHandler -Extension $ext -HandlerType "PreviewHandler"
    $effective += Resolve-EffectiveHandler -Extension $ext -HandlerType "ThumbnailProvider"
    $effective += Resolve-EffectiveHandler -Extension $ext -HandlerType "PropertyHandler"
}

Write-Host ""
Write-Host "Effective winner (HKCR merged view):"
Write-Host ""
$effective | Format-Table -AutoSize

$expectedByType = @{
    "PreviewHandler"    = $apiumPreviewClsid
    "ThumbnailProvider" = $apiumThumbnailClsid
    "PropertyHandler"   = $apiumPropertyClsid
}

$criticalExts = $normalizedExtensions
$warnings = @()
foreach ($row in $effective) {
    if ($row.Extension -in $criticalExts) {
        $expected = $expectedByType[$row.HandlerType]
        if ($row.HandlerType -eq "PropertyHandler" -and $row.Extension -notin $propertyExtensions) {
            if ($row.Clsid -and $row.Clsid.ToLowerInvariant() -eq $apiumPropertyClsid.ToLowerInvariant()) {
                $warnings += "$($row.Extension) unexpectedly has the Apium PropertyHandler."
            }
            continue
        }
        if (-not $row.Clsid) {
            $warnings += "No effective $($row.HandlerType) found for $($row.Extension)."
        } elseif ($row.Clsid.ToLowerInvariant() -ne $expected.ToLowerInvariant()) {
            $warnings += "$($row.Extension) uses non-Apium $($row.HandlerType): $($row.Clsid) (expected $expected)."
        }
    }
}

if ($warnings.Count -gt 0) {
    Write-Host ""
    Write-Host "Warnings:"
    $warnings | ForEach-Object { Write-Host " - $_" }
}

Write-Host ""
Write-Host "PreviewHandlers registry list (HKCU):"
$previewHandlersKey = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers"
if (Test-Path $previewHandlersKey) {
    $item = Get-ItemProperty -Path $previewHandlersKey
    $props = $item.PSObject.Properties |
        Where-Object { $_.Name -match '^\{[0-9A-Fa-f-]+\}$' } |
        Select-Object Name, Value
    if ($props) {
        $props | Sort-Object Name | Format-Table -AutoSize
    } else {
        Write-Host "  (no PreviewHandler CLSIDs registered in HKCU list)"
    }
} else {
    Write-Host "  (key not found)"
}
