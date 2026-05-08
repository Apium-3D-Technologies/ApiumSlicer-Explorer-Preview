cmake_minimum_required(VERSION 3.20)

project(ApiumSlicerExplorerPreview LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ApiumSlicerExplorerPreview SHARED
    src/dllmain.cpp
    src/resources.rc
    src/ApiumSlicerExplorerPreview.def
)

set_target_properties(ApiumSlicerExplorerPreview PROPERTIES
    OUTPUT_NAME "ApiumSlicerExplorerPreview"
)

target_compile_definitions(ApiumSlicerExplorerPreview PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(ApiumSlicerExplorerPreview PRIVATE
    Ole32
    Shlwapi
    Windowscodecs
    Crypt32
)
