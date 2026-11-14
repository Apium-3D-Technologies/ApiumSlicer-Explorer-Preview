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


## Scripts
- Build: `scripts/build.ps1`
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
