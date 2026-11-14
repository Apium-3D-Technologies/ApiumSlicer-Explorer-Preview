param(
    [string[]]$Extensions = @(".a3d", ".gcode", ".stl", ".obj")
)

$ErrorActionPreference = "Stop"

$previewIid = "{8895B1C6-B41F-4C1C-A562-0D564250836F}"
$thumbnailIid = "{E357FCCD-A995-4576-B01F-234630154E96}"
$assocStrShellExtension = 16

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

function Query-ShellExtension {
    param(
        [string]$Extension,
        [string]$ShellExtensionIid
    )

    $length = [uint32]0
    [void][AssocNative]::AssocQueryString(0, $assocStrShellExtension, $Extension, $ShellExtensionIid, $null, [ref]$length)
    if ($length -eq 0) {
        return [pscustomobject]@{
            Hr    = "0x80004005"
            Clsid = ""
        }
    }

    $buffer = [Text.StringBuilder]::new([int]$length)
    $hr = [AssocNative]::AssocQueryString(0, $assocStrShellExtension, $Extension, $ShellExtensionIid, $buffer, [ref]$length)
    return [pscustomobject]@{
        Hr    = ("0x{0:X8}" -f $hr)
        Clsid = $buffer.ToString()
    }
}

foreach ($extRaw in $Extensions) {
    $ext = $extRaw.Trim()
    if (-not $ext.StartsWith(".")) {
        $ext = ".$ext"
    }

    $preview = Query-ShellExtension -Extension $ext -ShellExtensionIid $previewIid
    $thumbnail = Query-ShellExtension -Extension $ext -ShellExtensionIid $thumbnailIid

    [pscustomobject]@{
        Extension     = $ext.ToLowerInvariant()
        PreviewHr     = $preview.Hr
        PreviewClsid  = $preview.Clsid
        ThumbnailHr   = $thumbnail.Hr
        ThumbnailClsid = $thumbnail.Clsid
    }
}
