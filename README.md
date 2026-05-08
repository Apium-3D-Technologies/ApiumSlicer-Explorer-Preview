![ApiumLogo](/doc/resource/apium_logo.png?raw=true)

# ApiumSlicerExplorerPreview
ApiumSlicerExplorerPreview is a lightweight Windows shell extension that adds Explorer thumbnail support for `.gcode` and `.3mf` files, including a built-in fallback image when no embedded preview is available.


## Prerequisites
**Note: The Windows 10/11 SDK should be installed with Visual Studio.**
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/)
- [CMake (>= 3.20)](https://cmake.org/download/)
- [Ninja](https://github.com/ninja-build/ninja/releases) (only required for Ninja builds)


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
- Register (per-user via `DllRegisterServer`): `scripts/register.ps1`
- Unregister: `scripts/unregister.ps1`

Examples:
- Visual Studio build: `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`
- Ninja build (flag): `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 --ninja`
