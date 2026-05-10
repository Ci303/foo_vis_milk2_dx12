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
struct CustomShapeDrawCommand
{
    int sides = 0;
    bool additive = false;
    bool thickBorder = false;
    bool textured = false;
    float x = 0.5f;
    float y = 0.5f;
    float radius = 0.0f;
    float angle = 0.0f;
    float texZoom = 1.0f;
    float texAngle = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 0.0f;
    float r2 = 1.0f;
    float g2 = 1.0f;
    float b2 = 1.0f;
    float a2 = 0.0f;
    float borderR = 1.0f;
    float borderG = 1.0f;
    float borderB = 1.0f;
    float borderA = 0.0f;
};

struct CustomWaveVertex
{
    float x = 0.0f;
    float y = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct CustomWaveDrawCommand
{
    size_t vertexOffset = 0;
    size_t vertexCount = 0;
    bool additive = false;
    bool triangleList = false;
};

struct TextureWarpVertex
{
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

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
    bool SetTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount);
    bool SetPresetTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount);
    void ClearPresetTextureOverride();
    void ResetVisualHistory();
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
                      float rot = 0.0f,
                      float motionCenterX = 0.5f,
                      float motionCenterY = 0.5f,
                      float motionDX = 0.0f,
                      float motionDY = 0.0f,
                      float motionStretchX = 1.0f,
                      float motionStretchY = 1.0f,
                      float motionWarp = 0.0f,
                      float echoAlpha = 0.0f,
                      float echoZoom = 2.0f,
                      int echoOrientation = 0,
                      bool waveUseDots = false,
                      bool waveThick = false,
                      bool waveAdditive = false,
                      bool waveBrighten = false,
                      int waveMode = 0,
                      float waveParam = 0.0f,
                      bool darkenCenter = false,
                      float postGamma = 1.0f,
                      bool postInvert = false,
                      bool postBrighten = false,
                      bool postDarken = false,
                      bool postSolarize = false,
                      float postShaderAmount = 0.0f,
                      float postBlurAmount = 0.0f,
                      float postBlurEdgeDarken = 0.25f,
                      float outerBorderSize = 0.0f,
                      float outerBorderR = 0.0f,
                      float outerBorderG = 0.0f,
                      float outerBorderB = 0.0f,
                      float outerBorderA = 0.0f,
                      float innerBorderSize = 0.0f,
                      float innerBorderR = 0.0f,
                      float innerBorderG = 0.0f,
                      float innerBorderB = 0.0f,
                      float innerBorderA = 0.0f,
                      float motionVectorX = 0.0f,
                      float motionVectorY = 0.0f,
                      float motionVectorDX = 0.0f,
                      float motionVectorDY = 0.0f,
                      float motionVectorLength = 0.0f,
                      float motionVectorR = 0.0f,
                      float motionVectorG = 0.0f,
                      float motionVectorB = 0.0f,
                      float motionVectorA = 0.0f,
                      const CustomShapeDrawCommand* customShapes = nullptr,
                      size_t customShapeCount = 0,
                      const CustomWaveVertex* customWaveVertices = nullptr,
                      size_t customWaveVertexCount = 0,
                      const CustomWaveDrawCommand* customWaveDraws = nullptr,
                      size_t customWaveDrawCount = 0,
                      const TextureWarpVertex* textureWarpVertices = nullptr,
                      size_t textureWarpVertexCount = 0);
    void Present();

    RECT GetOutputSize() const noexcept { return m_outputSize; }
    DXGI_FORMAT GetBackBufferFormat() const noexcept { return m_backBufferFormat; }
    std::wstring GetTextureDirectory() const { return m_textureDirectory; }
    bool HasPresetTextureOverride() const noexcept { return m_presetTextureOverride; }
    std::vector<std::wstring> GetActiveTextureFiles() const;

  private:
    void CreateFactory();
    void GetHardwareAdapter(IDXGIAdapter1** adapter);
    void MoveToNextFrame();
    bool WaitForGpu(DWORD timeoutMs = INFINITE);
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRenderTargetView() const noexcept;
    void CreateWaveformResources();
    void CreateTextureResources();
    void CreatePostProcessResources();
    void CreatePostProcessTexture();
    bool LoadTextureFromFile(const wchar_t* textureFile, UINT slotIndex);
    bool LoadTextureFromWic(const wchar_t* textureFile, UINT slotIndex);
    bool LoadTextureFromTga(const wchar_t* textureFile, UINT slotIndex);
    bool LoadTextureFromDds(const wchar_t* textureFile, UINT slotIndex);
    bool UploadTextureRGBA(UINT width, UINT height, const std::vector<uint8_t>& pixels, UINT slotIndex);
    bool UploadTextureData(UINT width,
                           UINT height,
                           DXGI_FORMAT format,
                           const uint8_t* pixels,
                           size_t pixelsSize,
                           UINT sourceRowPitch,
                           UINT sourceRowCount,
                           UINT slotIndex);
    bool SetTextureFilesInternal(const wchar_t* const* textureFiles, size_t textureFileCount, bool presetTextureOverride);
    void RefreshTextureFileList();
    bool IsPostProcessEnabled() const;
    bool IsPostProcessBlurEnabled() const;
    float GetPostProcessBlurAmount() const;
    bool IsTextureCyclingEnabled() const;
    DWORD GetTextureCycleIntervalMs() const;
    void MaybeCycleTexture();
    void CreateFeedbackResources();
    void CopyBackBufferToFeedback(UINT feedbackIndex);
    bool CopyBackBufferToPostProcessSource();
    std::wstring PickTextureFile() const;
    void DrawTextureQuad(float bass,
                         float mids,
                         float treble,
                         float decay,
                         float zoom,
                         float rot,
                         float motionCenterX,
                         float motionCenterY,
                         float motionDX,
                         float motionDY,
                         float motionStretchX,
                         float motionStretchY,
                         float motionWarp);
    void DrawTextureQuadFromSrv(D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
                                ID3D12DescriptorHeap* descriptorHeap,
                                float bass,
                                float mids,
                                float treble,
                                float decay,
                                float zoom,
                                float rot,
                                float alphaScale,
                                float zoomBias,
                                float angleBias,
                                bool flipU,
                                bool flipV,
                                float motionCenterX,
                                float motionCenterY,
                                float motionDX,
                                float motionDY,
                                float motionStretchX,
                                float motionStretchY,
                                float motionWarp);
    void DrawTexturedCustomShapesFromSrv(D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
                                         ID3D12DescriptorHeap* descriptorHeap,
                                         const CustomShapeDrawCommand* customShapes,
                                         size_t customShapeCount);
    void DrawTextureMeshFromSrv(D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
                                ID3D12DescriptorHeap* descriptorHeap,
                                const TextureWarpVertex* vertices,
                                size_t vertexCount,
                                float bass,
                                float mids,
                                float treble,
                                float decay,
                                float alphaScale);
    void DrawPostProcessFromSrv(D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
                                ID3D12DescriptorHeap* descriptorHeap,
                                float gamma,
                                bool brighten,
                                bool darken,
                                bool solarize,
                                bool invert,
                                float shaderAmount,
                                float blurAmount,
                                float blurEdgeDarken);

    static constexpr UINT c_maxBackBufferCount = 3;
    static constexpr UINT c_maxTextureLayers = 4;
    static constexpr UINT c_maxWaveformVertices = 65536;
    static constexpr UINT c_maxTextureVertices = 32768;

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

    struct TextureSlot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
        std::wstring file;
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
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_waveformAdditivePipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_solidPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_solidAdditivePipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_waveformVertexBuffers[c_maxBackBufferCount];
    D3D12_VERTEX_BUFFER_VIEW m_waveformVertexBufferViews[c_maxBackBufferCount]{};
    WaveformVertex* m_mappedWaveformVertexBuffers[c_maxBackBufferCount]{};
    D3D12_VERTEX_BUFFER_VIEW m_waveformVertexBufferView{};
    WaveformVertex* m_mappedWaveformVertices = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_textureRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_texturePipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_textureAlphaPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_textureAdditivePipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_textureVertexBuffers[c_maxBackBufferCount];
    TextureSlot m_textureSlots[c_maxTextureLayers];
    UINT m_activeTextureLayerCount = 0;
    UINT m_srvDescriptorSize = 0;
    D3D12_VERTEX_BUFFER_VIEW m_textureVertexBufferViews[c_maxBackBufferCount]{};
    TextureVertex* m_mappedTextureVertexBuffers[c_maxBackBufferCount]{};
    D3D12_VERTEX_BUFFER_VIEW m_textureVertexBufferView{};
    TextureVertex* m_mappedTextureVertices = nullptr;
    UINT m_textureVertexCursor = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_postProcessSrvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_postProcessRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_postProcessPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_postProcessTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_feedbackSrvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_feedbackTextures[2];
    UINT m_feedbackSrvDescriptorSize = 0;
    UINT m_feedbackIndex = 0;
    bool m_feedbackReady[2]{};
    std::wstring m_textureDirectory;
    std::wstring m_currentTextureFile;
    std::vector<std::wstring> m_textureFiles;
    size_t m_textureCycleIndex = 0;
    ULONGLONG m_lastTextureCycleTick = 0;
    bool m_presetTextureOverride = false;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValues[c_maxBackBufferCount]{};
    HANDLE m_fenceEvent = nullptr;
};
} // namespace DX
