cmake_minimum_required(VERSION 3.20)

project(ApiumSlicerExplorerPreview LANGUAGES C CXX RC)
include("${CMAKE_SOURCE_DIR}/version.inc")

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
    Propsys
    Uuid
    Windowscodecs
    Crypt32
)

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
endif()
