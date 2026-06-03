param(
    [string]$DllPath = "",
    [switch]$Silent,
    [switch]$Soft,
    [string[]]$Extensions = @(".gcode", ".a3d"),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"

if ($RemainingArgs -and $RemainingArgs.Count -gt 0) {
    $allowed = @("--soft")
    $unknown = @($RemainingArgs | Where-Object { $_ -notin $allowed })
    if ($unknown.Count -gt 0) {
        throw "Unknown argument(s): $($unknown -join ', ')"
    }
    if ($RemainingArgs -contains "--soft") {
        $Soft = $true
    }
}

$previewIid = "{8895B1C6-B41F-4C1C-A562-0D564250836F}"
$thumbnailIid = "{E357FCCD-A995-4576-B01F-234630154E96}"

$apiumPreviewClsid = "{B450D582-1E34-49E0-8AE7-AFF2D2CD2535}"
$apiumThumbnailClsid = "{8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}"
$apiumPropertyClsid = "{12D795E2-813D-4E87-93BB-DC5E6239BB42}"
$previewHostAppId = "{3B0FE91D-45E1-41E4-A6A7-6F8045AEF35A}"
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
        [string]$ClassesRoot = "Registry::HKEY_CURRENT_USER\Software\Classes"
    )

    $extBase = "$ClassesRoot\$Extension"
    Ensure-KeyAndSetDefault -Path "$extBase\shellex\$previewIid" -Value $apiumPreviewClsid
    Ensure-KeyAndSetDefault -Path "$extBase\shellex\$thumbnailIid" -Value $apiumThumbnailClsid

    $sfaBase = "$ClassesRoot\SystemFileAssociations\$Extension"
    Ensure-KeyAndSetDefault -Path "$sfaBase\shellex\$previewIid" -Value $apiumPreviewClsid
    Ensure-KeyAndSetDefault -Path "$sfaBase\shellex\$thumbnailIid" -Value $apiumThumbnailClsid

    Remove-ValueIfExists -Path "$extBase\shellex\$previewIid" -Name "PrevPreviewHandler"
    Remove-ValueIfExists -Path "$sfaBase\shellex\$previewIid" -Name "PrevPreviewHandler"
    Remove-KeyIfExists -Path "$extBase\shellex\PropertyHandler"
    Remove-KeyIfExists -Path "$sfaBase\shellex\PropertyHandler"

    $progIds = Resolve-ProgIds -Extension $Extension
    foreach ($progId in $progIds) {
        if (-not $progId) {
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
        [string]$ScopeName = "HKCU"
    )

    foreach ($extRaw in $TargetExtensions) {
        $ext = Normalize-Extension -Extension $extRaw
        Set-HandlerBindingsForExtension -Extension $ext -ClassesRoot $ClassesRoot
    }

    if ($PreviewHandlersPath) {
        New-Item -Path $PreviewHandlersPath -Force | Out-Null
        Set-ItemProperty -Path $PreviewHandlersPath -Name $apiumPreviewClsid -Value "Apium Slicer Preview Handler"
    }

    Write-Host "Force-bound Apium handlers in ${ScopeName}: $($TargetExtensions -join ', ')"
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

function Ensure-ApiumPreviewAppId {
    param(
        [string]$ClassesRoot = "Registry::HKEY_CURRENT_USER\Software\Classes"
    )

    $appIdKey = "$ClassesRoot\AppID\$previewHostAppId"
    New-Item -Path $appIdKey -Force | Out-Null
    Set-ItemProperty -Path $appIdKey -Name "(default)" -Value "Apium Slicer Preview Host"
    Set-ItemProperty -Path $appIdKey -Name "DllSurrogate" -Value "Prevhost.exe"
}

function Test-IsAdministrator {
    $principal = [Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
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
    New-ItemProperty -Path $ClsidKey -Name "DisableLowILProcessIsolation" -PropertyType DWord -Value 1 -Force | Out-Null
    New-ItemProperty -Path $ClsidKey -Name "DisableProcessIsolation" -PropertyType DWord -Value 1 -Force | Out-Null
    if ($SetAppId) {
        Set-ItemProperty -Path $ClsidKey -Name "AppID" -Value $previewHostAppId
    }

    New-Item -Path $InprocKey -Force | Out-Null
    Set-ItemProperty -Path $InprocKey -Name "(default)" -Value $ResolvedDllPath
    Set-ItemProperty -Path $InprocKey -Name "ThreadingModel" -Value "Apartment"
    Set-ItemProperty -Path $InprocKey -Name "ProgID" -Value "Apium.Slicer.1"
    Set-ItemProperty -Path $InprocKey -Name "VersionIndependentProgID" -Value "Apium.Slicer"
}

function Ensure-SystemClassAndPreviewRegistration {
    param(
        [string]$ResolvedDllPath,
        [string[]]$TargetExtensions
    )

    if (-not (Test-IsAdministrator)) {
        Write-Warning "Skipping HKLM class/PreviewHandlers registration (Run PowerShell as Administrator to enable prevhost compatibility)."
        return
    }

    Ensure-ApiumPreviewAppId -ClassesRoot "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes"

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
        -ScopeName "HKLM"
    Ensure-PropertySystemBindings `
        -TargetExtensions $TargetExtensions `
        -PropertyHandlersPath "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers"

    Write-Host "Ensured HKLM COM, PreviewHandlers, and file-type bindings for Apium CLSIDs."
}

$resolvedDll = Resolve-DllPath -InputPath $DllPath
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
    Ensure-SystemClassAndPreviewRegistration -ResolvedDllPath $resolvedDll -TargetExtensions $Extensions
    Ensure-ApiumPreviewAppId
    Force-BindApiumHandlers -TargetExtensions $Extensions
    Ensure-PropertySystemBindings -TargetExtensions $Extensions
    Ensure-PreviewHandlersEnabled
} else {
    Write-Host "Soft mode enabled: skipped force-binding of file-type handlers."
}
