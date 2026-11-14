#include "mesh_preview.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace {

constexpr size_t kMaxTriangles = 1'000'000;
constexpr UINT kDefaultPreviewSize = 512;
constexpr UINT kMaxRenderSize = 2048;
constexpr int kSupersample = 2;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Triangle {
    Vec3 a;
    Vec3 b;
    Vec3 c;
};

struct Mesh {
    std::vector<Triangle> triangles;
    size_t vertexCount = 0;
    size_t faceCount = 0;
};

struct Bounds {
    Vec3 min;
    Vec3 max;
    bool valid = false;
};

struct VertexKey {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;

    bool operator==(const VertexKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct VertexKeyHash {
    size_t operator()(const VertexKey& key) const {
        size_t h = static_cast<size_t>(key.x);
        h ^= static_cast<size_t>(key.y) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= static_cast<size_t>(key.z) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

struct ProjectedTriangle {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float z0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float z1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float z2 = 0.0f;
    float shade = 1.0f;
};

std::wstring ToLower(std::wstring_view text) {
    std::wstring out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return out;
}

std::wstring ExtensionFromHint(std::wstring_view pathHint) {
    const size_t slash = pathHint.find_last_of(L"\\/");
    const size_t dot = pathHint.find_last_of(L'.');
    if (dot == std::wstring_view::npos || (slash != std::wstring_view::npos && dot < slash)) {
        return L"";
    }
    return ToLower(pathHint.substr(dot));
}

bool IsFinite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 Add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 Sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Mul(Vec3 v, float scalar) {
    return {v.x * scalar, v.y * scalar, v.z * scalar};
}

float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {
        (a.y * b.z) - (a.z * b.y),
        (a.z * b.x) - (a.x * b.z),
        (a.x * b.y) - (a.y * b.x),
    };
}

float Length(Vec3 v) {
    return std::sqrt(Dot(v, v));
}

std::optional<Vec3> Normalize(Vec3 v) {
    const float len = Length(v);
    if (!std::isfinite(len) || len <= 1.0e-8f) {
        return std::nullopt;
    }
    return Mul(v, 1.0f / len);
}

bool IsDegenerate(const Triangle& tri) {
    const Vec3 ab = Sub(tri.b, tri.a);
    const Vec3 ac = Sub(tri.c, tri.a);
    return Length(Cross(ab, ac)) <= 1.0e-8f;
}

VertexKey MakeVertexKey(Vec3 v) {
    VertexKey key;
    std::memcpy(&key.x, &v.x, sizeof(key.x));
    std::memcpy(&key.y, &v.y, sizeof(key.y));
    std::memcpy(&key.z, &v.z, sizeof(key.z));
    return key;
}

size_t CountUniqueTriangleVertices(const std::vector<Triangle>& triangles) {
    std::unordered_set<VertexKey, VertexKeyHash> unique;
    unique.reserve(triangles.size() * 3);
    for (const Triangle& tri : triangles) {
        unique.insert(MakeVertexKey(tri.a));
        unique.insert(MakeVertexKey(tri.b));
        unique.insert(MakeVertexKey(tri.c));
    }
    return unique.size();
}

Bounds CalculateBounds(const std::vector<Triangle>& triangles) {
    Bounds bounds = {
        {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
        },
        {
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
        },
        false,
    };

    auto include = [&](Vec3 v) {
        bounds.min.x = std::min(bounds.min.x, v.x);
        bounds.min.y = std::min(bounds.min.y, v.y);
        bounds.min.z = std::min(bounds.min.z, v.z);
        bounds.max.x = std::max(bounds.max.x, v.x);
        bounds.max.y = std::max(bounds.max.y, v.y);
        bounds.max.z = std::max(bounds.max.z, v.z);
        bounds.valid = true;
    };

    for (const Triangle& tri : triangles) {
        include(tri.a);
        include(tri.b);
        include(tri.c);
    }

    return bounds;
}

bool AddTriangle(Mesh& mesh, Triangle tri) {
    if (mesh.triangles.size() >= kMaxTriangles) {
        return false;
    }
    if (!IsFinite(tri.a) || !IsFinite(tri.b) || !IsFinite(tri.c) || IsDegenerate(tri)) {
        return true;
    }
    mesh.triangles.push_back(tri);
    return true;
}

uint32_t ReadU32LE(const uint8_t* ptr) {
    return static_cast<uint32_t>(ptr[0]) |
           (static_cast<uint32_t>(ptr[1]) << 8) |
           (static_cast<uint32_t>(ptr[2]) << 16) |
           (static_cast<uint32_t>(ptr[3]) << 24);
}

float ReadF32LE(const uint8_t* ptr) {
    uint32_t bits = ReadU32LE(ptr);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool ParseBinaryStl(const std::vector<uint8_t>& bytes, Mesh& mesh, bool strictSize) {
    if (bytes.size() < 84) {
        return false;
    }

    const uint32_t count = ReadU32LE(bytes.data() + 80);
    const uint64_t expectedSize = 84ull + (static_cast<uint64_t>(count) * 50ull);
    if (strictSize && expectedSize != bytes.size()) {
        return false;
    }
    if (expectedSize > bytes.size() || count > kMaxTriangles) {
        return false;
    }

    Mesh parsed;
    parsed.triangles.reserve(std::min<size_t>(count, 100000));
    const uint8_t* ptr = bytes.data() + 84;
    for (uint32_t i = 0; i < count; ++i) {
        ptr += 12;
        Triangle tri = {
            {ReadF32LE(ptr + 0), ReadF32LE(ptr + 4), ReadF32LE(ptr + 8)},
            {ReadF32LE(ptr + 12), ReadF32LE(ptr + 16), ReadF32LE(ptr + 20)},
            {ReadF32LE(ptr + 24), ReadF32LE(ptr + 28), ReadF32LE(ptr + 32)},
        };
        if (!AddTriangle(parsed, tri)) {
            return false;
        }
        ptr += 38;
    }

    if (parsed.triangles.empty()) {
        return false;
    }
    parsed.faceCount = parsed.triangles.size();
    parsed.vertexCount = CountUniqueTriangleVertices(parsed.triangles);
    mesh = std::move(parsed);
    return true;
}

bool ParseFloatToken(std::string_view token, float& value) {
    if (token.empty()) {
        return false;
    }
    const char* begin = token.data();
    const char* end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec == std::errc{} && result.ptr == end && std::isfinite(value)) {
        return true;
    }

    std::string copy(token);
    char* parseEnd = nullptr;
    value = std::strtof(copy.c_str(), &parseEnd);
    return parseEnd == copy.c_str() + copy.size() && std::isfinite(value);
}

std::vector<std::string_view> SplitWhitespace(std::string_view line) {
    std::vector<std::string_view> tokens;
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        const size_t start = pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        if (start < pos) {
            tokens.push_back(line.substr(start, pos - start));
        }
    }
    return tokens;
}

bool ParseAsciiStl(const std::vector<uint8_t>& bytes, Mesh& mesh) {
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.find("vertex") == std::string::npos) {
        return false;
    }

    Mesh parsed;
    std::vector<Vec3> current;
    current.reserve(3);

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const auto tokens = SplitWhitespace(line);
        if (tokens.size() != 4 || tokens[0] != "vertex") {
            continue;
        }

        Vec3 v;
        if (!ParseFloatToken(tokens[1], v.x) ||
            !ParseFloatToken(tokens[2], v.y) ||
            !ParseFloatToken(tokens[3], v.z)) {
            current.clear();
            continue;
        }

        current.push_back(v);
        if (current.size() == 3) {
            if (!AddTriangle(parsed, {current[0], current[1], current[2]})) {
                return false;
            }
            current.clear();
        }
    }

    if (parsed.triangles.empty()) {
        return false;
    }
    parsed.faceCount = parsed.triangles.size();
    parsed.vertexCount = CountUniqueTriangleVertices(parsed.triangles);
    mesh = std::move(parsed);
    return true;
}

bool ParseIndex(std::string_view token, int vertexCount, int& outIndex) {
    const size_t slash = token.find('/');
    const std::string_view vertexToken = slash == std::string_view::npos ? token : token.substr(0, slash);
    if (vertexToken.empty()) {
        return false;
    }

    int raw = 0;
    const auto result = std::from_chars(vertexToken.data(), vertexToken.data() + vertexToken.size(), raw);
    if (result.ec != std::errc{} || result.ptr != vertexToken.data() + vertexToken.size() || raw == 0) {
        return false;
    }

    const int resolved = raw > 0 ? raw - 1 : vertexCount + raw;
    if (resolved < 0 || resolved >= vertexCount) {
        return false;
    }
    outIndex = resolved;
    return true;
}

bool ParseObj(const std::vector<uint8_t>& bytes, Mesh& mesh) {
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Mesh parsed;
    std::vector<Vec3> vertices;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }

        const auto tokens = SplitWhitespace(line);
        if (tokens.empty()) {
            continue;
        }

        if (tokens[0] == "v" && tokens.size() >= 4) {
            Vec3 v;
            if (ParseFloatToken(tokens[1], v.x) &&
                ParseFloatToken(tokens[2], v.y) &&
                ParseFloatToken(tokens[3], v.z) &&
                IsFinite(v)) {
                vertices.push_back(v);
            }
            continue;
        }

        if (tokens[0] != "f" || tokens.size() < 4) {
            continue;
        }

        std::vector<int> indices;
        indices.reserve(tokens.size() - 1);
        bool validFace = true;
        for (size_t i = 1; i < tokens.size(); ++i) {
            int index = -1;
            if (!ParseIndex(tokens[i], static_cast<int>(vertices.size()), index)) {
                validFace = false;
                break;
            }
            indices.push_back(index);
        }
        if (!validFace || indices.size() < 3) {
            continue;
        }

        ++parsed.faceCount;
        for (size_t i = 1; i + 1 < indices.size(); ++i) {
            if (!AddTriangle(parsed, {vertices[indices[0]], vertices[indices[i]], vertices[indices[i + 1]]})) {
                return false;
            }
        }
    }

    if (parsed.triangles.empty()) {
        return false;
    }
    parsed.vertexCount = vertices.size();
    mesh = std::move(parsed);
    return true;
}

bool ParseMesh(const std::vector<uint8_t>& bytes, std::wstring_view pathHint, Mesh& mesh, std::wstring& format) {
    if (bytes.empty()) {
        return false;
    }

    const std::wstring ext = ExtensionFromHint(pathHint);
    if (ext == L".obj") {
        if (ParseObj(bytes, mesh)) {
            format = L"OBJ";
            return true;
        }
        return false;
    }

    if (ext == L".stl") {
        if (ParseBinaryStl(bytes, mesh, true)) {
            format = L"STL Binary";
            return true;
        }
        if (ParseAsciiStl(bytes, mesh)) {
            format = L"STL ASCII";
            return true;
        }
        return false;
    }

    if (!ext.empty()) {
        return false;
    }

    if (ParseBinaryStl(bytes, mesh, true)) {
        format = L"STL Binary";
        return true;
    }
    if (ParseAsciiStl(bytes, mesh)) {
        format = L"STL ASCII";
        return true;
    }
    if (ParseObj(bytes, mesh)) {
        format = L"OBJ";
        return true;
    }
    return false;
}

std::optional<HBITMAP> CreateArgbBitmap(UINT width, UINT height, const std::vector<uint32_t>& pixels) {
    if (width == 0 || height == 0 || pixels.size() != static_cast<size_t>(width) * height) {
        return std::nullopt;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return std::nullopt;
    }

    std::memcpy(bits, pixels.data(), pixels.size() * sizeof(uint32_t));
    return bitmap;
}

uint32_t MakeBgra(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return static_cast<uint32_t>(b) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void RasterizeTriangle(
    const ProjectedTriangle& tri,
    int width,
    int height,
    std::vector<float>& zBuffer,
    std::vector<uint32_t>& pixels) {
    const float minXf = std::floor(std::min({tri.x0, tri.x1, tri.x2}));
    const float maxXf = std::ceil(std::max({tri.x0, tri.x1, tri.x2}));
    const float minYf = std::floor(std::min({tri.y0, tri.y1, tri.y2}));
    const float maxYf = std::ceil(std::max({tri.y0, tri.y1, tri.y2}));

    const int minX = std::clamp(static_cast<int>(minXf), 0, width - 1);
    const int maxX = std::clamp(static_cast<int>(maxXf), 0, width - 1);
    const int minY = std::clamp(static_cast<int>(minYf), 0, height - 1);
    const int maxY = std::clamp(static_cast<int>(maxYf), 0, height - 1);
    if (minX > maxX || minY > maxY) {
        return;
    }

    const float denom =
        ((tri.y1 - tri.y2) * (tri.x0 - tri.x2)) +
        ((tri.x2 - tri.x1) * (tri.y0 - tri.y2));
    if (std::fabs(denom) <= 1.0e-8f) {
        return;
    }

    const uint8_t baseR = 0xD7;
    const uint8_t baseG = 0x19;
    const uint8_t baseB = 0x20;
    const float shade = std::clamp(tri.shade, 0.18f, 1.0f);
    const uint32_t color = MakeBgra(
        static_cast<uint8_t>(std::clamp(baseR * shade, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(baseG * shade, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(baseB * shade, 0.0f, 255.0f)),
        255);

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float w0 =
                (((tri.y1 - tri.y2) * (px - tri.x2)) +
                 ((tri.x2 - tri.x1) * (py - tri.y2))) / denom;
            const float w1 =
                (((tri.y2 - tri.y0) * (px - tri.x2)) +
                 ((tri.x0 - tri.x2) * (py - tri.y2))) / denom;
            const float w2 = 1.0f - w0 - w1;
            constexpr float kEpsilon = -1.0e-5f;
            if (w0 < kEpsilon || w1 < kEpsilon || w2 < kEpsilon) {
                continue;
            }

            const float z = (w0 * tri.z0) + (w1 * tri.z1) + (w2 * tri.z2);
            const size_t index = static_cast<size_t>(y) * width + x;
            if (z < zBuffer[index]) {
                zBuffer[index] = z;
                pixels[index] = color;
            }
        }
    }
}

std::vector<uint32_t> Downsample2x(const std::vector<uint32_t>& src, UINT dstWidth, UINT dstHeight) {
    const UINT srcWidth = dstWidth * kSupersample;
    std::vector<uint32_t> dst(static_cast<size_t>(dstWidth) * dstHeight, 0);
    for (UINT y = 0; y < dstHeight; ++y) {
        for (UINT x = 0; x < dstWidth; ++x) {
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t a = 0;
            for (UINT yy = 0; yy < kSupersample; ++yy) {
                for (UINT xx = 0; xx < kSupersample; ++xx) {
                    const uint32_t p = src[(static_cast<size_t>(y * kSupersample + yy) * srcWidth) + (x * kSupersample + xx)];
                    b += p & 0xFF;
                    g += (p >> 8) & 0xFF;
                    r += (p >> 16) & 0xFF;
                    a += (p >> 24) & 0xFF;
                }
            }
            b /= 4;
            g /= 4;
            r /= 4;
            a /= 4;
            if (a > 0 && a < 255) {
                const float alphaScale = static_cast<float>(a) / 255.0f;
                r = static_cast<uint32_t>(std::round(r * alphaScale));
                g = static_cast<uint32_t>(std::round(g * alphaScale));
                b = static_cast<uint32_t>(std::round(b * alphaScale));
            }
            dst[static_cast<size_t>(y) * dstWidth + x] = MakeBgra(
                static_cast<uint8_t>(std::min<uint32_t>(r, 255)),
                static_cast<uint8_t>(std::min<uint32_t>(g, 255)),
                static_cast<uint8_t>(std::min<uint32_t>(b, 255)),
                static_cast<uint8_t>(std::min<uint32_t>(a, 255)));
        }
    }
    return dst;
}

std::optional<HBITMAP> RenderMeshToBitmap(const Mesh& mesh, UINT requestedMaxSize) {
    if (mesh.triangles.empty()) {
        return std::nullopt;
    }

    UINT size = requestedMaxSize == 0 ? kDefaultPreviewSize : requestedMaxSize;
    size = std::clamp(size, 16u, kMaxRenderSize);
    const UINT renderWidth = size * kSupersample;
    const UINT renderHeight = size * kSupersample;

    Vec3 minBounds = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    Vec3 maxBounds = {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };

    auto include = [&](Vec3 v) {
        minBounds.x = std::min(minBounds.x, v.x);
        minBounds.y = std::min(minBounds.y, v.y);
        minBounds.z = std::min(minBounds.z, v.z);
        maxBounds.x = std::max(maxBounds.x, v.x);
        maxBounds.y = std::max(maxBounds.y, v.y);
        maxBounds.z = std::max(maxBounds.z, v.z);
    };

    for (const Triangle& tri : mesh.triangles) {
        include(tri.a);
        include(tri.b);
        include(tri.c);
    }

    const Vec3 center = Mul(Add(minBounds, maxBounds), 0.5f);
    const Vec3 view = Normalize({1.0f, -1.0f, 0.72f}).value();
    const Vec3 worldUp = {0.0f, 0.0f, 1.0f};
    Vec3 right = Normalize(Cross(worldUp, view)).value_or(Vec3{1.0f, 0.0f, 0.0f});
    Vec3 up = Normalize(Cross(view, right)).value_or(Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 lightDir = Normalize({-0.35f, -0.45f, 0.82f}).value();

    float minU = std::numeric_limits<float>::max();
    float maxU = std::numeric_limits<float>::lowest();
    float minV = std::numeric_limits<float>::max();
    float maxV = std::numeric_limits<float>::lowest();

    auto boundsProject = [&](Vec3 p) {
        const Vec3 local = Sub(p, center);
        const float u = Dot(local, right);
        const float v = Dot(local, up);
        minU = std::min(minU, u);
        maxU = std::max(maxU, u);
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
    };

    for (const Triangle& tri : mesh.triangles) {
        boundsProject(tri.a);
        boundsProject(tri.b);
        boundsProject(tri.c);
    }

    const float spanU = std::max(maxU - minU, 1.0e-4f);
    const float spanV = std::max(maxV - minV, 1.0e-4f);
    const float padding = 0.88f;
    const float scale = padding * std::min(
        static_cast<float>(renderWidth - 1) / spanU,
        static_cast<float>(renderHeight - 1) / spanV);
    const float centerU = (minU + maxU) * 0.5f;
    const float centerV = (minV + maxV) * 0.5f;

    std::vector<float> zBuffer(static_cast<size_t>(renderWidth) * renderHeight, std::numeric_limits<float>::max());
    std::vector<uint32_t> hiPixels(static_cast<size_t>(renderWidth) * renderHeight, 0);

    auto project = [&](Vec3 p, float& x, float& y, float& z) {
        const Vec3 local = Sub(p, center);
        const float u = Dot(local, right);
        const float v = Dot(local, up);
        z = -Dot(local, view);
        x = ((u - centerU) * scale) + (static_cast<float>(renderWidth) * 0.5f);
        y = (static_cast<float>(renderHeight) * 0.5f) - ((v - centerV) * scale);
    };

    for (const Triangle& tri : mesh.triangles) {
        const auto normal = Normalize(Cross(Sub(tri.b, tri.a), Sub(tri.c, tri.a)));
        if (!normal.has_value()) {
            continue;
        }
        const float diffuse = std::fabs(Dot(normal.value(), lightDir));
        const float facing = 0.15f + (0.15f * std::fabs(Dot(normal.value(), view)));
        const float shade = std::clamp(0.26f + (0.62f * diffuse) + facing, 0.18f, 1.0f);

        ProjectedTriangle projected;
        project(tri.a, projected.x0, projected.y0, projected.z0);
        project(tri.b, projected.x1, projected.y1, projected.z1);
        project(tri.c, projected.x2, projected.y2, projected.z2);
        projected.shade = shade;
        RasterizeTriangle(projected, static_cast<int>(renderWidth), static_cast<int>(renderHeight), zBuffer, hiPixels);
    }

    std::vector<uint32_t> pixels = Downsample2x(hiPixels, size, size);
    return CreateArgbBitmap(size, size, pixels);
}

void PopulateMeshResult(MeshPreviewResult& result, const Mesh& mesh, std::wstring_view format) {
    result.isMesh = true;
    result.format = std::wstring(format);
    result.triangles = mesh.triangles.size();
    result.polygons = mesh.triangles.size();
    result.faces = mesh.faceCount == 0 ? mesh.triangles.size() : mesh.faceCount;
    result.vertices = mesh.vertexCount;

    const Bounds bounds = CalculateBounds(mesh.triangles);
    if (bounds.valid) {
        result.hasDimensions = true;
        result.dimensionX = static_cast<double>(bounds.max.x) - static_cast<double>(bounds.min.x);
        result.dimensionY = static_cast<double>(bounds.max.y) - static_cast<double>(bounds.min.y);
        result.dimensionZ = static_cast<double>(bounds.max.z) - static_cast<double>(bounds.min.z);
    }
}

} // namespace

bool IsMeshPreviewExtension(std::wstring_view pathOrExtension) {
    const std::wstring ext = ExtensionFromHint(pathOrExtension);
    if (!ext.empty()) {
        return ext == L".stl" || ext == L".obj";
    }
    const std::wstring normalized = ToLower(pathOrExtension);
    return normalized == L".stl" || normalized == L".obj";
}

MeshPreviewResult AnalyzeMeshPreview(
    const std::vector<uint8_t>& fileBytes,
    std::wstring_view pathHint) {
    MeshPreviewResult result;
    Mesh mesh;
    std::wstring format;
    if (!ParseMesh(fileBytes, pathHint, mesh, format)) {
        result.status = L"mesh parse failed";
        return result;
    }

    PopulateMeshResult(result, mesh, format);
    result.status = L"ok";
    return result;
}

MeshPreviewResult RenderMeshPreviewToHBitmap(
    const std::vector<uint8_t>& fileBytes,
    std::wstring_view pathHint,
    UINT requestedMaxSize,
    HBITMAP* outBitmap) {
    MeshPreviewResult result;
    if (outBitmap) {
        *outBitmap = nullptr;
    }
    if (!outBitmap) {
        result.status = L"missing output bitmap pointer";
        return result;
    }

    Mesh mesh;
    std::wstring format;
    if (!ParseMesh(fileBytes, pathHint, mesh, format)) {
        result.status = L"mesh parse failed";
        return result;
    }

    PopulateMeshResult(result, mesh, format);
    const auto bitmap = RenderMeshToBitmap(mesh, requestedMaxSize);
    if (!bitmap.has_value()) {
        result.status = L"mesh render failed";
        return result;
    }

    *outBitmap = bitmap.value();
    result.rendered = true;
    result.status = L"ok";
    return result;
}
