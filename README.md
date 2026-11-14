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

**Output:** `build/Release/ApiumSlicerExplorerPreview.dll`


## Build (Ninja Generator)
```powershell
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ninja
```

**Output:** `build-ninja/ApiumSlicerExplorerPreview.dll`


## Bundled Third-Party Code
See [NOTICES.md](NOTICES.md) for third-party attribution details.


### License
`ApiumSlicer-Explorer-Preview` is licensed under the [GNU Affero General Public License, version 3](LICENSE).
