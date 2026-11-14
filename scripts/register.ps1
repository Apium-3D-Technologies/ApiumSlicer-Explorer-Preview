param(
    [string]$DllPath = "",
    [string]$AppPath = "",
    [switch]$Silent,
    [switch]$Soft,
    [switch]$MachineOnly,
    [switch]$ForceForeignProgIds,
    [string[]]$Extensions = @(".gcode", ".a3d", ".stl", ".obj"),
    [string[]]$PropertyExtensions = @(".gcode", ".a3d"),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"

if ($RemainingArgs -and $RemainingArgs.Count -gt 0) {
    $allowed = @("--soft", "--machine-only", "--force-foreign-progids")
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
    if ($RemainingArgs -contains "--force-foreign-progids") {
        $ForceForeignProgIds = $true
    }
}

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
$privateDefaultExtensions = @(".gcode", ".a3d")
$legacyRemovedExtensions = @(".3mf")

function Normalize-Extension {
    param([string]$Extension)
    $ext = $Extension.Trim()
    if (-not $ext.StartsWith(".")) {
        $ext = "." + $ext
    }
    return $ext.ToLowerInvariant()
}

function Resolve-DllPath {
    param([string]$InputPath)

    if ($InputPath -and $InputPath.Trim().Length -gt 0) {
        return (Resolve-Path -LiteralPath $InputPath).Path
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

    throw "DLL not found. Pass -DllPath or build first."
}

function Get-VersionInfoFromInc {
    param([string]$RepoRoot)

    $versionInc = Join-Path $RepoRoot "version.inc"
    if (-not (Test-Path -LiteralPath $versionInc)) {
        return $null
    }

    $content = Get-Content -Raw -LiteralPath $versionInc
    $nameMatch = [regex]::Match($content, 'set\(ASXP_NAME\s+"([^"]*)"\)')
    $versionMatch = [regex]::Match($content, 'set\(ASXP_VERSION\s+"([^"]*)"\)')
    $authorMatch = [regex]::Match($content, 'set\(ASXP_AUTHOR\s+"([^"]*)"\)')

    if (-not $nameMatch.Success -or -not $versionMatch.Success) {
        return $null
    }

    $author = ""
    if ($authorMatch.Success) {
        $author = $authorMatch.Groups[1].Value
    }

    return [PSCustomObject]@{
        Name    = $nameMatch.Groups[1].Value
        Version = $versionMatch.Groups[1].Value
        Author  = $author
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

function Ensure-KeyAndSetDefault {
    param(
        [string]$Path,
        [string]$Value
    )
    New-Item -Path $Path -Force | Out-Null
    Set-ItemProperty -Path $Path -Name "(default)" -Value $Value
}

function Set-OpenWithProgId {
    param(
        [string]$Path,
        [string]$ProgId
    )
    New-Item -Path $Path -Force | Out-Null
    $regPath = $Path
    if ($regPath.StartsWith("Registry::")) {
        $regPath = $regPath.Substring("Registry::".Length)
    }
    & reg.exe add $regPath /v $ProgId /t REG_NONE /f | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Could not register OpenWithProgids value '$ProgId' at '$Path'."
    }
}

function Test-PrivateDefaultExtension {
    param([string]$Extension)
    return ((Normalize-Extension -Extension $Extension) -in $privateDefaultExtensions)
}

function Get-RegistryScope {
    param([string]$ClassesRoot)
    if ($ClassesRoot -like "Registry::HKEY_LOCAL_MACHINE*") {
        return "HKLM"
    }
    return "HKCU"
}

function Get-SoftwareRootForClassesRoot {
    param([string]$ClassesRoot)
    if ((Get-RegistryScope -ClassesRoot $ClassesRoot) -eq "HKLM") {
        return "Registry::HKEY_LOCAL_MACHINE\SOFTWARE"
    }
    return "Registry::HKEY_CURRENT_USER\Software"
}

function Resolve-AppOpenCommand {
    param([string]$RequestedAppPath)

    if ($RequestedAppPath -and $RequestedAppPath.Trim().Length -gt 0) {
        $resolvedApp = (Resolve-Path -LiteralPath $RequestedAppPath -ErrorAction Stop).Path
        return ('"{0}" "%1"' -f $resolvedApp)
    }

    $knownCommandPaths = @(
        "Registry::HKEY_CLASSES_ROOT\Apium.Slicer.Project.1\shell\open\command",
        "Registry::HKEY_CLASSES_ROOT\ApiumSlicer.GCodeViewer.1\shell\open\command",
        "Registry::HKEY_CLASSES_ROOT\Applications\apium-gcodeviewer.exe\shell\open\command"
    )
    foreach ($path in $knownCommandPaths) {
        $command = Get-DefaultValue -Path $path
        if ($command) {
            return $command
        }
    }

    $openWith = Join-Path $env:SystemRoot "System32\OpenWith.exe"
    return ('"{0}" "%1"' -f $openWith)
}

function Remove-ValueIfExists {
    param(
        [string]$Path,
        [string]$Name
    )
    try {
        $item = Get-ItemProperty -Path $Path -ErrorAction Stop
        if ($null -ne $item.$Name) {
            Remove-ItemProperty -Path $Path -Name $Name -ErrorAction Stop
        }
    } catch {
    }
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

function Ensure-KeyExists {
    param([string]$Path)

    if (Test-Path -Path $Path) {
        return
    }

    $parent = Split-Path -Path $Path -Parent
    if ($parent -and -not (Test-Path -Path $parent)) {
        Ensure-KeyExists -Path $parent
    }

    New-Item -Path $Path -Force | Out-Null
}

function Remove-StaleApiumBindingsForExtension {
    param(
        [string]$Extension,
        [string]$ClassesRoot = "Registry::HKEY_CURRENT_USER\Software\Classes"
    )

    $ext = Normalize-Extension -Extension $Extension
    $extBase = "$ClassesRoot\$ext"
    Remove-HandlerKeyIfMatches -Path "$extBase\shellex\$previewIid" -ExpectedClsid $apiumPreviewClsid
    Remove-HandlerKeyIfMatches -Path "$extBase\shellex\$thumbnailIid" -ExpectedClsid $apiumThumbnailClsid
    Remove-HandlerKeyIfMatches -Path "$extBase\shellex\PropertyHandler" -ExpectedClsid $apiumPropertyClsid

    $sfaBase = "$ClassesRoot\SystemFileAssociations\$ext"
    Remove-HandlerKeyIfMatches -Path "$sfaBase\shellex\$previewIid" -ExpectedClsid $apiumPreviewClsid
    Remove-HandlerKeyIfMatches -Path "$sfaBase\shellex\$thumbnailIid" -ExpectedClsid $apiumThumbnailClsid
    Remove-HandlerKeyIfMatches -Path "$sfaBase\shellex\PropertyHandler" -ExpectedClsid $apiumPropertyClsid

    foreach ($progId in (Resolve-ProgIds -Extension $ext)) {
        if (-not $progId) {
            continue
        }
        $progBase = "$ClassesRoot\$progId"
        Remove-HandlerKeyIfMatches -Path "$progBase\shellex\$previewIid" -ExpectedClsid $apiumPreviewClsid
        Remove-HandlerKeyIfMatches -Path "$progBase\shellex\$thumbnailIid" -ExpectedClsid $apiumThumbnailClsid
        Remove-HandlerKeyIfMatches -Path "$progBase\shellex\PropertyHandler" -ExpectedClsid $apiumPropertyClsid
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

function Assert-ApiumClassRegistration {
    param(
        [string]$ResolvedDllPath
    )

    $requiredClsids = @(
        $apiumPreviewClsid,
        $apiumThumbnailClsid,
        $apiumPropertyClsid
    )

    foreach ($clsid in $requiredClsids) {
        $inprocKey = "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$clsid\InprocServer32"
        $registeredDll = Get-DefaultValue -Path $inprocKey
        if (-not $registeredDll) {
            throw "Missing COM registration for $clsid in HKCU (InprocServer32 default)."
        }

        $registeredResolved = $registeredDll
        if (Test-Path -LiteralPath $registeredDll) {
            $registeredResolved = (Resolve-Path -LiteralPath $registeredDll).Path
        }
        if ($registeredResolved.ToLowerInvariant() -ne $ResolvedDllPath.ToLowerInvariant()) {
            throw "COM registration for $clsid points to '$registeredDll' instead of '$ResolvedDllPath'."
        }

        $threadingModel = $null
        try {
            $threadingModel = (Get-ItemProperty -Path $inprocKey -ErrorAction Stop).ThreadingModel
        } catch {
            $threadingModel = $null
        }
        if (-not $threadingModel -or $threadingModel.ToLowerInvariant() -ne "apartment") {
            throw "COM registration for $clsid has invalid ThreadingModel '$threadingModel' (expected 'Apartment')."
        }
    }
}

function Set-HandlerBindingsForExtension {
    param(
        [string]$Extension,
        [string]$ClassesRoot = "Registry::HKEY_CURRENT_USER\Software\Classes",
        [bool]$BindForeignProgIds = $false
    )

    $extBase = "$ClassesRoot\$Extension"
    if (Test-PrivateDefaultExtension -Extension $Extension) {
        Ensure-KeyAndSetDefault -Path $extBase -Value $apiumProgId
    } else {
        New-Item -Path $extBase -Force | Out-Null
    }
    Set-OpenWithProgId -Path "$extBase\OpenWithProgids" -ProgId $apiumProgId

    Ensure-KeyAndSetDefault -Path "$extBase\shellex\$previewIid" -Value $apiumPreviewClsid
    Ensure-KeyAndSetDefault -Path "$extBase\shellex\$thumbnailIid" -Value $apiumThumbnailClsid

    $sfaBase = "$ClassesRoot\SystemFileAssociations\$Extension"
    Ensure-KeyAndSetDefault -Path "$sfaBase\shellex\$previewIid" -Value $apiumPreviewClsid
    Ensure-KeyAndSetDefault -Path "$sfaBase\shellex\$thumbnailIid" -Value $apiumThumbnailClsid

    Remove-ValueIfExists -Path "$extBase\shellex\$previewIid" -Name "PrevPreviewHandler"
    Remove-ValueIfExists -Path "$sfaBase\shellex\$previewIid" -Name "PrevPreviewHandler"
    Remove-KeyIfExists -Path "$extBase\shellex\PropertyHandler"
    Remove-KeyIfExists -Path "$sfaBase\shellex\PropertyHandler"

    $apiumProgBase = "$ClassesRoot\$apiumProgId"
    Ensure-KeyAndSetDefault -Path "$apiumProgBase\shellex\$previewIid" -Value $apiumPreviewClsid
    Ensure-KeyAndSetDefault -Path "$apiumProgBase\shellex\$thumbnailIid" -Value $apiumThumbnailClsid
    Remove-KeyIfExists -Path "$apiumProgBase\shellex\PropertyHandler"

    if (-not $BindForeignProgIds) {
        return
    }

    $progIds = Resolve-ProgIds -Extension $Extension
    foreach ($progId in $progIds) {
        if (-not $progId) {
            continue
        }
        if ($progId -eq $apiumProgId) {
            continue
        }
        $progBase = "$ClassesRoot\$progId"
        Ensure-KeyAndSetDefault -Path "$progBase\shellex\$previewIid" -Value $apiumPreviewClsid
        Ensure-KeyAndSetDefault -Path "$progBase\shellex\$thumbnailIid" -Value $apiumThumbnailClsid
        Remove-ValueIfExists -Path "$progBase\shellex\$previewIid" -Name "PrevPreviewHandler"
        Remove-KeyIfExists -Path "$progBase\shellex\PropertyHandler"
    }
}

function Force-BindApiumHandlers {
    param(
        [string[]]$TargetExtensions,
        [string]$ClassesRoot = "Registry::HKEY_CURRENT_USER\Software\Classes",
        [string]$PreviewHandlersPath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers",
        [string]$ScopeName = "HKCU",
        [bool]$BindForeignProgIds = $false
    )

    foreach ($extRaw in $TargetExtensions) {
        $ext = Normalize-Extension -Extension $extRaw
        Set-HandlerBindingsForExtension -Extension $ext -ClassesRoot $ClassesRoot -BindForeignProgIds $BindForeignProgIds
    }

    if ($PreviewHandlersPath) {
        New-Item -Path $PreviewHandlersPath -Force | Out-Null
        Set-ItemProperty -Path $PreviewHandlersPath -Name $apiumPreviewClsid -Value "Apium Slicer Preview Handler"
    }

    $foreignNote = if ($BindForeignProgIds) { " including resolved foreign ProgIDs" } else { "" }
    Write-Host "Registered Apium handlers in ${ScopeName}: $($TargetExtensions -join ', ')$foreignNote"
}

function Ensure-PropertySystemBindings {
    param(
        [string[]]$TargetExtensions,
        [string]$PropertyHandlersPath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"
    )

    Ensure-KeyExists -Path $PropertyHandlersPath
    foreach ($extRaw in $TargetExtensions) {
        $ext = Normalize-Extension -Extension $extRaw
        Ensure-KeyAndSetDefault -Path "$PropertyHandlersPath\$ext" -Value $apiumPropertyClsid
    }
}

function Remove-StalePropertySystemBindings {
    param(
        [string[]]$TargetExtensions,
        [string]$PropertyHandlersPath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"
    )

    foreach ($extRaw in $TargetExtensions) {
        $ext = Normalize-Extension -Extension $extRaw
        Remove-HandlerKeyIfMatches -Path "$PropertyHandlersPath\$ext" -ExpectedClsid $apiumPropertyClsid
    }
}

function Ensure-PreviewHandlersEnabled {
    $advancedKey = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced"
    try {
        if (-not (Test-Path -LiteralPath $advancedKey)) {
            New-Item -Path $advancedKey -Force -ErrorAction Stop | Out-Null
        }
        New-ItemProperty -Path $advancedKey -Name "ShowPreviewHandlers" -PropertyType DWord -Value 1 -Force -ErrorAction Stop | Out-Null
        Write-Host "Ensured Explorer preview handlers are enabled (ShowPreviewHandlers=1)."
    } catch {
        Write-Warning ("Could not set HKCU Explorer ShowPreviewHandlers (non-fatal): {0}" -f $_.Exception.Message)
    }
}

function Ensure-ApiumApplicationRegistration {
    param(
        [string]$ClassesRoot = "Registry::HKEY_CURRENT_USER\Software\Classes",
        [string]$ResolvedDllPath,
        [string]$OpenCommand,
        [string[]]$TargetExtensions
    )

    $softwareRoot = Get-SoftwareRootForClassesRoot -ClassesRoot $ClassesRoot
    $progBase = "$ClassesRoot\$apiumProgId"

    New-Item -Path $progBase -Force | Out-Null
    Set-ItemProperty -Path $progBase -Name "(default)" -Value "ApiumSlicer"
    Set-ItemProperty -Path $progBase -Name "FriendlyTypeName" -Value "Apium Slicer File"

    Ensure-KeyAndSetDefault -Path "$progBase\DefaultIcon" -Value ('"{0}",0' -f $ResolvedDllPath)
    Ensure-KeyAndSetDefault -Path "$progBase\shell\open\command" -Value $OpenCommand
    Ensure-KeyAndSetDefault -Path "$progBase\shellex\$previewIid" -Value $apiumPreviewClsid
    Ensure-KeyAndSetDefault -Path "$progBase\shellex\$thumbnailIid" -Value $apiumThumbnailClsid
    Remove-KeyIfExists -Path "$progBase\shellex\PropertyHandler"

    $capabilitiesPath = "$softwareRoot\ApiumSlicerExplorerPreview\Capabilities"
    New-Item -Path $capabilitiesPath -Force | Out-Null
    Set-ItemProperty -Path $capabilitiesPath -Name "ApplicationName" -Value $apiumApplicationName
    Set-ItemProperty -Path $capabilitiesPath -Name "ApplicationDescription" -Value "Apium Slicer file previews and model files"

    $fileAssociationsPath = "$capabilitiesPath\FileAssociations"
    New-Item -Path $fileAssociationsPath -Force | Out-Null
    foreach ($extRaw in $TargetExtensions) {
        $ext = Normalize-Extension -Extension $extRaw
        Set-ItemProperty -Path $fileAssociationsPath -Name $ext -Value $apiumProgId
    }

    $registeredApplicationsPath = "$softwareRoot\RegisteredApplications"
    New-Item -Path $registeredApplicationsPath -Force | Out-Null
    Set-ItemProperty -Path $registeredApplicationsPath -Name $apiumApplicationName -Value $apiumCapabilitiesSubPath
}

function Test-IsAdministrator {
    $principal = [Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
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

function Get-EffectiveDefaultProgId {
    param([string]$Extension)

    $userChoicePath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\UserChoice"
    try {
        $value = (Get-ItemProperty -Path $userChoicePath -ErrorAction Stop).ProgId
        if ($value) {
            return [pscustomobject]@{ Source = "UserChoice"; ProgId = $value }
        }
    } catch {
    }

    $default = Get-DefaultValue -Path "Registry::HKEY_CLASSES_ROOT\$Extension"
    if ($default) {
        return [pscustomobject]@{ Source = "HKCR"; ProgId = $default }
    }

    $latestPath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\UserChoiceLatest\ProgId"
    try {
        $value = (Get-ItemProperty -Path $latestPath -ErrorAction Stop).ProgId
        if ($value) {
            return [pscustomobject]@{ Source = "UserChoiceLatest"; ProgId = $value }
        }
    } catch {
    }

    return [pscustomobject]@{ Source = "none"; ProgId = "" }
}

function Test-RegistryValueExists {
    param(
        [string]$Path,
        [string]$Name
    )
    try {
        $null = Get-ItemProperty -Path $Path -Name $Name -ErrorAction Stop
        return $true
    } catch {
        return $false
    }
}

function Write-DefaultAppStatus {
    param([string[]]$TargetExtensions)

    Write-Host ("Default app status for current user: {0}" -f ([Security.Principal.WindowsIdentity]::GetCurrent().Name))
    foreach ($extRaw in $TargetExtensions) {
        $ext = Normalize-Extension -Extension $extRaw
        $userChoicePath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext\UserChoice"
        $latestPath = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext\UserChoiceLatest\ProgId"
        $userChoice = $null
        $userChoiceLatest = $null
        try { $userChoice = (Get-ItemProperty -Path $userChoicePath -ErrorAction Stop).ProgId } catch {}
        try { $userChoiceLatest = (Get-ItemProperty -Path $latestPath -ErrorAction Stop).ProgId } catch {}
        $hasHash = Test-RegistryValueExists -Path $userChoicePath -Name "Hash"
        $default = Get-EffectiveDefaultProgId -Extension $ext
        $isPrivateDefault = Test-PrivateDefaultExtension -Extension $ext
        if ($default.ProgId -eq $apiumProgId -and $isPrivateDefault -and -not ($userChoiceLatest -like "AppX*")) {
            Write-Host "Default app for $ext is Apium ($($default.Source)=$($default.ProgId))."
        } elseif ($userChoice -eq $apiumProgId -and $hasHash -and -not ($userChoiceLatest -like "AppX*")) {
            Write-Host "Default app for $ext is Apium ($($default.Source)=$($default.ProgId))."
        } elseif ($userChoice -eq $apiumProgId -and $userChoiceLatest -like "AppX*") {
            Write-Warning "Default app for $ext is mixed (UserChoice=$userChoice, UserChoiceLatest=$userChoiceLatest). If Explorer still uses Microsoft Preview Creator, reselect Apium Slicer in Windows Settings."
        } elseif ($default.ProgId -like "AppX*") {
            Write-Warning "Default app for $ext is still AppX ($($default.Source)=$($default.ProgId)). Set Apium Slicer as default in Windows Settings for Preview Pane routing."
        } elseif ($userChoiceLatest -like "AppX*") {
            Write-Warning "Default routing for $ext still references AppX (UserChoiceLatest=$userChoiceLatest). Set Apium Slicer as default in Windows Settings for Preview Pane routing."
        } elseif ($default.ProgId) {
            Write-Warning "Default app for $ext is still $($default.ProgId) ($($default.Source)). Set Apium Slicer as default if Explorer keeps using another previewer."
        } else {
            Write-Warning "Default app for $ext is unresolved. Set Apium Slicer as default in Windows Settings."
        }
    }
}

function Set-ApiumComClassValues {
    param(
        [string]$ClsidKey,
        [string]$InprocKey,
        [string]$Name,
        [string]$ResolvedDllPath,
        [bool]$SetAppId
    )

    New-Item -Path $ClsidKey -Force | Out-Null
    Set-ItemProperty -Path $ClsidKey -Name "(default)" -Value $Name
    Set-ItemProperty -Path $ClsidKey -Name "DisplayName" -Value $Name
    New-ItemProperty -Path $ClsidKey -Name "AutomaticallyPreviewUntrustedFiles" -PropertyType DWord -Value 1 -Force | Out-Null
    Remove-ValueIfExists -Path $ClsidKey -Name "DisableLowILProcessIsolation"
    Remove-ValueIfExists -Path $ClsidKey -Name "DisableProcessIsolation"
    if ($SetAppId) {
        Set-ItemProperty -Path $ClsidKey -Name "AppID" -Value $previewHostAppId
    }

    New-Item -Path $InprocKey -Force | Out-Null
    Set-ItemProperty -Path $InprocKey -Name "(default)" -Value $ResolvedDllPath
    Set-ItemProperty -Path $InprocKey -Name "ThreadingModel" -Value "Apartment"
    Set-ItemProperty -Path $InprocKey -Name "ProgID" -Value $apiumProgId
    Set-ItemProperty -Path $InprocKey -Name "VersionIndependentProgID" -Value "Apium.Slicer"
}

function Ensure-SystemClassAndPreviewRegistration {
    param(
        [string]$ResolvedDllPath,
        [string]$OpenCommand,
        [string[]]$TargetExtensions,
        [string[]]$TargetPropertyExtensions,
        [bool]$BindForeignProgIds = $false
    )

    if (-not (Test-IsAdministrator)) {
        Write-Warning "Skipping HKLM class/PreviewHandlers registration (Run PowerShell as Administrator to enable prevhost compatibility)."
        return
    }

    Remove-KeyIfExists -Path "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\AppID\$legacyPreviewHostAppId"
    Ensure-ApiumApplicationRegistration `
        -ClassesRoot "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes" `
        -ResolvedDllPath $ResolvedDllPath `
        -OpenCommand $OpenCommand `
        -TargetExtensions $TargetExtensions

    $entries = @(
        @{ Clsid = $apiumPreviewClsid; Name = "Apium Slicer Preview Handler"; SetAppId = $true },
        @{ Clsid = $apiumThumbnailClsid; Name = "Apium Slicer Thumbnail Provider"; SetAppId = $false },
        @{ Clsid = $apiumPropertyClsid; Name = "Apium Slicer Property Handler"; SetAppId = $false }
    )

    foreach ($entry in $entries) {
        $clsidKey = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\$($entry.Clsid)"
        $inprocKey = "$clsidKey\InprocServer32"

        Set-ApiumComClassValues `
            -ClsidKey $clsidKey `
            -InprocKey $inprocKey `
            -Name $entry.Name `
            -ResolvedDllPath $ResolvedDllPath `
            -SetAppId $entry.SetAppId
    }

    $previewHandlers64 = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PreviewHandlers"
    New-Item -Path $previewHandlers64 -Force | Out-Null
    Set-ItemProperty -Path $previewHandlers64 -Name $apiumPreviewClsid -Value "Apium Slicer Preview Handler"

    $previewHandlers32 = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\PreviewHandlers"
    try {
        New-Item -Path $previewHandlers32 -Force | Out-Null
        Set-ItemProperty -Path $previewHandlers32 -Name $apiumPreviewClsid -Value "Apium Slicer Preview Handler"
    } catch {
    }

    Force-BindApiumHandlers `
        -TargetExtensions $TargetExtensions `
        -ClassesRoot "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes" `
        -PreviewHandlersPath $previewHandlers64 `
        -ScopeName "HKLM" `
        -BindForeignProgIds $BindForeignProgIds
    Ensure-PropertySystemBindings `
        -TargetExtensions $TargetPropertyExtensions `
        -PropertyHandlersPath "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"

    Write-Host "Ensured HKLM COM, PreviewHandlers, Default Apps registration, and file-type bindings for Apium CLSIDs."
}

$resolvedDll = Resolve-DllPath -InputPath $DllPath
$openCommand = Resolve-AppOpenCommand -RequestedAppPath $AppPath

if ($MachineOnly) {
    if (-not (Test-IsAdministrator)) {
        throw "MachineOnly registration requires an elevated PowerShell."
    }

    if (-not $Soft) {
        foreach ($ext in $legacyRemovedExtensions) {
            Remove-StaleApiumBindingsForExtension -Extension $ext -ClassesRoot "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes"
        }
        Remove-StalePropertySystemBindings `
            -TargetExtensions $legacyRemovedExtensions `
            -PropertyHandlersPath "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"
    }

    Ensure-SystemClassAndPreviewRegistration `
        -ResolvedDllPath $resolvedDll `
        -OpenCommand $openCommand `
        -TargetExtensions $Extensions `
        -TargetPropertyExtensions $PropertyExtensions `
        -BindForeignProgIds $ForceForeignProgIds
    Notify-AssociationChanged
    Write-Host "Registered HKLM only: $resolvedDll"
    Write-DefaultAppStatus -TargetExtensions $Extensions
    exit 0
}

$args = @("/s")
$args += ('"{0}"' -f $resolvedDll)

$process = Start-Process -FilePath "regsvr32.exe" -ArgumentList $args -PassThru -Wait
if ($process.ExitCode -ne 0) {
    throw "regsvr32 failed with exit code $($process.ExitCode)."
}

Assert-ApiumClassRegistration -ResolvedDllPath $resolvedDll
Write-Host "Registered: $resolvedDll"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$versionInfo = Get-VersionInfoFromInc -RepoRoot $repoRoot
if ($versionInfo) {
    $authorSegment = if ([string]::IsNullOrWhiteSpace($versionInfo.Author)) { "unknown" } else { $versionInfo.Author }
    Write-Host ("Detected version: [{0} - v{1} ~{2}]" -f $versionInfo.Name, $versionInfo.Version, $authorSegment)
} else {
    Write-Host "Detected version: unknown (version.inc not readable)"
}

if (-not $Soft) {
    foreach ($ext in $legacyRemovedExtensions) {
        Remove-StaleApiumBindingsForExtension -Extension $ext -ClassesRoot "Registry::HKEY_CURRENT_USER\Software\Classes"
    }
    Remove-StalePropertySystemBindings -TargetExtensions $legacyRemovedExtensions
    if (Test-IsAdministrator) {
        foreach ($ext in $legacyRemovedExtensions) {
            Remove-StaleApiumBindingsForExtension -Extension $ext -ClassesRoot "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes"
        }
        Remove-StalePropertySystemBindings `
            -TargetExtensions $legacyRemovedExtensions `
            -PropertyHandlersPath "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"
    }
    Ensure-SystemClassAndPreviewRegistration `
        -ResolvedDllPath $resolvedDll `
        -OpenCommand $openCommand `
        -TargetExtensions $Extensions `
        -TargetPropertyExtensions $PropertyExtensions `
        -BindForeignProgIds $ForceForeignProgIds
    Remove-KeyIfExists -Path "Registry::HKEY_CURRENT_USER\Software\Classes\AppID\$legacyPreviewHostAppId"
    Ensure-ApiumApplicationRegistration `
        -ClassesRoot "Registry::HKEY_CURRENT_USER\Software\Classes" `
        -ResolvedDllPath $resolvedDll `
        -OpenCommand $openCommand `
        -TargetExtensions $Extensions
    Force-BindApiumHandlers -TargetExtensions $Extensions -BindForeignProgIds $ForceForeignProgIds
    Ensure-PropertySystemBindings -TargetExtensions $PropertyExtensions
    Ensure-PreviewHandlersEnabled
    Notify-AssociationChanged
    Write-DefaultAppStatus -TargetExtensions $Extensions
} else {
    Write-Host "Soft mode enabled: skipped force-binding of file-type handlers."
    Notify-AssociationChanged
    Write-DefaultAppStatus -TargetExtensions $Extensions
}
