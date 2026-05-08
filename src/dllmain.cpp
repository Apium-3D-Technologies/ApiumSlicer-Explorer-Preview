#include <windows.h>

#include <msopc.h>
#include <objbase.h>
#include <shlobj_core.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resource.hpp"

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::wstring_view kThumbnailProviderIidString = L"{E357FCCD-A995-4576-B01F-234630154E96}";
constexpr std::wstring_view kTarget3mfPart = L"/Metadata/thumbnail.png";
constexpr size_t kTailScanBytes = 8ULL * 1024ULL * 1024ULL;

// {8A14EAA2-3FDB-4C5A-98A1-5D0A8700C8A5}
const CLSID CLSID_ApiumSlicerThumbnailProvider = {
    0x8a14eaa2, 0x3fdb, 0x4c5a, {0x98, 0xa1, 0x5d, 0x0a, 0x87, 0x00, 0xc8, 0xa5}
};

HINSTANCE g_module = nullptr;
std::atomic_ulong g_objCount = 0;
std::atomic_ulong g_lockCount = 0;

std::wstring GuidToString(const GUID& guid) {
    wchar_t buffer[64] = {};
    (void)StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return std::wstring(buffer);
}

std::wstring GetModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD len = 0;
    while (true) {
        len = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
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

HRESULT SetRegSz(HKEY key, const wchar_t* valueName, const std::wstring& value) {
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(
        key,
        valueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        size);
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

HRESULT RegisterProviderPerUser() {
    const std::wstring clsidString = GuidToString(CLSID_ApiumSlicerThumbnailProvider);
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
    hr = SetRegSz(clsidKey, nullptr, L"Apium Slicer Thumbnail Provider");
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
    RegCloseKey(inprocKey);
    if (FAILED(hr)) {
        return hr;
    }

    const std::array<std::wstring, 2> extPaths = {
        L"Software\\Classes\\.gcode\\shellex\\" + std::wstring(kThumbnailProviderIidString),
        L"Software\\Classes\\.3mf\\shellex\\" + std::wstring(kThumbnailProviderIidString)
    };

    for (const auto& extPath : extPaths) {
        HKEY extKey = nullptr;
        hr = CreateRegKeyPath(HKEY_CURRENT_USER, extPath, &extKey);
        if (FAILED(hr)) {
            return hr;
        }
        hr = SetRegSz(extKey, nullptr, clsidString);
        RegCloseKey(extKey);
        if (FAILED(hr)) {
            return hr;
        }
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

void TryDeleteKeyTree(HKEY root, const std::wstring& subKey) {
    (void)RegDeleteTreeW(root, subKey.c_str());
}

HRESULT UnregisterProviderPerUser() {
    const std::wstring clsidString = GuidToString(CLSID_ApiumSlicerThumbnailProvider);
    const std::wstring clsidKeyPath = L"Software\\Classes\\CLSID\\" + clsidString;

    TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\.gcode\\shellex\\" + std::wstring(kThumbnailProviderIidString));
    TryDeleteKeyTree(HKEY_CURRENT_USER, L"Software\\Classes\\.3mf\\shellex\\" + std::wstring(kThumbnailProviderIidString));
    TryDeleteKeyTree(HKEY_CURRENT_USER, clsidKeyPath);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

bool ReadStreamFully(IStream* stream, std::vector<uint8_t>& outBytes) {
    if (!stream) {
        return false;
    }

    LARGE_INTEGER zero = {};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
        return false;
    }

    outBytes.clear();

    STATSTG stat = {};
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
        if (stat.cbSize.QuadPart > static_cast<ULONGLONG>(SIZE_MAX)) {
            return false;
        }
        outBytes.resize(static_cast<size_t>(stat.cbSize.QuadPart));
        ULONG read = 0;
        if (FAILED(stream->Read(outBytes.data(), static_cast<ULONG>(outBytes.size()), &read))) {
            outBytes.clear();
            return false;
        }
        outBytes.resize(read);
        return true;
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

bool IsZipContainer(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 4) {
        return false;
    }
    return bytes[0] == 0x50 && bytes[1] == 0x4B &&
           (bytes[2] == 0x03 || bytes[2] == 0x05 || bytes[2] == 0x07) &&
           (bytes[3] == 0x04 || bytes[3] == 0x06 || bytes[3] == 0x08);
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
    const HRESULT hr = CreateStreamOnHGlobal(mem, TRUE, &stream);
    if (FAILED(hr)) {
        GlobalFree(mem);
        return false;
    }

    outStream.Attach(stream);
    LARGE_INTEGER zero = {};
    (void)outStream->Seek(zero, STREAM_SEEK_SET, nullptr);
    return true;
}

bool Extract3mfThumbnail(const std::vector<uint8_t>& fileBytes, std::vector<uint8_t>& outPngBytes) {
    outPngBytes.clear();
    ComPtr<IStream> packageStream;
    if (!CreateReadOnlyStreamOnBytes(fileBytes, packageStream)) {
        return false;
    }

    ComPtr<IOpcFactory> opcFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_OpcFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&opcFactory));
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IOpcPackage> package;
    hr = opcFactory->ReadPackageFromStream(packageStream.Get(), OPC_READ_DEFAULT, &package);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IOpcPartSet> partSet;
    hr = package->GetPartSet(&partSet);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IOpcPartUri> partUri;
    hr = opcFactory->CreatePartUri(kTarget3mfPart.data(), &partUri);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IOpcPart> part;
    hr = partSet->GetPart(partUri.Get(), &part);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IStream> thumbnailStream;
    hr = part->GetContentStream(&thumbnailStream);
    if (FAILED(hr)) {
        return false;
    }

    return ReadStreamFully(thumbnailStream.Get(), outPngBytes) && !outPngBytes.empty();
}

size_t FindLastSubstring(const std::string& haystack, std::string_view needle, size_t searchStartExclusive) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return std::string::npos;
    }

    size_t pos = std::min(searchStartExclusive, haystack.size() - needle.size());
    while (true) {
        const size_t found = haystack.rfind(needle, pos);
        if (found != std::string::npos) {
            return found;
        }
        if (pos == 0) {
            break;
        }
        --pos;
    }
    return std::string::npos;
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

bool ParseGcodeThumbnailBlock(const std::string& text, size_t scanStart, std::vector<uint8_t>& outPngBytes) {
    constexpr std::string_view kBeginTag = "; thumbnail_PNG begin";
    constexpr std::string_view kEndTag = "; thumbnail_PNG end";

    const size_t endPos = text.rfind(kEndTag);
    if (endPos == std::string::npos || endPos < scanStart) {
        return false;
    }

    const size_t beginPos = FindLastSubstring(text, kBeginTag, endPos);
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

bool ExtractGcodeThumbnail(const std::vector<uint8_t>& fileBytes, std::vector<uint8_t>& outPngBytes) {
    outPngBytes.clear();
    if (fileBytes.empty()) {
        return false;
    }

    std::string text(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
    const size_t tailStart = text.size() > kTailScanBytes ? (text.size() - kTailScanBytes) : 0;

    if (ParseGcodeThumbnailBlock(text, tailStart, outPngBytes)) {
        return true;
    }
    return ParseGcodeThumbnailBlock(text, 0, outPngBytes);
}

bool LoadFallbackPng(std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    HRSRC resource = FindResourceW(g_module, MAKEINTRESOURCEW(IDR_FALLBACK_PNG), L"PNG");
    if (!resource) {
        return false;
    }

    const DWORD size = SizeofResource(g_module, resource);
    if (size == 0) {
        return false;
    }

    HGLOBAL loaded = LoadResource(g_module, resource);
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

bool DecodePngToHBitmap(const std::vector<uint8_t>& pngBytes, UINT requestedCx, HBITMAP* outBitmap) {
    *outBitmap = nullptr;
    if (pngBytes.empty() || requestedCx == 0) {
        return false;
    }

    ComPtr<IWICImagingFactory> imagingFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&imagingFactory));
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICStream> wicStream;
    hr = imagingFactory->CreateStream(&wicStream);
    if (FAILED(hr)) {
        return false;
    }

    hr = wicStream->InitializeFromMemory(
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(pngBytes.data())),
        static_cast<DWORD>(pngBytes.size()));
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = imagingFactory->CreateDecoderFromStream(wicStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        return false;
    }

    const float scale = std::min(
        static_cast<float>(requestedCx) / static_cast<float>(width),
        static_cast<float>(requestedCx) / static_cast<float>(height));
    const UINT targetW = std::max(1U, static_cast<UINT>(width * scale));
    const UINT targetH = std::max(1U, static_cast<UINT>(height * scale));

    ComPtr<IWICBitmapSource> source = frame;

    if (targetW != width || targetH != height) {
        ComPtr<IWICBitmapScaler> scaler;
        hr = imagingFactory->CreateBitmapScaler(&scaler);
        if (FAILED(hr)) {
            return false;
        }
        hr = scaler->Initialize(frame.Get(), targetW, targetH, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) {
            return false;
        }
        source = scaler;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = imagingFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return false;
    }

    hr = converter->Initialize(
        source.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return false;
    }

    const UINT stride = targetW * 4;
    const UINT bufferSize = stride * targetH;
    std::vector<uint8_t> pixels(bufferSize);
    hr = converter->CopyPixels(nullptr, stride, bufferSize, pixels.data());
    if (FAILED(hr)) {
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

class ThumbnailProvider final : public IThumbnailProvider, public IInitializeWithStream {
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
        } else {
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
            return E_FAIL;
        }
        initialized_ = true;
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

        std::vector<uint8_t> pngBytes;
        bool gotThumb = false;

        if (IsZipContainer(fileBytes_)) {
            gotThumb = Extract3mfThumbnail(fileBytes_, pngBytes);
        } else {
            gotThumb = ExtractGcodeThumbnail(fileBytes_, pngBytes);
        }

        if (!gotThumb || pngBytes.empty()) {
            if (!LoadFallbackPng(pngBytes)) {
                return E_FAIL;
            }
        }

        HBITMAP bitmap = nullptr;
        if (!DecodePngToHBitmap(pngBytes, cx, &bitmap) || !bitmap) {
            // If extracted content failed, retry once with fallback.
            if (!LoadFallbackPng(pngBytes) || !DecodePngToHBitmap(pngBytes, cx, &bitmap) || !bitmap) {
                return E_FAIL;
            }
        }

        *phbmp = bitmap;
        *pdwAlpha = WTSAT_ARGB;
        return S_OK;
    }

private:
    LONG refCount_ = 1;
    bool initialized_ = false;
    std::vector<uint8_t> fileBytes_;
};

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() : refCount_(1) {}

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
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        auto* provider = new (std::nothrow) ThumbnailProvider();
        if (!provider) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = provider->QueryInterface(riid, ppvObject);
        provider->Release();
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

    if (clsid != CLSID_ApiumSlicerThumbnailProvider) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* factory = new (std::nothrow) ClassFactory();
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
