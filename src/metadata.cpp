#include "metadata.hpp"

#include <msopc.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "resource.hpp"
extern "C" {
#include "miniz.h"
}

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kTailScanBytes = 8ULL * 1024ULL * 1024ULL;
constexpr size_t kInitialTailBytes = 64ULL * 1024ULL;
constexpr size_t kHeaderScanBytes = 1ULL * 1024ULL * 1024ULL;
constexpr std::wstring_view kConfigPartPath = L"/Metadata/Slic3r_PE.config";

const std::array<std::wstring_view, 8> kThumbnailA3dCandidates = {
    L"/Metadata/thumbnail.png",
    L"/Metadata/Thumbnail.png",
    L"/Metadata/thumbnail.jpg",
    L"/Metadata/thumbnail.jpeg",
    L"/Metadata/plate_1.png",
    L"/Metadata/Plate_1.png",
    L"/Thumbnails/thumbnail.png",
    L"/Thumbnails/Thumbnail.png",
};

struct ParsedMapResult {
    std::unordered_map<std::string, std::string> keyValues;
    bool hasApiumGeneratorMarker = false;
    bool hasApiumConfigBlock = false;
};

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string original;
};

void DebugLog(std::string_view message) {
    std::string output = "[ApiumPreview] ";
    output.append(message.begin(), message.end());
    output.push_back('\n');
    OutputDebugStringA(output.c_str());
}

std::wstring TrimWide(std::wstring value) {
    const auto notSpace = [](wchar_t ch) { return iswspace(ch) == 0; };
    const auto begin = std::find_if(value.begin(), value.end(), notSpace);
    if (begin == value.end()) {
        return L"";
    }
    const auto end = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    return std::wstring(begin, end);
}

std::string TrimAscii(std::string value) {
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    const auto begin = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto end = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    return begin < end ? std::string(begin, end) : std::string{};
}

std::string ToLowerAscii(std::string_view text) {
    std::string out(text.begin(), text.end());
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string NormalizeZipPathAscii(std::string path) {
    for (char& ch : path) {
        if (ch == '\\') {
            ch = '/';
        }
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    return path;
}

std::string WidePathToAscii(std::wstring_view path) {
    std::string out;
    out.reserve(path.size());
    for (wchar_t ch : path) {
        out.push_back((ch <= 0x7F) ? static_cast<char>(ch) : '?');
    }
    return out;
}

struct ZipEntryRecord {
    mz_uint index = 0;
    std::string path;
    std::string normalizedPath;
};

bool BuildZipEntryIndex(
    const std::vector<uint8_t>& fileBytes,
    mz_zip_archive& zipArchive,
    std::vector<ZipEntryRecord>& entries) {
    entries.clear();
    mz_zip_zero_struct(&zipArchive);

    if (!mz_zip_reader_init_mem(&zipArchive, fileBytes.data(), fileBytes.size(), 0)) {
        return false;
    }

    const mz_uint fileCount = mz_zip_reader_get_num_files(&zipArchive);
    entries.reserve(fileCount);
    for (mz_uint i = 0; i < fileCount; ++i) {
        mz_zip_archive_file_stat stat = {};
        if (!mz_zip_reader_file_stat(&zipArchive, i, &stat)) {
            continue;
        }
        if (stat.m_is_directory || !stat.m_is_supported) {
            continue;
        }
        ZipEntryRecord record;
        record.index = i;
        record.path = stat.m_filename ? stat.m_filename : "";
        record.normalizedPath = NormalizeZipPathAscii(record.path);
        entries.push_back(std::move(record));
    }
    return true;
}

void EndZipReader(mz_zip_archive& zipArchive) {
    if (zipArchive.m_pState) {
        (void)mz_zip_reader_end(&zipArchive);
    }
    mz_zip_zero_struct(&zipArchive);
}

bool ExtractZipEntryToBytes(
    mz_zip_archive& zipArchive,
    mz_uint fileIndex,
    std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    size_t size = 0;
    void* heapData = mz_zip_reader_extract_to_heap(&zipArchive, fileIndex, &size, 0);
    if (!heapData || size == 0) {
        if (heapData) {
            mz_free(heapData);
        }
        return false;
    }
    const auto* begin = static_cast<const uint8_t*>(heapData);
    outBytes.assign(begin, begin + size);
    mz_free(heapData);
    return true;
}

bool ExtractZipEntryByNormalizedPath(
    mz_zip_archive& zipArchive,
    const std::vector<ZipEntryRecord>& entries,
    std::string_view normalizedPath,
    std::vector<uint8_t>& outBytes,
    std::string* outMatchedPath) {
    outBytes.clear();
    if (outMatchedPath) {
        outMatchedPath->clear();
    }

    const std::string target = NormalizeZipPathAscii(std::string(normalizedPath));
    for (const ZipEntryRecord& entry : entries) {
        if (entry.normalizedPath != target) {
            continue;
        }
        if (!ExtractZipEntryToBytes(zipArchive, entry.index, outBytes)) {
            return false;
        }
        if (outMatchedPath) {
            *outMatchedPath = entry.path;
        }
        return true;
    }
    return false;
}

bool IsLikelyThumbnailPathAscii(std::string_view normalizedPath) {
    if (normalizedPath.find("metadata/") == std::string::npos &&
        normalizedPath.find("thumbnails/") == std::string::npos) {
        return false;
    }
    const bool hasImageExt =
        (normalizedPath.size() >= 4 &&
         (normalizedPath.ends_with(".png") || normalizedPath.ends_with(".jpg"))) ||
        (normalizedPath.size() >= 5 && normalizedPath.ends_with(".jpeg"));
    if (!hasImageExt) {
        return false;
    }
    return normalizedPath.find("thumbnail") != std::string::npos ||
           normalizedPath.find("plate_1") != std::string::npos;
}

std::wstring ToLowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

bool EndsWithWide(std::wstring_view value, std::wstring_view suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

std::wstring NormalizeOpcPath(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'\\') {
            ch = L'/';
        }
        ch = static_cast<wchar_t>(towlower(ch));
    }
    while (!path.empty() && path.front() == L'/') {
        path.erase(path.begin());
    }
    return path;
}

std::wstring Utf8OrAnsiToWide(const std::string& text) {
    if (text.empty()) {
        return L"";
    }

    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (len <= 0) {
        codePage = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(codePage, flags, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    }
    if (len <= 0) {
        return L"";
    }

    std::wstring out(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(codePage, flags, text.c_str(), static_cast<int>(text.size()), out.data(), len) <= 0) {
        return L"";
    }
    return out;
}

bool ReadPartByPath(IOpcFactory* factory, IOpcPartSet* partSet, std::wstring_view path, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    if (!factory || !partSet) {
        return false;
    }

    ComPtr<IOpcPartUri> uri;
    if (FAILED(factory->CreatePartUri(path.data(), &uri))) {
        return false;
    }

    ComPtr<IOpcPart> part;
    if (FAILED(partSet->GetPart(uri.Get(), &part))) {
        return false;
    }

    ComPtr<IStream> content;
    if (FAILED(part->GetContentStream(&content))) {
        return false;
    }

    return ReadStreamFully(content.Get(), outBytes);
}

bool ReadPartContent(IOpcPart* part, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    if (!part) {
        return false;
    }
    ComPtr<IStream> content;
    if (FAILED(part->GetContentStream(&content))) {
        return false;
    }
    return ReadStreamFully(content.Get(), outBytes);
}

bool GetPartPath(IOpcPart* part, std::wstring& outPath) {
    outPath.clear();
    if (!part) {
        return false;
    }

    ComPtr<IOpcPartUri> partUri;
    if (FAILED(part->GetName(&partUri))) {
        return false;
    }

    BSTR pathBstr = nullptr;
    if (FAILED(partUri->GetPath(&pathBstr)) || !pathBstr) {
        return false;
    }
    outPath.assign(pathBstr, SysStringLen(pathBstr));
    SysFreeString(pathBstr);
    return !outPath.empty();
}

bool ReadPartByPathCaseInsensitive(
    IOpcPartSet* partSet,
    std::wstring_view targetPathLower,
    std::vector<uint8_t>& outBytes,
    std::wstring* outMatchedPath) {
    outBytes.clear();
    if (outMatchedPath) {
        outMatchedPath->clear();
    }
    if (!partSet) {
        return false;
    }

    const std::wstring normalizedTarget = NormalizeOpcPath(std::wstring(targetPathLower));

    ComPtr<IOpcPartEnumerator> enumerator;
    if (FAILED(partSet->GetEnumerator(&enumerator))) {
        return false;
    }

    BOOL hasCurrent = FALSE;
    while (SUCCEEDED(enumerator->MoveNext(&hasCurrent)) && hasCurrent) {
        ComPtr<IOpcPart> part;
        if (FAILED(enumerator->GetCurrent(&part)) || !part) {
            continue;
        }
        std::wstring partPath;
        if (!GetPartPath(part.Get(), partPath)) {
            continue;
        }
        if (NormalizeOpcPath(partPath) == normalizedTarget) {
            if (outMatchedPath) {
                *outMatchedPath = partPath;
            }
            return ReadPartContent(part.Get(), outBytes);
        }
    }
    return false;
}

bool ReadConfigPartWithFallback(IOpcFactory* factory, IOpcPartSet* partSet, std::vector<uint8_t>& outBytes, std::wstring* outMatchedPath) {
    if (outMatchedPath) {
        outMatchedPath->clear();
    }
    if (ReadPartByPath(factory, partSet, kConfigPartPath, outBytes) && !outBytes.empty()) {
        if (outMatchedPath) {
            *outMatchedPath = std::wstring(kConfigPartPath);
        }
        return true;
    }
    const std::wstring configNoLeadingSlash = std::wstring(kConfigPartPath).substr(1);
    if (ReadPartByPath(factory, partSet, configNoLeadingSlash, outBytes) && !outBytes.empty()) {
        if (outMatchedPath) {
            *outMatchedPath = configNoLeadingSlash;
        }
        return true;
    }
    return ReadPartByPathCaseInsensitive(partSet, ToLowerWide(std::wstring(kConfigPartPath)), outBytes, outMatchedPath);
}

bool IsLikelyThumbnailPath(std::wstring_view pathLower) {
    if (pathLower.find(L"/metadata/") == std::wstring::npos &&
        pathLower.find(L"/thumbnails/") == std::wstring::npos) {
        return false;
    }
    if (!(EndsWithWide(pathLower, L".png") || EndsWithWide(pathLower, L".jpg") || EndsWithWide(pathLower, L".jpeg"))) {
        return false;
    }
    return pathLower.find(L"thumbnail") != std::wstring::npos || pathLower.find(L"plate_1") != std::wstring::npos;
}

bool ReadThumbnailPartWithFallback(IOpcFactory* factory, IOpcPartSet* partSet, std::vector<uint8_t>& outBytes, std::wstring* outMatchedPath) {
    if (outMatchedPath) {
        outMatchedPath->clear();
    }
    for (const std::wstring_view path : kThumbnailA3dCandidates) {
        if (ReadPartByPath(factory, partSet, path, outBytes) && !outBytes.empty()) {
            if (outMatchedPath) {
                *outMatchedPath = std::wstring(path);
            }
            return true;
        }
    }

    if (!partSet) {
        return false;
    }

    ComPtr<IOpcPartEnumerator> enumerator;
    if (FAILED(partSet->GetEnumerator(&enumerator))) {
        return false;
    }

    BOOL hasCurrent = FALSE;
    while (SUCCEEDED(enumerator->MoveNext(&hasCurrent)) && hasCurrent) {
        ComPtr<IOpcPart> part;
        if (FAILED(enumerator->GetCurrent(&part)) || !part) {
            continue;
        }
        std::wstring partPath;
        if (!GetPartPath(part.Get(), partPath)) {
            continue;
        }
        const std::wstring lowerPath = ToLowerWide(partPath);
        if (!IsLikelyThumbnailPath(lowerPath)) {
            continue;
        }
        if (ReadPartContent(part.Get(), outBytes) && !outBytes.empty()) {
            if (outMatchedPath) {
                *outMatchedPath = partPath;
            }
            return true;
        }
    }
    return false;
}

bool WideToUtf8(const std::wstring& text, std::string& outText) {
    outText.clear();
    if (text.empty()) {
        return true;
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return false;
    }
    outText.resize(static_cast<size_t>(needed));
    const int converted = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), outText.data(), needed, nullptr, nullptr);
    return converted == needed;
}

bool DecodeUtf16BytesToUtf8(const std::vector<uint8_t>& bytes, bool littleEndian, size_t startOffset, std::string& outText) {
    outText.clear();
    if (bytes.size() <= startOffset + 1) {
        return false;
    }

    const size_t usable = ((bytes.size() - startOffset) / 2) * 2;
    if (usable == 0) {
        return false;
    }

    std::wstring wide;
    wide.reserve(usable / 2);
    for (size_t i = startOffset; i + 1 < startOffset + usable; i += 2) {
        wchar_t ch = 0;
        if (littleEndian) {
            ch = static_cast<wchar_t>(static_cast<uint16_t>(bytes[i]) | (static_cast<uint16_t>(bytes[i + 1]) << 8));
        } else {
            ch = static_cast<wchar_t>(static_cast<uint16_t>(bytes[i + 1]) | (static_cast<uint16_t>(bytes[i]) << 8));
        }
        wide.push_back(ch);
    }
    return WideToUtf8(wide, outText);
}

bool DetectLikelyUtf16(const std::vector<uint8_t>& bytes, bool& littleEndianOut) {
    littleEndianOut = true;
    const size_t sample = std::min<size_t>(bytes.size() & ~static_cast<size_t>(1), 2048);
    if (sample < 8) {
        return false;
    }

    size_t evenZero = 0;
    size_t oddZero = 0;
    for (size_t i = 0; i + 1 < sample; i += 2) {
        if (bytes[i] == 0) {
            ++evenZero;
        }
        if (bytes[i + 1] == 0) {
            ++oddZero;
        }
    }

    const size_t pairs = sample / 2;
    if (oddZero * 4 >= pairs) {
        littleEndianOut = true;
        return true;
    }
    if (evenZero * 4 >= pairs) {
        littleEndianOut = false;
        return true;
    }
    return false;
}

bool DecodeConfigBytesToText(const std::vector<uint8_t>& bytes, std::string& outText, std::wstring* outDecodeKind) {
    outText.clear();
    if (outDecodeKind) {
        *outDecodeKind = L"raw";
    }
    if (bytes.empty()) {
        return false;
    }

    if (bytes.size() >= 3 &&
        bytes[0] == 0xEF &&
        bytes[1] == 0xBB &&
        bytes[2] == 0xBF) {
        outText.assign(reinterpret_cast<const char*>(bytes.data() + 3), bytes.size() - 3);
        if (outDecodeKind) {
            *outDecodeKind = L"UTF8";
        }
        return true;
    }

    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        const bool ok = DecodeUtf16BytesToUtf8(bytes, true, 2, outText);
        if (ok && outDecodeKind) {
            *outDecodeKind = L"UTF16LE";
        }
        return ok;
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        const bool ok = DecodeUtf16BytesToUtf8(bytes, false, 2, outText);
        if (ok && outDecodeKind) {
            *outDecodeKind = L"UTF16BE";
        }
        return ok;
    }

    bool littleEndian = true;
    if (DetectLikelyUtf16(bytes, littleEndian)) {
        if (DecodeUtf16BytesToUtf8(bytes, littleEndian, 0, outText)) {
            if (outDecodeKind) {
                *outDecodeKind = littleEndian ? L"UTF16LE" : L"UTF16BE";
            }
            return true;
        }
    }

    outText.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (outDecodeKind) {
        *outDecodeKind = L"raw";
    }
    return true;
}

bool LooksLikeApiumConfigByKeys(const std::unordered_map<std::string, std::string>& keyValues) {
    if (keyValues.empty()) {
        return false;
    }

    const bool hasApiumSpecificKey = std::any_of(
        keyValues.begin(), keyValues.end(), [](const auto& entry) { return entry.first.rfind("apium_", 0) == 0; });
    if (!hasApiumSpecificKey) {
        return false;
    }

    const std::array<const char*, 7> importantKeys = {
        "printer_model",
        "filament_type",
        "nozzle_diameter",
        "temperature",
        "layer_height",
        "perimeters",
        "fill_density"};
    return std::any_of(importantKeys.begin(), importantKeys.end(), [&](const char* key) {
        return keyValues.find(ToLowerAscii(key)) != keyValues.end();
    });
}

bool CreateReadOnlyStreamOnBytes(const std::vector<uint8_t>& bytes, ComPtr<IStream>& outStream) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem) {
        return false;
    }

    void* data = GlobalLock(mem);
    if (!data) {
        GlobalFree(mem);
        return false;
    }
    if (!bytes.empty()) {
        std::memcpy(data, bytes.data(), bytes.size());
    }
    GlobalUnlock(mem);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
        GlobalFree(mem);
        return false;
    }

    outStream.Attach(stream);
    LARGE_INTEGER zero = {};
    (void)outStream->Seek(zero, STREAM_SEEK_SET, nullptr);
    return true;
}

bool BuildOpcPackage(const std::vector<uint8_t>& fileBytes, ComPtr<IOpcFactory>& factoryOut, ComPtr<IOpcPartSet>& partSetOut) {
    factoryOut.Reset();
    partSetOut.Reset();

    ComPtr<IStream> packageStream;
    if (!CreateReadOnlyStreamOnBytes(fileBytes, packageStream)) {
        return false;
    }

    ComPtr<IOpcFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_OpcFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IOpcPackage> package;
    if (FAILED(factory->ReadPackageFromStream(packageStream.Get(), OPC_READ_DEFAULT, &package))) {
        return false;
    }

    ComPtr<IOpcPartSet> partSet;
    if (FAILED(package->GetPartSet(&partSet))) {
        return false;
    }

    factoryOut = factory;
    partSetOut = partSet;
    return true;
}

bool ParseSemVerTag(std::string tag, SemVer& out) {
    if (tag.empty()) {
        return false;
    }
    if (tag.front() == 'v' || tag.front() == 'V') {
        tag.erase(tag.begin());
    }

    int values[3] = {0, 0, 0};
    size_t cursor = 0;
    for (int i = 0; i < 3; ++i) {
        if (cursor >= tag.size() || !std::isdigit(static_cast<unsigned char>(tag[cursor]))) {
            return false;
        }
        int value = 0;
        while (cursor < tag.size() && std::isdigit(static_cast<unsigned char>(tag[cursor]))) {
            value = (value * 10) + (tag[cursor] - '0');
            ++cursor;
        }
        values[i] = value;
        if (i < 2) {
            if (cursor >= tag.size() || tag[cursor] != '.') {
                if (i == 0) {
                    return false;
                }
                break;
            }
            ++cursor;
        }
    }

    if (cursor < tag.size()) {
        const char suffix = tag[cursor];
        if (suffix != '-' && suffix != '+' && suffix != '_') {
            return false;
        }
    }

    out.major = values[0];
    out.minor = values[1];
    out.patch = values[2];
    out.original = tag;
    return true;
}

bool IsSemVerGreater(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) {
        return a.major > b.major;
    }
    if (a.minor != b.minor) {
        return a.minor > b.minor;
    }
    if (a.patch != b.patch) {
        return a.patch > b.patch;
    }
    return false;
}

std::optional<std::string> GetHighestTagFromGitParts(
    IOpcFactory* factory,
    IOpcPartSet* partSet,
    bool* gitFound) {
    if (gitFound) {
        *gitFound = false;
    }
    if (!factory || !partSet) {
        return std::nullopt;
    }

    ComPtr<IOpcPartEnumerator> enumerator;
    if (FAILED(partSet->GetEnumerator(&enumerator))) {
        return std::nullopt;
    }

    bool hasSemVer = false;
    SemVer best;
    bool sawGit = false;

    BOOL hasCurrent = FALSE;
    while (SUCCEEDED(enumerator->MoveNext(&hasCurrent)) && hasCurrent) {
        ComPtr<IOpcPart> part;
        if (FAILED(enumerator->GetCurrent(&part))) {
            continue;
        }

        ComPtr<IOpcPartUri> partUri;
        if (FAILED(part->GetName(&partUri))) {
            continue;
        }

        BSTR pathBstr = nullptr;
        if (FAILED(partUri->GetPath(&pathBstr)) || !pathBstr) {
            continue;
        }
        std::wstring path(pathBstr, SysStringLen(pathBstr));
        SysFreeString(pathBstr);

        std::wstring pathLower = path;
        std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        if (pathLower.find(L"/metadata/.git/") != 0) {
            continue;
        }
        sawGit = true;

        constexpr std::wstring_view kTagPrefix = L"/metadata/.git/refs/tags/";
        if (pathLower.find(kTagPrefix) == 0 && path.size() > kTagPrefix.size()) {
            const std::string tag = WidePathToAscii(std::wstring_view(path).substr(kTagPrefix.size()));
            SemVer semver;
            if (ParseSemVerTag(tag, semver)) {
                if (!hasSemVer || IsSemVerGreater(semver, best)) {
                    best = semver;
                    hasSemVer = true;
                }
            }
        }

        if (pathLower == L"/metadata/.git/packed-refs") {
            std::vector<uint8_t> packedBytes;
            ComPtr<IStream> packedStream;
            if (SUCCEEDED(part->GetContentStream(&packedStream)) && ReadStreamFully(packedStream.Get(), packedBytes) && !packedBytes.empty()) {
                std::string text(reinterpret_cast<const char*>(packedBytes.data()), packedBytes.size());
                size_t start = 0;
                while (start < text.size()) {
                    size_t end = text.find('\n', start);
                    if (end == std::string::npos) {
                        end = text.size();
                    }
                    std::string line = TrimAscii(text.substr(start, end - start));
                    if (!line.empty() && line[0] != '#' && line[0] != '^') {
                        const size_t space = line.find(' ');
                        if (space != std::string::npos) {
                            std::string refName = TrimAscii(line.substr(space + 1));
                            constexpr std::string_view packedPrefix = "refs/tags/";
                            if (refName.rfind(packedPrefix, 0) == 0) {
                                std::string tag = refName.substr(packedPrefix.size());
                                SemVer semver;
                                if (ParseSemVerTag(tag, semver)) {
                                    if (!hasSemVer || IsSemVerGreater(semver, best)) {
                                        best = semver;
                                        hasSemVer = true;
                                    }
                                }
                            }
                        }
                    }
                    start = (end == text.size()) ? end : end + 1;
                }
            }
        }
    }

    if (gitFound) {
        *gitFound = sawGit;
    }
    if (!hasSemVer) {
        return std::nullopt;
    }
    return best.original;
}

std::optional<std::string> GetHighestTagFromZipEntries(
    mz_zip_archive& zipArchive,
    const std::vector<ZipEntryRecord>& entries,
    bool* gitFound) {
    if (gitFound) {
        *gitFound = false;
    }

    bool hasSemVer = false;
    SemVer best;
    bool sawGit = false;

    for (const ZipEntryRecord& entry : entries) {
        const std::string& pathLower = entry.normalizedPath;
        if (pathLower.rfind("metadata/.git/", 0) != 0) {
            continue;
        }
        sawGit = true;

        constexpr std::string_view kTagPrefix = "metadata/.git/refs/tags/";
        if (pathLower.rfind(kTagPrefix, 0) == 0 && pathLower.size() > kTagPrefix.size()) {
            std::string tag = pathLower.substr(kTagPrefix.size());
            SemVer semver;
            if (ParseSemVerTag(tag, semver)) {
                if (!hasSemVer || IsSemVerGreater(semver, best)) {
                    best = semver;
                    hasSemVer = true;
                }
            }
            continue;
        }

        if (pathLower == "metadata/.git/packed-refs") {
            std::vector<uint8_t> packedBytes;
            if (!ExtractZipEntryToBytes(zipArchive, entry.index, packedBytes) || packedBytes.empty()) {
                continue;
            }

            std::string text(reinterpret_cast<const char*>(packedBytes.data()), packedBytes.size());
            size_t start = 0;
            while (start < text.size()) {
                size_t end = text.find('\n', start);
                if (end == std::string::npos) {
                    end = text.size();
                }
                std::string line = TrimAscii(text.substr(start, end - start));
                if (!line.empty() && line[0] != '#' && line[0] != '^') {
                    const size_t space = line.find(' ');
                    if (space != std::string::npos) {
                        std::string refName = TrimAscii(line.substr(space + 1));
                        constexpr std::string_view packedPrefix = "refs/tags/";
                        if (refName.rfind(packedPrefix, 0) == 0) {
                            std::string tag = refName.substr(packedPrefix.size());
                            SemVer semver;
                            if (ParseSemVerTag(tag, semver)) {
                                if (!hasSemVer || IsSemVerGreater(semver, best)) {
                                    best = semver;
                                    hasSemVer = true;
                                }
                            }
                        }
                    }
                }
                start = (end == text.size()) ? end : end + 1;
            }
        }
    }

    if (gitFound) {
        *gitFound = sawGit;
    }
    if (!hasSemVer) {
        return std::nullopt;
    }
    return best.original;
}

std::string TakeFirstCsvValue(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return TrimAscii(value);
    }
    return TrimAscii(value.substr(0, comma));
}

std::string PickValue(
    const std::unordered_map<std::string, std::string>& keyValues,
    const std::initializer_list<const char*>& keys) {
    for (const char* key : keys) {
        const auto it = keyValues.find(ToLowerAscii(key));
        if (it != keyValues.end()) {
            std::string candidate = TakeFirstCsvValue(it->second);
            if (!candidate.empty()) {
                return candidate;
            }
        }
    }
    return "";
}

std::string NormalizeNozzleDiameter(const std::unordered_map<std::string, std::string>& keyValues) {
    std::string value = PickValue(keyValues, {"nozzle_diameter"});
    if (!value.empty()) {
        return value;
    }

    value = PickValue(keyValues, {"printer_variant"});
    if (value.empty()) {
        return "";
    }

    std::string out;
    bool seenDot = false;
    for (char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            out.push_back(ch);
            continue;
        }
        if (ch == '.' && !seenDot) {
            out.push_back(ch);
            seenDot = true;
            continue;
        }
        if (!out.empty()) {
            break;
        }
    }
    return out;
}

std::wstring ToWideField(const std::string& value) {
    return TrimWide(Utf8OrAnsiToWide(value));
}

void FillMetadataFieldsFromMap(ApiumMetadata& metadata, const std::unordered_map<std::string, std::string>& keyValues) {
    metadata.printer = ToWideField(PickValue(keyValues, {"printer_model", "printer_settings_id", "printer_vendor"}));
    metadata.materialType = ToWideField(PickValue(keyValues, {"filament_type", "filament_settings_id"}));
    metadata.nozzleDiameter = ToWideField(NormalizeNozzleDiameter(keyValues));
    metadata.nozzleTemperature = ToWideField(PickValue(keyValues, {"temperature", "nozzle_temperature", "first_layer_temperature"}));
    metadata.bedTemperature = ToWideField(PickValue(keyValues, {"bed_temperature", "first_layer_bed_temperature"}));
    metadata.layerHeight = ToWideField(PickValue(keyValues, {"layer_height"}));
    metadata.perimeters = ToWideField(PickValue(keyValues, {"perimeters"}));
    metadata.infillPattern = ToWideField(PickValue(keyValues, {"fill_pattern", "infill_pattern"}));
    metadata.infillDensity = ToWideField(PickValue(keyValues, {"fill_density", "infill_density"}));
    metadata.printTime = ToWideField(PickValue(keyValues, {"estimated printing time (normal mode)", "estimated printing time"}));
}

ParsedMapResult ParseKeyValuesFromText(std::string_view text, bool commentOnly) {
    ParsedMapResult result;
    size_t lineStart = 0;
    while (lineStart < text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string_view::npos) {
            lineEnd = text.size();
        }
        std::string line(text.substr(lineStart, lineEnd - lineStart));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::string work = line;
        if (commentOnly) {
            std::string trimmed = TrimAscii(work);
            if (trimmed.empty() || trimmed.front() != ';') {
                lineStart = (lineEnd == text.size()) ? lineEnd : lineEnd + 1;
                continue;
            }
            trimmed.erase(trimmed.begin());
            work = TrimAscii(trimmed);
        } else {
            work = TrimAscii(work);
            // Some slicers store project settings as '; key = value' comment lines
            // inside Slic3r_PE.config. Accept them in non-comment-only mode too.
            if (!work.empty() && work.front() == ';') {
                work.erase(work.begin());
                work = TrimAscii(work);
            }
        }

        if (!work.empty()) {
            std::string lowerWork = ToLowerAscii(work);
            if (lowerWork.find("generated by apiumslicer") != std::string::npos) {
                result.hasApiumGeneratorMarker = true;
            }
            if (lowerWork.find("apiumslicer_config = begin") != std::string::npos) {
                result.hasApiumConfigBlock = true;
            }
        }

        const size_t equalPos = work.find('=');
        if (equalPos != std::string::npos) {
            std::string key = TrimAscii(work.substr(0, equalPos));
            std::string value = TrimAscii(work.substr(equalPos + 1));
            if (!key.empty()) {
                result.keyValues[ToLowerAscii(key)] = value;
            }
        }

        lineStart = (lineEnd == text.size()) ? lineEnd : lineEnd + 1;
    }
    return result;
}

bool DecodeBase64(const std::string& base64Text, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    DWORD binarySize = 0;
    if (!CryptStringToBinaryA(
            base64Text.c_str(),
            static_cast<DWORD>(base64Text.size()),
            CRYPT_STRING_BASE64,
            nullptr,
            &binarySize,
            nullptr,
            nullptr)) {
        return false;
    }
    outBytes.resize(binarySize);
    if (!CryptStringToBinaryA(
            base64Text.c_str(),
            static_cast<DWORD>(base64Text.size()),
            CRYPT_STRING_BASE64,
            outBytes.data(),
            &binarySize,
            nullptr,
            nullptr)) {
        outBytes.clear();
        return false;
    }
    outBytes.resize(binarySize);
    return !outBytes.empty();
}

bool ParseGcodeThumbnailBlock(std::string_view text, size_t scanStart, std::vector<uint8_t>& outPngBytes) {
    constexpr std::string_view kBeginTag = "; thumbnail_PNG begin";
    constexpr std::string_view kEndTag = "; thumbnail_PNG end";

    const size_t endPos = text.rfind(kEndTag);
    if (endPos == std::string::npos || endPos < scanStart) {
        return false;
    }

    const size_t beginPos = text.rfind(kBeginTag, endPos);
    if (beginPos == std::string::npos || beginPos < scanStart) {
        return false;
    }

    const size_t beginLineEnd = text.find('\n', beginPos);
    if (beginLineEnd == std::string::npos || beginLineEnd >= endPos) {
        return false;
    }

    std::string base64Data;
    base64Data.reserve((endPos - beginLineEnd) * 3 / 4);

    size_t cursor = beginLineEnd + 1;
    while (cursor < endPos) {
        size_t lineEnd = text.find('\n', cursor);
        if (lineEnd == std::string::npos || lineEnd > endPos) {
            lineEnd = endPos;
        }
        std::string_view line(&text[cursor], lineEnd - cursor);

        if (!line.empty() && line.back() == '\r') {
            line = line.substr(0, line.size() - 1);
        }
        if (!line.empty() && line.front() == ';') {
            line.remove_prefix(1);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())) != 0) {
                line.remove_prefix(1);
            }
            for (char ch : line) {
                if (!std::isspace(static_cast<unsigned char>(ch))) {
                    base64Data.push_back(ch);
                }
            }
        }

        if (lineEnd == endPos) {
            break;
        }
        cursor = lineEnd + 1;
    }

    if (base64Data.empty()) {
        return false;
    }
    return DecodeBase64(base64Data, outPngBytes);
}

std::wstring BuildMissingFields(const ApiumMetadata& metadata) {
    std::vector<std::wstring_view> missing;
    if (metadata.printer.empty()) {
        missing.push_back(L"printer");
    }
    if (metadata.materialType.empty()) {
        missing.push_back(L"material");
    }
    if (metadata.nozzleDiameter.empty()) {
        missing.push_back(L"nozzle_diameter");
    }
    if (metadata.nozzleTemperature.empty()) {
        missing.push_back(L"nozzle_temperature");
    }
    if (metadata.bedTemperature.empty()) {
        missing.push_back(L"bed_temperature");
    }
    if (metadata.layerHeight.empty()) {
        missing.push_back(L"layer_height");
    }
    if (metadata.perimeters.empty()) {
        missing.push_back(L"perimeters");
    }
    if (metadata.infillPattern.empty()) {
        missing.push_back(L"infill_pattern");
    }
    if (metadata.infillDensity.empty()) {
        missing.push_back(L"infill_density");
    }

    if (missing.empty()) {
        return L"none";
    }

    std::wstring text;
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) {
            text += L", ";
        }
        text += missing[i];
    }
    return text;
}

void ParseGcodeWindows(std::string_view tail, std::string_view header, ApiumMetadata& metadata) {
    metadata.diagnostics.configPartStatus = L"n/a";
    metadata.diagnostics.configPartPath = L"n/a";
    metadata.diagnostics.decode = L"n/a";
    metadata.diagnostics.apiumDetect = L"none";

    ParsedMapResult tailResult = ParseKeyValuesFromText(tail, true);
    ParsedMapResult headerResult = ParseKeyValuesFromText(header, true);
    // Footer values always take precedence over legacy header comments.
    for (auto& [key, value] : headerResult.keyValues) {
        tailResult.keyValues.try_emplace(key, std::move(value));
    }
    tailResult.hasApiumGeneratorMarker |= headerResult.hasApiumGeneratorMarker;
    if (tailResult.hasApiumGeneratorMarker) {
        metadata.diagnostics.apiumDetect = L"marker";
    } else if (tailResult.hasApiumConfigBlock) {
        metadata.diagnostics.apiumDetect = L"configblock";
    }
    metadata.isApium = tailResult.hasApiumGeneratorMarker || tailResult.hasApiumConfigBlock;
    FillMetadataFieldsFromMap(metadata, tailResult.keyValues);
    (void)ParseGcodeThumbnailBlock(tail, 0, metadata.thumbnailBytes);
    if (metadata.thumbnailBytes.empty()) {
        (void)ParseGcodeThumbnailBlock(header, 0, metadata.thumbnailBytes);
    }
    metadata.diagnostics.missingFields = BuildMissingFields(metadata);
}

std::string_view CompleteTailLines(std::string_view text, bool truncated) {
    if (truncated) {
        const size_t newline = text.find('\n');
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
    }
    return text;
}

std::string_view CompleteHeaderLines(std::string_view text, bool truncated) {
    if (truncated) {
        const size_t newline = text.rfind('\n');
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(0, newline + 1);
    }
    return text;
}

void ParseGcodeMetadata(const std::vector<uint8_t>& fileBytes, ApiumMetadata& metadata) {
    const std::string_view text(fileBytes.empty() ? "" : reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
    const size_t tailStart = text.size() > kTailScanBytes ? text.size() - kTailScanBytes : 0;
    const auto tail = CompleteTailLines(text.substr(tailStart), tailStart != 0);
    const size_t headerSize = std::min(tailStart, kHeaderScanBytes);
    const auto header = CompleteHeaderLines(text.substr(0, headerSize), headerSize < text.size());
    ParseGcodeWindows(tail, header, metadata);
}

bool ReadStreamRange(IStream* stream, ULONGLONG offset, size_t size, std::vector<uint8_t>& bytes) {
    bytes.clear();
    LARGE_INTEGER position = {};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (FAILED(stream->Seek(position, STREAM_SEEK_SET, nullptr))) {
        return false;
    }
    bytes.resize(size);
    size_t total = 0;
    while (total < size) {
        ULONG read = 0;
        const ULONG request = static_cast<ULONG>(std::min<size_t>(size - total, 1U << 20));
        if (FAILED(stream->Read(bytes.data() + total, request, &read)) || read == 0 || read > request) {
            bytes.clear();
            return false;
        }
        total += read;
    }
    return true;
}

std::string_view ByteView(const std::vector<uint8_t>& bytes) {
    return {bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

size_t FindFooterStart(std::string_view text) {
    return std::min(text.rfind("; apiumslicer_config = begin"), text.rfind("; prusaslicer_config = begin"));
}

bool LooksLikeGcode(std::string_view header, std::string_view tail) {
    if (FindFooterStart(tail) != std::string_view::npos) {
        return true;
    }
    // IInitializeWithStream has no extension. Keep ZIP/STL/OBJ inputs on their
    // existing full-read path, including binary STL headers starting with ';'.
    if (header.find('\0') != std::string_view::npos) {
        return false;
    }
    if (header.starts_with("\xEF\xBB\xBF")) {
        header.remove_prefix(3);
    }
    const size_t start = header.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return false;
    }
    header.remove_prefix(start);
    return header.front() == ';' ||
           (header.size() > 1 && (header.front() == 'G' || header.front() == 'M' || header.front() == 'T') &&
            std::isdigit(static_cast<unsigned char>(header[1])));
}

void ParseA3dMetadata(const std::vector<uint8_t>& fileBytes, ApiumMetadata& metadata) {
    metadata.diagnostics.configPartStatus = L"not found";
    metadata.diagnostics.configPartPath = L"";
    metadata.diagnostics.decode = L"n/a";
    metadata.diagnostics.apiumDetect = L"none";

    mz_zip_archive zipArchive = {};
    std::vector<ZipEntryRecord> entries;
    if (!BuildZipEntryIndex(fileBytes, zipArchive, entries)) {
        metadata.diagnostics.configPartStatus = L"package open failed";
        metadata.diagnostics.missingFields = BuildMissingFields(metadata);
        return;
    }

    std::vector<uint8_t> configBytes;
    std::string matchedConfigPath;
    const bool hasConfig = ExtractZipEntryByNormalizedPath(
        zipArchive,
        entries,
        "metadata/slic3r_pe.config",
        configBytes,
        &matchedConfigPath);
    if (hasConfig && !configBytes.empty()) {
        metadata.diagnostics.configPartStatus = L"found";
        metadata.diagnostics.configPartPath = Utf8OrAnsiToWide(matchedConfigPath);

        std::string configText;
        if (DecodeConfigBytesToText(configBytes, configText, &metadata.diagnostics.decode)) {
            ParsedMapResult parsed = ParseKeyValuesFromText(configText, false);
            const bool heuristicApium = LooksLikeApiumConfigByKeys(parsed.keyValues);
            metadata.isApium = parsed.hasApiumGeneratorMarker || parsed.hasApiumConfigBlock || heuristicApium;
            if (parsed.hasApiumGeneratorMarker) {
                metadata.diagnostics.apiumDetect = L"marker";
            } else if (parsed.hasApiumConfigBlock) {
                metadata.diagnostics.apiumDetect = L"configblock";
            } else if (heuristicApium) {
                metadata.diagnostics.apiumDetect = L"heuristic";
            } else {
                metadata.diagnostics.apiumDetect = L"none";
            }
            FillMetadataFieldsFromMap(metadata, parsed.keyValues);
        } else {
            metadata.diagnostics.decode = L"decode failed";
            DebugLog("A3D config decode failed");
        }
    } else {
        metadata.diagnostics.configPartStatus = L"not found";
        DebugLog("A3D config part not found");
    }

    std::vector<uint8_t> thumbnailBytes;
    std::string matchedThumbnailPath;
    for (const std::wstring_view path : kThumbnailA3dCandidates) {
        const std::string candidate = NormalizeZipPathAscii(WidePathToAscii(path));
        if (ExtractZipEntryByNormalizedPath(zipArchive, entries, candidate, thumbnailBytes, &matchedThumbnailPath) &&
            !thumbnailBytes.empty()) {
            break;
        }
    }
    if (thumbnailBytes.empty()) {
        for (const ZipEntryRecord& entry : entries) {
            if (!IsLikelyThumbnailPathAscii(entry.normalizedPath)) {
                continue;
            }
            if (ExtractZipEntryToBytes(zipArchive, entry.index, thumbnailBytes) && !thumbnailBytes.empty()) {
                matchedThumbnailPath = entry.path;
                break;
            }
        }
    }
    metadata.thumbnailBytes = std::move(thumbnailBytes);

    bool gitFound = false;
    const std::optional<std::string> bestTag = GetHighestTagFromZipEntries(zipArchive, entries, &gitFound);
    if (bestTag.has_value()) {
        metadata.projectVersion = ToWideField(bestTag.value());
    } else if (gitFound) {
        metadata.projectVersion = L"Version unknown";
    } else {
        metadata.projectVersion = L"Not versioned";
    }

    EndZipReader(zipArchive);

    metadata.printTime.clear();
    metadata.diagnostics.missingFields = BuildMissingFields(metadata);
}

} // namespace

bool ReadPreviewStream(IStream* stream, std::vector<uint8_t>& outBytes, ApiumMetadata& metadata, bool forceGcode) {
    outBytes.clear();
    metadata = {};
    if (!stream) {
        return false;
    }

    LARGE_INTEGER zero = {};
    ULARGE_INTEGER end = {};
    // Seek to EOF before any content read. Never fall back to reading an entire
    // GCODE stream if the provider cannot support random access.
    if (FAILED(stream->Seek(zero, STREAM_SEEK_END, &end)) || end.QuadPart > LLONG_MAX) {
        return false;
    }
    const ULONGLONG fileSize = end.QuadPart;
    size_t tailSize = static_cast<size_t>(std::min<ULONGLONG>(fileSize, kInitialTailBytes));
    std::vector<uint8_t> tailBytes;
    if (!ReadStreamRange(stream, fileSize - tailSize, tailSize, tailBytes)) {
        return false;
    }

    std::vector<uint8_t> headerBytes;
    if (!forceGcode) {
        if (!ReadStreamRange(stream, 0, static_cast<size_t>(std::min<ULONGLONG>(fileSize, 4096)), headerBytes)) {
            return false;
        }
        if (IsZipContainer(headerBytes) || !LooksLikeGcode(ByteView(headerBytes), ByteView(tailBytes))) {
            if (!ReadStreamFully(stream, outBytes)) {
                return false;
            }
            metadata = ParseApiumMetadata(outBytes);
            return true;
        }
    }

    // Grow backwards only until the footer config is covered. An absent PNG or
    // missing field must never trigger a scan through the G-code body.
    const size_t tailLimit = static_cast<size_t>(std::min<ULONGLONG>(fileSize, kTailScanBytes));
    while (tailSize < tailLimit) {
        const size_t configStart = FindFooterStart(ByteView(tailBytes));
        // ApiumSlicer writes time/statistics immediately before the config.
        // Keep some preceding context when the marker is near a window edge.
        if (configStart != std::string_view::npos && configStart >= 4096) {
            break;
        }
        const size_t nextSize = std::min(tailSize * 2, tailLimit);
        std::vector<uint8_t> prefix;
        if (!ReadStreamRange(stream, fileSize - nextSize, nextSize - tailSize, prefix)) {
            return false;
        }
        prefix.insert(prefix.end(), tailBytes.begin(), tailBytes.end());
        tailBytes = std::move(prefix);
        tailSize = nextSize;
    }

    const auto tail = CompleteTailLines(ByteView(tailBytes), tailSize < fileSize);
    metadata.diagnostics.container = L"GCODE";
    ParseGcodeWindows(tail, {}, metadata);
    const bool needsHeader = !metadata.isApium || metadata.thumbnailBytes.empty();
    std::string_view header;
    if (needsHeader && tailSize < fileSize) {
        const size_t headerSize = static_cast<size_t>(std::min<ULONGLONG>(fileSize - tailSize, kHeaderScanBytes));
        if (!ReadStreamRange(stream, 0, headerSize, headerBytes)) {
            return false;
        }
        header = CompleteHeaderLines(ByteView(headerBytes), headerSize < fileSize);
    }
    if (!header.empty()) {
        ParseGcodeWindows(tail, header, metadata);
    }
    return true;
}

bool ReadPreviewFile(const wchar_t* filePath, std::vector<uint8_t>& outBytes, ApiumMetadata& metadata) {
    outBytes.clear();
    metadata = {};
    if (!filePath || !*filePath) {
        return false;
    }
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(filePath, STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream))) {
        return false;
    }
    const bool isGcode = _wcsicmp(PathFindExtensionW(filePath), L".gcode") == 0;
    return ReadPreviewStream(stream.Get(), outBytes, metadata, isGcode);
}

bool ReadStreamFully(IStream* stream, std::vector<uint8_t>& outBytes) {
    if (!stream) {
        return false;
    }

    LARGE_INTEGER zero = {};
    (void)stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    outBytes.clear();

    STATSTG stat = {};
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
        if (stat.cbSize.QuadPart > static_cast<ULONGLONG>(SIZE_MAX)) {
            return false;
        }
        const size_t expectedSize = static_cast<size_t>(stat.cbSize.QuadPart);
        outBytes.resize(expectedSize);

        size_t totalRead = 0;
        while (totalRead < expectedSize) {
            const ULONG toRead = static_cast<ULONG>(std::min<size_t>(expectedSize - totalRead, 1U << 20));
            ULONG read = 0;
            const HRESULT hr = stream->Read(outBytes.data() + totalRead, toRead, &read);
            if (FAILED(hr)) {
                outBytes.clear();
                return false;
            }
            if (read == 0) {
                break;
            }
            totalRead += read;
        }

        outBytes.resize(totalRead);
        return !outBytes.empty();
    }

    constexpr ULONG kChunkSize = 64 * 1024;
    std::array<uint8_t, kChunkSize> chunk = {};
    while (true) {
        ULONG read = 0;
        const HRESULT hr = stream->Read(chunk.data(), kChunkSize, &read);
        if (FAILED(hr)) {
            outBytes.clear();
            return false;
        }
        if (read == 0) {
            break;
        }
        outBytes.insert(outBytes.end(), chunk.begin(), chunk.begin() + read);
    }
    return true;
}

bool ReadFileFully(const wchar_t* filePath, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    if (!filePath || filePath[0] == L'\0') {
        return false;
    }

    HANDLE handle = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > static_cast<unsigned long long>(SIZE_MAX)) {
        CloseHandle(handle);
        return false;
    }

    outBytes.resize(static_cast<size_t>(size.QuadPart));
    size_t totalRead = 0;
    while (totalRead < outBytes.size()) {
        const DWORD toRead = static_cast<DWORD>(std::min<size_t>(outBytes.size() - totalRead, 1U << 20));
        DWORD read = 0;
        if (!ReadFile(handle, outBytes.data() + totalRead, toRead, &read, nullptr)) {
            outBytes.clear();
            CloseHandle(handle);
            return false;
        }
        if (read == 0) {
            break;
        }
        totalRead += read;
    }

    CloseHandle(handle);
    outBytes.resize(totalRead);
    return !outBytes.empty();
}

bool IsZipContainer(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 4) {
        return false;
    }
    return bytes[0] == 0x50 && bytes[1] == 0x4B &&
           (bytes[2] == 0x03 || bytes[2] == 0x05 || bytes[2] == 0x07) &&
           (bytes[3] == 0x04 || bytes[3] == 0x06 || bytes[3] == 0x08);
}

bool LoadFallbackPng(HINSTANCE module, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_FALLBACK_PNG), L"PNG");
    if (!resource) {
        return false;
    }

    const DWORD size = SizeofResource(module, resource);
    if (size == 0) {
        return false;
    }

    HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) {
        return false;
    }

    const void* resourceData = LockResource(loaded);
    if (!resourceData) {
        return false;
    }

    const auto* start = static_cast<const uint8_t*>(resourceData);
    outBytes.assign(start, start + size);
    return true;
}

bool DecodePngToHBitmap(const std::vector<uint8_t>& pngBytes, UINT requestedMaxSize, HBITMAP* outBitmap) {
    if (!outBitmap) {
        return false;
    }
    *outBitmap = nullptr;
    if (pngBytes.empty() || requestedMaxSize == 0) {
        return false;
    }

    ComPtr<IWICImagingFactory> imagingFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imagingFactory)))) {
        return false;
    }

    ComPtr<IWICStream> wicStream;
    if (FAILED(imagingFactory->CreateStream(&wicStream))) {
        return false;
    }

    if (FAILED(wicStream->InitializeFromMemory(
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(pngBytes.data())),
            static_cast<DWORD>(pngBytes.size())))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(imagingFactory->CreateDecoderFromStream(wicStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        return false;
    }

    const float scale = std::min(
        static_cast<float>(requestedMaxSize) / static_cast<float>(width),
        static_cast<float>(requestedMaxSize) / static_cast<float>(height));
    const UINT targetW = std::max(1U, static_cast<UINT>(width * scale));
    const UINT targetH = std::max(1U, static_cast<UINT>(height * scale));

    ComPtr<IWICBitmapSource> source = frame;
    if (targetW != width || targetH != height) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(imagingFactory->CreateBitmapScaler(&scaler))) {
            return false;
        }
        if (FAILED(scaler->Initialize(frame.Get(), targetW, targetH, WICBitmapInterpolationModeFant))) {
            return false;
        }
        source = scaler;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(imagingFactory->CreateFormatConverter(&converter))) {
        return false;
    }
    if (FAILED(converter->Initialize(
            source.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const UINT stride = targetW * 4;
    const UINT bufferSize = stride * targetH;
    std::vector<uint8_t> pixels(bufferSize);
    if (FAILED(converter->CopyPixels(nullptr, stride, bufferSize, pixels.data()))) {
        return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(targetW);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(targetH);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!bmp || !dibBits) {
        if (bmp) {
            DeleteObject(bmp);
        }
        return false;
    }

    std::memcpy(dibBits, pixels.data(), pixels.size());
    *outBitmap = bmp;
    return true;
}

ApiumMetadata ParseApiumMetadata(const std::vector<uint8_t>& fileBytes) {
    ApiumMetadata metadata;
    metadata.isZipContainer = IsZipContainer(fileBytes);
    metadata.diagnostics.container = metadata.isZipContainer ? L"ZIP(A3D)" : L"GCODE";

    if (metadata.isZipContainer) {
        ParseA3dMetadata(fileBytes, metadata);
    } else {
        ParseGcodeMetadata(fileBytes, metadata);
    }

    if (metadata.isZipContainer) {
        DebugLog(metadata.isApium ? "Parse result: container=A3D, isApium=true" : "Parse result: container=A3D, isApium=false");
    } else {
        DebugLog(metadata.isApium ? "Parse result: container=GCODE, isApium=true" : "Parse result: container=GCODE, isApium=false");
    }
    if (metadata.diagnostics.missingFields.empty()) {
        metadata.diagnostics.missingFields = BuildMissingFields(metadata);
    }
    return metadata;
}
