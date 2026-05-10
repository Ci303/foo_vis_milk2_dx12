/*
  LICENSE
  -------
  Copyright 2005-2013 Nullsoft, Inc.
  Copyright 2021-2024 Jimmy Cassis
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

    * Neither the name of Nullsoft nor the names of its contributors may be used to
      endorse or promote products derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
  FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
  OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef __NULLSOFT_DX_PLUGIN_SHELL_DXCONTEXT_H__
#define __NULLSOFT_DX_PLUGIN_SHELL_DXCONTEXT_H__

#include "shell_defines.h"
#include "deviceresources.h"
#include "d3d12resources.h"
#include "d3d11shim.h"

#define DX_ERR_REGWIN -2
#define DX_ERR_CREATEWIN -3
#define DX_ERR_CREATE3D -4
#define DX_ERR_GETFORMAT -5
#define DX_ERR_FORMAT -6
#define DX_ERR_CREATEDEV_PROBABLY_OUTOFVIDEOMEMORY -7
#define DX_ERR_RESIZEFAILED -8
#define DX_ERR_CAPSFAIL -9
#define DX_ERR_BAD_FS_DISPLAYMODE -10
#define DX_ERR_USER_CANCELED -11
#define DX_ERR_CREATEDEV_NOT_AVAIL -12
#define DX_ERR_CREATEDDRAW -13
#define DX_ERR_SWAPFAIL -14

typedef struct _DXCONTEXT_PARAMS
{
    unsigned int allow_page_tearing;
    unsigned int enable_hdr;
    DXGI_FORMAT back_buffer_format;
    DXGI_FORMAT depth_buffer_format;
    UINT back_buffer_count;
    DXGI_SAMPLE_DESC msaa;
    D3D_FEATURE_LEVEL min_feature_level;
    LUID adapter_guid;
    wchar_t adapter_devicename[256];
    eScrMode screenmode; // WINDOWED, DESKTOP, FULLSCREEN, or FAKE FULLSCREEN
    int m_skin;
    HWND parent_window;
} DXCONTEXT_PARAMS;

class DXContext final
{
  public:
    DXContext(HWND hWndWinamp, DXCONTEXT_PARAMS* pParams) noexcept(false);
    ~DXContext();

    BOOL StartOrRestartDevice(DXCONTEXT_PARAMS* pParams); // also serves as Init() function
    BOOL OnWindowSizeChanged(int width, int height);
    BOOL OnWindowSwap(HWND window, int width, int height);
    void OnWindowMoved();
    void OnDisplayChange();
    inline HWND GetHwnd() const { return m_hwnd; };
    void Show();
    void Clear();
    void SetTextureDirectory(const wchar_t* textureDirectory);
    bool SetTextureFile(const wchar_t* textureFile);
    bool SetTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount);
    bool SetPresetTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount);
    void ClearPresetTextureOverride();
    bool SetD3D12PresetCompositeShader(const void* bytecode, size_t bytecodeSize);
    void ClearD3D12PresetCompositeShader();
    void SetD3D12PresetShaderRuntimeConstants(float time,
                                              float fps,
                                              float frame,
                                              float progress,
                                              float bass,
                                              float mids,
                                              float treble,
                                              float bassAtt,
                                              float midsAtt,
                                              float trebleAtt,
                                              const float* qValues,
                                              const float* randFrame,
                                              const float* randPreset,
                                              const float* blurMin,
                                              const float* blurMax,
                                              const float* rotMatrices);
    bool CaptureD3D12Frame(std::vector<uint8_t>* pixels, UINT* width, UINT* height);
    bool SetD3D12ResumeFeedback(UINT width, UINT height, const std::vector<uint8_t>& pixels);
    void SetD3D12TextOverlay(const wchar_t* topLeft,
                             const wchar_t* topRight,
                             const wchar_t* bottomLeft,
                             const wchar_t* debugLine = nullptr,
                             const wchar_t* centerText = nullptr,
                             float centerX = 0.5f,
                             float centerY = 0.5f,
                             float centerScale = 1.0f,
                             float centerR = 1.0f,
                             float centerG = 1.0f,
                             float centerB = 1.0f,
                             float centerA = 0.0f);
    void DrawWaveform(const float* left,
                      const float* right,
                      const float* spectrumLeft,
                      const float* spectrumRight,
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
                      float waveSmoothing = 0.0f,
                      float waveAlphaVolumeScale = 1.0f,
                      bool darkenCenter = false,
                      float postGamma = 1.0f,
                      bool postInvert = false,
                      bool postBrighten = false,
                      bool postDarken = false,
                      bool postSolarize = false,
                      float postShaderAmount = 0.0f,
                      const float* postHueShaderColors = nullptr,
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
                      const DX::CustomShapeDrawCommand* customShapes = nullptr,
                      size_t customShapeCount = 0,
                      const DX::CustomWaveVertex* customWaveVertices = nullptr,
                      size_t customWaveVertexCount = 0,
                      const DX::CustomWaveDrawCommand* customWaveDraws = nullptr,
                      size_t customWaveDrawCount = 0,
                      const DX::TextureWarpVertex* textureWarpVertices = nullptr,
                      size_t textureWarpVertexCount = 0,
                      int textureWarpGridX = 0,
                      int textureWarpGridY = 0);
    void RestoreTarget();
    int GetBitDepth() const { return m_bpp; };

    void CreateDeviceIndependentResources();
    void CreateDeviceDependentResources();
    void CreateWindowSizeDependentResources();

    ID3D11Device1* GetD3DDevice() const noexcept { return m_deviceResources->GetD3DDevice(); }
    DXGI_FORMAT GetBackBufferFormat() const noexcept { return m_useD3D12 ? m_d3d12Resources->GetBackBufferFormat() : m_deviceResources->GetBackBufferFormat(); }
    ID2D1Factory1* GetD2DFactory() const noexcept { return m_deviceResources->GetD2DFactory(); }
    ID2D1Device* GetD2DDevice() const noexcept { return m_deviceResources->GetD2DDevice(); }
    ID2D1DeviceContext* GetD2DDeviceContext() const noexcept { return m_deviceResources->GetD2DDeviceContext(); }
    IDWriteFactory1* GetDWriteFactory() const noexcept { return m_deviceResources->GetDWriteFactory(); }
    DX::DeviceResources* GetDeviceResources() const noexcept { return m_deviceResources.get(); }
    bool IsD3D12Mode() const noexcept { return m_useD3D12; }

    // DO NOT WRITE TO THESE FROM OUTSIDE THE CLASS
    int m_ready;
    HRESULT m_lastErr;
    int m_client_width;
    int m_client_height;
    int m_frame_delay;
    DXCONTEXT_PARAMS m_current_mode;
    std::unique_ptr<D3D11Shim> m_lpDevice;

  protected:
    HWND m_hwnd;
    int m_bpp;

    BOOL Internal_Init(DXCONTEXT_PARAMS* pParams, BOOL bFirstInit);
    void Internal_CleanUp();
    unsigned int GetD3D12Options() const noexcept;
    bool RecreateD3D12ResourcesForWindow(HWND window, int width, int height);

    bool m_useD3D12;
    bool m_d3d12ResizeSwapChain = false;
    bool m_d3d12ResizePending = false;
    int m_d3d12PendingWidth = 0;
    int m_d3d12PendingHeight = 0;
    std::unique_ptr<DX::DeviceResources> m_deviceResources;
    std::unique_ptr<DX::D3D12Resources> m_d3d12Resources;
};

#endif
