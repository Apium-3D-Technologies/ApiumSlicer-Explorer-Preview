#include <windows.h>

#include <objbase.h>
#include <oleidl.h>
#include <propkeydef.h>
#include <propsys.h>
#include <propvarutil.h>
#include <richedit.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <strsafe.h>
#include <thumbcache.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "mesh_preview.hpp"
#include "metadata.hpp"
#include "generated/apium_version.h"


// Show debug information in preview
//#define SHOW_DEBUG

// CMake defines ASXP_INTERACTIVE_MESH_PREVIEW as 0 or 1.
#if ASXP_INTERACTIVE_MESH_PREVIEW
#include <windowsx.h>
#include "mesh_viewport.hpp"
#endif

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::wstring_view kThumbnailProviderIidString = L"{E357FCCD-A995-4576-B01F-234630154E96}";
constexpr std::wstring_view kPreviewHandlerIidString = L"{8895B1C6-B41F-4C1C-A562-0D564250836F}";
constexpr std::wstring_view kPreviewHostAppId = L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";
constexpr std::wstring_view kLegacyPreviewHostAppId = L"{3B0FE91D-45E1-41E4-A6A7-6F8045AEF35A}";
constexpr wchar_t kPreviewPaneWindowClassName[] = L"ApiumSlicerPreviewPaneWindow";
constexpr wchar_t kPreviewImageWindowClassName[] = L"ApiumSlicerPreviewImageWindow";
#if ASXP_INTERACTIVE_MESH_PREVIEW
constexpr wchar_t kMeshViewportWindowClassName[] = L"ApiumSlicerMeshViewportWindow";
constexpr UINT_PTR kMeshPresentRetryTimer = 1;
#endif

// {8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}
const CLSID CLSID_ApiumSlicerThumbnailProvider = {
    0x8a14eaa2, 0x3fdb, 0x4c5a, {0x98, 0xa1, 0x5d, 0x0a, 0x87, 0x00, 0xc8, 0xa5}
};

// {B450D582-1E34-49E0-8AE7-AFF2D2CD2535}
const CLSID CLSID_ApiumSlicerPreviewHandler = {
    0xb450d582, 0x1e34, 0x49e0, {0x8a, 0xe7, 0xaf, 0xf2, 0xd2, 0xcd, 0x25, 0x35}
};

// {12D795E2-813D-4E87-93BB-DC5E6239BB42}
const CLSID CLSID_ApiumSlicerPropertyHandler = {
    0x12d795e2, 0x813d, 0x4e87, {0x93, 0xbb, 0xdc, 0x5e, 0x62, 0x39, 0xbb, 0x42}
};

// {A19E4D20-CA76-47BD-B369-98EC0A8A6A30}
const GUID FMTID_ApiumSlicerProperties = {
    0xa19e4d20, 0xca76, 0x47bd, {0xb3, 0x69, 0x98, 0xec, 0x0a, 0x8a, 0x6a, 0x30}
};

const PROPERTYKEY PKEY_Apium_Printer = {FMTID_ApiumSlicerProperties, 2};
const PROPERTYKEY PKEY_Apium_ProjectVersion = {FMTID_ApiumSlicerProperties, 3};
const PROPERTYKEY PKEY_Apium_MaterialType = {FMTID_ApiumSlicerProperties, 4};
const PROPERTYKEY PKEY_Apium_NozzleDiameter = {FMTID_ApiumSlicerProperties, 5};
const PROPERTYKEY PKEY_Apium_NozzleTemperature = {FMTID_ApiumSlicerProperties, 6};
const PROPERTYKEY PKEY_Apium_BedTemperature = {FMTID_ApiumSlicerProperties, 7};
const PROPERTYKEY PKEY_Apium_LayerHeight = {FMTID_ApiumSlicerProperties, 8};
const PROPERTYKEY PKEY_Apium_Perimeters = {FMTID_ApiumSlicerProperties, 9};
const PROPERTYKEY PKEY_Apium_InfillPattern = {FMTID_ApiumSlicerProperties, 10};
const PROPERTYKEY PKEY_Apium_InfillDensity = {FMTID_ApiumSlicerProperties, 11};
const PROPERTYKEY PKEY_Apium_PrintTime = {FMTID_ApiumSlicerProperties, 12};

constexpr std::wstring_view kCanonicalPrinter = L"Apium.ApiumSlicer.Printer";
constexpr std::wstring_view kCanonicalProjectVersion = L"Apium.ApiumSlicer.ProjectVersion";
constexpr std::wstring_view kCanonicalMaterialType = L"Apium.ApiumSlicer.MaterialType";
constexpr std::wstring_view kCanonicalNozzleDiameter = L"Apium.ApiumSlicer.NozzleDiameter";
constexpr std::wstring_view kCanonicalNozzleTemperature = L"Apium.ApiumSlicer.NozzleTemperature";
constexpr std::wstring_view kCanonicalBedTemperature = L"Apium.ApiumSlicer.BedTemperature";
constexpr std::wstring_view kCanonicalLayerHeight = L"Apium.ApiumSlicer.LayerHeight";
constexpr std::wstring_view kCanonicalPerimeters = L"Apium.ApiumSlicer.Perimeters";
constexpr std::wstring_view kCanonicalInfillPattern = L"Apium.ApiumSlicer.InfillPattern";
constexpr std::wstring_view kCanonicalInfillDensity = L"Apium.ApiumSlicer.InfillDensity";
constexpr std::wstring_view kCanonicalPrintTime = L"Apium.ApiumSlicer.PrintTime";

constexpr std::wstring_view kPreviewDetailsList =
    L"prop:"
    L"Apium.ApiumSlicer.Printer;"
    L"Apium.ApiumSlicer.ProjectVersion;"
    L"Apium.ApiumSlicer.MaterialType;"
    L"Apium.ApiumSlicer.NozzleDiameter;"
    L"Apium.ApiumSlicer.NozzleTemperature;"
    L"Apium.ApiumSlicer.BedTemperature;"
    L"Apium.ApiumSlicer.LayerHeight;"
    L"Apium.ApiumSlicer.Perimeters;"
    L"Apium.ApiumSlicer.InfillPattern;"
    L"Apium.ApiumSlicer.InfillDensity;"
    L"Apium.ApiumSlicer.PrintTime";

constexpr std::wstring_view kInfoTipList =
    L"prop:"
    L"Apium.ApiumSlicer.Printer;"
    L"Apium.ApiumSlicer.MaterialType;"
    L"Apium.ApiumSlicer.ProjectVersion";

const std::array<PROPERTYKEY, 11> kApiumPropertyKeys = {
    PKEY_Apium_Printer,
    PKEY_Apium_ProjectVersion,
    PKEY_Apium_MaterialType,
    PKEY_Apium_NozzleDiameter,
    PKEY_Apium_NozzleTemperature,
    PKEY_Apium_BedTemperature,
    PKEY_Apium_LayerHeight,
    PKEY_Apium_Perimeters,
    PKEY_Apium_InfillPattern,
    PKEY_Apium_InfillDensity,
    PKEY_Apium_PrintTime,
};

constexpr std::array<std::wstring_view, 4> kSupportedExtensions = {L".gcode", L".a3d", L".stl", L".obj"};
constexpr std::array<std::wstring_view, 2> kPropertyExtensions = {L".gcode", L".a3d"};
constexpr std::array<std::wstring_view, 2> kPrivateDefaultExtensions = {L".gcode", L".a3d"};
constexpr std::wstring_view kApiumProgId = L"Apium.Slicer.1";
constexpr std::wstring_view kApiumVersionIndependentProgId = L"Apium.Slicer";
constexpr std::wstring_view kApiumRegisteredApplicationName = L"Apium Slicer";
constexpr std::wstring_view kApiumCapabilitiesPath = L"Software\\ApiumSlicerExplorerPreview\\Capabilities";

HINSTANCE g_module = nullptr;
std::atomic_ulong g_objCount = 0;
std::atomic_ulong g_lockCount = 0;

enum class HandlerKind {
    Thumbnail,
    Preview,
    Property,
};

std::wstring HandlerKindToString(HandlerKind kind) {
    switch (kind) {
    case HandlerKind::Thumbnail:
        return L"Thumbnail";
    case HandlerKind::Preview:
        return L"Preview";
    case HandlerKind::Property:
        return L"Property";
    }
    return L"Unknown";
}

void DebugLogWin(std::wstring_view message) {
    std::wstring output = L"[ApiumPreview] ";
    output.append(message.begin(), message.end());
    output.push_back(L'\n');
    OutputDebugStringW(output.c_str());

    PWSTR localAppDataLow = nullptr;
    PWSTR localAppData = nullptr;
    std::wstring logDir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, KF_FLAG_CREATE, nullptr, &localAppDataLow)) && localAppDataLow) {
        logDir = localAppDataLow;
        CoTaskMemFree(localAppDataLow);
    } else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)) && localAppData) {
        logDir = localAppData;
        CoTaskMemFree(localAppData);
    }

    if (logDir.empty()) {
        return;
    }

    logDir += L"\\ApiumSlicerExplorerPreview";
    if (!CreateDirectoryW(logDir.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return;
        }
    }

    const std::wstring logPath = logDir + L"\\preview.log";
    HANDLE handle = CreateFileW(
        logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t prefix[96] = {};
    (void)StringCchPrintfW(
        prefix,
        std::size(prefix),
        L"%04u-%02u-%02u %02u:%02u:%02u pid=%lu ",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        GetCurrentProcessId());

    std::wstring line = prefix;
    line.append(output);
    DWORD written = 0;
    (void)WriteFile(handle, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(handle);
}

std::wstring GuidToString(const GUID& guid) {
    wchar_t buffer[64] = {};
    (void)StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return std::wstring(buffer);
}

std::wstring HResultToString(HRESULT hr) {
    wchar_t buffer[16] = {};
    (void)StringCchPrintfW(buffer, std::size(buffer), L"0x%08lX", static_cast<unsigned long>(hr));
    return std::wstring(buffer);
}

std::wstring GetModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD len = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) {
            return L"";
        }
        if (len < path.size() - 1) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring GetModuleDirectory() {
    std::wstring modulePath = GetModulePath();
    if (modulePath.empty()) {
        return L"";
    }
    const size_t slashPos = modulePath.find_last_of(L"\\/");
    if (slashPos == std::wstring::npos) {
        return L"";
    }
    return modulePath.substr(0, slashPos);
}

std::optional<std::wstring> ReadDefaultRegString(HKEY root, const std::wstring& subKey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return std::nullopt;
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return std::nullopt;
    }
    RegCloseKey(key);

    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::wstring> ReadRegString(HKEY root, const std::wstring& subKey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return std::nullopt;
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return std::nullopt;
    }
    RegCloseKey(key);

    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::wstring> ResolveProgIdsForExtension(std::wstring_view ext) {
    std::vector<std::wstring> progIds;
    std::set<std::wstring> seen;

    auto addProgId = [&](const std::optional<std::wstring>& value) {
        if (!value.has_value() || value.value().empty()) {
            return;
        }
        if (seen.insert(value.value()).second) {
            progIds.push_back(value.value());
        }
    };

    const std::wstring userChoiceKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + std::wstring(ext) + L"\\UserChoice";
    addProgId(ReadRegString(HKEY_CURRENT_USER, userChoiceKey, L"ProgId"));

    const std::wstring userChoiceLatestKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + std::wstring(ext) + L"\\UserChoiceLatest\\ProgId";
    addProgId(ReadRegString(HKEY_CURRENT_USER, userChoiceLatestKey, L"ProgId"));

    const std::wstring extKey = L"Software\\Classes\\" + std::wstring(ext);
    addProgId(ReadDefaultRegString(HKEY_CURRENT_USER, extKey));

    const std::wstring hkcrExt = std::wstring(ext);
    addProgId(ReadDefaultRegString(HKEY_CLASSES_ROOT, hkcrExt));

    return progIds;
}

bool IsPropertyExtension(std::wstring_view ext) {
    for (const std::wstring_view propertyExt : kPropertyExtensions) {
        if (_wcsicmp(std::wstring(ext).c_str(), std::wstring(propertyExt).c_str()) == 0) {
            return true;
        }
    }
    return false;
}

bool IsPrivateDefaultExtension(std::wstring_view ext) {
    for (const std::wstring_view privateExt : kPrivateDefaultExtensions) {
        if (_wcsicmp(std::wstring(ext).c_str(), std::wstring(privateExt).c_str()) == 0) {
            return true;
        }
    }
    return false;
}

bool IsAppsDarkModeEnabled() {
    DWORD value = 1;
    DWORD valueSize = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &valueSize);
    if (status == ERROR_SUCCESS) {
        return value == 0;
    }
    return false;
}

HRESULT SetRegSz(HKEY key, const wchar_t* valueName, std::wstring_view value) {
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(
        key,
        valueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.data()),
        size);
    return HRESULT_FROM_WIN32(status);
}

HRESULT SetRegDword(HKEY key, const wchar_t* valueName, DWORD value) {
    const LSTATUS status = RegSetValueExW(
        key,
        valueName,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
    return HRESULT_FROM_WIN32(status);
}

HRESULT SetRegNone(HKEY key, const wchar_t* valueName) {
    const LSTATUS status = RegSetValueExW(key, valueName, 0, REG_NONE, nullptr, 0);
    return HRESULT_FROM_WIN32(status);
}

HRESULT CreateRegKeyPath(HKEY root, const std::wstring& subKey, HKEY* outKey) {
    DWORD disposition = 0;
    HKEY key = nullptr;
    const LSTATUS status = RegCreateKeyExW(
        root,
        subKey.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE,
        nullptr,
        &key,
        &disposition);
    if (status != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(status);
    }
    *outKey = key;
    return S_OK;
}

void TryDeleteKeyTree(HKEY root, const std::wstring& subKey) {
    (void)RegDeleteTreeW(root, subKey.c_str());
}

void TryDeleteValue(HKEY root, const std::wstring& subKey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    (void)RegDeleteValueW(key, valueName);
    RegCloseKey(key);
}

void TryDeleteDefaultValueIfEquals(HKEY root, const std::wstring& subKey, std::wstring_view expectedValue) {
    const std::optional<std::wstring> current = ReadDefaultRegString(root, subKey);
    if (!current.has_value() || _wcsicmp(current->c_str(), std::wstring(expectedValue).c_str()) != 0) {
        return;
    }
    TryDeleteValue(root, subKey, nullptr);
}

bool WriteTextFileUtf8(const std::wstring& path, const std::string& content) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    static const BYTE bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    BOOL ok = WriteFile(handle, bom, static_cast<DWORD>(std::size(bom)), &written, nullptr);
    if (!ok) {
        CloseHandle(handle);
        return false;
    }

    ok = WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(handle);
    return ok == TRUE;
}

std::wstring GetPropertySchemaFilePath() {
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &appData))) {
        return L"";
    }
    std::wstring dirPath(appData);
    CoTaskMemFree(appData);
    dirPath += L"\\ApiumSlicerExplorerPreview";

    if (!CreateDirectoryW(dirPath.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return L"";
        }
    }

    return dirPath + L"\\ApiumSlicer.propschema.propdesc";
}

std::string BuildPropertySchemaXml() {
    const std::wstring fmtid = GuidToString(FMTID_ApiumSlicerProperties);
    auto narrowAscii = [](std::wstring_view text) {
        std::string out;
        out.reserve(text.size());
        for (wchar_t ch : text) {
            out.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
        }
        return out;
    };

    const std::string fmtidUtf8 = narrowAscii(fmtid);

    auto entry = [&](std::wstring_view canonical, int propId, std::wstring_view label) {
        std::string name = narrowAscii(canonical);
        std::string display = narrowAscii(label);
        return std::string(
                   "  <propertyDescription name=\"" + name + "\" formatID=\"" + fmtidUtf8 +
                   "\" propID=\"" + std::to_string(propId) + "\">\n") +
               "    <searchInfo inInvertedIndex=\"false\"/>\n"
               "    <labelInfo label=\"" + display + "\"/>\n"
               "    <typeInfo type=\"String\"/>\n"
               "  </propertyDescription>\n";
    };

    std::string xml;
    xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml += "<schema xmlns=\"http://schemas.microsoft.com/windows/2006/propertydescription\" schemaVersion=\"1.0\">\n";
    xml += "<propertyDescriptionList publisher=\"Apium\" product=\"ApiumSlicerExplorerPreview\">\n";
    xml += entry(kCanonicalPrinter, 2, L"Printer");
    xml += entry(kCanonicalProjectVersion, 3, L"Project Version");
    xml += entry(kCanonicalMaterialType, 4, L"Material Type");
    xml += entry(kCanonicalNozzleDiameter, 5, L"Nozzle Diameter");
    xml += entry(kCanonicalNozzleTemperature, 6, L"Nozzle Temperature");
    xml += entry(kCanonicalBedTemperature, 7, L"Bed Temperature");
    xml += entry(kCanonicalLayerHeight, 8, L"Layer Height");
    xml += entry(kCanonicalPerimeters, 9, L"Perimeters");
    xml += entry(kCanonicalInfillPattern, 10, L"Infill Pattern");
    xml += entry(kCanonicalInfillDensity, 11, L"Infill Density");
    xml += entry(kCanonicalPrintTime, 12, L"Print Time");
    xml += "</propertyDescriptionList>\n";
    xml += "</schema>\n";
    return xml;
}

void TryRegisterPropertySchema() {
    const std::wstring schemaPath = GetPropertySchemaFilePath();
    if (schemaPath.empty()) {
        return;
    }

    const std::string schemaXml = BuildPropertySchemaXml();
    if (!WriteTextFileUtf8(schemaPath, schemaXml)) {
        return;
    }

    const HRESULT hr = PSRegisterPropertySchema(schemaPath.c_str());
    if (SUCCEEDED(hr)) {
        (void)PSRefreshPropertySchema();
    }
}

void TryUnregisterPropertySchema() {
    const std::wstring schemaPath = GetPropertySchemaFilePath();
    if (schemaPath.empty()) {
        return;
    }
    const HRESULT hr = PSUnregisterPropertySchema(schemaPath.c_str());
    if (SUCCEEDED(hr)) {
        (void)PSRefreshPropertySchema();
    }
    (void)DeleteFileW(schemaPath.c_str());
}

bool GetBitmapSize(HBITMAP bitmap, int& width, int& height) {
    BITMAP bmpInfo = {};
    if (!bitmap || GetObjectW(bitmap, sizeof(bmpInfo), &bmpInfo) != sizeof(bmpInfo)) {
        width = 0;
        height = 0;
        return false;
    }
    width = std::max(1L, bmpInfo.bmWidth);
    height = std::max(1L, bmpInfo.bmHeight);
    return true;
}

std::wstring AppendUnit(std::wstring value, std::wstring_view unit) {
    if (value.empty()) {
        return value;
    }
    std::wstring lower = value;
    for (wchar_t& ch : lower) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    std::wstring lowerUnit(unit);
    for (wchar_t& ch : lowerUnit) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    if (lower.find(lowerUnit) != std::wstring::npos) {
        return value;
    }
    value += L" ";
    value += unit;
    return value;
}

struct PreviewRow {
    std::wstring label;
    std::wstring value;
};

std::wstring GetCurrentProcessName() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (len == 0) {
        return L"unknown";
    }
    path.resize(len);
    const size_t slashPos = path.find_last_of(L"\\/");
    if (slashPos == std::wstring::npos) {
        return path;
    }
    return path.substr(slashPos + 1);
}

std::wstring EscapeRtf(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() * 2);
    for (wchar_t ch : text) {
        if (ch == L'\\' || ch == L'{' || ch == L'}') {
            out.push_back(L'\\');
            out.push_back(ch);
            continue;
        }
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            out += L"\\line ";
            continue;
        }
        if (ch >= 0x20 && ch <= 0x7E) {
            out.push_back(ch);
            continue;
        }
        out += L"\\u";
        out += std::to_wstring(static_cast<short>(ch));
        out += L"?";
    }
    return out;
}

std::string ToAsciiBytes(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        out.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
    }
    return out;
}

std::vector<PreviewRow> BuildPreviewRows(const ApiumMetadata& metadata) {
    std::vector<PreviewRow> rows;
    if (!metadata.isApium) {
        return rows;
    }

    auto addLine = [&](std::wstring_view label, std::wstring value) {
        if (value.empty()) {
            return;
        }
        rows.push_back(PreviewRow{std::wstring(label), std::move(value)});
    };

    addLine(L"Project Version", metadata.projectVersion);
    addLine(L"Printer", metadata.printer);
    addLine(L"Material Type", metadata.materialType);
    addLine(L"Nozzle Diameter", AppendUnit(metadata.nozzleDiameter, L"mm"));
    addLine(L"Nozzle Temperature", AppendUnit(metadata.nozzleTemperature, L"\xB0""C"));
    addLine(L"Bed Temperature", AppendUnit(metadata.bedTemperature, L"\xB0""C"));
    addLine(L"Layer Height", AppendUnit(metadata.layerHeight, L"mm"));
    addLine(L"Perimeters", metadata.perimeters);
    addLine(L"Infill Pattern", metadata.infillPattern);
    addLine(L"Infill Density", metadata.infillDensity);
    if (!metadata.printTime.empty()) {
        addLine(L"Print Time", metadata.printTime);
    }
    return rows;
}

std::wstring FileNameFromPath(std::wstring_view path) {
    if (path.empty()) {
        return L"";
    }
    const size_t slashPos = path.find_last_of(L"\\/");
    if (slashPos == std::wstring_view::npos) {
        return std::wstring(path);
    }
    return std::wstring(path.substr(slashPos + 1));
}

std::wstring FormatInteger(size_t value) {
    return std::to_wstring(static_cast<unsigned long long>(value));
}

std::wstring FormatFileSize(size_t bytes) {
    wchar_t buffer[64] = {};
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    if (bytes >= static_cast<size_t>(kGiB)) {
        (void)StringCchPrintfW(buffer, std::size(buffer), L"%.2f GiB", static_cast<double>(bytes) / kGiB);
    } else if (bytes >= static_cast<size_t>(kMiB)) {
        (void)StringCchPrintfW(buffer, std::size(buffer), L"%.2f MiB", static_cast<double>(bytes) / kMiB);
    } else if (bytes >= static_cast<size_t>(kKiB)) {
        (void)StringCchPrintfW(buffer, std::size(buffer), L"%.2f KiB", static_cast<double>(bytes) / kKiB);
    } else {
        (void)StringCchPrintfW(buffer, std::size(buffer), L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::wstring FormatModelNumber(double value) {
    wchar_t buffer[64] = {};
    if (value > 0.0 && value < 0.001) {
        (void)StringCchPrintfW(buffer, std::size(buffer), L"%.3g", value);
        return buffer;
    }
    (void)StringCchPrintfW(buffer, std::size(buffer), L"%.3f", value);
    std::wstring text(buffer);
    while (text.find(L'.') != std::wstring::npos && !text.empty() && text.back() == L'0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == L'.') {
        text.pop_back();
    }
    return text.empty() ? L"0" : text;
}

std::wstring FormatDimensions(const MeshPreviewResult& meshInfo) {
    if (!meshInfo.hasDimensions) {
        return L"n/a";
    }
    return FormatModelNumber(meshInfo.dimensionX) + L" x " +
           FormatModelNumber(meshInfo.dimensionY) + L" x " +
           FormatModelNumber(meshInfo.dimensionZ);
}

std::wstring FormatModelMeasure(double value, wchar_t exponent) {
    wchar_t buffer[64] = {};
    (void)StringCchPrintfW(buffer, std::size(buffer), L"%.1f unit", value);
    return std::wstring(buffer) + exponent;
}

std::wstring FormatMeshErrors(const MeshPreviewResult& mesh) {
    std::wstring details;
    const auto add = [&](std::wstring_view label, size_t count) {
        if (count == 0) { return; }
        if (!details.empty()) { details += L"; "; }
        details += std::wstring(label) + L": " + FormatInteger(count);
    };
    add(L"open edges", mesh.boundaryEdges);
    add(L"non-manifold edges", mesh.nonManifoldEdges);
    add(L"orientation", mesh.inconsistentEdges);
    add(L"degenerate triangles", mesh.degenerateTriangles);
    add(L"duplicate triangles", mesh.duplicateTriangles);
    add(L"invalid triangles", mesh.invalidTriangles);
    return FormatInteger(mesh.meshErrors) + (details.empty() ? L"" : L" (" + details + L")");
}

std::vector<PreviewRow> BuildMeshPreviewRows(
    const MeshPreviewResult& meshInfo,
    std::wstring_view sourceFilePath,
    size_t fileSizeBytes) {
    std::vector<PreviewRow> rows;
    auto addLine = [&](std::wstring_view label, std::wstring value) {
        if (!value.empty()) {
            rows.push_back(PreviewRow{std::wstring(label), std::move(value)});
        }
    };

    addLine(L"Filename", FileNameFromPath(sourceFilePath));
    addLine(L"Format", meshInfo.format.empty() ? L"n/a" : meshInfo.format);
    addLine(L"Volume", meshInfo.hasVolume
        ? FormatModelMeasure(meshInfo.volume, L'\u00b3')
        : L"n/a (see mesh errors)");
    addLine(L"Surface area", FormatModelMeasure(meshInfo.surfaceArea, L'\u00b2'));
    addLine(L"Size", FormatDimensions(meshInfo));
    addLine(L"Parts", FormatInteger(meshInfo.parts));
    addLine(L"Mesh errors", FormatMeshErrors(meshInfo));
    addLine(L"Triangles", FormatInteger(meshInfo.triangles));
    addLine(L"File size", FormatFileSize(fileSizeBytes));
    return rows;
}

std::vector<std::wstring> BuildNoMetadataLines(
    const ApiumMetadata& metadata,
    std::wstring_view initPath,
    std::wstring_view hostProcess) {
    std::vector<std::wstring> lines;
    lines.push_back(L"No Apium metadata detected for this file.");
#ifdef SHOW_DEBUG
    lines.push_back(L"");
    lines.push_back(L"Debug:");
    lines.push_back(L"Build: " + std::wstring(ASXP_NAME_W) + L" v" + std::wstring(ASXP_VERSION_W));
    lines.push_back(L"Host: " + std::wstring(hostProcess));
    lines.push_back(L"InitPath: " + std::wstring(initPath));
    lines.push_back(L"Container: " + metadata.diagnostics.container);

    std::wstring configLine = L"ConfigPart: " + metadata.diagnostics.configPartStatus;
    if (!metadata.diagnostics.configPartPath.empty()) {
        configLine += L" (" + metadata.diagnostics.configPartPath + L")";
    }
    lines.push_back(configLine);
    lines.push_back(L"Decode: " + metadata.diagnostics.decode);
    lines.push_back(L"ApiumDetect: " + metadata.diagnostics.apiumDetect);
    lines.push_back(L"MissingFields: " + metadata.diagnostics.missingFields);
#endif
    return lines;
}

std::string BuildPreviewRtf(
    const std::vector<PreviewRow>& rows,
    const std::vector<std::wstring>& plainLines,
    COLORREF textColor,
    COLORREF backgroundColor) {
    const int textR = GetRValue(textColor);
    const int textG = GetGValue(textColor);
    const int textB = GetBValue(textColor);
    const int backR = GetRValue(backgroundColor);
    const int backG = GetGValue(backgroundColor);
    const int backB = GetBValue(backgroundColor);

    std::wstring rtf =
        L"{\\rtf1\\ansi\\deff0"
        L"{\\fonttbl{\\f0 Segoe UI;}}"
        L"{\\colortbl ;"
        L"\\red" + std::to_wstring(textR) + L"\\green" + std::to_wstring(textG) + L"\\blue" + std::to_wstring(textB) + L";"
        L"\\red" + std::to_wstring(backR) + L"\\green" + std::to_wstring(backG) + L"\\blue" + std::to_wstring(backB) + L";}"
        L"\\viewkind4\\uc1\\pard\\cf1\\highlight2\\fs18 ";

    for (const PreviewRow& row : rows) {
        rtf += L"\\b ";
        rtf += EscapeRtf(row.label + L": ");
        // Delimit control word \b0 so leading digits in values are not swallowed by the RTF parser.
        rtf += L"\\b0 ";
        rtf += EscapeRtf(row.value);
        rtf += L"\\line ";
    }

    for (const std::wstring& line : plainLines) {
        rtf += EscapeRtf(line);
        rtf += L"\\line ";
    }

    rtf += L"}";
    return ToAsciiBytes(rtf);
}

std::wstring BuildPreviewPlainText(
    const std::vector<PreviewRow>& rows,
    const std::vector<std::wstring>& plainLines) {
    std::wstring text;
    for (const PreviewRow& row : rows) {
        text += row.label;
        text += L": ";
        text += row.value;
        text += L"\r\n";
    }

    for (const std::wstring& line : plainLines) {
        text += line;
        text += L"\r\n";
    }

    return text;
}

HRESULT RegisterComClassPerUser(const CLSID& clsid, std::wstring_view friendlyName) {
    const std::wstring clsidString = GuidToString(clsid);
    const std::wstring modulePath = GetModulePath();
    if (modulePath.empty()) {
        return E_FAIL;
    }

    const std::wstring clsidKeyPath = L"Software\\Classes\\CLSID\\" + clsidString;
    const std::wstring inprocKeyPath = clsidKeyPath + L"\\InprocServer32";

    HKEY clsidKey = nullptr;
    HRESULT hr = CreateRegKeyPath(HKEY_CURRENT_USER, clsidKeyPath, &clsidKey);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(clsidKey, nullptr, friendlyName);
    if (SUCCEEDED(hr)) {
        hr = SetRegSz(clsidKey, L"DisplayName", friendlyName);
    }
    if (SUCCEEDED(hr)) {
        hr = SetRegDword(clsidKey, L"AutomaticallyPreviewUntrustedFiles", 1);
    }
    (void)RegDeleteValueW(clsidKey, L"DisableLowILProcessIsolation");
    (void)RegDeleteValueW(clsidKey, L"DisableProcessIsolation");
    RegCloseKey(clsidKey);
    if (FAILED(hr)) {
        return hr;
    }

    HKEY inprocKey = nullptr;
    hr = CreateRegKeyPath(HKEY_CURRENT_USER, inprocKeyPath, &inprocKey);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(inprocKey, nullptr, modulePath);
    if (SUCCEEDED(hr)) {
        hr = SetRegSz(inprocKey, L"ThreadingModel", L"Apartment");
    }
    if (SUCCEEDED(hr)) {
        hr = SetRegSz(inprocKey, L"ProgID", L"Apium.Slicer.1");
    }
    if (SUCCEEDED(hr)) {
        hr = SetRegSz(inprocKey, L"VersionIndependentProgID", L"Apium.Slicer");
    }
    RegCloseKey(inprocKey);
    return hr;
}

HRESULT SetClsidValue(const CLSID& clsid, const wchar_t* valueName, std::wstring_view value) {
    const std::wstring clsidKeyPath = L"Software\\Classes\\CLSID\\" + GuidToString(clsid);
    HKEY clsidKey = nullptr;
    HRESULT hr = CreateRegKeyPath(HKEY_CURRENT_USER, clsidKeyPath, &clsidKey);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(clsidKey, valueName, value);
    RegCloseKey(clsidKey);
    return hr;
}

std::wstring BuildOpenWithCommand() {
    wchar_t systemDir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDir, static_cast<UINT>(std::size(systemDir)));
    std::wstring openWithPath = L"OpenWith.exe";
    if (length > 0 && length < std::size(systemDir)) {
        openWithPath = systemDir;
        openWithPath += L"\\OpenWith.exe";
    }
    return L"\"" + openWithPath + L"\" \"%1\"";
}

HRESULT RegisterApiumApplicationPerUser(
    const std::wstring& thumbClsid,
    const std::wstring& previewClsid) {
    const std::wstring modulePath = GetModulePath();
    if (modulePath.empty()) {
        return E_FAIL;
    }

    HKEY key = nullptr;
    const std::wstring progIdKeyPath = L"Software\\Classes\\" + std::wstring(kApiumProgId);
    HRESULT hr = CreateRegKeyPath(HKEY_CURRENT_USER, progIdKeyPath, &key);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(key, nullptr, L"ApiumSlicer");
    if (SUCCEEDED(hr)) {
        hr = SetRegSz(key, L"FriendlyTypeName", L"Apium Slicer File");
    }
    RegCloseKey(key);
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring defaultIconPath = progIdKeyPath + L"\\DefaultIcon";
    hr = CreateRegKeyPath(HKEY_CURRENT_USER, defaultIconPath, &key);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(key, nullptr, L"\"" + modulePath + L"\",0");
    RegCloseKey(key);
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring openCommandPath = progIdKeyPath + L"\\shell\\open\\command";
    hr = CreateRegKeyPath(HKEY_CURRENT_USER, openCommandPath, &key);
    if (FAILED(hr)) {
        return hr;
    }
    if (ReadDefaultRegString(HKEY_CURRENT_USER, openCommandPath).value_or(L"").empty()) {
        hr = SetRegSz(key, nullptr, BuildOpenWithCommand());
    }
    RegCloseKey(key);
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring previewPath = progIdKeyPath + L"\\shellex\\" + std::wstring(kPreviewHandlerIidString);
    hr = CreateRegKeyPath(HKEY_CURRENT_USER, previewPath, &key);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(key, nullptr, previewClsid);
    RegCloseKey(key);
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring thumbPath = progIdKeyPath + L"\\shellex\\" + std::wstring(kThumbnailProviderIidString);
    hr = CreateRegKeyPath(HKEY_CURRENT_USER, thumbPath, &key);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(key, nullptr, thumbClsid);
    RegCloseKey(key);
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring capabilitiesPath = std::wstring(kApiumCapabilitiesPath);
    hr = CreateRegKeyPath(HKEY_CURRENT_USER, capabilitiesPath, &key);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(key, L"ApplicationName", kApiumRegisteredApplicationName);
    if (SUCCEEDED(hr)) {
        hr = SetRegSz(key, L"ApplicationDescription", L"Apium Slicer file previews and model files");
    }
    RegCloseKey(key);
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring fileAssociationsPath = capabilitiesPath + L"\\FileAssociations";
    hr = CreateRegKeyPath(HKEY_CURRENT_USER, fileAssociationsPath, &key);
    if (FAILED(hr)) {
        return hr;
    }
    for (const std::wstring_view ext : kSupportedExtensions) {
        hr = SetRegSz(key, std::wstring(ext).c_str(), kApiumProgId);
        if (FAILED(hr)) {
            break;
        }
    }
    RegCloseKey(key);
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring registeredApplicationsPath = L"Software\\RegisteredApplications";
    hr = CreateRegKeyPath(HKEY_CURRENT_USER, registeredApplicationsPath, &key);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(key, std::wstring(kApiumRegisteredApplicationName).c_str(), kApiumCapabilitiesPath);
    RegCloseKey(key);
    return hr;
}

HRESULT RegisterProviderPerUser() {
    HRESULT hr = RegisterComClassPerUser(CLSID_ApiumSlicerThumbnailProvider, L"Apium Slicer Thumbnail Provider");
    if (FAILED(hr)) {
        return hr;
    }
    hr = RegisterComClassPerUser(CLSID_ApiumSlicerPreviewHandler, L"Apium Slicer Preview Handler");
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetClsidValue(CLSID_ApiumSlicerPreviewHandler, L"AppID", kPreviewHostAppId);
    if (FAILED(hr)) {
        return hr;
    }
    hr = RegisterComClassPerUser(CLSID_ApiumSlicerPropertyHandler, L"Apium Slicer Property Handler");
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring thumbClsid = GuidToString(CLSID_ApiumSlicerThumbnailProvider);
    const std::wstring previewClsid = GuidToString(CLSID_ApiumSlicerPreviewHandler);
    const std::wstring propertyClsid = GuidToString(CLSID_ApiumSlicerPropertyHandler);

    hr = RegisterApiumApplicationPerUser(thumbClsid, previewClsid);
    if (FAILED(hr)) {
        return hr;
    }

    for (const std::wstring_view ext : kSupportedExtensions) {
        {
            const std::wstring keyPath = L"Software\\Classes\\" + std::wstring(ext);
            HKEY key = nullptr;
            hr = CreateRegKeyPath(HKEY_CURRENT_USER, keyPath, &key);
            if (FAILED(hr)) {
                return hr;
            }
            if (IsPrivateDefaultExtension(ext) ||
                ReadDefaultRegString(HKEY_CLASSES_ROOT, std::wstring(ext)).value_or(L"").empty()) {
                hr = SetRegSz(key, nullptr, kApiumProgId);
            }
            RegCloseKey(key);
            if (FAILED(hr)) {
                return hr;
            }
        }

        {
            const std::wstring keyPath = L"Software\\Classes\\" + std::wstring(ext) + L"\\OpenWithProgids";
            HKEY key = nullptr;
            hr = CreateRegKeyPath(HKEY_CURRENT_USER, keyPath, &key);
            if (FAILED(hr)) {
                return hr;
            }
            hr = SetRegNone(key, std::wstring(kApiumProgId).c_str());
            RegCloseKey(key);
            if (FAILED(hr)) {
                return hr;
            }
        }

        {
            const std::wstring keyPath =
                L"Software\\Classes\\" + std::wstring(ext) + L"\\shellex\\" + std::wstring(kThumbnailProviderIidString);
            HKEY key = nullptr;
            hr = CreateRegKeyPath(HKEY_CURRENT_USER, keyPath, &key);
            if (FAILED(hr)) {
                return hr;
            }
            hr = SetRegSz(key, nullptr, thumbClsid);
            RegCloseKey(key);
            if (FAILED(hr)) {
                return hr;
            }
        }

        {
            const std::wstring keyPath =
                L"Software\\Classes\\" + std::wstring(ext) + L"\\shellex\\" + std::wstring(kPreviewHandlerIidString);
            HKEY key = nullptr;
            hr = CreateRegKeyPath(HKEY_CURRENT_USER, keyPath, &key);
            if (FAILED(hr)) {
                return hr;
            }
            hr = SetRegSz(key, nullptr, previewClsid);
            RegCloseKey(key);
            if (FAILED(hr)) {
                return hr;
            }
        }

        {
            const std::wstring keyPath =
                L"Software\\Classes\\SystemFileAssociations\\" + std::wstring(ext) + L"\\shellex\\" + std::wstring(kPreviewHandlerIidString);
            HKEY key = nullptr;
            hr = CreateRegKeyPath(HKEY_CURRENT_USER, keyPath, &key);
            if (FAILED(hr)) {
                return hr;
            }
            hr = SetRegSz(key, nullptr, previewClsid);
            RegCloseKey(key);
            if (FAILED(hr)) {
                return hr;
            }
        }

        {
            const std::wstring keyPath =
                L"Software\\Classes\\SystemFileAssociations\\" + std::wstring(ext) + L"\\shellex\\" + std::wstring(kThumbnailProviderIidString);
            HKEY key = nullptr;
            hr = CreateRegKeyPath(HKEY_CURRENT_USER, keyPath, &key);
            if (FAILED(hr)) {
                return hr;
            }
            hr = SetRegSz(key, nullptr, thumbClsid);
            RegCloseKey(key);
            if (FAILED(hr)) {
                return hr;
            }
        }

        if (IsPropertyExtension(ext)) {
            const std::wstring keyPath =
                L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\" + std::wstring(ext);
            HKEY key = nullptr;
            hr = CreateRegKeyPath(HKEY_CURRENT_USER, keyPath, &key);
            if (FAILED(hr)) {
                return hr;
            }
            hr = SetRegSz(key, nullptr, propertyClsid);
            RegCloseKey(key);
            if (FAILED(hr)) {
                return hr;
            }
        }

        if (IsPropertyExtension(ext)) {
            const std::wstring keyPath = L"Software\\Classes\\" + std::wstring(ext);
            HKEY key = nullptr;
            hr = CreateRegKeyPath(HKEY_CURRENT_USER, keyPath, &key);
            if (FAILED(hr)) {
                return hr;
            }
            hr = SetRegSz(key, L"PreviewDetails", kPreviewDetailsList);
            if (SUCCEEDED(hr)) {
                hr = SetRegSz(key, L"FullDetails", kPreviewDetailsList);
            }
            if (SUCCEEDED(hr)) {
                hr = SetRegSz(key, L"InfoTip", kInfoTipList);
            }
            RegCloseKey(key);
            if (FAILED(hr)) {
                return hr;
            }
        }

    }

    {
        const std::wstring keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";
        HKEY key = nullptr;
        hr = CreateRegKeyPath(HKEY_CURRENT_USER, keyPath, &key);
        if (FAILED(hr)) {
            return hr;
        }
        hr = SetRegSz(key, previewClsid.c_str(), L"Apium Slicer Preview Handler");
        RegCloseKey(key);
        if (FAILED(hr)) {
            return hr;
        }
    }

    TryRegisterPropertySchema();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    return S_OK;
}

HRESULT UnregisterProviderPerUser() {
    const std::wstring thumbClsid = GuidToString(CLSID_ApiumSlicerThumbnailProvider);
    const std::wstring previewClsid = GuidToString(CLSID_ApiumSlicerPreviewHandler);
    const std::wstring propertyClsid = GuidToString(CLSID_ApiumSlicerPropertyHandler);

    for (const std::wstring_view ext : kSupportedExtensions) {
        TryDeleteKeyTree(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\" + std::wstring(ext) + L"\\shellex\\" + std::wstring(kThumbnailProviderIidString));
        TryDeleteKeyTree(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\" + std::wstring(ext) + L"\\shellex\\" + std::wstring(kPreviewHandlerIidString));
        TryDeleteKeyTree(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\SystemFileAssociations\\" + std::wstring(ext) + L"\\shellex\\" + std::wstring(kPreviewHandlerIidString));
        TryDeleteKeyTree(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\SystemFileAssociations\\" + std::wstring(ext) + L"\\shellex\\" + std::wstring(kThumbnailProviderIidString));
        TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\" + std::wstring(ext) + L"\\shellex\\PropertyHandler");
        TryDeleteKeyTree(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\" + std::wstring(ext));
        TryDeleteValue(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\" + std::wstring(ext) + L"\\OpenWithProgids",
            std::wstring(kApiumProgId).c_str());
        TryDeleteDefaultValueIfEquals(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\" + std::wstring(ext),
            kApiumProgId);

        const std::wstring extKey = L"Software\\Classes\\" + std::wstring(ext);
        TryDeleteValue(HKEY_CURRENT_USER, extKey, L"PreviewDetails");
        TryDeleteValue(HKEY_CURRENT_USER, extKey, L"FullDetails");
        TryDeleteValue(HKEY_CURRENT_USER, extKey, L"InfoTip");

        for (const std::wstring& progId : ResolveProgIdsForExtension(ext)) {
            const std::wstring progIdKey = L"Software\\Classes\\" + progId;
            TryDeleteKeyTree(HKEY_CURRENT_USER, progIdKey + L"\\shellex\\" + std::wstring(kThumbnailProviderIidString));
            TryDeleteKeyTree(HKEY_CURRENT_USER, progIdKey + L"\\shellex\\" + std::wstring(kPreviewHandlerIidString));
            TryDeleteKeyTree(HKEY_CURRENT_USER, progIdKey + L"\\shellex\\PropertyHandler");
            TryDeleteValue(HKEY_CURRENT_USER, progIdKey, L"PreviewDetails");
            TryDeleteValue(HKEY_CURRENT_USER, progIdKey, L"FullDetails");
            TryDeleteValue(HKEY_CURRENT_USER, progIdKey, L"InfoTip");
        }
    }

    TryDeleteValue(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers",
        previewClsid.c_str());

    TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" + thumbClsid);
    TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" + previewClsid);
    TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" + propertyClsid);
    TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\" + std::wstring(kApiumProgId));
    TryDeleteKeyTree(HKEY_CURRENT_USER, std::wstring(kApiumCapabilitiesPath));
    TryDeleteValue(
        HKEY_CURRENT_USER,
        L"Software\\RegisteredApplications",
        std::wstring(kApiumRegisteredApplicationName).c_str());
    TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\AppID\\" + std::wstring(kLegacyPreviewHostAppId));

    TryUnregisterPropertySchema();

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    return S_OK;
}

class ThumbnailProvider final : public IThumbnailProvider,
                                public IInitializeWithStream,
                                public IInitializeWithFile,
                                public IInitializeWithItem {
public:
    ThumbnailProvider() : refCount_(1) {
        g_objCount.fetch_add(1);
    }
    ~ThumbnailProvider() {
        g_objCount.fetch_sub(1);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        *ppvObject = nullptr;

        if (riid == IID_IUnknown || riid == IID_IThumbnailProvider) {
            *ppvObject = static_cast<IThumbnailProvider*>(this);
        } else if (riid == IID_IInitializeWithStream) {
            *ppvObject = static_cast<IInitializeWithStream*>(this);
        } else if (riid == IID_IInitializeWithFile) {
            *ppvObject = static_cast<IInitializeWithFile*>(this);
        } else if (riid == IID_IInitializeWithItem) {
            *ppvObject = static_cast<IInitializeWithItem*>(this);
        } else {
            DebugLogWin(L"ThumbnailProvider QueryInterface: unsupported riid=" + GuidToString(riid));
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG value = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (value == 0) {
            delete this;
        }
        return value;
    }

    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (initialized_) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        if (!stream) {
            return E_INVALIDARG;
        }
        if (!ReadPreviewStream(stream, fileBytes_, metadata_)) {
            DebugLogWin(L"ThumbnailProvider Initialize(IStream): ReadPreviewStream failed");
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
            sourceFilePath_.clear();
            initialized_ = true;
            return S_OK;
        }
        sourceFilePath_.clear();
        initialized_ = true;
        DebugLogWin(
            L"ThumbnailProvider Initialize(IStream): success, container=" + metadata_.diagnostics.container +
            L", isApium=" + std::wstring(metadata_.isApium ? L"true" : L"false"));
        return S_OK;
    }

    IFACEMETHODIMP Initialize(LPCWSTR filePath, DWORD) override {
        if (initialized_) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        if (!filePath) {
            return E_INVALIDARG;
        }
        sourceFilePath_ = filePath;
        if (!ReadPreviewFile(filePath, fileBytes_, metadata_)) {
            DebugLogWin(L"ThumbnailProvider Initialize(IFile): ReadPreviewFile failed");
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
            initialized_ = true;
            return S_OK;
        }
        initialized_ = true;
        DebugLogWin(
            L"ThumbnailProvider Initialize(IFile): success, container=" + metadata_.diagnostics.container +
            L", isApium=" + std::wstring(metadata_.isApium ? L"true" : L"false"));
        return S_OK;
    }

    IFACEMETHODIMP Initialize(IShellItem* item, DWORD) override {
        if (initialized_) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        if (!item) {
            return E_INVALIDARG;
        }

        PWSTR filePath = nullptr;
        const HRESULT displayNameHr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
        if (FAILED(displayNameHr) || !filePath) {
            DebugLogWin(L"ThumbnailProvider Initialize(IShellItem): GetDisplayName failed, hr=" + HResultToString(displayNameHr));
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
            initialized_ = true;
            return S_OK;
        }

        std::wstring path(filePath);
        CoTaskMemFree(filePath);
        sourceFilePath_ = path;
        if (!ReadPreviewFile(path.c_str(), fileBytes_, metadata_)) {
            DebugLogWin(L"ThumbnailProvider Initialize(IShellItem): ReadPreviewFile failed for " + path);
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
        }
        initialized_ = true;
        DebugLogWin(
            L"ThumbnailProvider Initialize(IShellItem): success, container=" + metadata_.diagnostics.container +
            L", isApium=" + std::wstring(metadata_.isApium ? L"true" : L"false"));
        return S_OK;
    }

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override {
        if (!phbmp || !pdwAlpha) {
            return E_POINTER;
        }
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_UNKNOWN;
        if (!initialized_) {
            return E_UNEXPECTED;
        }

        HBITMAP meshBitmap = nullptr;
        MeshPreviewResult meshResult = RenderMeshPreviewToHBitmap(fileBytes_, sourceFilePath_, cx, &meshBitmap);
        if (meshResult.rendered && meshBitmap) {
            DebugLogWin(
                L"ThumbnailProvider GetThumbnail: mesh rendered, format=" + meshResult.format +
                L", triangles=" + std::to_wstring(meshResult.triangles));
            *phbmp = meshBitmap;
            *pdwAlpha = WTSAT_ARGB;
            return S_OK;
        }
        if (meshResult.isMesh || IsMeshPreviewExtension(sourceFilePath_)) {
            DebugLogWin(
                L"ThumbnailProvider GetThumbnail: mesh render unavailable, status=" + meshResult.status +
                L", hint=" + (sourceFilePath_.empty() ? std::wstring(L"<stream>") : sourceFilePath_));
            if (IsMeshPreviewExtension(sourceFilePath_)) {
                return E_FAIL;
            }
        }

        std::vector<uint8_t> pngBytes = metadata_.thumbnailBytes;
        if (pngBytes.empty()) {
            if (!LoadFallbackPng(g_module, pngBytes)) {
                return E_FAIL;
            }
        }

        HBITMAP bmp = nullptr;
        if (!DecodePngToHBitmap(pngBytes, cx, &bmp) || !bmp) {
            if (!LoadFallbackPng(g_module, pngBytes) || !DecodePngToHBitmap(pngBytes, cx, &bmp) || !bmp) {
                return E_FAIL;
            }
        }

        *phbmp = bmp;
        *pdwAlpha = WTSAT_ARGB;
        return S_OK;
    }

private:
    LONG refCount_ = 1;
    bool initialized_ = false;
    std::vector<uint8_t> fileBytes_;
    std::wstring sourceFilePath_;
    ApiumMetadata metadata_;
};

class PreviewHandler final : public IPreviewHandler,
                             public IPreviewHandlerVisuals,
                             public IOleWindow,
                             public IInitializeWithStream,
                             public IInitializeWithFile,
                             public IInitializeWithItem,
                             public IObjectWithSite {
public:
    PreviewHandler() : refCount_(1) {
        g_objCount.fetch_add(1);
        hostProcessName_ = GetCurrentProcessName();
        initPath_ = L"unknown";
        RefreshSystemColors();
    }
    ~PreviewHandler() {
        CleanupWindow();
        g_objCount.fetch_sub(1);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        *ppvObject = nullptr;

        if (riid == IID_IUnknown || riid == IID_IPreviewHandler) {
            *ppvObject = static_cast<IPreviewHandler*>(this);
        } else if (riid == IID_IPreviewHandlerVisuals) {
            *ppvObject = static_cast<IPreviewHandlerVisuals*>(this);
        } else if (riid == IID_IOleWindow) {
            *ppvObject = static_cast<IOleWindow*>(this);
        } else if (riid == IID_IInitializeWithStream) {
            *ppvObject = static_cast<IInitializeWithStream*>(this);
        } else if (riid == IID_IInitializeWithFile) {
            *ppvObject = static_cast<IInitializeWithFile*>(this);
        } else if (riid == IID_IInitializeWithItem) {
            *ppvObject = static_cast<IInitializeWithItem*>(this);
        } else if (riid == IID_IObjectWithSite) {
            *ppvObject = static_cast<IObjectWithSite*>(this);
        } else {
            DebugLogWin(L"PreviewHandler QueryInterface: unsupported riid=" + GuidToString(riid));
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP SetBackgroundColor(COLORREF color) override {
        hostBackgroundColor_ = color;
        RefreshSystemColors();
        if (hwndPreview_) {
            InvalidateRect(hwndPreview_, nullptr, TRUE);
        }
        DebugLogWin(L"PreviewHandlerVisuals SetBackgroundColor");
        return S_OK;
    }

    IFACEMETHODIMP SetFont(const LOGFONTW* plf) override {
        if (!plf) {
            return E_INVALIDARG;
        }
        HFONT nextFont = CreateFontIndirectW(plf);
        if (!nextFont) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        font_.reset(nextFont);
        if (hwndText_) {
            SendMessageW(hwndText_, WM_SETFONT, reinterpret_cast<WPARAM>(font_.get()), TRUE);
        }
        if (hwndFooter_) {
            SendMessageW(hwndFooter_, WM_SETFONT, reinterpret_cast<WPARAM>(font_.get()), TRUE);
        }
        DebugLogWin(L"PreviewHandlerVisuals SetFont");
        return S_OK;
    }

    IFACEMETHODIMP SetTextColor(COLORREF color) override {
        hostTextColor_ = color;
        RefreshSystemColors();
        DebugLogWin(L"PreviewHandlerVisuals SetTextColor");
        return S_OK;
    }

    IFACEMETHODIMP Initialize(IShellItem* item, DWORD) override {
        if (initialized_) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        if (!item) {
            return E_INVALIDARG;
        }

        PWSTR filePath = nullptr;
        const HRESULT displayNameHr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
        if (FAILED(displayNameHr) || !filePath) {
            DebugLogWin(L"PreviewHandler Initialize(IShellItem): GetDisplayName(SIGDN_FILESYSPATH) failed, hr=" + HResultToString(displayNameHr));
            initPath_ = L"IShellItem";
            sourceFilePath_.clear();
            metadata_ = ApiumMetadata{};
            metadata_.diagnostics.container = L"unknown";
            metadata_.diagnostics.configPartStatus = L"shell item path unavailable";
            metadata_.diagnostics.decode = L"n/a";
            metadata_.diagnostics.apiumDetect = L"n/a";
            metadata_.diagnostics.missingFields = L"file path unavailable";
            initialized_ = true;
            return S_OK;
        }

        std::wstring path(filePath);
        CoTaskMemFree(filePath);

        std::vector<uint8_t> fileBytes;
        if (!ReadPreviewFile(path.c_str(), fileBytes, metadata_)) {
            DebugLogWin(L"PreviewHandler Initialize(IShellItem): ReadPreviewFile failed for " + path);
            fileBytes.clear();
            metadata_ = ApiumMetadata{};
            metadata_.diagnostics.container = L"unknown";
            metadata_.diagnostics.configPartStatus = L"shell item file read failed";
            metadata_.diagnostics.decode = L"n/a";
            metadata_.diagnostics.apiumDetect = L"n/a";
            metadata_.diagnostics.missingFields = L"file bytes unavailable";
        }

        initPath_ = L"IShellItem";
        sourceFilePath_ = std::move(path);
        LoadMeshInfo(fileBytes);
        initialized_ = true;
        DebugLogWin(
            L"PreviewHandler Initialize(IShellItem): host=" + hostProcessName_ +
            L", container=" + metadata_.diagnostics.container +
            L", isApium=" + std::wstring(metadata_.isApium ? L"true" : L"false") +
            L", meshCandidate=" + std::wstring(knownMeshFile_ ? L"true" : L"false"));
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG value = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (value == 0) {
            delete this;
        }
        return value;
    }

    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (initialized_) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        if (!stream) {
            return E_INVALIDARG;
        }
        std::vector<uint8_t> fileBytes;
        if (!ReadPreviewStream(stream, fileBytes, metadata_)) {
            DebugLogWin(L"PreviewHandler Initialize(IStream): ReadPreviewStream failed");
            fileBytes.clear();
            metadata_ = ApiumMetadata{};
            metadata_.diagnostics.container = L"unknown";
            metadata_.diagnostics.configPartStatus = L"stream read failed";
            metadata_.diagnostics.decode = L"n/a";
            metadata_.diagnostics.apiumDetect = L"n/a";
            metadata_.diagnostics.missingFields = L"file bytes unavailable";
        }
        initPath_ = L"IStream";
        sourceFilePath_.clear();
        LoadMeshInfo(fileBytes);
        initialized_ = true;
        DebugLogWin(
            L"PreviewHandler Initialize(IStream): host=" + hostProcessName_ +
            L", container=" + metadata_.diagnostics.container +
            L", isApium=" + std::wstring(metadata_.isApium ? L"true" : L"false") +
            L", meshCandidate=" + std::wstring(knownMeshFile_ ? L"true" : L"false"));
        return S_OK;
    }

    IFACEMETHODIMP Initialize(LPCWSTR filePath, DWORD) override {
        if (initialized_) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        if (!filePath) {
            return E_INVALIDARG;
        }
        std::vector<uint8_t> fileBytes;
        if (!ReadPreviewFile(filePath, fileBytes, metadata_)) {
            DebugLogWin(L"PreviewHandler Initialize(IFile): ReadPreviewFile failed");
            fileBytes.clear();
            metadata_ = ApiumMetadata{};
            metadata_.diagnostics.container = L"unknown";
            metadata_.diagnostics.configPartStatus = L"file read failed";
            metadata_.diagnostics.decode = L"n/a";
            metadata_.diagnostics.apiumDetect = L"n/a";
            metadata_.diagnostics.missingFields = L"file bytes unavailable";
        }
        initPath_ = L"IFile";
        sourceFilePath_ = filePath;
        LoadMeshInfo(fileBytes);
        initialized_ = true;
        DebugLogWin(
            L"PreviewHandler Initialize(IFile): host=" + hostProcessName_ +
            L", container=" + metadata_.diagnostics.container +
            L", isApium=" + std::wstring(metadata_.isApium ? L"true" : L"false") +
            L", meshCandidate=" + std::wstring(knownMeshFile_ ? L"true" : L"false"));
        return S_OK;
    }

    IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* prc) override {
        if (!hwnd || !prc) {
            return E_INVALIDARG;
        }
        hwndParent_ = hwnd;
        rect_ = *prc;
        DebugLogWin(
            L"PreviewHandler SetWindow: hwnd=" + std::to_wstring(reinterpret_cast<uintptr_t>(hwndParent_)) +
            L", rect=" + std::to_wstring(rect_.left) + L"," + std::to_wstring(rect_.top) +
            L"," + std::to_wstring(rect_.right) + L"," + std::to_wstring(rect_.bottom));
        Layout();
        return S_OK;
    }

    IFACEMETHODIMP SetRect(const RECT* prc) override {
        if (!prc) {
            return E_INVALIDARG;
        }
        rect_ = *prc;
        DebugLogWin(
            L"PreviewHandler SetRect: rect=" + std::to_wstring(rect_.left) + L"," + std::to_wstring(rect_.top) +
            L"," + std::to_wstring(rect_.right) + L"," + std::to_wstring(rect_.bottom));
        Layout();
        return S_OK;
    }

    IFACEMETHODIMP DoPreview() override {
        if (!initialized_) {
            return E_UNEXPECTED;
        }
        if (!hwndParent_) {
            return E_UNEXPECTED;
        }
        const std::wstring fileHint = sourceFilePath_.empty() ? L"<stream>" : sourceFilePath_;
        DebugLogWin(
            L"PreviewHandler DoPreview: host=" + hostProcessName_ +
            L", pid=" + std::to_wstring(GetCurrentProcessId()) +
            L", initPath=" + initPath_ +
            L", file=" + fileHint +
            L", isApium=" + std::wstring(metadata_.isApium ? L"true" : L"false") +
            L", container=" + metadata_.diagnostics.container +
            L", meshCandidate=" + std::wstring(knownMeshFile_ ? L"true" : L"false"));
        if (hwndPreview_) {
            Layout();
            ShowWindow(hwndPreview_, SW_SHOWNA);
            UpdateWindow(hwndPreview_);
            DebugLogWin(L"PreviewHandler DoPreview: reused existing preview window");
            return S_OK;
        }
        if (!EnsurePreviewPaneClass()) {
            DebugLogWin(L"PreviewHandler DoPreview: EnsurePreviewPaneClass failed");
            return E_FAIL;
        }
        if (!EnsurePreviewImageClass()) {
            DebugLogWin(L"PreviewHandler DoPreview: EnsurePreviewImageClass failed");
            return E_FAIL;
        }
        EnsureRichEditLoaded();

        RECT initialRect = rect_;
        int initialWidth = initialRect.right - initialRect.left;
        int initialHeight = initialRect.bottom - initialRect.top;
        if ((initialWidth <= 0 || initialHeight <= 0) && hwndParent_) {
            RECT parentClient = {};
            if (GetClientRect(hwndParent_, &parentClient)) {
                initialRect = parentClient;
                initialWidth = initialRect.right - initialRect.left;
                initialHeight = initialRect.bottom - initialRect.top;
            }
        }
        initialWidth = std::max(1, initialWidth);
        initialHeight = std::max(1, initialHeight);

        hwndPreview_ = CreateWindowExW(
            0,
            kPreviewPaneWindowClassName,
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            initialRect.left,
            initialRect.top,
            initialWidth,
            initialHeight,
            hwndParent_,
            nullptr,
            g_module,
            this);
        if (!hwndPreview_) {
            const DWORD error = GetLastError();
            DebugLogWin(L"PreviewHandler DoPreview: Create preview pane failed, error=" + std::to_wstring(error));
            return HRESULT_FROM_WIN32(error);
        }

        hwndImage_ = CreateWindowExW(
            0,
            kPreviewImageWindowClassName,
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0,
            0,
            10,
            10,
            hwndPreview_,
            nullptr,
            g_module,
            this);
        if (!hwndImage_) {
            const DWORD error = GetLastError();
            DebugLogWin(L"PreviewHandler DoPreview: Create image control failed, error=" + std::to_wstring(error));
            CleanupWindow();
            return HRESULT_FROM_WIN32(error);
        }

        hwndText_ = CreateWindowExW(
            0,
            MSFTEDIT_CLASS,
            nullptr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            0,
            0,
            10,
            10,
            hwndPreview_,
            nullptr,
            g_module,
            nullptr);
        if (!hwndText_) {
            const DWORD richEditError = GetLastError();
            DebugLogWin(L"PreviewHandler DoPreview: Create RichEdit failed, retrying EDIT, error=" + std::to_wstring(richEditError));
            hwndText_ = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                nullptr,
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                0,
                0,
                10,
                10,
                hwndPreview_,
                nullptr,
                g_module,
                nullptr);
            richEditActive_ = false;
        } else {
            richEditActive_ = true;
        }
        if (!hwndText_) {
            const DWORD error = GetLastError();
            DebugLogWin(L"PreviewHandler DoPreview: Create EDIT fallback failed, error=" + std::to_wstring(error));
            CleanupWindow();
            return HRESULT_FROM_WIN32(error);
        }

        hwndFooter_ = CreateWindowExW(
            0,
            L"STATIC",
            nullptr,
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0,
            0,
            10,
            10,
            hwndPreview_,
            nullptr,
            g_module,
            nullptr);
        if (!hwndFooter_) {
            const DWORD error = GetLastError();
            DebugLogWin(L"PreviewHandler DoPreview: Create footer control failed, error=" + std::to_wstring(error));
            CleanupWindow();
            return HRESULT_FROM_WIN32(error);
        }

        const std::wstring footer = L"Apium Preview v" + std::wstring(ASXP_VERSION_W);
        SetWindowTextW(hwndFooter_, footer.c_str());

        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
            font_.reset(CreateFontIndirectW(&metrics.lfMessageFont));
        }
        if (font_) {
            SendMessageW(hwndText_, WM_SETFONT, reinterpret_cast<WPARAM>(font_.get()), TRUE);
            SendMessageW(hwndFooter_, WM_SETFONT, reinterpret_cast<WPARAM>(font_.get()), TRUE);
        }
        SendMessageW(hwndText_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        RefreshSystemColors();

        RenderPreviewContent();

        Layout();
        ShowWindow(hwndPreview_, SW_SHOWNA);
        UpdateWindow(hwndPreview_);
        DebugLogWin(
            L"PreviewHandler DoPreview: created preview window successfully, hwnd=" +
            std::to_wstring(reinterpret_cast<uintptr_t>(hwndPreview_)));
        return S_OK;
    }

    IFACEMETHODIMP Unload() override {
        DebugLogWin(
            L"PreviewHandler Unload: hwnd=" +
            std::to_wstring(reinterpret_cast<uintptr_t>(hwndPreview_)));
        CleanupWindow();
        initialized_ = false;
        sourceFileSize_ = 0;
        metadata_ = ApiumMetadata{};
        sourceFilePath_.clear();
        meshInfo_ = MeshPreviewResult{};
        meshGeometry_ = {};
        knownMeshFile_ = false;
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() override {
        if (hwndText_) {
            ::SetFocus(hwndText_);
            return S_OK;
        }
        if (hwndPreview_) {
            ::SetFocus(hwndPreview_);
            return S_OK;
        }
        return S_FALSE;
    }

    IFACEMETHODIMP QueryFocus(HWND* phwnd) override {
        if (!phwnd) {
            return E_POINTER;
        }
        *phwnd = GetFocus();
        return S_OK;
    }

    IFACEMETHODIMP TranslateAccelerator(MSG* msg) override {
        return S_FALSE;
    }

    IFACEMETHODIMP GetWindow(HWND* phwnd) override {
        if (!phwnd) {
            return E_POINTER;
        }
        *phwnd = hwndPreview_ ? hwndPreview_ : hwndParent_;
        DebugLogWin(
            L"PreviewHandler GetWindow: returning hwnd=" +
            std::to_wstring(reinterpret_cast<uintptr_t>(*phwnd)));
        return S_OK;
    }

    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override {
        return E_NOTIMPL;
    }

    IFACEMETHODIMP SetSite(IUnknown* pUnkSite) override {
        site_ = pUnkSite;
        return S_OK;
    }

    IFACEMETHODIMP GetSite(REFIID riid, void** ppvSite) override {
        if (!ppvSite) {
            return E_POINTER;
        }
        *ppvSite = nullptr;
        if (!site_) {
            return E_FAIL;
        }
        return site_->QueryInterface(riid, ppvSite);
    }

private:
    void LoadMeshInfo(const std::vector<uint8_t>& fileBytes) {
        sourceFileSize_ = fileBytes.size();
        knownMeshFile_ = IsMeshPreviewExtension(sourceFilePath_);
        if (knownMeshFile_ || sourceFilePath_.empty()) {
            meshInfo_ = AnalyzeMeshPreview(fileBytes, sourceFilePath_, &meshGeometry_);
            knownMeshFile_ = knownMeshFile_ || meshInfo_.isMesh;
        }
    }

    struct DeleteObjectDeleter {
        void operator()(HGDIOBJ obj) const {
            if (obj) {
                DeleteObject(obj);
            }
        }
    };

    static ATOM RegisterPreviewWindowClass(const wchar_t* name, WNDPROC proc) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = proc;
        wc.hInstance = g_module;
        wc.lpszClassName = name;
        const ATOM atom = RegisterClassW(&wc);
        return atom == 0 && GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 1 : atom;
    }

    static ATOM EnsurePreviewPaneClass() {
        static const ATOM atom = RegisterPreviewWindowClass(kPreviewPaneWindowClassName, PreviewPaneWndProc);
        return atom;
    }

    static ATOM EnsurePreviewImageClass() {
        static const ATOM atom = RegisterPreviewWindowClass(kPreviewImageWindowClassName, PreviewImageWndProc);
        return atom;
    }

    static void EnsureRichEditLoaded() {
        static std::once_flag once;
        std::call_once(once, []() {
            (void)LoadLibraryW(L"Msftedit.dll");
        });
    }

    static LRESULT CALLBACK PreviewPaneWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        PreviewHandler* self = reinterpret_cast<PreviewHandler*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<PreviewHandler*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        if (self) {
            if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT) {
                return reinterpret_cast<LRESULT>(self->OnCtlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
            }
            if (msg == WM_ERASEBKGND) {
                return self->OnEraseBackground(reinterpret_cast<HDC>(wParam));
            }
            if (msg == WM_SETTINGCHANGE || msg == WM_THEMECHANGED || msg == WM_SYSCOLORCHANGE) {
                self->RefreshSystemColors();
                self->Layout();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK PreviewImageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        PreviewHandler* self = reinterpret_cast<PreviewHandler*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<PreviewHandler*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        if (self) {
            if (msg == WM_ERASEBKGND) {
                return 1;
            }
            if (msg == WM_PAINT) {
                self->OnImagePaint(hwnd);
                return 0;
            }
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

#if ASXP_INTERACTIVE_MESH_PREVIEW
    static LRESULT CALLBACK MeshViewportWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<PreviewHandler*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<PreviewHandler*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self || !self->meshViewport_) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            self->PaintMeshViewport(hwnd);
            return 0;
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wParam == kMeshPresentRetryTimer) {
                KillTimer(hwnd, kMeshPresentRetryTimer);
                self->meshPresentRetryPending_ = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            ::SetFocus(hwnd);
            SetCapture(hwnd);
            self->draggingMesh_ = GetCapture() == hwnd;
            self->lastMeshDrag_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            return 0;
        case WM_MOUSEMOVE:
            if (self->draggingMesh_) {
                const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (point.x != self->lastMeshDrag_.x || point.y != self->lastMeshDrag_.y) {
                    self->meshViewport_->Rotate(point.x - self->lastMeshDrag_.x, point.y - self->lastMeshDrag_.y);
                    self->lastMeshDrag_ = point;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            break;
        case WM_LBUTTONUP:
        case WM_CANCELMODE:
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            self->draggingMesh_ = false;
            return 0;
        case WM_MOUSEWHEEL: {
            RECT area = {};
            GetWindowRect(hwnd, &area);
            if (PtInRect(&area, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) {
                if (self->meshViewport_->Zoom(GET_WHEEL_DELTA_WPARAM(wParam))) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            break;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                static const HCURSOR hand = LoadCursorW(nullptr, IDC_HAND);
                SetCursor(hand);
                return TRUE;
            }
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool EnsureMeshViewport(UINT size) {
        if (meshViewport_) {
            return true;
        }
        if (meshViewportFailed_ || meshGeometry_.triangles.empty()) {
            return false;
        }

        static const ATOM atom = RegisterPreviewWindowClass(kMeshViewportWindowClassName, MeshViewportWndProc);
        if (atom) {
            hwndMeshViewport_ = CreateWindowExW(0, kMeshViewportWindowClassName, nullptr, WS_CHILD,
                0, 0, size, size, hwndImage_, nullptr, g_module, this);
        }
        HRESULT hr = E_FAIL;
        if (hwndMeshViewport_) {
            auto viewport = std::unique_ptr<MeshViewport>(new (std::nothrow) MeshViewport());
            hr = viewport ? viewport->Initialize(hwndMeshViewport_, size, size, meshGeometry_) : E_OUTOFMEMORY;
            if (SUCCEEDED(hr)) {
                meshViewport_ = std::move(viewport);
                return true;
            }
        }
        DebugLogWin(L"PreviewHandler: Direct3D mesh initialization failed, hr=" + HResultToString(hr));
        DestroyMeshViewport();
        meshViewportFailed_ = true;
        return false;
    }

    void PaintMeshViewport(HWND hwnd) {
        PAINTSTRUCT paint = {};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        if (meshPresentRetryPending_) {
            return;
        }
        RECT area = {};
        GetClientRect(hwnd, &area);
        const HRESULT hr = meshViewport_->Render(area.right, area.bottom, backgroundColor_);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            meshPresentRetryPending_ = SetTimer(hwnd, kMeshPresentRetryTimer, 16, nullptr) != 0;
        } else if (FAILED(hr)) {
            DebugLogWin(L"PreviewHandler: Direct3D mesh render failed, hr=" + HResultToString(hr));
            DestroyMeshViewport();
            meshViewportFailed_ = true;
            Layout();
        }
    }

    void DestroyMeshViewport() {
        if (hwndMeshViewport_) {
            if (GetCapture() == hwndMeshViewport_) {
                ReleaseCapture();
            }
            meshViewport_.reset();
            DestroyWindow(hwndMeshViewport_);
            hwndMeshViewport_ = nullptr;
        }
        draggingMesh_ = false;
        meshPresentRetryPending_ = false;
    }
#endif

    HBRUSH OnCtlColor(HDC hdc, HWND) {
        if (!backgroundBrush_) {
            RefreshSystemColors();
        }
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, backgroundColor_);
        ::SetTextColor(hdc, textColor_);
        return backgroundBrush_.get();
    }

    LRESULT OnEraseBackground(HDC hdc) {
        if (!backgroundBrush_) {
            RefreshSystemColors();
        }
        RECT rc = {};
        GetClientRect(hwndPreview_, &rc);
        FillRect(hdc, &rc, backgroundBrush_.get());
        return 1;
    }

    void OnImagePaint(HWND hwnd) {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!hdc) {
            return;
        }

        RECT rc = {};
        GetClientRect(hwnd, &rc);
        if (!backgroundBrush_) {
            RefreshSystemColors();
        }
        FillRect(hdc, &rc, backgroundBrush_.get());

        if (bitmap_ && bitmapPixelWidth_ > 0 && bitmapPixelHeight_ > 0) {
            HDC memDc = CreateCompatibleDC(hdc);
            if (memDc) {
                HGDIOBJ oldBitmap = SelectObject(memDc, bitmap_.get());
                BLENDFUNCTION blend = {};
                blend.BlendOp = AC_SRC_OVER;
                blend.SourceConstantAlpha = 255;
                blend.AlphaFormat = AC_SRC_ALPHA;
                (void)AlphaBlend(
                    hdc,
                    0,
                    0,
                    rc.right - rc.left,
                    rc.bottom - rc.top,
                    memDc,
                    0,
                    0,
                    bitmapPixelWidth_,
                    bitmapPixelHeight_,
                    blend);
                SelectObject(memDc, oldBitmap);
                DeleteDC(memDc);
            }
        }

        EndPaint(hwnd, &ps);
    }

    void RefreshSystemColors() {
        constexpr COLORREF light = RGB(255, 255, 255);
        const bool explorerHost = _wcsicmp(hostProcessName_.c_str(), L"prevhost.exe") == 0;
        const COLORREF dark = explorerHost ? RGB(0x20, 0x20, 0x20) : RGB(0x19, 0x19, 0x19);
        const bool darkMode = IsAppsDarkModeEnabled();
        backgroundColor_ = hostBackgroundColor_.value_or(darkMode ? dark : light);
        // Explorer's Preview Host can pass white even when the shell uses dark mode.
        const bool useExplorerDarkPalette = explorerHost && darkMode && backgroundColor_ == light;
        if (useExplorerDarkPalette) {
            backgroundColor_ = dark;
        }
        const int brightness = 299 * GetRValue(backgroundColor_) + 587 * GetGValue(backgroundColor_) + 114 * GetBValue(backgroundColor_);
        textColor_ = brightness < 128000 ? light : RGB(0, 0, 0);
        if (!useExplorerDarkPalette && hostTextColor_ && *hostTextColor_ != backgroundColor_) {
            textColor_ = *hostTextColor_;
        }
        backgroundBrush_.reset(CreateSolidBrush(backgroundColor_));
        if (hwndText_) {
            SendMessageW(hwndText_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(backgroundColor_));
            RenderPreviewContent();
            InvalidateRect(hwndText_, nullptr, TRUE);
        }
        if (hwndFooter_) {
            InvalidateRect(hwndFooter_, nullptr, TRUE);
        }
        if (hwndImage_) {
            InvalidateRect(hwndImage_, nullptr, TRUE);
        }
#if ASXP_INTERACTIVE_MESH_PREVIEW
        if (hwndMeshViewport_) {
            InvalidateRect(hwndMeshViewport_, nullptr, FALSE);
        }
#endif
    }

    void CleanupWindow() {
#if ASXP_INTERACTIVE_MESH_PREVIEW
        DestroyMeshViewport();
        meshViewportFailed_ = false;
#endif
        bitmap_.reset();
        bitmapPixelWidth_ = 0;
        bitmapPixelHeight_ = 0;
        meshBitmapSize_ = 0;
        if (hwndText_) {
            DestroyWindow(hwndText_);
            hwndText_ = nullptr;
        }
        if (hwndFooter_) {
            DestroyWindow(hwndFooter_);
            hwndFooter_ = nullptr;
        }
        if (hwndImage_) {
            DestroyWindow(hwndImage_);
            hwndImage_ = nullptr;
        }
        if (hwndPreview_) {
            DestroyWindow(hwndPreview_);
            hwndPreview_ = nullptr;
        }
        font_.reset();
        previewRtf_.clear();
        previewText_.clear();
        richEditActive_ = true;
    }

    void RenderPreviewContent() {
        if (!hwndText_) {
            return;
        }

        std::vector<PreviewRow> rows = knownMeshFile_
            ? BuildMeshPreviewRows(meshInfo_, sourceFilePath_, sourceFileSize_)
            : BuildPreviewRows(metadata_);
        std::vector<std::wstring> extraLines;
        if (knownMeshFile_) {
            if (!meshInfo_.isMesh) {
                extraLines.push_back(L"Mesh metadata could not be parsed.");
            }
        } else if (!metadata_.isApium) {
            extraLines = BuildNoMetadataLines(metadata_, initPath_, hostProcessName_);
        } else if (rows.empty()) {
            extraLines.push_back(L"No Apium metadata values available.");
        }

        SendMessageW(hwndText_, WM_SETREDRAW, FALSE, 0);

        if (richEditActive_) {
            previewRtf_ = BuildPreviewRtf(rows, extraLines, textColor_, backgroundColor_);

            SETTEXTEX setText = {};
            setText.flags = ST_DEFAULT;
            setText.codepage = CP_ACP;
            SendMessageA(hwndText_, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&setText), reinterpret_cast<LPARAM>(previewRtf_.c_str()));

            CHARFORMAT2W normalFormat = {};
            normalFormat.cbSize = sizeof(normalFormat);
            normalFormat.dwMask = CFM_COLOR | CFM_BACKCOLOR;
            normalFormat.crTextColor = textColor_;
            normalFormat.crBackColor = backgroundColor_;
            SendMessageW(hwndText_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&normalFormat));
        } else {
            previewText_ = BuildPreviewPlainText(rows, extraLines);
            SetWindowTextW(hwndText_, previewText_.c_str());
        }

        SendMessageW(hwndText_, EM_SETSEL, -1, -1);
        SendMessageW(hwndText_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hwndText_, nullptr, TRUE);
    }

    void EnsurePreviewBitmap(UINT maxSize) {
        if (!hwndImage_ || maxSize == 0) {
            return;
        }

        HBITMAP nextBitmap = nullptr;
        if (knownMeshFile_) {
            if (meshGeometry_.triangles.empty() || meshBitmapSize_ == maxSize) {
                return;
            }
            nextBitmap = RenderMeshPreviewBitmap(meshGeometry_, maxSize);
            if (nextBitmap) {
                meshBitmapSize_ = maxSize;
            }
        } else {
            std::vector<uint8_t> png = metadata_.thumbnailBytes;
            if (png.empty()) {
                (void)LoadFallbackPng(g_module, png);
            }
            if (!png.empty()) {
                (void)DecodePngToHBitmap(png, maxSize, &nextBitmap);
            }
        }
        if (!nextBitmap) {
            return;
        }

        if (!GetBitmapSize(nextBitmap, bitmapPixelWidth_, bitmapPixelHeight_)) {
            bitmapPixelWidth_ = 0;
            bitmapPixelHeight_ = 0;
        }

        if (bitmap_) {
            bitmap_.reset();
        }
        bitmap_.reset(nextBitmap);
        InvalidateRect(hwndImage_, nullptr, TRUE);
    }

    void Layout() {
        if (!hwndPreview_ || !hwndImage_ || !hwndText_ || !hwndFooter_) {
            return;
        }

        RECT target = rect_;
        int width = target.right - target.left;
        int height = target.bottom - target.top;
        if ((width <= 0 || height <= 0) && hwndParent_) {
            RECT parentClient = {};
            if (GetClientRect(hwndParent_, &parentClient)) {
                target = parentClient;
                width = target.right - target.left;
                height = target.bottom - target.top;
            }
        }
        if (width <= 0 || height <= 0) {
            DebugLogWin(
                L"PreviewHandler Layout: skipped, rect=" +
                std::to_wstring(rect_.left) + L"," + std::to_wstring(rect_.top) +
                L"," + std::to_wstring(rect_.right) + L"," + std::to_wstring(rect_.bottom));
            return;
        }

        SetWindowPos(hwndPreview_, HWND_TOP, target.left, target.top, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        DebugLogWin(
            L"PreviewHandler Layout: target=" +
            std::to_wstring(target.left) + L"," + std::to_wstring(target.top) +
            L"," + std::to_wstring(target.right) + L"," + std::to_wstring(target.bottom) +
            L", size=" + std::to_wstring(width) + L"x" + std::to_wstring(height));

        const int footerHeight = std::min(24, std::max(0, height - 2));
        const int contentHeight = std::max(1, height - footerHeight);
        const int imageTop = std::min(8, contentHeight / 4);
        const int imagePadding = 8;
        const int imageAvailableWidth = std::max(1, width - (imagePadding * 2));
        const int imageAvailableHeight = std::max(1, (contentHeight * 55) / 100 - imageTop);
        const int imageBottomGap = 3;

        const int imageSize = std::min(imageAvailableWidth, imageAvailableHeight);
        const int detailsReserve = std::min(96, contentHeight / 3);
        const int viewportSize = std::min({width, contentHeight - 1,
            std::max(1, contentHeight - detailsReserve - imageBottomGap)});
#if ASXP_INTERACTIVE_MESH_PREVIEW
        const bool interactive = EnsureMeshViewport(static_cast<UINT>(std::max(1, viewportSize)));
#else
        constexpr bool interactive = false;
#endif
        if (!interactive) {
            EnsurePreviewBitmap(static_cast<UINT>(imageSize));
        }

        ShowWindow(hwndText_, SW_SHOWNA);
        ShowWindow(hwndFooter_, SW_SHOWNA);

        int renderW = imageAvailableWidth;
        int renderH = imageAvailableHeight;
        if (interactive) {
            renderW = renderH = viewportSize;
        } else if (bitmapPixelWidth_ > 0 && bitmapPixelHeight_ > 0) {
            renderW = std::min(imageAvailableWidth, bitmapPixelWidth_);
            renderH = std::min(imageAvailableHeight, bitmapPixelHeight_);
        }

        const int imageX = (width - renderW) / 2;
        const int imageY = interactive ? 0 : imageTop;
        SetWindowPos(hwndImage_, nullptr, imageX, imageY, renderW, renderH, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
#if ASXP_INTERACTIVE_MESH_PREVIEW
        if (interactive) {
            SetWindowPos(hwndMeshViewport_, nullptr, 0, 0, renderW, renderH, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
#endif

        const int textTop = std::min(imageY + renderH + imageBottomGap, contentHeight - 1);
        const int textBottom = contentHeight;
        SetWindowPos(hwndText_, nullptr, 0, textTop, width, textBottom - textTop, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        SetWindowPos(hwndFooter_, nullptr, 0, textBottom, width, height - textBottom, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }

    LONG refCount_ = 1;
    bool initialized_ = false;
    HWND hwndParent_ = nullptr;
    HWND hwndPreview_ = nullptr;
    HWND hwndImage_ = nullptr;
    HWND hwndText_ = nullptr;
    HWND hwndFooter_ = nullptr;
    RECT rect_ = {};
    size_t sourceFileSize_ = 0;
    ApiumMetadata metadata_;
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, DeleteObjectDeleter> bitmap_;
    int bitmapPixelWidth_ = 0;
    int bitmapPixelHeight_ = 0;
    UINT meshBitmapSize_ = 0;
    std::unique_ptr<std::remove_pointer_t<HFONT>, DeleteObjectDeleter> font_;
    std::unique_ptr<std::remove_pointer_t<HBRUSH>, DeleteObjectDeleter> backgroundBrush_;
    COLORREF backgroundColor_ = RGB(255, 255, 255);
    std::optional<COLORREF> hostBackgroundColor_;
    COLORREF textColor_ = RGB(0, 0, 0);
    std::optional<COLORREF> hostTextColor_;
    std::string previewRtf_;
    std::wstring previewText_;
    std::wstring hostProcessName_;
    std::wstring initPath_;
    std::wstring sourceFilePath_;
    MeshPreviewResult meshInfo_;
    MeshPreviewGeometry meshGeometry_;
    bool richEditActive_ = true;
    bool knownMeshFile_ = false;
#if ASXP_INTERACTIVE_MESH_PREVIEW
    std::unique_ptr<MeshViewport> meshViewport_;
    HWND hwndMeshViewport_ = nullptr;
    bool meshViewportFailed_ = false;
    bool draggingMesh_ = false;
    bool meshPresentRetryPending_ = false;
    POINT lastMeshDrag_ = {};
#endif
    ComPtr<IUnknown> site_;
};

class PropertyHandler final : public IPropertyStore,
                              public IInitializeWithStream,
                              public IInitializeWithFile,
                              public IInitializeWithItem {
public:
    PropertyHandler() : refCount_(1) {
        g_objCount.fetch_add(1);
    }
    ~PropertyHandler() {
        g_objCount.fetch_sub(1);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        *ppvObject = nullptr;

        if (riid == IID_IUnknown || riid == IID_IPropertyStore) {
            *ppvObject = static_cast<IPropertyStore*>(this);
        } else if (riid == IID_IInitializeWithStream) {
            *ppvObject = static_cast<IInitializeWithStream*>(this);
        } else if (riid == IID_IInitializeWithFile) {
            *ppvObject = static_cast<IInitializeWithFile*>(this);
        } else if (riid == IID_IInitializeWithItem) {
            *ppvObject = static_cast<IInitializeWithItem*>(this);
        } else {
            DebugLogWin(L"PropertyHandler QueryInterface: unsupported riid=" + GuidToString(riid));
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG value = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (value == 0) {
            delete this;
        }
        return value;
    }

    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (initialized_) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        if (!stream) {
            return E_INVALIDARG;
        }
        std::vector<uint8_t> fileBytes;
        if (!ReadPreviewStream(stream, fileBytes, metadata_)) {
            DebugLogWin(L"PropertyHandler Initialize(IStream): ReadPreviewStream failed");
            metadata_ = ApiumMetadata{};
            initialized_ = true;
            return S_OK;
        }
        initialized_ = true;
        DebugLogWin(metadata_.isApium ? L"PropertyHandler Initialize(IStream): metadata isApium=true" : L"PropertyHandler Initialize(IStream): metadata isApium=false");
        return S_OK;
    }

    IFACEMETHODIMP Initialize(LPCWSTR filePath, DWORD) override {
        if (initialized_) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        if (!filePath) {
            return E_INVALIDARG;
        }
        std::vector<uint8_t> fileBytes;
        if (!ReadPreviewFile(filePath, fileBytes, metadata_)) {
            DebugLogWin(L"PropertyHandler Initialize(IFile): ReadPreviewFile failed");
            metadata_ = ApiumMetadata{};
            initialized_ = true;
            return S_OK;
        }
        initialized_ = true;
        DebugLogWin(metadata_.isApium ? L"PropertyHandler Initialize(IFile): metadata isApium=true" : L"PropertyHandler Initialize(IFile): metadata isApium=false");
        return S_OK;
    }

    IFACEMETHODIMP Initialize(IShellItem* item, DWORD) override {
        if (initialized_) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        if (!item) {
            return E_INVALIDARG;
        }

        PWSTR filePath = nullptr;
        const HRESULT displayNameHr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
        if (FAILED(displayNameHr) || !filePath) {
            DebugLogWin(L"PropertyHandler Initialize(IShellItem): GetDisplayName failed, hr=" + HResultToString(displayNameHr));
            metadata_ = ApiumMetadata{};
            initialized_ = true;
            return S_OK;
        }

        std::wstring path(filePath);
        CoTaskMemFree(filePath);

        std::vector<uint8_t> fileBytes;
        if (!ReadPreviewFile(path.c_str(), fileBytes, metadata_)) {
            DebugLogWin(L"PropertyHandler Initialize(IShellItem): ReadPreviewFile failed for " + path);
            metadata_ = ApiumMetadata{};
        }
        initialized_ = true;
        DebugLogWin(metadata_.isApium ? L"PropertyHandler Initialize(IShellItem): metadata isApium=true" : L"PropertyHandler Initialize(IShellItem): metadata isApium=false");
        return S_OK;
    }

    IFACEMETHODIMP GetCount(DWORD* cProps) override {
        if (!cProps) {
            return E_POINTER;
        }
        *cProps = static_cast<DWORD>(kApiumPropertyKeys.size());
        return S_OK;
    }

    IFACEMETHODIMP GetAt(DWORD iProp, PROPERTYKEY* pkey) override {
        if (!pkey) {
            return E_POINTER;
        }
        if (iProp >= kApiumPropertyKeys.size()) {
            return E_INVALIDARG;
        }
        *pkey = kApiumPropertyKeys[iProp];
        return S_OK;
    }

    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        if (!pv) {
            return E_POINTER;
        }
        PropVariantInit(pv);
        if (!initialized_) {
            DebugLogWin(L"PropertyHandler GetValue: called before initialization");
            return S_OK;
        }

        if (!metadata_.isApium) {
            return S_OK;
        }

        const std::wstring* value = LookupValue(key);
        if (!value || value->empty()) {
            return S_OK;
        }

        return InitPropVariantFromString(value->c_str(), pv);
    }

    IFACEMETHODIMP SetValue(REFPROPERTYKEY, REFPROPVARIANT) override {
        return STG_E_ACCESSDENIED;
    }

    IFACEMETHODIMP Commit() override {
        return STG_E_ACCESSDENIED;
    }

private:
    const std::wstring* LookupValue(REFPROPERTYKEY key) const {
        if (IsEqualPropertyKey(key, PKEY_Apium_Printer)) {
            return &metadata_.printer;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_ProjectVersion)) {
            return &metadata_.projectVersion;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_MaterialType)) {
            return &metadata_.materialType;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_NozzleDiameter)) {
            return &metadata_.nozzleDiameter;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_NozzleTemperature)) {
            return &metadata_.nozzleTemperature;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_BedTemperature)) {
            return &metadata_.bedTemperature;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_LayerHeight)) {
            return &metadata_.layerHeight;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_Perimeters)) {
            return &metadata_.perimeters;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_InfillPattern)) {
            return &metadata_.infillPattern;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_InfillDensity)) {
            return &metadata_.infillDensity;
        }
        if (IsEqualPropertyKey(key, PKEY_Apium_PrintTime)) {
            return &metadata_.printTime;
        }
        return nullptr;
    }

    LONG refCount_ = 1;
    bool initialized_ = false;
    ApiumMetadata metadata_;
};

class ClassFactory final : public IClassFactory {
public:
    explicit ClassFactory(HandlerKind kind) : refCount_(1), kind_(kind) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppvObject = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG value = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (value == 0) {
            delete this;
        }
        return value;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppvObject) override {
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }

        DebugLogWin(L"ClassFactory CreateInstance: kind=" + HandlerKindToString(kind_) + L", riid=" + GuidToString(riid));

        IUnknown* obj = nullptr;
        switch (kind_) {
        case HandlerKind::Thumbnail:
            obj = static_cast<IThumbnailProvider*>(new (std::nothrow) ThumbnailProvider());
            break;
        case HandlerKind::Preview:
            obj = static_cast<IPreviewHandler*>(new (std::nothrow) PreviewHandler());
            break;
        case HandlerKind::Property:
            obj = static_cast<IPropertyStore*>(new (std::nothrow) PropertyHandler());
            break;
        }

        if (!obj) {
            return E_OUTOFMEMORY;
        }

        const HRESULT hr = obj->QueryInterface(riid, ppvObject);
        obj->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            g_lockCount.fetch_add(1);
        } else {
            g_lockCount.fetch_sub(1);
        }
        return S_OK;
    }

private:
    LONG refCount_ = 1;
    HandlerKind kind_;
};

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return (g_objCount.load() == 0 && g_lockCount.load() == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;

    HandlerKind kind;
    if (clsid == CLSID_ApiumSlicerThumbnailProvider) {
        kind = HandlerKind::Thumbnail;
    } else if (clsid == CLSID_ApiumSlicerPreviewHandler) {
        kind = HandlerKind::Preview;
    } else if (clsid == CLSID_ApiumSlicerPropertyHandler) {
        kind = HandlerKind::Property;
    } else {
        DebugLogWin(L"DllGetClassObject: class unavailable clsid=" + GuidToString(clsid));
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    DebugLogWin(L"DllGetClassObject: clsid=" + GuidToString(clsid) + L", riid=" + GuidToString(riid));

    auto* factory = new (std::nothrow) ClassFactory(kind);
    if (!factory) {
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllRegisterServer() {
    return RegisterProviderPerUser();
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    return UnregisterProviderPerUser();
}
