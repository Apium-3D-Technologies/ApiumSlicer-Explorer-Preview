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
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "metadata.hpp"
#include "generated/apium_version.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::wstring_view kThumbnailProviderIidString = L"{E357FCCD-A995-4576-B01F-234630154E96}";
constexpr std::wstring_view kPreviewHandlerIidString = L"{8895B1C6-B41F-4C1C-A562-0D564250836F}";
constexpr std::wstring_view kPreviewHostAppId = L"{3B0FE91D-45E1-41E4-A6A7-6F8045AEF35A}";
constexpr wchar_t kPreviewPaneWindowClassName[] = L"ApiumSlicerPreviewPaneWindow";

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

constexpr std::array<std::wstring_view, 2> kSupportedExtensions = {L".gcode", L".a3d"};

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

std::optional<std::wstring> ResolveProgIdForExtension(std::wstring_view ext) {
    const std::wstring extKey = L"Software\\Classes\\" + std::wstring(ext);
    if (auto value = ReadDefaultRegString(HKEY_CURRENT_USER, extKey); value.has_value()) {
        return value;
    }
    const std::wstring hkcrExt = std::wstring(ext);
    if (auto value = ReadDefaultRegString(HKEY_CLASSES_ROOT, hkcrExt); value.has_value()) {
        return value;
    }
    return std::nullopt;
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

std::vector<std::wstring> BuildNoMetadataDebugLines(
    const ApiumMetadata& metadata,
    std::wstring_view initPath,
    std::wstring_view hostProcess) {
    std::vector<std::wstring> lines;
    lines.push_back(L"No Apium metadata detected for this file.");
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
    if (SUCCEEDED(hr)) {
        hr = SetRegDword(clsidKey, L"DisableLowILProcessIsolation", 1);
    }
    if (SUCCEEDED(hr)) {
        hr = SetRegDword(clsidKey, L"DisableProcessIsolation", 1);
    }
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

HRESULT RegisterPreviewAppIdPerUser() {
    const std::wstring appIdKeyPath = L"Software\\Classes\\AppID\\" + std::wstring(kPreviewHostAppId);
    HKEY appIdKey = nullptr;
    HRESULT hr = CreateRegKeyPath(HKEY_CURRENT_USER, appIdKeyPath, &appIdKey);
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegSz(appIdKey, nullptr, L"Apium Slicer Preview Host");
    if (SUCCEEDED(hr)) {
        hr = SetRegSz(appIdKey, L"DllSurrogate", L"Prevhost.exe");
    }
    RegCloseKey(appIdKey);
    return hr;
}

HRESULT RegisterProviderPerUser() {
    HRESULT hr = RegisterComClassPerUser(CLSID_ApiumSlicerThumbnailProvider, L"Apium Slicer Thumbnail Provider");
    if (FAILED(hr)) {
        return hr;
    }
    hr = RegisterPreviewAppIdPerUser();
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

    for (const std::wstring_view ext : kSupportedExtensions) {
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

        {
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

        if (const auto progId = ResolveProgIdForExtension(ext); progId.has_value()) {
            const std::wstring progIdKey = L"Software\\Classes\\" + progId.value();

            {
                const std::wstring keyPath = progIdKey + L"\\shellex\\" + std::wstring(kThumbnailProviderIidString);
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
                const std::wstring keyPath = progIdKey + L"\\shellex\\" + std::wstring(kPreviewHandlerIidString);
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
                HKEY key = nullptr;
                hr = CreateRegKeyPath(HKEY_CURRENT_USER, progIdKey, &key);
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
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
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
        TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\" + std::wstring(ext) + L"\\shellex\\PropertyHandler");
        TryDeleteKeyTree(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\" + std::wstring(ext));

        const std::wstring extKey = L"Software\\Classes\\" + std::wstring(ext);
        TryDeleteValue(HKEY_CURRENT_USER, extKey, L"PreviewDetails");
        TryDeleteValue(HKEY_CURRENT_USER, extKey, L"FullDetails");
        TryDeleteValue(HKEY_CURRENT_USER, extKey, L"InfoTip");

        if (const auto progId = ResolveProgIdForExtension(ext); progId.has_value()) {
            const std::wstring progIdKey = L"Software\\Classes\\" + progId.value();
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
    TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\AppID\\" + std::wstring(kPreviewHostAppId));

    TryUnregisterPropertySchema();

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
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
        if (!ReadStreamFully(stream, fileBytes_)) {
            DebugLogWin(L"ThumbnailProvider Initialize(IStream): ReadStreamFully failed");
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
            initialized_ = true;
            return S_OK;
        }
        metadata_ = ParseApiumMetadata(fileBytes_);
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
        if (!ReadFileFully(filePath, fileBytes_)) {
            DebugLogWin(L"ThumbnailProvider Initialize(IFile): ReadFileFully failed");
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
            initialized_ = true;
            return S_OK;
        }
        metadata_ = ParseApiumMetadata(fileBytes_);
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
        if (!ReadFileFully(path.c_str(), fileBytes_)) {
            DebugLogWin(L"ThumbnailProvider Initialize(IShellItem): ReadFileFully failed for " + path);
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
        } else {
            metadata_ = ParseApiumMetadata(fileBytes_);
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
        backgroundColor_ = color;
        backgroundBrush_.reset(CreateSolidBrush(backgroundColor_));
        if (hwndPreview_) {
            InvalidateRect(hwndPreview_, nullptr, TRUE);
        }
        if (hwndText_) {
            SendMessageW(hwndText_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(backgroundColor_));
            RenderPreviewContent();
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
        textColor_ = color;
        if (hwndText_) {
            RenderPreviewContent();
        }
        if (hwndFooter_) {
            InvalidateRect(hwndFooter_, nullptr, TRUE);
        }
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
            fileBytes_.clear();
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

        if (!ReadFileFully(path.c_str(), fileBytes_)) {
            DebugLogWin(L"PreviewHandler Initialize(IShellItem): ReadFileFully failed for " + path);
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
            metadata_.diagnostics.container = L"unknown";
            metadata_.diagnostics.configPartStatus = L"shell item file read failed";
            metadata_.diagnostics.decode = L"n/a";
            metadata_.diagnostics.apiumDetect = L"n/a";
            metadata_.diagnostics.missingFields = L"file bytes unavailable";
        } else {
            metadata_ = ParseApiumMetadata(fileBytes_);
        }

        initPath_ = L"IShellItem";
        sourceFilePath_ = std::move(path);
        initialized_ = true;
        DebugLogWin(
            L"PreviewHandler Initialize(IShellItem): host=" + hostProcessName_ +
            L", container=" + metadata_.diagnostics.container +
            L", isApium=" + std::wstring(metadata_.isApium ? L"true" : L"false"));
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
        if (!ReadStreamFully(stream, fileBytes_)) {
            DebugLogWin(L"PreviewHandler Initialize(IStream): ReadStreamFully failed");
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
            metadata_.diagnostics.container = L"unknown";
            metadata_.diagnostics.configPartStatus = L"stream read failed";
            metadata_.diagnostics.decode = L"n/a";
            metadata_.diagnostics.apiumDetect = L"n/a";
            metadata_.diagnostics.missingFields = L"file bytes unavailable";
        } else {
            metadata_ = ParseApiumMetadata(fileBytes_);
        }
        initPath_ = L"IStream";
        sourceFilePath_.clear();
        initialized_ = true;
        DebugLogWin(
            L"PreviewHandler Initialize(IStream): host=" + hostProcessName_ +
            L", container=" + metadata_.diagnostics.container +
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
        if (!ReadFileFully(filePath, fileBytes_)) {
            DebugLogWin(L"PreviewHandler Initialize(IFile): ReadFileFully failed");
            fileBytes_.clear();
            metadata_ = ApiumMetadata{};
            metadata_.diagnostics.container = L"unknown";
            metadata_.diagnostics.configPartStatus = L"file read failed";
            metadata_.diagnostics.decode = L"n/a";
            metadata_.diagnostics.apiumDetect = L"n/a";
            metadata_.diagnostics.missingFields = L"file bytes unavailable";
        } else {
            metadata_ = ParseApiumMetadata(fileBytes_);
        }
        initPath_ = L"IFile";
        sourceFilePath_ = filePath;
        initialized_ = true;
        DebugLogWin(
            L"PreviewHandler Initialize(IFile): host=" + hostProcessName_ +
            L", container=" + metadata_.diagnostics.container +
            L", isApium=" + std::wstring(metadata_.isApium ? L"true" : L"false"));
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
            L", container=" + metadata_.diagnostics.container);
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
            L"STATIC",
            nullptr,
            WS_CHILD | WS_VISIBLE | SS_BITMAP,
            0,
            0,
            10,
            10,
            hwndPreview_,
            nullptr,
            g_module,
            nullptr);
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

        const std::wstring footer =
            L"[" + std::wstring(ASXP_NAME_W) + L" - v" + std::wstring(ASXP_VERSION_W) + std::wstring(ASXP_AUTHOR_W) + L"]";
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
        fileBytes_.clear();
        metadata_ = ApiumMetadata{};
        sourceFilePath_.clear();
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
    struct DeleteObjectDeleter {
        void operator()(HGDIOBJ obj) const {
            if (obj) {
                DeleteObject(obj);
            }
        }
    };

    static ATOM EnsurePreviewPaneClass() {
        static std::once_flag once;
        static ATOM atom = 0;
        std::call_once(once, []() {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = &PreviewPaneWndProc;
            wc.hInstance = g_module;
            wc.hbrBackground = nullptr;
            wc.lpszClassName = kPreviewPaneWindowClassName;
            atom = RegisterClassW(&wc);
            if (atom == 0 && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
                atom = 1;
            }
        });
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

    void RefreshSystemColors() {
        if (IsAppsDarkModeEnabled()) {
            backgroundColor_ = RGB(0x19, 0x19, 0x19);
            textColor_ = RGB(0xFF, 0xFF, 0xFF);
        } else {
            backgroundColor_ = RGB(0xFF, 0xFF, 0xFF);
            textColor_ = RGB(0x00, 0x00, 0x00);
        }
        if (backgroundColor_ == RGB(0xFF, 0xFF, 0xFF)) {
            textColor_ = RGB(0x00, 0x00, 0x00);
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
    }

    void CleanupWindow() {
        if (hwndImage_ && bitmap_) {
            SendMessageW(hwndImage_, STM_SETIMAGE, IMAGE_BITMAP, 0);
            bitmap_.reset();
        }
        bitmapPixelWidth_ = 0;
        bitmapPixelHeight_ = 0;
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

        std::vector<PreviewRow> rows = BuildPreviewRows(metadata_);
        std::vector<std::wstring> extraLines;
        if (!metadata_.isApium) {
            extraLines = BuildNoMetadataDebugLines(metadata_, initPath_, hostProcessName_);
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

        std::vector<uint8_t> png = metadata_.thumbnailBytes;
        if (png.empty()) {
            (void)LoadFallbackPng(g_module, png);
        }

        HBITMAP nextBitmap = nullptr;
        if (!png.empty()) {
            (void)DecodePngToHBitmap(png, maxSize, &nextBitmap);
        }
        if (!nextBitmap) {
            return;
        }

        BITMAP bmpInfo = {};
        if (GetObjectW(nextBitmap, sizeof(bmpInfo), &bmpInfo) == sizeof(bmpInfo)) {
            bitmapPixelWidth_ = std::max(1L, bmpInfo.bmWidth);
            bitmapPixelHeight_ = std::max(1L, bmpInfo.bmHeight);
        } else {
            bitmapPixelWidth_ = 0;
            bitmapPixelHeight_ = 0;
        }

        SendMessageW(hwndImage_, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(nextBitmap));
        if (bitmap_) {
            bitmap_.reset();
        }
        bitmap_.reset(nextBitmap);
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

        const int footerHeight = 24;
        const int imageTop = 8;
        const int imageHeight = std::max(120, ((height - imageTop) * 58) / 100);
        const int imagePadding = 8;
        const int imageAvailableWidth = std::max(1, width - (imagePadding * 2));
        const int imageAvailableHeight = std::max(1, imageHeight - imageTop - imagePadding);
        const int imageBottomGap = 3;

        EnsurePreviewBitmap(static_cast<UINT>(std::min(imageAvailableWidth, imageAvailableHeight)));

        int renderW = imageAvailableWidth;
        int renderH = imageAvailableHeight;
        if (bitmapPixelWidth_ > 0 && bitmapPixelHeight_ > 0) {
            renderW = std::min(imageAvailableWidth, bitmapPixelWidth_);
            renderH = std::min(imageAvailableHeight, bitmapPixelHeight_);
        }

        const int imageX = (width - renderW) / 2;
        const int imageY = imageTop;
        SetWindowPos(hwndImage_, nullptr, imageX, imageY, renderW, renderH, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);

        const int textTop = std::clamp(imageY + renderH + imageBottomGap, imageHeight, height - 1);
        const int textBottom = std::max(textTop + 1, height - footerHeight);
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
    std::vector<uint8_t> fileBytes_;
    ApiumMetadata metadata_;
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, DeleteObjectDeleter> bitmap_;
    int bitmapPixelWidth_ = 0;
    int bitmapPixelHeight_ = 0;
    std::unique_ptr<std::remove_pointer_t<HFONT>, DeleteObjectDeleter> font_;
    std::unique_ptr<std::remove_pointer_t<HBRUSH>, DeleteObjectDeleter> backgroundBrush_;
    COLORREF backgroundColor_ = RGB(255, 255, 255);
    COLORREF textColor_ = RGB(0, 0, 0);
    std::string previewRtf_;
    std::wstring previewText_;
    std::wstring hostProcessName_;
    std::wstring initPath_;
    std::wstring sourceFilePath_;
    bool richEditActive_ = true;
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
        if (!ReadStreamFully(stream, fileBytes)) {
            DebugLogWin(L"PropertyHandler Initialize(IStream): ReadStreamFully failed");
            metadata_ = ApiumMetadata{};
            initialized_ = true;
            return S_OK;
        }
        metadata_ = ParseApiumMetadata(fileBytes);
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
        if (!ReadFileFully(filePath, fileBytes)) {
            DebugLogWin(L"PropertyHandler Initialize(IFile): ReadFileFully failed");
            metadata_ = ApiumMetadata{};
            initialized_ = true;
            return S_OK;
        }
        metadata_ = ParseApiumMetadata(fileBytes);
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
        if (!ReadFileFully(path.c_str(), fileBytes)) {
            DebugLogWin(L"PropertyHandler Initialize(IShellItem): ReadFileFully failed for " + path);
            metadata_ = ApiumMetadata{};
        } else {
            metadata_ = ParseApiumMetadata(fileBytes);
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
