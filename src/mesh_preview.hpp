#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct MeshPreviewPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct MeshPreviewTriangle {
    MeshPreviewPosition a;
    MeshPreviewPosition b;
    MeshPreviewPosition c;
};

struct MeshPreviewGeometry {
    std::vector<MeshPreviewTriangle> triangles;
    MeshPreviewPosition min;
    MeshPreviewPosition max;
};

namespace mesh_preview_style {
inline constexpr MeshPreviewPosition kViewDirection{1.0f, -1.0f, 0.72f};
inline constexpr MeshPreviewPosition kLightDirection{-0.35f, -0.45f, 0.82f};
inline constexpr COLORREF kModelColor = RGB(0xD7, 0x19, 0x20);
inline constexpr float kAmbient = 0.26f;
inline constexpr float kDiffuse = 0.62f;
inline constexpr float kFacing = 0.15f;
inline constexpr float kMinShade = 0.18f;
inline constexpr float kFit = 0.88f;
}

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
    std::wstring_view pathHint,
    MeshPreviewGeometry* geometry = nullptr);

HBITMAP RenderMeshPreviewBitmap(const MeshPreviewGeometry& geometry, UINT requestedMaxSize);

MeshPreviewResult RenderMeshPreviewToHBitmap(
    const std::vector<uint8_t>& fileBytes,
    std::wstring_view pathHint,
    UINT requestedMaxSize,
    HBITMAP* outBitmap);
