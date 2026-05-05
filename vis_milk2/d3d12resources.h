/*
 * d3d12resources.h - Minimal Direct3D 12 device and swap chain path.
 */

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstddef>
#include <string>
#include <vector>

namespace DX
{
class D3D12Resources
{
  public:
    D3D12Resources(DXGI_FORMAT backBufferFormat, UINT backBufferCount, unsigned int flags);
    ~D3D12Resources();

    D3D12Resources(D3D12Resources const&) = delete;
    D3D12Resources& operator=(D3D12Resources const&) = delete;

    void SetWindow(HWND window, int width, int height) noexcept;
    void CreateDeviceResources();
    void CreateWindowSizeDependentResources();
    bool WindowSizeChanged(int width, int height);
    bool WindowSwap(HWND window, int width, int height);
    void SetTextureDirectory(const wchar_t* textureDirectory);
    bool SetTextureFile(const wchar_t* textureFile);
    void Clear();
    void DrawWaveform(const float* left,
                      const float* right,
                      size_t sampleCount,
                      float bass,
                      float mids,
                      float treble,
                      float waveR = 0.35f,
                      float waveG = 0.85f,
                      float waveB = 1.0f,
                      float waveA = 1.0f,
                      float waveScale = 1.0f,
                      float waveX = 0.5f,
                      float waveY = 0.5f,
                      float decay = 0.97f,
                      float zoom = 1.0f,
                      float rot = 0.0f);
    void Present();

    RECT GetOutputSize() const noexcept { return m_outputSize; }
    DXGI_FORMAT GetBackBufferFormat() const noexcept { return m_backBufferFormat; }

  private:
    void CreateFactory();
    void GetHardwareAdapter(IDXGIAdapter1** adapter);
    void MoveToNextFrame();
    void WaitForGpu();
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRenderTargetView() const noexcept;
    void CreateWaveformResources();
    void CreateTextureResources();
    bool LoadTextureFromFile(const wchar_t* textureFile);
    std::wstring PickTextureFile() const;
    void DrawTextureQuad();

    static constexpr UINT c_maxBackBufferCount = 3;
    static constexpr UINT c_maxWaveformVertices = 2048;

    struct WaveformVertex
    {
        float position[2];
        float color[4];
    };

    struct TextureVertex
    {
        float position[2];
        float uv[2];
        float color[4];
    };

    HWND m_window = nullptr;
    RECT m_outputSize{0, 0, 1, 1};
    DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT m_backBufferCount = 2;
    unsigned int m_options = 0;
    UINT m_frameIndex = 0;
    UINT m_rtvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<IDXGIFactory4> m_dxgiFactory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_renderTargets[c_maxBackBufferCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocators[c_maxBackBufferCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_waveformRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_waveformPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_waveformVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_waveformVertexBufferView{};
    WaveformVertex* m_mappedWaveformVertices = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_textureRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_texturePipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_textureVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_textureUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_textureVertexBufferView{};
    TextureVertex* m_mappedTextureVertices = nullptr;
    std::wstring m_textureDirectory;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValues[c_maxBackBufferCount]{};
    HANDLE m_fenceEvent = nullptr;
};
} // namespace DX
