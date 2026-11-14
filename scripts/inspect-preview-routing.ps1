param(
    [string[]]$Extensions = @(".stl", ".obj"),
    [string]$Path = ""
)

$ErrorActionPreference = "Stop"

$previewIid = "{8895B1C6-B41F-4C1C-A562-0D564250836F}"
$thumbnailIid = "{E357FCCD-A995-4576-B01F-234630154E96}"
$apiumPreviewClsid = "{B450D582-1E34-49E0-8AE7-AFF2D2CD2535}"
$apiumThumbnailClsid = "{8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}"
$apiumProgId = "Apium.Slicer.1"
$assocStrShellExtension = 16

if (-not ([System.Management.Automation.PSTypeName]'AssocNative').Type) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class AssocNative
{
    [DllImport("Shlwapi.dll", CharSet = CharSet.Unicode)]
    public static extern int AssocQueryString(
        int flags,
        int str,
        string pszAssoc,
        string pszExtra,
        StringBuilder pszOut,
        ref uint pcchOut);
}
"@
}

function Normalize-Extension {
    param([string]$Extension)
    $ext = $Extension.Trim()
    if (-not $ext.StartsWith(".")) {
        $ext = "." + $ext
    }
    return $ext.ToLowerInvariant()
}

function Get-DefaultValue {
    param([string]$Path)
    try {
        return (Get-Item -LiteralPath $Path -ErrorAction Stop).GetValue("")
    } catch {
        return $null
    }
}

function Get-Value {
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

function Test-ValueExists {
    param(
        [string]$Path,
        [string]$Name
    )
    try {
        $null = Get-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction Stop
        return $true
    } catch {
        return $false
    }
}

function Query-ShellExtension {
    param(
        [string]$Extension,
        [string]$ShellExtensionIid
    )

    $length = [uint32]0
    [void][AssocNative]::AssocQueryString(0, $assocStrShellExtension, $Extension, $ShellExtensionIid, $null, [ref]$length)
    if ($length -eq 0) {
        return ""
    }

    $buffer = [Text.StringBuilder]::new([int]$length)
    $hr = [AssocNative]::AssocQueryString(0, $assocStrShellExtension, $Extension, $ShellExtensionIid, $buffer, [ref]$length)
    if ($hr -ne 0) {
        return ""
    }
    return $buffer.ToString()
}

function Add-Row {
    param(
        [System.Collections.Generic.List[object]]$Rows,
        [string]$Extension,
        [string]$Area,
        [string]$Path,
        [string]$Name,
        [string]$Value
    )
    if ($null -eq $Value -or $Value -eq "") {
        return
    }
    $Rows.Add([pscustomobject]@{
        Extension = $Extension
        Area      = $Area
        Name      = $Name
        Value     = $Value
        Path      = $Path
    }) | Out-Null
}

function Add-HandlerRows {
    param(
        [System.Collections.Generic.List[object]]$Rows,
        [string]$Extension,
        [string]$Area,
        [string]$BasePath
    )
    Add-Row -Rows $Rows -Extension $Extension -Area "$Area PreviewHandler" -Path "$BasePath\shellex\$previewIid" -Name "(default)" -Value (Get-DefaultValue "$BasePath\shellex\$previewIid")
    Add-Row -Rows $Rows -Extension $Extension -Area "$Area ThumbnailProvider" -Path "$BasePath\shellex\$thumbnailIid" -Name "(default)" -Value (Get-DefaultValue "$BasePath\shellex\$thumbnailIid")
    Add-Row -Rows $Rows -Extension $Extension -Area "$Area PersistentHandler" -Path "$BasePath\PersistentHandler" -Name "(default)" -Value (Get-DefaultValue "$BasePath\PersistentHandler")
}

Write-Host ("Identity: {0}" -f ([Security.Principal.WindowsIdentity]::GetCurrent().Name))
Write-Host ""

$rows = [System.Collections.Generic.List[object]]::new()
$normalizedExtensions = @($Extensions | ForEach-Object { Normalize-Extension $_ })
if ($Path) {
    $resolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $pathExt = [IO.Path]::GetExtension($resolvedPath)
    if ($pathExt) {
        $normalizedExtensions += (Normalize-Extension $pathExt)
    }
}
$normalizedExtensions = @($normalizedExtensions | Sort-Object -Unique)

foreach ($ext in $normalizedExtensions) {
    $fileExtsRoot = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext"
    $userChoicePath = "$fileExtsRoot\UserChoice"
    $userChoice = Get-Value -Path $userChoicePath -Name "ProgId"
    Add-Row -Rows $rows -Extension $ext -Area "UserChoice" -Path $userChoicePath -Name "ProgId" -Value $userChoice
    Add-Row -Rows $rows -Extension $ext -Area "UserChoice" -Path $userChoicePath -Name "HashPresent" -Value ([string](Test-ValueExists -Path $userChoicePath -Name "Hash"))

    $userChoiceLatestPath = "$fileExtsRoot\UserChoiceLatest\ProgId"
    $userChoiceLatest = Get-Value -Path $userChoiceLatestPath -Name "ProgId"
    Add-Row -Rows $rows -Extension $ext -Area "UserChoiceLatest" -Path $userChoiceLatestPath -Name "ProgId" -Value $userChoiceLatest

    Add-Row -Rows $rows -Extension $ext -Area "AssocQueryString" -Path "Shlwapi.dll" -Name "PreviewHandler" -Value (Query-ShellExtension -Extension $ext -ShellExtensionIid $previewIid)
    Add-Row -Rows $rows -Extension $ext -Area "AssocQueryString" -Path "Shlwapi.dll" -Name "ThumbnailProvider" -Value (Query-ShellExtension -Extension $ext -ShellExtensionIid $thumbnailIid)

    $hkcrExt = "Registry::HKEY_CLASSES_ROOT\$ext"
    Add-Row -Rows $rows -Extension $ext -Area "Extension" -Path $hkcrExt -Name "(default)" -Value (Get-DefaultValue $hkcrExt)
    Add-HandlerRows -Rows $rows -Extension $ext -Area "Extension" -BasePath $hkcrExt

    $sfaBase = "Registry::HKEY_CLASSES_ROOT\SystemFileAssociations\$ext"
    Add-HandlerRows -Rows $rows -Extension $ext -Area "SystemFileAssociations" -BasePath $sfaBase

    $progIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    if ($userChoice) {
        $null = $progIds.Add($userChoice)
    }
    if ($userChoiceLatest) {
        $null = $progIds.Add($userChoiceLatest)
    }
    $extDefault = Get-DefaultValue $hkcrExt
    if ($extDefault) {
        $null = $progIds.Add($extDefault)
    }

    $openWithPaths = @(
        "$fileExtsRoot\OpenWithProgids",
        "Registry::HKEY_CLASSES_ROOT\$ext\OpenWithProgids"
    )
    foreach ($path in $openWithPaths) {
        try {
            $props = (Get-ItemProperty -LiteralPath $path -ErrorAction Stop).PSObject.Properties |
                Where-Object { $_.Name -notlike "PS*" -and $_.Name -ne "(default)" }
            foreach ($prop in $props) {
                $null = $progIds.Add($prop.Name)
                Add-Row -Rows $rows -Extension $ext -Area "OpenWithProgids" -Path $path -Name $prop.Name -Value "(listed)"
            }
        } catch {
        }
    }

    foreach ($progId in $progIds) {
        $progBase = "Registry::HKEY_CLASSES_ROOT\$progId"
        Add-Row -Rows $rows -Extension $ext -Area "ProgID" -Path $progBase -Name "(default)" -Value (Get-DefaultValue $progBase)
        Add-HandlerRows -Rows $rows -Extension $ext -Area "ProgID:$progId" -BasePath $progBase
    }
}

$resolveHostsScript = Join-Path $PSScriptRoot "resolve-preview-hosts.ps1"
if (Test-Path -LiteralPath $resolveHostsScript) {
    Write-Host ""
    Write-Host "Live preview host snapshot:"
    & $resolveHostsScript
}

if ($rows.Count -eq 0) {
    Write-Host "No preview routing data found."
} else {
    $rows | Sort-Object Extension, Area, Name, Value -Unique | Format-Table -Wrap -AutoSize
}

Write-Host ""
Write-Host "Registered PreviewHandlers:"
$previewHandlerRoots = @(
    "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers",
    "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PreviewHandlers",
    "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\PreviewHandlers"
)
foreach ($path in $previewHandlerRoots) {
    Write-Host $path
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "  (missing)"
        continue
    }
    $props = (Get-ItemProperty -LiteralPath $path).PSObject.Properties |
        Where-Object { $_.Name -match '^\{[0-9A-Fa-f-]+\}$' } |
        Sort-Object Name
    if (-not $props) {
        Write-Host "  (empty)"
        continue
    }
    foreach ($prop in $props) {
        Write-Host ("  {0} = {1}" -f $prop.Name, $prop.Value)
    }
}

Write-Host ""
Write-Host "PowerToys processes:"
$pt = Get-Process PowerToys* -ErrorAction SilentlyContinue | Select-Object ProcessName, Id, Path
if ($pt) {
    $pt | Format-Table -AutoSize
} else {
    Write-Host "  (not running)"
}

if ($Path) {
    Write-Host ""
    Write-Host "Selected file:"
    Write-Host ("  Path: {0}" -f $resolvedPath)
    if ($env:USERPROFILE -and -not $resolvedPath.StartsWith($env:USERPROFILE, [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-Warning ("  The selected file is outside the current user's profile ({0}). UserChoice/default-app registry data is per-user; run this script as the same Windows user that owns the Explorer session you are testing." -f $env:USERPROFILE)
    }
    try {
        $zone = Get-Content -LiteralPath $resolvedPath -Stream Zone.Identifier -ErrorAction Stop
        Write-Host "  Zone.Identifier:"
        $zone | ForEach-Object { Write-Host ("    {0}" -f $_) }
    } catch {
        Write-Host "  Zone.Identifier: (none)"
    }
}

Write-Host ""
Write-Host "Routing verdicts:"
foreach ($ext in $normalizedExtensions) {
    $fileExtsRoot = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext"
    $userChoice = Get-Value -Path "$fileExtsRoot\UserChoice" -Name "ProgId"
    $userChoiceLatest = Get-Value -Path "$fileExtsRoot\UserChoiceLatest\ProgId" -Name "ProgId"
    $hkcrDefault = Get-DefaultValue "Registry::HKEY_CLASSES_ROOT\$ext"
    $assocPreview = Query-ShellExtension -Extension $ext -ShellExtensionIid $previewIid
    $assocThumb = Query-ShellExtension -Extension $ext -ShellExtensionIid $thumbnailIid

    if ($assocPreview -and $assocPreview.ToLowerInvariant() -eq $apiumPreviewClsid.ToLowerInvariant()) {
        Write-Host (" - {0}: AssocQueryString resolves Apium PreviewHandler." -f $ext)
    } else {
        Write-Warning (" - {0}: AssocQueryString does not resolve Apium PreviewHandler (value: {1})." -f $ext, $(if ($assocPreview) { $assocPreview } else { "(none)" }))
    }

    if ($assocThumb -and $assocThumb.ToLowerInvariant() -eq $apiumThumbnailClsid.ToLowerInvariant()) {
        Write-Host ("   ThumbnailProvider resolves Apium.")
    }

    if ($userChoice -eq $apiumProgId -or ((-not $userChoice) -and $hkcrDefault -eq $apiumProgId)) {
        Write-Host ("   Default app is Apium ({0})." -f $(if ($userChoice) { "UserChoice=$userChoice" } else { "HKCR=$hkcrDefault" }))
    } elseif ($userChoice -like "AppX*" -or $userChoiceLatest -like "AppX*") {
        Write-Warning ("   Default routing still references AppX (UserChoice={0}, UserChoiceLatest={1}). Explorer can keep using Microsoft Preview Creator until Apium is selected in Default Apps." -f $(if ($userChoice) { $userChoice } else { "(none)" }), $(if ($userChoiceLatest) { $userChoiceLatest } else { "(none)" }))
    } else {
        Write-Warning ("   Default app is not Apium (UserChoice={0}, HKCR={1})." -f $(if ($userChoice) { $userChoice } else { "(none)" }), $(if ($hkcrDefault) { $hkcrDefault } else { "(none)" }))
    }
}
