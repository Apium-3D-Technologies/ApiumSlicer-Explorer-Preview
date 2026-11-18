#include "mesh_preview.hpp"

#include <shlwapi.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::vector<uint8_t> Tetrahedron(float offset, bool reversed = false, bool open = false, bool mixed = false) {
    const std::array<std::array<float, 3>, 4> points = {{
        {offset, offset, offset}, {offset + 2, offset, offset},
        {offset, offset + 3, offset}, {offset, offset, offset + 4}
    }};
    const std::array<std::array<int, 3>, 4> faces = {{{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}};
    const uint32_t count = open ? 3 : 4;
    std::vector<uint8_t> bytes(84 + 50 * count, 0);
    std::memcpy(bytes.data() + 80, &count, sizeof(count));
    for (uint32_t i = 0; i < count; ++i) {
        auto face = faces[i];
        if (reversed || (mixed && i == 0)) { std::swap(face[1], face[2]); }
        for (int j = 0; j < 3; ++j) {
            std::memcpy(bytes.data() + 84 + 50 * i + 12 + 12 * j, points[face[j]].data(), 12);
        }
    }
    return bytes;
}

std::vector<uint8_t> CombineStl(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    uint32_t countA = 0, countB = 0;
    std::memcpy(&countA, a.data() + 80, 4);
    std::memcpy(&countB, b.data() + 80, 4);
    const uint32_t count = countA + countB;
    std::memcpy(a.data() + 80, &count, 4);
    a.insert(a.end(), b.begin() + 84, b.end());
    return a;
}

std::vector<uint8_t> SingleTriangle(const std::array<std::array<float, 3>, 3>& points) {
    std::vector<uint8_t> bytes(134, 0);
    const uint32_t count = 1;
    std::memcpy(bytes.data() + 80, &count, 4);
    for (int i = 0; i < 3; ++i) {
        std::memcpy(bytes.data() + 96 + 12 * i, points[i].data(), 12);
    }
    return bytes;
}

void CheckGeometryAnalysis() {
    const double tetrahedronArea = 13 + std::sqrt(61.0);
    const auto separate = AnalyzeMeshPreview(CombineStl(Tetrahedron(0), Tetrahedron(10)), L"parts.stl");
    Check(separate.parts == 2 && separate.meshErrors == 0 && separate.triangles == 8, "Separated solids must have two parts and no mesh errors");
    Check(std::abs(separate.surfaceArea - 2 * tetrahedronArea) < 1e-9 && separate.hasVolume && std::abs(separate.volume - 8) < 1e-9, "Measures of separate solids must add up");

    const auto open = AnalyzeMeshPreview(Tetrahedron(0, false, true), L"open.stl");
    Check(open.parts == 1 && open.boundaryEdges == 3 && open.meshErrors == 3 && !open.hasVolume, "A missing face must expose three open edges");
    Check(std::abs(open.surfaceArea - 13) < 1e-9, "Open mesh surface area must still be available");
    const auto mixed = AnalyzeMeshPreview(Tetrahedron(0, false, false, true), L"mixed.stl");
    Check(mixed.parts == 1 && mixed.boundaryEdges == 0 && mixed.inconsistentEdges == 3 && mixed.meshErrors == 3, "Reversed face must expose three inconsistent edges");

    const auto duplicate = SingleTriangle({{{0, 0, 0}, {2, 0, 0}, {0, 3, 0}}});
    const auto duplicated = AnalyzeMeshPreview(CombineStl(Tetrahedron(0), duplicate), L"duplicate.stl");
    Check(duplicated.duplicateTriangles == 1 && duplicated.nonManifoldEdges == 3 && duplicated.meshErrors == 4 && duplicated.parts == 1 && !duplicated.hasVolume, "Duplicate faces must be detected regardless of winding");
    Check(std::abs(duplicated.surfaceArea - (tetrahedronArea + 3)) < 1e-9, "Surface area must include duplicate input faces");

    const auto degenerate = SingleTriangle({{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}});
    const auto onlyDegenerate = AnalyzeMeshPreview(degenerate, L"degenerate.stl");
    Check(onlyDegenerate.isMesh && onlyDegenerate.parts == 0 && onlyDegenerate.triangles == 1 && onlyDegenerate.surfaceArea == 0 && onlyDegenerate.degenerateTriangles == 1 && onlyDegenerate.meshErrors == 1 && !onlyDegenerate.hasVolume, "Degenerate-only meshes must retain their error count");
    const auto withDegenerate = AnalyzeMeshPreview(CombineStl(Tetrahedron(0), degenerate), L"degenerate-face.stl");
    Check(withDegenerate.parts == 1 && withDegenerate.triangles == 5 && withDegenerate.degenerateTriangles == 1 && withDegenerate.meshErrors == 1 && !withDegenerate.hasVolume, "Discarded degenerate faces must remain visible in metrics");

    const auto tinyTriangle = AnalyzeMeshPreview(SingleTriangle({{{0, 0, 0}, {1e-5f, 0, 0}, {0, 1e-5f, 0}}}), L"tiny.stl");
    Check(tinyTriangle.degenerateTriangles == 0 && tinyTriangle.parts == 1 && std::abs(tinyTriangle.surfaceArea - 5e-11) < 1e-17, "Small valid triangles must not be classified as degenerate");
    const auto invalid = AnalyzeMeshPreview(SingleTriangle({{{0, 0, 0}, {INFINITY, 0, 0}, {0, 1, 0}}}), L"invalid.stl");
    Check(invalid.isMesh && invalid.invalidTriangles == 1 && invalid.meshErrors == 1 && invalid.parts == 0 && !invalid.hasVolume, "Nonfinite coordinates must be counted instead of entering topology analysis");

    const auto first = SingleTriangle({{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}});
    const auto pointContact = AnalyzeMeshPreview(CombineStl(first, SingleTriangle({{{0, 0, 0}, {-1, 0, 0}, {0, -1, 0}}})), L"point-contact.stl");
    Check(pointContact.parts == 2 && pointContact.boundaryEdges == 6, "A shared point alone must not merge parts");
    const auto threeFaces = CombineStl(CombineStl(first, SingleTriangle({{{1, 0, 0}, {0, 0, 0}, {0, -1, 0}}})), SingleTriangle({{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}}));
    const auto nonManifold = AnalyzeMeshPreview(threeFaces, L"non-manifold.stl");
    Check(nonManifold.parts == 1 && nonManifold.nonManifoldEdges == 1 && nonManifold.boundaryEdges == 6 && nonManifold.meshErrors == 7, "Three faces sharing an edge must form one part with a non-manifold edge");
}

std::wstring WindowText(HWND window) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    const int count = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(count);
    return text;
}

BOOL CALLBACK CollectChildText(HWND child, LPARAM data) {
    auto& text = *reinterpret_cast<std::wstring*>(data);
    text += WindowText(child) + L"\n";
    return TRUE;
}

void CheckThumbnail(HMODULE dll, const std::vector<uint8_t>& bytes) {
    using GetFactory = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
    const auto getFactory = reinterpret_cast<GetFactory>(GetProcAddress(dll, "DllGetClassObject"));
    CLSID clsid = {};
    CLSIDFromString(L"{8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}", &clsid);
    ComPtr<IClassFactory> factory;
    Check(SUCCEEDED(getFactory(clsid, IID_PPV_ARGS(&factory))), "Thumbnail factory creation failed");
    ComPtr<IThumbnailProvider> thumbnail;
    Check(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&thumbnail))), "Thumbnail provider creation failed");
    ComPtr<IInitializeWithStream> init;
    Check(SUCCEEDED(thumbnail.As(&init)), "Thumbnail stream initializer missing");
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    Check(SUCCEEDED(init->Initialize(stream.Get(), STGM_READ)), "Thumbnail stream initialization failed");
    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    Check(SUCCEEDED(thumbnail->GetThumbnail(96, &bitmap, &alpha)) && bitmap, "Thumbnail rendering failed");
    try {
        DIBSECTION dib = {};
        Check(GetObjectW(bitmap, sizeof(dib), &dib) == sizeof(dib) && dib.dsBm.bmBits && dib.dsBm.bmBitsPixel == 32, "Thumbnail must be a 32-bit DIB");
        Check(alpha == WTSAT_ARGB, "Thumbnail provider must report an alpha channel");
        const auto* pixels = static_cast<const uint32_t*>(dib.dsBm.bmBits);
        Check(pixels[0] == 0 && pixels[dib.dsBm.bmWidth - 1] == 0, "Thumbnail corners must be transparent");
        size_t transparent = 0, opaque = 0, partial = 0;
        for (LONG y = 0; y < dib.dsBm.bmHeight; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(dib.dsBm.bmBits) + y * dib.dsBm.bmWidthBytes);
            for (LONG x = 0; x < dib.dsBm.bmWidth; ++x) {
                const uint32_t p = row[x], a = p >> 24;
                transparent += a == 0;
                opaque += a == 255;
                partial += a > 0 && a < 255;
                Check((p & 255) <= a && ((p >> 8) & 255) <= a && ((p >> 16) & 255) <= a, "Thumbnail pixels must use premultiplied alpha");
            }
        }
        Check(transparent > 0 && opaque > 0 && partial > 0, "Thumbnail must contain a transparent background, an opaque model and antialiased edges");
    } catch (...) {
        DeleteObject(bitmap);
        throw;
    }
    DeleteObject(bitmap);
}

std::wstring CheckPreview(HMODULE dll, const std::vector<uint8_t>& bytes, const wchar_t* sourcePath = nullptr, bool tetrahedron = false, bool expectMesh = true) {
    using GetFactory = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
    const auto getFactory = reinterpret_cast<GetFactory>(GetProcAddress(dll, "DllGetClassObject"));
    Check(getFactory != nullptr, "Missing COM factory export");
    CLSID clsid = {};
    CLSIDFromString(L"{B450D582-1E34-49E0-8AE7-AFF2D2CD2535}", &clsid);
    ComPtr<IClassFactory> factory;
    Check(SUCCEEDED(getFactory(clsid, IID_PPV_ARGS(&factory))), "Preview factory creation failed");
    ComPtr<IPreviewHandler> preview;
    Check(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&preview))), "Preview creation failed");
    ComPtr<IPreviewHandlerVisuals> visuals;
    Check(SUCCEEDED(preview.As(&visuals)), "Preview visual settings are unavailable");
    Check(SUCCEEDED(visuals->SetBackgroundColor(RGB(255, 255, 255))), "Setting the light background failed");
    if (sourcePath) {
        ComPtr<IInitializeWithFile> init;
        Check(SUCCEEDED(preview.As(&init)), "Missing file initializer");
        Check(SUCCEEDED(init->Initialize(sourcePath, STGM_READ)), "File initialization failed");
    } else {
        ComPtr<IInitializeWithStream> init;
        Check(SUCCEEDED(preview.As(&init)), "Missing stream initializer");
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
        Check(stream != nullptr, "Stream creation failed");
        Check(SUCCEEDED(init->Initialize(stream.Get(), STGM_READ)), "Stream initialization failed");
    }

    HWND parent = CreateWindowExW(0, L"STATIC", L"Preview test", WS_POPUP, 0, 0, 275, 799,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "Test host creation failed");
    std::wstring content;
    try {
        // Explorer can call DoPreview before it provides a nonzero preview rectangle.
        RECT rect = {};
        Check(SUCCEEDED(preview->SetWindow(parent, &rect)), "SetWindow failed");
        Check(SUCCEEDED(preview->DoPreview()), "DoPreview failed");
        rect = {0, 0, 275, 799};
        Check(SUCCEEDED(preview->SetRect(&rect)), "SetRect failed");
        EnumChildWindows(parent, CollectChildText, reinterpret_cast<LPARAM>(&content));
        Check(content.find(L"Apium Preview v") != std::wstring::npos, "Missing version footer");
        if (expectMesh) {
            for (const auto* expected : {L"Volume:", L"Surface area:", L"Size:", L"Parts:", L"Mesh errors:", L"Triangles:", L"File size:"}) {
                Check(content.find(expected) != std::wstring::npos, "Missing requested preview detail");
            }
        }
        if (tetrahedron) {
            Check(content.find(L"Volume: 4.0 unit\u00b3") != std::wstring::npos, "Volume missing from preview text");
            Check(content.find(L"Surface area: 20.8 unit\u00b2") != std::wstring::npos, "Surface area missing from preview text");
            Check(content.find(L"Parts: 1") != std::wstring::npos && content.find(L"Mesh errors: 0") != std::wstring::npos, "Topology metrics missing from preview text");
            Check(content.find(L"Size: 2 x 3 x 4") != std::wstring::npos, "Size missing from preview text");
            Check(content.find(L"model units") == std::wstring::npos, "Obsolete model units suffix in preview text");
        }
        HWND pane = GetWindow(parent, GW_CHILD);
        HWND image = FindWindowExW(pane, nullptr, L"ApiumSlicerPreviewImageWindow", nullptr);
        RECT imageRect = {};
        Check(image && GetWindowRect(image, &imageRect), "Missing preview image control");
        Check(imageRect.right > imageRect.left && imageRect.bottom > imageRect.top, "Preview image has no size");
        const HWND viewport = FindWindowExW(image, nullptr, L"ApiumSlicerMeshViewportWindow", nullptr);
        Check(!viewport || (ASXP_INTERACTIVE_MESH_PREVIEW && expectMesh), "Unexpected Direct3D viewport");
        if (ASXP_INTERACTIVE_MESH_PREVIEW && tetrahedron && GetEnvironmentVariableW(L"ASXP_TEST_REQUIRE_GPU", nullptr, 0)) {
            Check(viewport != nullptr, "The Direct3D viewport is required for this run");
        }
        if (viewport) {
            RECT viewportRect = {};
            Check(GetWindowRect(viewport, &viewportRect) && EqualRect(&viewportRect, &imageRect), "Direct3D viewport must fill the image area");
            POINT center{(viewportRect.left + viewportRect.right) / 2, (viewportRect.top + viewportRect.bottom) / 2};
            SendMessageW(viewport, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(center.x, center.y));
            SendMessageW(viewport, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 20));
            SendMessageW(viewport, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(55, 40));
            SendMessageW(viewport, WM_LBUTTONUP, 0, MAKELPARAM(55, 40));
            SendMessageW(viewport, WM_PAINT, 0, 0);
            Check(IsWindow(viewport), "Direct3D frame failed after mouse input");
        }
        HWND details = FindWindowExW(pane, nullptr, L"RICHEDIT50W", nullptr);
        if (!details) { details = FindWindowExW(pane, nullptr, L"EDIT", nullptr); }
        HWND footer = FindWindowExW(pane, nullptr, L"STATIC", nullptr);
        RECT detailsRect = {}, footerRect = {}, paneRect = {};
        Check(details && footer && GetWindowRect(details, &detailsRect) && GetWindowRect(footer, &footerRect) && GetWindowRect(pane, &paneRect), "Missing details or footer controls");
        if (viewport) {
            Check(imageRect.left == paneRect.left && imageRect.right == paneRect.right && imageRect.top == paneRect.top,
                "The 3D view must fill the pane width from the top");
            Check(imageRect.bottom - imageRect.top == imageRect.right - imageRect.left, "The 3D view must be square");
        }
        Check(detailsRect.top >= imageRect.bottom && footerRect.top >= detailsRect.bottom, "Image, details and version must not overlap");
        Check(footerRect.bottom == paneRect.bottom, "Version footer must stay at the bottom");
        HDC colorDc = CreateCompatibleDC(nullptr);
        Check(colorDc != nullptr, "Creating a device context for theme checking failed");
        SendMessageW(pane, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(colorDc), reinterpret_cast<LPARAM>(footer));
        const bool lightColors = GetBkColor(colorDc) == RGB(255, 255, 255) && GetTextColor(colorDc) == RGB(0, 0, 0);
        const HRESULT darkBackground = visuals->SetBackgroundColor(RGB(0x20, 0x20, 0x20));
        SendMessageW(pane, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(colorDc), reinterpret_cast<LPARAM>(footer));
        const bool darkColors = GetBkColor(colorDc) == RGB(0x20, 0x20, 0x20) && GetTextColor(colorDc) == RGB(255, 255, 255);
        DeleteDC(colorDc);
        Check(lightColors && SUCCEEDED(darkBackground) && darkColors, "Preview colors must follow the light and dark backgrounds");
        for (const auto size : {1L, 16L, 80L}) {
            rect = {0, 0, size, size};
            Check(SUCCEEDED(preview->SetRect(&rect)), "Small preview layout failed");
            if (viewport) {
                SendMessageW(viewport, WM_PAINT, 0, 0);
                Check(IsWindow(viewport), "Direct3D frame failed after resize");
                Check(GetWindowRect(viewport, &imageRect) && GetWindowRect(details, &detailsRect) &&
                    imageRect.bottom <= detailsRect.top && detailsRect.bottom > detailsRect.top,
                    "Short panes must retain room for mesh details");
            }
        }
        std::wcout << content;
        preview->Unload();
    } catch (...) {
        preview->Unload();
        DestroyWindow(parent);
        throw;
    }
    DestroyWindow(parent);
    return content;
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc >= 2, "Pass the preview DLL path");
        CheckGeometryAnalysis();
        for (const float offset : {0.0f, 1000000.0f}) {
            for (const bool reversed : {false, true}) {
                const auto bytes = Tetrahedron(offset, reversed);
                const auto info = AnalyzeMeshPreview(bytes, L"test.stl");
                Check(info.isMesh && info.triangles == 4, "STL triangle count is incorrect");
                Check(info.hasDimensions && info.dimensionX == 2 && info.dimensionY == 3 && info.dimensionZ == 4, "Bounding box is incorrect");
                Check(info.hasVolume && std::abs(info.volume - 4) < 1e-9, "Volume must tolerate translation and reversed winding");
                Check(info.parts == 1 && info.meshErrors == 0 && std::abs(info.surfaceArea - (13 + std::sqrt(61.0))) < 1e-9, "Surface area and topology must tolerate translation and reversed winding");
            }
        }
        Check(!AnalyzeMeshPreview(Tetrahedron(0, false, true), L"test.stl").hasVolume, "Open meshes must not report a solid volume");
        Check(!AnalyzeMeshPreview(Tetrahedron(0, false, false, true), L"test.stl").hasVolume, "Inconsistent winding must not report a solid volume");
        const auto bytes = Tetrahedron(0);
        std::ostringstream ascii, obj;
        ascii << "solid tetrahedron\n";
        for (int i = 0; i < 4; ++i) {
            ascii << "facet normal 0 0 0\nouter loop\n";
            for (int j = 0; j < 3; ++j) {
                std::array<float, 3> vertex = {};
                std::memcpy(vertex.data(), bytes.data() + 84 + 50 * i + 12 + 12 * j, 12);
                ascii << "vertex " << vertex[0] << ' ' << vertex[1] << ' ' << vertex[2] << '\n';
                obj << "v " << vertex[0] << ' ' << vertex[1] << ' ' << vertex[2] << '\n';
            }
            ascii << "endloop\nendfacet\n";
            obj << "f " << i * 3 + 1 << ' ' << i * 3 + 2 << ' ' << i * 3 + 3 << '\n';
        }
        ascii << "endsolid tetrahedron\n";
        const std::string asciiText = ascii.str(), objText = obj.str();
        const std::vector<uint8_t> asciiBytes(asciiText.begin(), asciiText.end());
        const std::vector<uint8_t> objBytes(objText.begin(), objText.end());
        const std::string quadCube =
            "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 0 2 0\n"
            "v 0 0 2\nv 2 0 2\nv 2 2 2\nv 0 2 2\n"
            "f 4 3 2 1\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n";
        const auto cube = AnalyzeMeshPreview({quadCube.begin(), quadCube.end()}, L"cube.obj");
        Check(cube.triangles == 12 && cube.faces == 6 && cube.hasVolume && std::abs(cube.volume - 8) < 1e-9, "OBJ polygon triangulation must preserve solid volume");
        Check(cube.parts == 1 && cube.meshErrors == 0 && std::abs(cube.surfaceArea - 24) < 1e-9, "Triangulated OBJ cube must retain its surface area and topology");
        for (const auto& [fixture, path] : {
            std::pair{asciiBytes, L"test.stl"}, std::pair{objBytes, L"test.obj"}
        }) {
            const auto info = AnalyzeMeshPreview(fixture, path);
            Check(info.isMesh && info.triangles == 4 && info.hasVolume && std::abs(info.volume - 4) < 1e-9, "ASCII STL/OBJ volume or triangle count is incorrect");
            Check(info.dimensionX == 2 && info.dimensionY == 3 && info.dimensionZ == 4, "ASCII STL/OBJ bounding box is incorrect");
            Check(info.parts == 1 && info.meshErrors == 0 && std::abs(info.surfaceArea - (13 + std::sqrt(61.0))) < 1e-9, "ASCII STL/OBJ geometry metrics are incorrect");
            Check(AnalyzeMeshPreview(fixture, L"").hasVolume, "Stream format detection failed");
            HBITMAP fixtureBitmap = nullptr;
            Check(RenderMeshPreviewToHBitmap(fixture, path, 128, &fixtureBitmap).rendered && fixtureBitmap, "ASCII STL/OBJ rendering failed");
            DeleteObject(fixtureBitmap);
        }
        HBITMAP bitmap = nullptr;
        const auto rendered = RenderMeshPreviewToHBitmap(bytes, L"test.stl", 256, &bitmap);
        Check(rendered.rendered && bitmap && rendered.hasVolume, "Mesh render failed");
        DeleteObject(bitmap);

        Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
        HMODULE dll = LoadLibraryW(argv[1]);
        Check(dll != nullptr, "Cannot load preview DLL");
        const bool expectPreviewDebug = argc >= 3 && std::wstring_view(argv[2]) == L"--expect-debug";
        const std::string plainGcode = "; Non-Apium G-code without metadata\nG28\nG1 X0 Y0\n";
        const auto noMetadataPreview = CheckPreview(dll, {plainGcode.begin(), plainGcode.end()}, nullptr, false, false);
        Check(noMetadataPreview.find(L"No Apium metadata detected for this file.") != std::wstring::npos, "Missing no-metadata notice");
        for (const auto* marker : {L"Debug:", L"Build:", L"Host:", L"InitPath:", L"Container:", L"ConfigPart:", L"Decode:", L"ApiumDetect:", L"MissingFields:"}) {
            Check((noMetadataPreview.find(marker) != std::wstring::npos) == expectPreviewDebug, "Preview diagnostics must match SHOW_DEBUG");
        }
        CheckThumbnail(dll, bytes);
        CheckThumbnail(dll, asciiBytes);
        CheckThumbnail(dll, objBytes);
        CheckPreview(dll, bytes, nullptr, true);
        CheckPreview(dll, asciiBytes, nullptr, true);
        CheckPreview(dll, objBytes, nullptr, true);
        const auto openPreview = CheckPreview(dll, Tetrahedron(0, false, true));
        Check(openPreview.find(L"Mesh errors: 3 (open edges: 3)") != std::wstring::npos && openPreview.find(L"Volume: n/a") != std::wstring::npos, "Open-edge error breakdown missing from preview");
        const auto degeneratePreview = CheckPreview(dll, SingleTriangle({{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}}));
        Check(degeneratePreview.find(L"Mesh errors: 1 (degenerate triangles: 1)") != std::wstring::npos && degeneratePreview.find(L"Parts: 0") != std::wstring::npos, "Degenerate-only mesh diagnostics missing from preview");
        if (argc >= 3 && !expectPreviewDebug) {
            std::ifstream file(std::filesystem::path(argv[2]), std::ios::binary);
            Check(file.good(), "Cannot open requested sample");
            const std::vector<uint8_t> sample{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            const auto info = AnalyzeMeshPreview(sample, argv[2]);
            Check(info.isMesh, "Sample mesh parsing failed");
            std::wcout << L"Sample triangles=" << info.triangles << L" volume=" << info.volume
                << L" surfaceArea=" << info.surfaceArea << L" parts=" << info.parts << L" meshErrors=" << info.meshErrors
                << L" hasVolume=" << info.hasVolume << L" dimensions=" << info.dimensionX << L" x " << info.dimensionY << L" x " << info.dimensionZ << L"\n";
            CheckPreview(dll, sample);
            CheckPreview(dll, sample, argv[2]);
        }
        FreeLibrary(dll);
        CoUninitialize();
        std::cout << "Mesh preview checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
