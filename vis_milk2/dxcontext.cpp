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

#include "pch.h"
#include "dxcontext.h"

namespace
{
bool IsEnvFlagEnabled(const wchar_t* name, bool defaultValue = false)
{
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    if (length == 0)
        return defaultValue;

    return wcscmp(value, L"0") != 0;
}
}

DXContext::DXContext(HWND hWndWinamp, DXCONTEXT_PARAMS* pParams) noexcept(false)
{
    m_hwnd = hWndWinamp;
    m_bpp = 32;
    m_frame_delay = 0;
    m_client_height = 0;
    m_client_width = 0;
    memcpy_s(&m_current_mode, sizeof(m_current_mode), pParams, sizeof(DXCONTEXT_PARAMS));

    // Clear the error register.
    m_lastErr = S_OK;

    // Clear the active flag.
    m_ready = FALSE;
    m_useD3D12 = IsEnvFlagEnabled(L"FOO_VIS_MILK2_DX12_DEV");
    m_d3d12ResizeSwapChain = IsEnvFlagEnabled(L"FOO_VIS_MILK2_DX12_RESIZE_SWAPCHAIN");

    // Create the device.
    // Provide parameters for swap chain format, depth/stencil format, and back buffer count.
    const unsigned int flags = GetD3D12Options();
    if (m_useD3D12)
    {
        m_d3d12Resources = std::make_unique<DX::D3D12Resources>(m_current_mode.back_buffer_format, m_current_mode.back_buffer_count, flags);
    }
    else
    {
        m_deviceResources = std::make_unique<DX::DeviceResources>(
            m_current_mode.back_buffer_format, // backBufferFormat (default: DXGI_FORMAT_B8G8R8A8_UNORM)
            m_current_mode.depth_buffer_format, // depthBufferFormat (default: DXGI_FORMAT_D24_UNORM_S8_UINT)
            m_current_mode.back_buffer_count, // backBufferCount (default: 2)
            m_current_mode.min_feature_level, // minFeatureLevel (default: D3D_FEATURE_LEVEL_9_1)
            flags // flags (default: flip, noTearing, noHDR)
        );
    }
}

DXContext::~DXContext()
{
    Internal_CleanUp();
}

void DXContext::Internal_CleanUp()
{
    // Clear active flag.
    m_ready = FALSE;

    // Release 3D interfaces.
    if (m_lpDevice)
        m_lpDevice.reset();
}

unsigned int DXContext::GetD3D12Options() const noexcept
{
    return DX::DeviceResources::c_FlipPresent |
           ((m_current_mode.allow_page_tearing << 1) & DX::DeviceResources::c_AllowTearing) |
           ((m_current_mode.enable_hdr << 2) & DX::DeviceResources::c_EnableHDR);
}

bool DXContext::RecreateD3D12ResourcesForWindow(HWND window, int width, int height)
{
    std::wstring textureDirectory;
    std::vector<std::wstring> textureFiles;
    bool presetTextureOverride = false;
    if (m_d3d12Resources)
    {
        textureDirectory = m_d3d12Resources->GetTextureDirectory();
        textureFiles = m_d3d12Resources->GetActiveTextureFiles();
        presetTextureOverride = m_d3d12Resources->HasPresetTextureOverride();
    }

    auto replacement = std::make_unique<DX::D3D12Resources>(m_current_mode.back_buffer_format, m_current_mode.back_buffer_count, GetD3D12Options());
    replacement->SetWindow(window, width, height);
    replacement->CreateDeviceResources();
    replacement->CreateWindowSizeDependentResources();

    if (presetTextureOverride && !textureFiles.empty())
    {
        std::vector<const wchar_t*> textureFilePtrs;
        textureFilePtrs.reserve(textureFiles.size());
        for (const auto& textureFile : textureFiles)
        {
            textureFilePtrs.push_back(textureFile.c_str());
        }
        replacement->SetPresetTextureFiles(textureFilePtrs.data(), textureFilePtrs.size());
    }
    else if (!textureDirectory.empty())
    {
        replacement->SetTextureDirectory(textureDirectory.c_str());
        if (!textureFiles.empty())
        {
            std::vector<const wchar_t*> textureFilePtrs;
            textureFilePtrs.reserve(textureFiles.size());
            for (const auto& textureFile : textureFiles)
            {
                textureFilePtrs.push_back(textureFile.c_str());
            }
            replacement->SetTextureFiles(textureFilePtrs.data(), textureFilePtrs.size());
        }
    }

    auto retired = std::move(m_d3d12Resources);
    m_d3d12Resources = std::move(replacement);
    return true;
}

BOOL DXContext::Internal_Init(DXCONTEXT_PARAMS* /* pParams */, BOOL /* bFirstInit */)
{
    RECT r;
    GetClientRect(m_hwnd, &r);
    m_client_width = std::max(1l, r.right - r.left);
    m_client_height = std::max(1l, r.bottom - r.top);

    if (m_useD3D12)
    {
        m_d3d12Resources->SetWindow(m_hwnd, m_client_width, m_client_height);
        m_d3d12Resources->CreateDeviceResources();
        m_d3d12Resources->CreateWindowSizeDependentResources();
        m_ready = TRUE;
        return TRUE;
    }

    m_deviceResources->SetWindow(m_hwnd, m_client_width, m_client_height);
    m_deviceResources->CreateDeviceIndependentResources();
    CreateDeviceIndependentResources();

    m_deviceResources->SetDpi();

    m_deviceResources->CreateDeviceResources();
    CreateDeviceDependentResources();

    m_deviceResources->CreateWindowSizeDependentResources();
    CreateWindowSizeDependentResources();

    m_ready = TRUE;
    return TRUE;
}

// Display the swap chain contents to the screen.
void DXContext::Show()
{
    if (m_useD3D12)
        m_d3d12Resources->Present();
    else
        m_deviceResources->Present();
}

// Clear the back buffers.
void DXContext::Clear()
{
    if (m_useD3D12)
    {
        m_d3d12Resources->Clear();
        return;
    }

    m_deviceResources->PIXBeginEvent(L"Clear");

    // Clear the views.
    auto context = m_deviceResources->GetD3DDeviceContext();
    auto renderTarget = m_deviceResources->GetRenderTargetView();
    auto depthStencil = m_deviceResources->GetDepthStencilView();

    context->ClearRenderTargetView(renderTarget, Colors::CornflowerBlue);
    context->ClearDepthStencilView(depthStencil, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    context->OMSetRenderTargets(1, &renderTarget, depthStencil);

    // Set the viewport.
    auto const viewport = m_deviceResources->GetScreenViewport();
    context->RSSetViewports(1, &viewport);

    m_deviceResources->PIXEndEvent();
}

void DXContext::SetTextureDirectory(const wchar_t* textureDirectory)
{
    if (m_useD3D12 && m_d3d12Resources)
    {
        m_d3d12Resources->SetTextureDirectory(textureDirectory);
    }
}

bool DXContext::SetTextureFile(const wchar_t* textureFile)
{
    return m_useD3D12 && m_d3d12Resources && m_d3d12Resources->SetTextureFile(textureFile);
}

bool DXContext::SetTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount)
{
    return m_useD3D12 && m_d3d12Resources && m_d3d12Resources->SetTextureFiles(textureFiles, textureFileCount);
}

void DXContext::ClearTextureFiles()
{
    if (m_useD3D12 && m_d3d12Resources)
    {
        m_d3d12Resources->ClearTextureFiles();
    }
}

bool DXContext::SetPresetTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount)
{
    return m_useD3D12 && m_d3d12Resources && m_d3d12Resources->SetPresetTextureFiles(textureFiles, textureFileCount);
}

bool DXContext::SetStandaloneTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount)
{
    return m_useD3D12 && m_d3d12Resources && m_d3d12Resources->SetStandaloneTextureFiles(textureFiles, textureFileCount);
}

void DXContext::ClearPresetTextureOverride()
{
    if (m_useD3D12 && m_d3d12Resources)
    {
        m_d3d12Resources->ClearPresetTextureOverride();
    }
}

bool DXContext::SetD3D12PresetWarpShader(const void* bytecode, size_t bytecodeSize)
{
    return m_useD3D12 && m_d3d12Resources && m_d3d12Resources->SetPresetWarpShader(bytecode, bytecodeSize);
}

void DXContext::ClearD3D12PresetWarpShader()
{
    if (m_useD3D12 && m_d3d12Resources)
    {
        m_d3d12Resources->ClearPresetWarpShader();
    }
}

bool DXContext::SetD3D12PresetCompositeShader(const void* bytecode, size_t bytecodeSize)
{
    return m_useD3D12 && m_d3d12Resources && m_d3d12Resources->SetPresetCompositeShader(bytecode, bytecodeSize);
}

void DXContext::ClearD3D12PresetCompositeShader()
{
    if (m_useD3D12 && m_d3d12Resources)
    {
        m_d3d12Resources->ClearPresetCompositeShader();
    }
}

void DXContext::SetD3D12PresetShaderRuntimeConstants(float presetTime,
                                                     float globalTime,
                                                     float fps,
                                                     float frame,
                                                     float progress,
                                                     float canvasWidth,
                                                     float canvasHeight,
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
                                                     const float* rotMatrices)
{
    if (m_useD3D12 && m_d3d12Resources)
    {
        m_d3d12Resources->SetPresetShaderRuntimeConstants(presetTime,
                                                          globalTime,
                                                          fps,
                                                          frame,
                                                          progress,
                                                          canvasWidth,
                                                          canvasHeight,
                                                          bass,
                                                          mids,
                                                          treble,
                                                          bassAtt,
                                                          midsAtt,
                                                          trebleAtt,
                                                          qValues,
                                                          randFrame,
                                                          randPreset,
                                                          blurMin,
                                                          blurMax,
                                                          rotMatrices);
    }
}

bool DXContext::CaptureD3D12Frame(std::vector<uint8_t>* pixels, UINT* width, UINT* height)
{
    return m_useD3D12 && m_d3d12Resources && m_d3d12Resources->CaptureCurrentFrame(pixels, width, height);
}

bool DXContext::SetD3D12ResumeFeedback(UINT width, UINT height, const std::vector<uint8_t>& pixels)
{
    return m_useD3D12 && m_d3d12Resources && m_d3d12Resources->SetResumeFeedbackFromFrame(width, height, pixels);
}

void DXContext::ResetD3D12VisualHistory()
{
    if (m_useD3D12 && m_d3d12Resources)
    {
        m_d3d12Resources->ResetVisualHistory();
    }
}

void DXContext::SetD3D12TextOverlay(const wchar_t* topLeft,
                                    const wchar_t* topRight,
                                    const wchar_t* bottomLeft,
                                    const wchar_t* debugLine,
                                    const wchar_t* centerText,
                                    float centerX,
                                    float centerY,
                                    float centerScale,
                                    float centerR,
                                    float centerG,
                                    float centerB,
                                    float centerA)
{
    if (m_useD3D12 && m_d3d12Resources)
    {
        m_d3d12Resources->SetTextOverlay(topLeft, topRight, bottomLeft, debugLine, centerText, centerX, centerY, centerScale, centerR, centerG, centerB, centerA);
    }
}

void DXContext::DrawWaveform(const float* left,
                             const float* right,
                             const float* spectrumLeft,
                             const float* spectrumRight,
                             size_t sampleCount,
                             float bass,
                             float mids,
                             float treble,
                             float waveR,
                             float waveG,
                             float waveB,
                             float waveA,
                             float waveScale,
                             float waveX,
                             float waveY,
                             float decay,
                             float zoom,
                             float rot,
                             float motionCenterX,
                             float motionCenterY,
                             float motionDX,
                             float motionDY,
                             float motionStretchX,
                             float motionStretchY,
                             float motionWarp,
                             bool textureWrap,
                             bool suppressVisualFeedback,
                             float echoAlpha,
                             float echoZoom,
                             int echoOrientation,
                             bool waveUseDots,
                             bool waveThick,
                             bool waveAdditive,
                             bool waveBrighten,
                             int waveMode,
                             float waveParam,
                             float waveSmoothing,
                             float waveAlphaVolumeScale,
                             bool darkenCenter,
                             float postGamma,
                             bool postInvert,
                             bool postBrighten,
                             bool postDarken,
                             bool postSolarize,
                             float postShaderAmount,
                             const float* postHueShaderColors,
                             float postBlurAmount,
                             float postBlurEdgeDarken,
                             float outerBorderSize,
                             float outerBorderR,
                             float outerBorderG,
                             float outerBorderB,
                             float outerBorderA,
                             float innerBorderSize,
                             float innerBorderR,
                             float innerBorderG,
                             float innerBorderB,
                             float innerBorderA,
                             float motionVectorX,
                             float motionVectorY,
                             float motionVectorDX,
                             float motionVectorDY,
                             float motionVectorLength,
                             float motionVectorR,
                             float motionVectorG,
                             float motionVectorB,
                             float motionVectorA,
                             const DX::CustomShapeDrawCommand* customShapes,
                             size_t customShapeCount,
                             const DX::CustomWaveVertex* customWaveVertices,
                             size_t customWaveVertexCount,
                             const DX::CustomWaveDrawCommand* customWaveDraws,
                             size_t customWaveDrawCount,
                             const DX::TextureWarpVertex* textureWarpVertices,
                             size_t textureWarpVertexCount,
                             int textureWarpGridX,
                             int textureWarpGridY,
                             const DX::TextureWarpVertex* compositeVertices,
                             size_t compositeVertexCount)
{
    if (m_useD3D12)
    {
        if (m_d3d12ResizePending)
        {
            try
            {
                if (m_d3d12Resources->WindowSizeChanged(m_d3d12PendingWidth, m_d3d12PendingHeight))
                {
                    m_client_width = m_d3d12PendingWidth;
                    m_client_height = m_d3d12PendingHeight;
                }
            }
            catch (...)
            {
            }
            m_d3d12ResizePending = false;
        }

        m_d3d12Resources->DrawWaveform(left, right, spectrumLeft, spectrumRight, sampleCount, bass, mids, treble, waveR, waveG, waveB, waveA, waveScale, waveX, waveY, decay, zoom, rot, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, textureWrap, suppressVisualFeedback, echoAlpha, echoZoom, echoOrientation, waveUseDots, waveThick, waveAdditive, waveBrighten, waveMode, waveParam, waveSmoothing, waveAlphaVolumeScale, darkenCenter, postGamma, postInvert, postBrighten, postDarken, postSolarize, postShaderAmount, postHueShaderColors, postBlurAmount, postBlurEdgeDarken, outerBorderSize, outerBorderR, outerBorderG, outerBorderB, outerBorderA, innerBorderSize, innerBorderR, innerBorderG, innerBorderB, innerBorderA, motionVectorX, motionVectorY, motionVectorDX, motionVectorDY, motionVectorLength, motionVectorR, motionVectorG, motionVectorB, motionVectorA, customShapes, customShapeCount, customWaveVertices, customWaveVertexCount, customWaveDraws, customWaveDrawCount, textureWarpVertices, textureWarpVertexCount, textureWarpGridX, textureWarpGridY, compositeVertices, compositeVertexCount);
    }
}

void DXContext::RestoreTarget()
{
    if (m_useD3D12)
        return;

    auto rt = m_deviceResources->GetRenderTarget();
    m_lpDevice->SetRenderTarget(rt);
}

// Call this to [re]initialize the DirectX environment with new parameters.
// Examples: startup; toggle windowed/fullscreen mode; change fullscreen resolution;
//           and so on.
// Clean up all the DirectX collateral first (textures, vertex buffers,
// D3DX allocations, etc...) and reallocate it afterwards!
// Note: for windowed mode, `pParams->disp_mode` (w/h/r/f) is ignored.
BOOL DXContext::StartOrRestartDevice(DXCONTEXT_PARAMS* pParams)
{
    if (!m_ready)
    {
        // First time init: create a fresh new device.
        return Internal_Init(pParams, TRUE);
    }
    else
    {
        // Re-init: preserve the DX11 object (m_lpD3D),
        // but destroy and re-create the DX11 device (m_lpDevice).
        m_ready = FALSE;

        m_lpDevice.reset();

        // But leave the D3D object!
        return Internal_Init(pParams, FALSE);
    }
}

void DXContext::OnWindowMoved()
{
    if (m_useD3D12)
        return;

    auto const r = m_useD3D12 ? m_d3d12Resources->GetOutputSize() : m_deviceResources->GetOutputSize();
    m_deviceResources->WindowSizeChanged(r.right, r.bottom);
}

void DXContext::OnDisplayChange()
{
    if (!m_useD3D12)
        m_deviceResources->UpdateColorSpace();
}

// Call this function on `WM_EXITSIZEMOVE`.
// Clean up all the DirectX stuff first (textures, vertex
// buffers, etc...) and reallocate it afterwards!
BOOL DXContext::OnWindowSizeChanged(int width, int height)
{
    if (!m_ready)
        return FALSE;

    if ((m_client_width == width) && (m_client_height == height))
        return TRUE;

    if (m_useD3D12)
    {
        m_client_width = std::max(width, 1);
        m_client_height = std::max(height, 1);

        const bool allowSwapChainResize = m_d3d12ResizeSwapChain || m_current_mode.screenmode == FULLSCREEN;
        if (!allowSwapChainResize)
        {
            m_d3d12ResizePending = false;
            return TRUE;
        }

        m_d3d12PendingWidth = m_client_width;
        m_d3d12PendingHeight = m_client_height;
        m_d3d12ResizePending = true;
        return TRUE;
    }

    m_client_width = width;
    m_client_height = height;

    bool resized = false;
    try
    {
        resized = m_deviceResources->WindowSizeChanged(width, height);
    }
    catch (...)
    {
        resized = false;
    }
    if (!resized)
    {
        m_lastErr = DX_ERR_RESIZEFAILED;
        m_ready = FALSE;
        return FALSE;
    }

    CreateWindowSizeDependentResources();

    m_ready = TRUE;
    return TRUE;
}

bool DXContext::EnsureD3D12WindowSize(int width, int height)
{
    if (!m_ready || !m_useD3D12 || !m_d3d12Resources)
    {
        return false;
    }

    const int targetWidth = std::max(width, 1);
    const int targetHeight = std::max(height, 1);
    const RECT outputSize = m_d3d12Resources->GetOutputSize();
    if (outputSize.right != targetWidth || outputSize.bottom != targetHeight)
    {
        try
        {
            if (!m_d3d12Resources->WindowSizeChanged(targetWidth, targetHeight))
            {
                m_lastErr = DX_ERR_RESIZEFAILED;
                m_ready = FALSE;
                return false;
            }
        }
        catch (...)
        {
            m_lastErr = DX_ERR_RESIZEFAILED;
            m_ready = FALSE;
            return false;
        }
    }

    m_client_width = targetWidth;
    m_client_height = targetHeight;
    m_d3d12PendingWidth = targetWidth;
    m_d3d12PendingHeight = targetHeight;
    m_d3d12ResizePending = false;
    return true;
}

BOOL DXContext::OnWindowSwap(HWND window, int width, int height)
{
    if (!m_ready)
        return FALSE;

    if (!window)
        return FALSE;

    if (!m_useD3D12)
    {
        Clear();
    }

    m_hwnd = window;
    m_client_width = std::max(width, 1);
    m_client_height = std::max(height, 1);

    if (m_useD3D12)
    {
        try
        {
            if (!m_d3d12Resources->WindowSwap(m_hwnd, m_client_width, m_client_height) &&
                !RecreateD3D12ResourcesForWindow(m_hwnd, m_client_width, m_client_height))
            {
                m_lastErr = DX_ERR_SWAPFAIL;
                m_ready = FALSE;
                return FALSE;
            }
        }
        catch (...)
        {
            m_lastErr = DX_ERR_SWAPFAIL;
            m_ready = FALSE;
            return FALSE;
        }

        m_ready = TRUE;
        return TRUE;
    }

    const bool swapped = m_deviceResources->WindowSwap(m_hwnd, m_client_width, m_client_height);
    if (!swapped)
    {
        m_lastErr = DX_ERR_SWAPFAIL;
        m_ready = FALSE;
        return FALSE;
    }

    if (!m_useD3D12)
        CreateWindowSizeDependentResources();

    m_ready = TRUE;
    return TRUE;
}

#pragma region Direct3D Resources
// Allocate the resources that do not depend on the device.
void DXContext::CreateDeviceIndependentResources()
{
}

// Allocate the resources that depend on the device.
void DXContext::CreateDeviceDependentResources()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto context = m_deviceResources->GetD3DDeviceContext();
    m_lpDevice = std::make_unique<D3D11Shim>(device, context);
    m_lpDevice->Initialize();
}

// Allocate all memory resources that change on a window SizeChanged event.
void DXContext::CreateWindowSizeDependentResources()
{
}

#pragma endregion
