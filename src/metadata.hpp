#pragma once

#include <windows.h>
#include <objidl.h>

#include <cstdint>
#include <string>
#include <vector>

struct ApiumMetadata {
    bool isApium = false;
    bool isZipContainer = false;

    std::wstring printer;
    std::wstring projectVersion;
    std::wstring materialType;
    std::wstring nozzleDiameter;
    std::wstring nozzleTemperature;
    std::wstring bedTemperature;
    std::wstring layerHeight;
    std::wstring perimeters;
    std::wstring infillPattern;
    std::wstring infillDensity;
    std::wstring printTime;

    std::vector<uint8_t> thumbnailBytes;

    struct ParseDiagnostics {
        std::wstring container;
        std::wstring configPartStatus;
        std::wstring configPartPath;
        std::wstring decode;
        std::wstring apiumDetect;
        std::wstring missingFields;
    } diagnostics;
};

bool ReadStreamFully(IStream* stream, std::vector<uint8_t>& outBytes);
bool ReadFileFully(const wchar_t* filePath, std::vector<uint8_t>& outBytes);
bool IsZipContainer(const std::vector<uint8_t>& bytes);
bool LoadFallbackPng(HINSTANCE module, std::vector<uint8_t>& outBytes);
bool DecodePngToHBitmap(const std::vector<uint8_t>& pngBytes, UINT requestedMaxSize, HBITMAP* outBitmap);

ApiumMetadata ParseApiumMetadata(const std::vector<uint8_t>& fileBytes);
