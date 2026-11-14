![ApiumLogo](/doc/resource/apium_logo.png?raw=true)

# ApiumSlicerExplorerPreview
ApiumSlicerExplorerPreview is a Windows shell extension for `.gcode`, `.a3d`, `.stl`, and `.obj` files with:
- Explorer thumbnails (with fallback image)
- Preview Handler support (Explorer Preview Pane + PowerToys Peek host)
- Apium-specific metadata properties exposed to Explorer for `.gcode` and `.a3d`


## Prerequisites
**Note: The Windows 10/11 SDK should be installed with Visual Studio.**
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/)
- [CMake (>= 3.20)](https://cmake.org/download/)
- [Ninja](https://github.com/ninja-build/ninja/releases) (only required for Ninja builds)

Build toolchain requirement:
- MSVC (`cl.exe`) is required. GCC/MinGW builds are not supported by this project.


## Build (Visual Studio Generator)
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

_**Output:** `build/Release/ApiumSlicerExplorerPreview.dll`_


## Build (Ninja Generator)
Run this from an environment with the MSVC toolchain (for example, "x64 Native Tools Command Prompt for VS 2022"):

```powershell
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ninja
```

_**Output:** `build-ninja/ApiumSlicerExplorerPreview.dll`_


## GitHub releases

Pushing a tag runs [`.github/workflows/release.yml`](.github/workflows/release.yml).
The workflow builds with Visual Studio 2022 for **Windows x64 / Release**, runs
CTest, packages the output, and publishes a GitHub release with generated release
notes. It uses the automatic `GITHUB_TOKEN` with `contents: write`; no additional
release secret is required.

Every tag triggers the workflow. Stable version tags such as `v1.2.3` or `1.2.3`
produce regular releases. Tags such as `v1.2.3-rc.1` and other non-version tags
produce prereleases and do not replace the latest stable release. For version
tags, the DLL's displayed version comes from the tag (without the optional `v`).
Local builds and non-version tags use `ASXP_VERSION` from `version.inc`.

Commit and push the source and workflow changes before creating a tag on that
commit, for example:

```powershell
git tag -a v1.0.0 -m "Release v1.0.0"
git push origin v1.0.0
```

The following assets have fixed names across releases:

| Asset | Contents |
| --- | --- |
| `ApiumSlicerExplorerPreview.dll` | Ready-to-use Windows x64 DLL, built with the static MSVC runtime |
| `ApiumSlicerExplorerPreview-windows-x64.zip` | DLL, `ApiumSlicer.propschema.propdesc`, `LICENSE`, and `NOTICES.md`, all at the ZIP root |
| `SHA256SUMS.txt` | SHA-256 checksums for the DLL and ZIP |

The release stays a draft until all assets have uploaded. If uploading or
publishing fails, re-run the failed job to complete that draft. Published
releases are not overwritten; create a new tag for a new version.

To create the same assets locally after a Visual Studio build:

```powershell
./scripts/package-release.ps1
# Or use a separate Visual Studio build directory:
./scripts/package-release.ps1 -BuildDir build-release-ci -OutputDir dist
```

### Consume assets in the Slicer build

Pin a release tag in the Slicer's CI/CD workflow so builds always use the same
preview binary. For a public repository, a Windows GitHub Actions step can
download and verify the package as follows:

```yaml
- name: Download Explorer Preview
  shell: pwsh
  env:
    GH_TOKEN: ${{ github.token }}
    PREVIEW_RELEASE_TAG: v1.0.0
  run: |
    $repo = 'Apium-3D-Technologies/ApiumSlicer-Explorer-Preview'
    $dir = 'deps/explorer-preview'
    $archive = 'ApiumSlicerExplorerPreview-windows-x64.zip'
    gh release download $env:PREVIEW_RELEASE_TAG --repo $repo --dir $dir --pattern $archive --pattern SHA256SUMS.txt
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $line = Get-Content "$dir/SHA256SUMS.txt" | Where-Object { $_.EndsWith("  $archive") }
    $expected = ($line -split '\s+')[0]
    $actual = (Get-FileHash "$dir/$archive" -Algorithm SHA256).Hash
    if ($expected -notmatch '^[0-9a-f]{64}$' -or $actual -ne $expected) {
      throw 'Explorer Preview package checksum mismatch.'
    }
    Expand-Archive -LiteralPath "$dir/$archive" -DestinationPath "$dir/package" -Force
```

The Slicer installer can then include
`deps/explorer-preview/package/ApiumSlicerExplorerPreview.dll` and the accompanying
schema and license files. Shell extension registration remains part of the
Slicer's installer; downloading the package does not register it.

If this repository is private, replace `GH_TOKEN` in the **Slicer repository's**
download step with a secret containing a fine-grained PAT or GitHub App token
with **Contents: read** access to this repository. A workflow's automatic
`GITHUB_TOKEN` only has access to its own private repository. GitHub CLI also
handles authenticated downloads of private release assets.

For public releases, fixed download URLs are also available:

```text
# Pinned release (recommended for CI/CD):
https://github.com/Apium-3D-Technologies/ApiumSlicer-Explorer-Preview/releases/download/v1.0.0/ApiumSlicerExplorerPreview-windows-x64.zip

# Latest stable release:
https://github.com/Apium-3D-Technologies/ApiumSlicer-Explorer-Preview/releases/latest/download/ApiumSlicerExplorerPreview-windows-x64.zip
```

See the [GitHub CLI release download documentation](https://cli.github.com/manual/gh_release_download)
for additional download options.

## Scripts
- Build: `scripts/build.ps1`
- Package release assets: `scripts/package-release.ps1`
- Register (per-user via `DllRegisterServer`, Apium ProgID, Default Apps capabilities, and shell handlers): `scripts/register.ps1`
- Unregister (default with Explorer cache clear): `scripts/unregister.ps1`
- Shell handler diagnostics: `scripts/list-shell-handlers.ps1`
- Preview routing diagnostics: `scripts/inspect-preview-routing.ps1`
- Register property schema (admin, optional): `scripts/register-properties-admin.ps1`
- Unregister property schema (admin, optional): `scripts/unregister-properties-admin.ps1`

Examples:
- Visual Studio build: `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`
- Ninja build (flag): `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 --ninja`
- Machine register (admin HKLM registration): `powershell -ExecutionPolicy Bypass -File .\scripts\register.ps1 -MachineOnly`
- Soft register (skip file-type binding refresh): `powershell -ExecutionPolicy Bypass -File .\scripts\register.ps1 --soft`
- Soft unregister (skip cache clear): `powershell -ExecutionPolicy Bypass -File .\scripts\unregister.ps1 --soft`

## Public 3D File Types
`.stl` and `.obj` are public Windows/AppX file types. The extension registers
Apium as an Open With / Default Apps candidate, but Windows 10/11 does not allow
an installer or script to silently rewrite the user's protected default app
choice. After registration, select "Apium Slicer" as the default app for `.stl`
and `.obj` in Windows Settings if Explorer still routes the Preview Pane to
Microsoft Preview Creator.

## Runtime Dependencies
- The produced `ApiumSlicerExplorerPreview.dll` is built with the static MSVC runtime (`/MT`) to avoid requiring a separate Visual C++ Redistributable install.
- The DLL only depends on standard Windows system libraries (for example `Ole32`, `Propsys`, `Windowscodecs`, `User32`, `Gdi32`, `Crypt32`, `Shlwapi`, `Uuid`).
- No external runtime DLLs are required beyond what is normally present on supported Windows systems.

## Bundled Third-Party Code
- ZIP parsing is implemented with bundled `miniz` sources included directly in this repository under `deps/miniz`.
- No separate install step is required for `miniz`.
- See [NOTICES.md](NOTICES.md) for third-party attribution details.

## License
This project is licensed under the GNU General Public License v3.0
(GPLv3).

See the full license text in [LICENSE](LICENSE).
