#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

struct MeshPreviewGeometry;

class MeshViewport {
public:
    HRESULT Initialize(HWND window, UINT width, UINT height, const MeshPreviewGeometry& mesh);
    HRESULT Render(UINT width, UINT height, COLORREF background);
    void Rotate(int deltaX, int deltaY);
    bool Zoom(int wheelDelta);

private:
    HRESULT CreateTargets(UINT width, UINT height);
    HRESULT UploadMesh(const MeshPreviewGeometry& mesh);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> multisampledColor_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> colorTarget_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthTarget_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT vertexCount_ = 0;
    bool supportsMsaa_ = false;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float fitScale_ = 1.0f;
    float zoom_ = 1.0f;
};
