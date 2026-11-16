#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct MeshPreviewResult {
    bool rendered = false;
    bool isMesh = false;
    std::wstring format;
    std::wstring status;
    size_t triangles = 0;
    size_t polygons = 0;
    size_t vertices = 0;
    size_t faces = 0;
    bool hasDimensions = false;
    double dimensionX = 0.0;
    double dimensionY = 0.0;
    double dimensionZ = 0.0;
    bool hasVolume = false;
    double volume = 0.0;
    double surfaceArea = 0.0;
    size_t parts = 0;
    size_t meshErrors = 0;
    size_t degenerateTriangles = 0;
    size_t invalidTriangles = 0;
    size_t duplicateTriangles = 0;
    size_t boundaryEdges = 0;
    size_t nonManifoldEdges = 0;
    size_t inconsistentEdges = 0;
};

bool IsMeshPreviewExtension(std::wstring_view pathOrExtension);

MeshPreviewResult AnalyzeMeshPreview(
    const std::vector<uint8_t>& fileBytes,
    std::wstring_view pathHint);

MeshPreviewResult RenderMeshPreviewToHBitmap(
    const std::vector<uint8_t>& fileBytes,
    std::wstring_view pathHint,
    UINT requestedMaxSize,
    HBITMAP* outBitmap);
