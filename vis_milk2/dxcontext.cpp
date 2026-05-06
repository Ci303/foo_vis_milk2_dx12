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
    wchar_t dx12Mode[8]{};
    m_useD3D12 = GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_DEV", dx12Mode, static_cast<DWORD>(std::size(dx12Mode))) > 0 && wcscmp(dx12Mode, L"0") != 0;

    // Create the device.
    // Provide parameters for swap chain format, depth/stencil format, and back buffer count.
    const unsigned int flags = DX::DeviceResources::c_FlipPresent | ((m_current_mode.allow_page_tearing << 1) & DX::DeviceResources::c_AllowTearing) |
        ((m_current_mode.enable_hdr << 2) & DX::DeviceResources::c_EnableHDR);
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

void DXContext::DrawWaveform(const float* left,
                             const float* right,
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
                             float echoAlpha,
                             float echoZoom,
                             int echoOrientation,
                             bool waveUseDots,
                             bool waveThick,
                             bool waveAdditive,
                             bool waveBrighten,
                             int waveMode,
                             float waveParam,
                             bool darkenCenter,
                             float postGamma,
                             bool postInvert,
                             bool postBrighten,
                             bool postDarken,
                             bool postSolarize,
                             float postShaderAmount,
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
                             size_t textureWarpVertexCount)
{
    if (m_useD3D12)
    {
        m_d3d12Resources->DrawWaveform(left, right, sampleCount, bass, mids, treble, waveR, waveG, waveB, waveA, waveScale, waveX, waveY, decay, zoom, rot, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, echoAlpha, echoZoom, echoOrientation, waveUseDots, waveThick, waveAdditive, waveBrighten, waveMode, waveParam, darkenCenter, postGamma, postInvert, postBrighten, postDarken, postSolarize, postShaderAmount, outerBorderSize, outerBorderR, outerBorderG, outerBorderB, outerBorderA, innerBorderSize, innerBorderR, innerBorderG, innerBorderB, innerBorderA, motionVectorX, motionVectorY, motionVectorDX, motionVectorDY, motionVectorLength, motionVectorR, motionVectorG, motionVectorB, motionVectorA, customShapes, customShapeCount, customWaveVertices, customWaveVertexCount, customWaveDraws, customWaveDrawCount, textureWarpVertices, textureWarpVertexCount);
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
    auto const r = m_useD3D12 ? m_d3d12Resources->GetOutputSize() : m_deviceResources->GetOutputSize();
    if (m_useD3D12)
        m_d3d12Resources->WindowSizeChanged(r.right, r.bottom);
    else
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

    m_client_width = width;
    m_client_height = height;

    const bool resized = m_useD3D12 ? m_d3d12Resources->WindowSizeChanged(width, height) : m_deviceResources->WindowSizeChanged(width, height);
    if (!resized)
    {
        m_lastErr = DX_ERR_RESIZEFAILED;
        m_ready = FALSE;
        return FALSE;
    }

    if (!m_useD3D12)
        CreateWindowSizeDependentResources();

    m_ready = TRUE;
    return TRUE;
}

BOOL DXContext::OnWindowSwap(HWND window, int width, int height)
{
    if (!m_ready)
        return FALSE;

    if (!window)
        return FALSE;

    Clear();

    m_hwnd = window;
    m_client_width = width;
    m_client_height = height;

    const bool swapped = m_useD3D12 ? m_d3d12Resources->WindowSwap(m_hwnd, m_client_width, m_client_height) : m_deviceResources->WindowSwap(m_hwnd, m_client_width, m_client_height);
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
