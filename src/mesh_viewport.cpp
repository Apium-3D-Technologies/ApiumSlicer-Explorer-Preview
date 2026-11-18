#include "mesh_viewport.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "generated/mesh_ps.h"
#include "generated/mesh_vs.h"
#include "mesh_preview.hpp"

namespace {

using namespace DirectX;
using namespace mesh_preview_style;
using Microsoft::WRL::ComPtr;

constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr UINT kMsaaSamples = 4;
constexpr UINT kMsaaMaxDimension = 1024;

struct Vertex {
    MeshPreviewPosition position;
    std::array<int8_t, 4> normal;
};
static_assert(sizeof(Vertex) == 16);

struct SceneConstants {
    XMFLOAT4 projectX;
    XMFLOAT4 projectY;
    XMFLOAT4 projectZ;
    XMFLOAT4 lightAndDiffuse;
    XMFLOAT4 viewAndFacing;
    XMFLOAT4 colorAndAmbient;
    XMFLOAT4 shadeLimits;
};
static_assert(sizeof(SceneConstants) % 16 == 0);

XMVECTOR LoadDirection(MeshPreviewPosition direction) {
    return XMVector3Normalize(XMVectorSet(direction.x, direction.y, direction.z, 0.0f));
}

XMFLOAT4 VectorAndScalar(FXMVECTOR vector, float scalar) {
    XMFLOAT4 value;
    XMStoreFloat4(&value, XMVectorSetW(vector, scalar));
    return value;
}

XMFLOAT4 Color(COLORREF color, float w) {
    constexpr float kByteToFloat = 1.0f / 255.0f;
    return {GetRValue(color) * kByteToFloat, GetGValue(color) * kByteToFloat, GetBValue(color) * kByteToFloat, w};
}

std::array<int8_t, 4> PackNormal(const MeshPreviewTriangle& triangle) {
    // Compute the normal before rounding the centered positions to float.
    const double abX = static_cast<double>(triangle.b.x) - triangle.a.x;
    const double abY = static_cast<double>(triangle.b.y) - triangle.a.y;
    const double abZ = static_cast<double>(triangle.b.z) - triangle.a.z;
    const double acX = static_cast<double>(triangle.c.x) - triangle.a.x;
    const double acY = static_cast<double>(triangle.c.y) - triangle.a.y;
    const double acZ = static_cast<double>(triangle.c.z) - triangle.a.z;
    const double x = abY * acZ - abZ * acY;
    const double y = abZ * acX - abX * acZ;
    const double z = abX * acY - abY * acX;
    const double scale = 127.0 / std::sqrt(x * x + y * y + z * z);
    return {static_cast<int8_t>(std::round(x * scale)), static_cast<int8_t>(std::round(y * scale)),
        static_cast<int8_t>(std::round(z * scale)), 0};
}

} // namespace

HRESULT MeshViewport::Initialize(HWND window, UINT width, UINT height, const MeshPreviewGeometry& mesh) {
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) { return hr; }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) { return hr; }
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) { return hr; }
    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { return hr; }

    DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
    swapDesc.Width = width;
    swapDesc.Height = height;
    swapDesc.Format = kColorFormat;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), window, &swapDesc, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) { return hr; }
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    UINT colorQuality = 0;
    UINT depthQuality = 0;
    supportsMsaa_ = SUCCEEDED(device_->CheckMultisampleQualityLevels(kColorFormat, kMsaaSamples, &colorQuality)) && colorQuality > 0 &&
        SUCCEEDED(device_->CheckMultisampleQualityLevels(kDepthFormat, kMsaaSamples, &depthQuality)) && depthQuality > 0;

    ComPtr<ID3D11VertexShader> vertexShader;
    hr = device_->CreateVertexShader(mesh_vs, sizeof(mesh_vs), nullptr, &vertexShader);
    if (FAILED(hr)) { return hr; }
    ComPtr<ID3D11PixelShader> pixelShader;
    hr = device_->CreatePixelShader(mesh_ps, sizeof(mesh_ps), nullptr, &pixelShader);
    if (FAILED(hr)) { return hr; }
    constexpr D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R8G8B8A8_SNORM, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ComPtr<ID3D11InputLayout> layout;
    hr = device_->CreateInputLayout(elements, ARRAYSIZE(elements), mesh_vs, sizeof(mesh_vs), &layout);
    if (FAILED(hr)) { return hr; }

    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = supportsMsaa_;
    ComPtr<ID3D11RasterizerState> rasterizer;
    hr = device_->CreateRasterizerState(&rasterizerDesc, &rasterizer);
    if (FAILED(hr)) { return hr; }

    D3D11_BUFFER_DESC constantDesc = {};
    constantDesc.ByteWidth = sizeof(SceneConstants);
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&constantDesc, nullptr, &constants_);
    if (FAILED(hr)) { return hr; }

    hr = UploadMesh(mesh);
    if (FAILED(hr)) { return hr; }
    const auto view = LoadDirection(kViewDirection);
    yaw_ = std::atan2(kViewDirection.y, kViewDirection.x);
    pitch_ = std::asin(XMVectorGetZ(view));

    context_->IASetInputLayout(layout.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->RSSetState(rasterizer.Get());
    context_->VSSetShader(vertexShader.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context_->PSSetShader(pixelShader.Get(), nullptr, 0);
    return CreateTargets(width, height);
}

HRESULT MeshViewport::UploadMesh(const MeshPreviewGeometry& mesh) {
    const double extentX = static_cast<double>(mesh.max.x) - mesh.min.x;
    const double extentY = static_cast<double>(mesh.max.y) - mesh.min.y;
    const double extentZ = static_cast<double>(mesh.max.z) - mesh.min.z;
    const double extent = std::max({extentX, extentY, extentZ});
    if (mesh.triangles.empty() || extent <= 0.0) { return E_INVALIDARG; }

    const double inverseExtent = 1.0 / extent;
    const double centerX = mesh.min.x + extentX * 0.5;
    const double centerY = mesh.min.y + extentY * 0.5;
    const double centerZ = mesh.min.z + extentZ * 0.5;
    float radiusSquared = 0.0f;
    std::vector<Vertex> vertices;
    try {
        vertices.reserve(mesh.triangles.size() * 3);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    for (const auto& triangle : mesh.triangles) {
        const auto normal = PackNormal(triangle);
        for (const auto& point : {triangle.a, triangle.b, triangle.c}) {
            const MeshPreviewPosition p{static_cast<float>((point.x - centerX) * inverseExtent),
                static_cast<float>((point.y - centerY) * inverseExtent),
                static_cast<float>((point.z - centerZ) * inverseExtent)};
            radiusSquared = std::max(radiusSquared, p.x * p.x + p.y * p.y + p.z * p.z);
            vertices.push_back({p, normal});
        }
    }
    // Use a single fit scale throughout the orbit so the model stays in view.
    fitScale_ = kFit / std::sqrt(radiusSquared);

    D3D11_BUFFER_DESC vertexDesc = {};
    vertexDesc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    vertexDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexData = {};
    vertexData.pSysMem = vertices.data();
    ComPtr<ID3D11Buffer> vertexBuffer;
    const HRESULT hr = device_->CreateBuffer(&vertexDesc, &vertexData, &vertexBuffer);
    if (FAILED(hr)) { return hr; }
    constexpr UINT stride = sizeof(Vertex);
    constexpr UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
    vertexCount_ = static_cast<UINT>(vertices.size());
    return S_OK;
}

HRESULT MeshViewport::CreateTargets(UINT width, UINT height) {
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_));
    if (FAILED(hr)) { return hr; }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    const bool useMsaa = supportsMsaa_ && std::max(width, height) <= kMsaaMaxDimension;
    desc.SampleDesc.Count = useMsaa ? kMsaaSamples : 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    if (useMsaa) {
        desc.Format = kColorFormat;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = device_->CreateTexture2D(&desc, nullptr, &multisampledColor_);
        if (FAILED(hr)) { return hr; }
    }
    ID3D11Texture2D* color = multisampledColor_ ? multisampledColor_.Get() : backBuffer_.Get();
    hr = device_->CreateRenderTargetView(color, nullptr, &colorTarget_);
    if (FAILED(hr)) { return hr; }

    desc.Format = kDepthFormat;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    hr = device_->CreateTexture2D(&desc, nullptr, &depth);
    if (FAILED(hr)) { return hr; }
    hr = device_->CreateDepthStencilView(depth.Get(), nullptr, &depthTarget_);
    if (FAILED(hr)) { return hr; }

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    width_ = width;
    height_ = height;
    return S_OK;
}

HRESULT MeshViewport::Render(UINT width, UINT height, COLORREF background) {
    if (width == 0 || height == 0) { return S_OK; }
    if (width != width_ || height != height_) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        depthTarget_.Reset();
        colorTarget_.Reset();
        multisampledColor_.Reset();
        backBuffer_.Reset();
        HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) { return hr; }
        hr = CreateTargets(width, height);
        if (FAILED(hr)) { return hr; }
    }

    const float sinYaw = std::sin(yaw_);
    const float cosYaw = std::cos(yaw_);
    const float sinPitch = std::sin(pitch_);
    const float cosPitch = std::cos(pitch_);
    const XMVECTOR view = XMVectorSet(cosPitch * cosYaw, cosPitch * sinYaw, sinPitch, 0.0f);
    const XMVECTOR right = XMVectorSet(-sinYaw, cosYaw, 0.0f, 0.0f);
    const XMVECTOR up = XMVector3Cross(view, right);
    static const XMFLOAT3 cameraLight = [] {
        const auto view = LoadDirection(kViewDirection);
        const auto right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), view));
        const auto up = XMVector3Cross(view, right);
        const auto light = LoadDirection(kLightDirection);
        return XMFLOAT3{XMVectorGetX(XMVector3Dot(light, right)), XMVectorGetX(XMVector3Dot(light, up)),
            XMVectorGetX(XMVector3Dot(light, view))};
    }();
    const auto light = right * cameraLight.x + up * cameraLight.y + view * cameraLight.z;
    const float scale = fitScale_ * zoom_;
    const float fit = static_cast<float>(std::min(width, height));
    const SceneConstants scene{
        VectorAndScalar(right * (scale * fit / width), 0.0f),
        VectorAndScalar(up * (scale * fit / height), 0.0f),
        VectorAndScalar(view * -0.5f, 0.5f),
        VectorAndScalar(light, kDiffuse),
        VectorAndScalar(view, kFacing),
        Color(kModelColor, kAmbient + kFacing),
        {kMinShade, 0.0f, 0.0f, 0.0f},
    };
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT hr = context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { return hr; }
    std::memcpy(mapped.pData, &scene, sizeof(scene));
    context_->Unmap(constants_.Get(), 0);

    const auto color = Color(background, 1.0f);
    const float clear[] = {color.x, color.y, color.z, color.w};
    context_->ClearRenderTargetView(colorTarget_.Get(), clear);
    context_->ClearDepthStencilView(depthTarget_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    context_->OMSetRenderTargets(1, colorTarget_.GetAddressOf(), depthTarget_.Get());
    context_->Draw(vertexCount_, 0);
    if (multisampledColor_) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->ResolveSubresource(backBuffer_.Get(), 0, multisampledColor_.Get(), 0, kColorFormat);
    }
    return swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
}

void MeshViewport::Rotate(int deltaX, int deltaY) {
    constexpr float kRadiansPerPixel = 0.008f;
    constexpr float kPitchLimit = XM_PIDIV2 - 0.005f;
    yaw_ = std::remainder(yaw_ - deltaX * kRadiansPerPixel, XM_2PI);
    pitch_ = std::clamp(pitch_ + deltaY * kRadiansPerPixel, -kPitchLimit, kPitchLimit);
}

bool MeshViewport::Zoom(int wheelDelta) {
    const float previous = zoom_;
    zoom_ = std::clamp(zoom_ * std::pow(1.15f, static_cast<float>(wheelDelta) / WHEEL_DELTA), 0.1f, 20.0f);
    return zoom_ != previous;
}
