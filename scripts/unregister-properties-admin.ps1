param(
    [string]$SchemaPath = ""
)

$ErrorActionPreference = "Stop"

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "This script must be run as Administrator."
    }
}

function Resolve-SchemaPath {
    param([string]$InputPath)

    if ($InputPath -and $InputPath.Trim().Length -gt 0) {
        return (Resolve-Path $InputPath).Path
    }

    $repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
    $defaultPath = Join-Path $repoRoot "resources\ApiumSlicer.propschema.propdesc"
    if (-not (Test-Path $defaultPath)) {
        throw "Schema file not found: $defaultPath"
    }

    return (Resolve-Path $defaultPath).Path
}

Assert-Admin
$resolvedSchema = Resolve-SchemaPath -InputPath $SchemaPath

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class PropertySchemaNative {
    [DllImport("propsys.dll", CharSet = CharSet.Unicode)]
    public static extern int PSUnregisterPropertySchema(string pszPath);

    [DllImport("propsys.dll")]
    public static extern int PSRefreshPropertySchema();
}
"@

$hr = [PropertySchemaNative]::PSUnregisterPropertySchema($resolvedSchema)
if ($hr -ne 0) {
    throw ("PSUnregisterPropertySchema failed with HRESULT 0x{0:X8}" -f $hr)
}

$refreshHr = [PropertySchemaNative]::PSRefreshPropertySchema()
if ($refreshHr -ne 0) {
    throw ("PSRefreshPropertySchema failed with HRESULT 0x{0:X8}" -f $refreshHr)
}

Write-Host "Unregistered property schema: $resolvedSchema"
