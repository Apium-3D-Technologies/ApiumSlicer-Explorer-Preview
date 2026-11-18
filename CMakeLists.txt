cmake_minimum_required(VERSION 3.20)

project(ApiumSlicerExplorerPreview LANGUAGES C CXX RC)
include("${CMAKE_SOURCE_DIR}/version.inc")

# Keep local builds on version.inc; release builds can use the pushed tag.
if (DEFINED ASXP_RELEASE_VERSION)
    set(ASXP_VERSION "${ASXP_RELEASE_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (NOT MSVC)
    message(FATAL_ERROR "This project requires the MSVC toolchain (cl.exe). Configure from a Visual Studio Developer Prompt or use the Visual Studio generator.")
endif()

set(AP_VERSION_HEADER "${CMAKE_BINARY_DIR}/generated/apium_version.h")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file("${CMAKE_SOURCE_DIR}/src/apium_version.h.in" "${AP_VERSION_HEADER}" @ONLY)

add_library(ApiumSlicerExplorerPreview SHARED
    src/dllmain.cpp
    src/mesh_preview.cpp
    src/metadata.cpp
    deps/miniz/miniz.c
    src/resources.rc
    src/ApiumSlicerExplorerPreview.def
)

set_target_properties(ApiumSlicerExplorerPreview PROPERTIES
    OUTPUT_NAME "ApiumSlicerExplorerPreview"
)

target_include_directories(ApiumSlicerExplorerPreview PRIVATE
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/deps/miniz"
)

target_compile_definitions(ApiumSlicerExplorerPreview PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(ApiumSlicerExplorerPreview PRIVATE
    Ole32
    OleAut32
    Shlwapi
    User32
    Gdi32
    Msimg32
    Propsys
    Uuid
    Windowscodecs
    Crypt32
)

option(ASXP_INTERACTIVE_MESH_PREVIEW "Build the Direct3D 11 preview for STL and OBJ files" ON)
target_compile_definitions(ApiumSlicerExplorerPreview PRIVATE
    ASXP_INTERACTIVE_MESH_PREVIEW=$<BOOL:${ASXP_INTERACTIVE_MESH_PREVIEW}>)
if (ASXP_INTERACTIVE_MESH_PREVIEW)
    get_filename_component(ASXP_RC_DIRECTORY "${CMAKE_RC_COMPILER}" DIRECTORY)
    set(ASXP_SDK_BIN "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin")
    file(GLOB ASXP_SDK_FXC_DIRECTORIES LIST_DIRECTORIES TRUE "${ASXP_SDK_BIN}/*/x86" "${ASXP_SDK_BIN}/*/x64")
    list(SORT ASXP_SDK_FXC_DIRECTORIES COMPARE NATURAL ORDER DESCENDING)
    find_program(ASXP_FXC NAMES fxc HINTS
        "${ASXP_RC_DIRECTORY}"
        "$ENV{WindowsSdkVerBinPath}/x86"
        "${ASXP_SDK_BIN}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x86"
        "${ASXP_SDK_BIN}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        ${ASXP_SDK_FXC_DIRECTORIES}
        REQUIRED
    )

    set(ASXP_MESH_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/mesh_viewport.hlsl")
    foreach(ASXP_SHADER_STAGE IN ITEMS vs ps)
        set(ASXP_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/mesh_${ASXP_SHADER_STAGE}.h")
        add_custom_command(OUTPUT "${ASXP_SHADER_HEADER}"
            COMMAND "${ASXP_FXC}" /nologo /O3 /T "${ASXP_SHADER_STAGE}_4_0" /E "${ASXP_SHADER_STAGE}Main"
                /Fh "${ASXP_SHADER_HEADER}" /Vn "mesh_${ASXP_SHADER_STAGE}" "${ASXP_MESH_SHADER_SOURCE}"
            DEPENDS "${ASXP_MESH_SHADER_SOURCE}"
            VERBATIM
        )
        target_sources(ApiumSlicerExplorerPreview PRIVATE "${ASXP_SHADER_HEADER}")
    endforeach()
    target_sources(ApiumSlicerExplorerPreview PRIVATE src/mesh_viewport.cpp)
    target_link_libraries(ApiumSlicerExplorerPreview PRIVATE D3D11 DXGI)
endif()

include(CTest)
if (BUILD_TESTING)
    add_executable(ApiumMetadataTests
        src/metadata_tests.cpp
        src/metadata.cpp
        deps/miniz/miniz.c
    )
    target_include_directories(ApiumMetadataTests PRIVATE "${CMAKE_SOURCE_DIR}/deps/miniz")
    target_compile_definitions(ApiumMetadataTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(ApiumMetadataTests PRIVATE Ole32 OleAut32 Shlwapi Gdi32 Uuid Windowscodecs Crypt32)
    add_test(NAME ApiumMetadataTests COMMAND ApiumMetadataTests)

    add_executable(ApiumMeshPreviewTests src/mesh_preview_tests.cpp src/mesh_preview.cpp)
    target_compile_definitions(ApiumMeshPreviewTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        ASXP_INTERACTIVE_MESH_PREVIEW=$<BOOL:${ASXP_INTERACTIVE_MESH_PREVIEW}>)
    target_link_libraries(ApiumMeshPreviewTests PRIVATE Ole32 Shlwapi User32 Gdi32 Uuid)
    add_dependencies(ApiumMeshPreviewTests ApiumSlicerExplorerPreview)
    add_test(NAME ApiumMeshPreviewTests COMMAND ApiumMeshPreviewTests "$<TARGET_FILE:ApiumSlicerExplorerPreview>")

    find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if (POWERSHELL_EXECUTABLE)
        add_test(NAME ApiumRegistrationTests COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/test-registration.ps1")
    endif()
endif()
