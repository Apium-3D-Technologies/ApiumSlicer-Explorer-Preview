# By default all registry access is mocked. -Registry also tests isolated real keys.
param([switch]$Registry)
$ErrorActionPreference = "Stop"

function Import-ScriptFunction {
    param([string]$Path, [string]$Name)
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $Name
    }, $true)
    if (-not $definition) { throw "Missing function: $Name" }
    return [scriptblock]::Create($definition.Extent.Text)
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$registerScript = Join-Path $PSScriptRoot "register.ps1"
. (Import-ScriptFunction -Path $registerScript -Name "Copy-DllForRegistration")
. (Import-ScriptFunction -Path (Join-Path $PSScriptRoot "unregister.ps1") -Name "Resolve-DllPath")
function Test-IsAdministrator { return $script:adminMode }
function Get-DefaultValue { param([string]$Path) return $script:registryValues[$Path] }

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$testRoot = Join-Path $repoRoot ("build-registration-tests/" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$originalLocalAppData = $env:LOCALAPPDATA
$originalProgramFiles = $env:ProgramFiles
$lockedCopy = $null
try {
    $env:LOCALAPPDATA = Join-Path $testRoot "User data's"
    $env:ProgramFiles = Join-Path $testRoot "Program Files"
    $sourceDll = Join-Path $testRoot "Build's output.dll"
    [IO.File]::WriteAllText($sourceDll, "first build")
    $script:adminMode = $false
    $firstCopy = Copy-DllForRegistration -SourcePath $sourceDll
    Assert-True ($firstCopy.StartsWith($env:LOCALAPPDATA + [IO.Path]::DirectorySeparatorChar)) "User copy must be under LocalAppData."
    Assert-True ($firstCopy -ne $sourceDll) "The source DLL must never be registered."

    # Keep the old installation locked while rebuilding and staging a replacement.
    $lockedCopy = [IO.File]::Open($firstCopy, 'Open', 'Read', 'Read')
    [IO.File]::WriteAllText($sourceDll, "second build")
    $secondCopy = Copy-DllForRegistration -SourcePath $sourceDll
    Assert-True ($secondCopy -ne $firstCopy) "Re-registration must use a fresh directory."
    Assert-True ([IO.File]::ReadAllText($firstCopy) -eq "first build") "The loaded copy must stay intact."
    Assert-True ([IO.File]::ReadAllText($secondCopy) -eq "second build") "The new copy must contain the latest build."
    [IO.File]::WriteAllText($sourceDll, "third build")
    Assert-True ([IO.File]::ReadAllText($secondCopy) -eq "second build") "Further builds must not change installed copies."

    $script:adminMode = $true
    $machineCopy = Copy-DllForRegistration -SourcePath $sourceDll
    Assert-True ($machineCopy.StartsWith($env:ProgramFiles + [IO.Path]::DirectorySeparatorChar)) "Machine registration must use Program Files."

    $apiumPreviewClsid = "{B450D582-1E34-49E0-8AE7-AFF2D2CD2535}"
    $userKey = "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$apiumPreviewClsid\InprocServer32"
    $machineKey = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\$apiumPreviewClsid\InprocServer32"
    $script:registryValues = @{ $userKey = $secondCopy; $machineKey = $machineCopy }
    Remove-Item -LiteralPath $sourceDll
    $MachineOnly = $false
    Assert-True ((Resolve-DllPath -InputPath "") -eq $secondCopy) "Unregister must find the installed user copy without a build."
    $MachineOnly = $true
    Assert-True ((Resolve-DllPath -InputPath "") -eq $machineCopy) "MachineOnly must find the machine copy."
    Assert-True ((Resolve-DllPath -InputPath $firstCopy) -eq $firstCopy) "Explicit unregister paths must still work."

    & {
        . (Import-ScriptFunction -Path $registerScript -Name "Resolve-AppOpenCommand")
        $appKey = 'Registry::HKEY_CLASSES_ROOT\Apium.Slicer.1\shell\open\command'
        $projectKey = 'Registry::HKEY_CLASSES_ROOT\Apium.Slicer.Project.1\shell\open\command'
        $appCommand = '"C:\Apium Slicer\apium-slicer.exe" "%1"'
        $projectCommand = '"C:\Apium\project-viewer.exe" "%1"'
        $script:registryValues = @{ $appKey = $appCommand; $projectKey = $projectCommand }
        Assert-True ((Resolve-AppOpenCommand -RequestedAppPath '') -eq $appCommand) 'Re-registration must preserve the configured Slicer command.'
        $script:registryValues[$appKey] = '"C:\Windows\System32\OpenWith.exe" "%1"'
        Assert-True ((Resolve-AppOpenCommand -RequestedAppPath '') -eq $projectCommand) 'A placeholder OpenWith command must not hide a known application.'
    }

    & {
        . (Import-ScriptFunction -Path $registerScript -Name "Set-HandlerBindingsForExtension")
        function Test-PrivateDefaultExtension { param([string]$Extension) return $Extension -in @('.gcode', '.a3d') }
        function New-Item { param([string]$Path, [switch]$Force) }
        function Ensure-KeyExists { param([string]$Path) }
        function Ensure-KeyAndSetDefault { param([string]$Path, [string]$Value) $script:registryValues[$Path] = $Value }
        function Set-OpenWithProgId { param([string]$Path, [string]$ProgId) }
        function Remove-ValueIfExists { param([string]$Path, [string]$Name) }
        function Remove-KeyIfExists { param([string]$Path) }
        $apiumProgId = 'Apium.Slicer.1'
        $previewIid = '{8895B1C6-B41F-4C1C-A562-0D564250836F}'
        $thumbnailIid = '{E357FCCD-A995-4576-B01F-234630154E96}'
        $apiumThumbnailClsid = '{8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}'
        $classesRoot = 'Registry::HKEY_CURRENT_USER\Software\Classes'
        $script:registryValues = @{}
        Set-HandlerBindingsForExtension -Extension '.stl'
        Assert-True ($script:registryValues["$classesRoot\.stl"] -eq $apiumProgId) "Missing base ProgIDs must be registered for Explorer previews."
        $script:registryValues = @{ "$classesRoot\.obj" = 'Existing.ObjViewer' }
        Set-HandlerBindingsForExtension -Extension '.obj'
        Assert-True ($script:registryValues["$classesRoot\.obj"] -eq 'Existing.ObjViewer') "Existing user associations must be preserved."
        $script:registryValues = @{ 'Registry::HKEY_CLASSES_ROOT\.stl' = 'Machine.StlViewer' }
        Set-HandlerBindingsForExtension -Extension '.stl'
        Assert-True (-not $script:registryValues.ContainsKey("$classesRoot\.stl")) "Machine associations must not be hidden by a fallback in HKCU."
    }
    if ($Registry) {
        & {
            foreach ($name in @('Get-DefaultValue', 'Ensure-KeyExists', 'Ensure-KeyAndSetDefault', 'Set-OpenWithProgId', 'Set-HandlerBindingsForExtension', 'Remove-ValueIfExists', 'Remove-KeyIfExists')) {
                . (Import-ScriptFunction -Path $registerScript -Name $name)
            }
            function Test-PrivateDefaultExtension { param([string]$Extension) return $Extension -in @('.gcode', '.a3d') }
            $apiumProgId = 'Apium.Slicer.1'
            $previewIid = '{8895B1C6-B41F-4C1C-A562-0D564250836F}'
            $thumbnailIid = '{E357FCCD-A995-4576-B01F-234630154E96}'
            $apiumThumbnailClsid = '{8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}'
            $registryPrefix = 'Registry::HKEY_CURRENT_USER\Software\ApiumSlicerExplorerPreview\RegistrationTests\'
            $registryRoot = $registryPrefix + [Guid]::NewGuid().ToString('N')
            try {
                Ensure-KeyAndSetDefault -Path "$registryRoot\.stl" -Value 'Existing.StlViewer'
                Ensure-KeyExists -Path "$registryRoot\.stl\OpenWithProgids"
                New-ItemProperty -LiteralPath "$registryRoot\.stl\OpenWithProgids" -Name 'Other.App' -Value '' -PropertyType String | Out-Null
                Ensure-KeyExists -Path "$registryRoot\.stl\shellex\$previewIid"
                New-ItemProperty -LiteralPath "$registryRoot\.stl\shellex\$previewIid" -Name 'KeepValue' -Value 'keep' -PropertyType String | Out-Null
                Set-HandlerBindingsForExtension -Extension '.stl' -ClassesRoot $registryRoot
                Assert-True ((Get-DefaultValue -Path "$registryRoot\.stl") -eq 'Existing.StlViewer') 'Real registry registration must preserve the existing ProgID.'
                Assert-True ((Get-Item -LiteralPath "$registryRoot\.stl\OpenWithProgids").GetValueNames() -contains 'Other.App') 'Other OpenWith registrations must survive.'
                Assert-True ((Get-ItemProperty -LiteralPath "$registryRoot\.stl\shellex\$previewIid").KeepValue -eq 'keep') 'Setting a handler must preserve unrelated values.'
                Set-HandlerBindingsForExtension -Extension '.obj' -ClassesRoot $registryRoot
                Set-HandlerBindingsForExtension -Extension '.obj' -ClassesRoot $registryRoot
                Assert-True ((Get-DefaultValue -Path "$registryRoot\.obj") -eq $apiumProgId) 'A new base ProgID must survive repeated registration.'
            } finally {
                if (-not $registryRoot.StartsWith($registryPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected test registry root.' }
                if (Test-Path -LiteralPath $registryRoot) { Remove-Item -LiteralPath $registryRoot -Recurse -Force }
            }
        }
    }
    Write-Host "Registration regression checks passed."
} finally {
    if ($lockedCopy) { $lockedCopy.Dispose() }
    $env:LOCALAPPDATA = $originalLocalAppData
    $env:ProgramFiles = $originalProgramFiles
    $workspacePrefix = $repoRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    if (-not $resolvedTestRoot.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove test data outside the repository: $resolvedTestRoot"
    }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}
