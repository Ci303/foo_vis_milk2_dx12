/*
 * d3d12resources.cpp - Minimal Direct3D 12 device and swap chain path.
 */

#include "pch.h"
#include "d3d12resources.h"
#include "deviceresources.h"

#include <wincodec.h>
#include <fstream>

using Microsoft::WRL::ComPtr;

namespace DX
{
D3D12Resources::D3D12Resources(DXGI_FORMAT backBufferFormat, UINT backBufferCount, unsigned int flags) :
    m_backBufferFormat(backBufferFormat),
    m_backBufferCount(std::clamp(backBufferCount, 2u, c_maxBackBufferCount)),
    m_options(flags)
{
}

D3D12Resources::~D3D12Resources()
{
    if (m_commandQueue && m_fence)
    {
        WaitForGpu();
    }

    if (m_waveformVertexBuffer && m_mappedWaveformVertices)
    {
        m_waveformVertexBuffer->Unmap(0, nullptr);
        m_mappedWaveformVertices = nullptr;
    }
    if (m_textureVertexBuffer && m_mappedTextureVertices)
    {
        m_textureVertexBuffer->Unmap(0, nullptr);
        m_mappedTextureVertices = nullptr;
    }

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void D3D12Resources::SetWindow(HWND window, int width, int height) noexcept
{
    m_window = window;
    m_outputSize.left = m_outputSize.top = 0;
    m_outputSize.right = static_cast<long>(std::max(width, 1));
    m_outputSize.bottom = static_cast<long>(std::max(height, 1));
}

void D3D12Resources::CreateDeviceResources()
{
    CreateFactory();

    ComPtr<IDXGIAdapter1> adapter;
    GetHardwareAdapter(adapter.GetAddressOf());
    if (!adapter)
    {
        throw std::runtime_error("No Direct3D 12 hardware adapter found");
    }

    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_d3dDevice.ReleaseAndGetAddressOf())));

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_commandQueue.ReleaseAndGetAddressOf())));

    for (UINT i = 0; i < m_backBufferCount; ++i)
    {
        ThrowIfFailed(m_d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_commandAllocators[i].ReleaseAndGetAddressOf())));
    }

    ThrowIfFailed(m_d3dDevice->CreateCommandList(0,
                                                 D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 m_commandAllocators[0].Get(),
                                                 nullptr,
                                                 IID_PPV_ARGS(m_commandList.ReleaseAndGetAddressOf())));
    ThrowIfFailed(m_commandList->Close());

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = m_backBufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(m_rtvHeap.ReleaseAndGetAddressOf())));
    m_rtvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    ThrowIfFailed(m_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.ReleaseAndGetAddressOf())));
    m_fenceValues[0] = 1;
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
    {
        throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreateEventW");
    }

    CreateWaveformResources();
    CreateTextureResources();
    if (IsPostProcessEnabled())
    {
        CreatePostProcessResources();
    }
}

void D3D12Resources::CreateWindowSizeDependentResources()
{
    if (!m_window)
    {
        throw std::logic_error("Call SetWindow with a valid Win32 window handle");
    }

    WaitForGpu();
    for (auto& renderTarget : m_renderTargets)
    {
        renderTarget.Reset();
    }
    for (auto& feedbackTexture : m_feedbackTextures)
    {
        feedbackTexture.Reset();
    }
    m_postProcessTexture.Reset();
    m_feedbackReady[0] = false;
    m_feedbackReady[1] = false;

    const UINT width = std::max<UINT>(static_cast<UINT>(m_outputSize.right - m_outputSize.left), 1u);
    const UINT height = std::max<UINT>(static_cast<UINT>(m_outputSize.bottom - m_outputSize.top), 1u);
    const UINT swapChainFlags = (m_options & DeviceResources::c_AllowTearing) ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    if (m_swapChain)
    {
        ThrowIfFailed(m_swapChain->ResizeBuffers(m_backBufferCount, width, height, m_backBufferFormat, swapChainFlags));
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    }
    else
    {
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = m_backBufferFormat;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = m_backBufferCount;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapChainDesc.Flags = swapChainFlags;

        ComPtr<IDXGISwapChain1> swapChain;
        ThrowIfFailed(m_dxgiFactory->CreateSwapChainForHwnd(m_commandQueue.Get(), m_window, &swapChainDesc, nullptr, nullptr, swapChain.GetAddressOf()));
        ThrowIfFailed(m_dxgiFactory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER));
        ThrowIfFailed(swapChain.As(&m_swapChain));
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    }

    auto rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < m_backBufferCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(m_renderTargets[i].ReleaseAndGetAddressOf())));
        m_d3dDevice->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    CreateFeedbackResources();
    if (IsPostProcessEnabled())
    {
        CreatePostProcessTexture();
    }
}

bool D3D12Resources::WindowSizeChanged(int width, int height)
{
    RECT newRc{};
    newRc.right = static_cast<long>(std::max(width, 1));
    newRc.bottom = static_cast<long>(std::max(height, 1));
    if (newRc.right == m_outputSize.right && newRc.bottom == m_outputSize.bottom)
    {
        return false;
    }

    m_outputSize = newRc;
    CreateWindowSizeDependentResources();
    return true;
}

bool D3D12Resources::WindowSwap(HWND window, int width, int height)
{
    WaitForGpu();
    for (auto& renderTarget : m_renderTargets)
    {
        renderTarget.Reset();
    }
    m_swapChain.Reset();

    SetWindow(window, width, height);
    CreateWindowSizeDependentResources();
    return true;
}

void D3D12Resources::SetTextureDirectory(const wchar_t* textureDirectory)
{
    m_textureDirectory = textureDirectory ? textureDirectory : L"";
    if (!m_textureDirectory.empty() && m_textureDirectory.back() != L'\\' && m_textureDirectory.back() != L'/')
    {
        m_textureDirectory.push_back(L'\\');
    }

    if (m_d3dDevice && m_commandQueue)
    {
        RefreshTextureFileList();
        const std::wstring textureFile = PickTextureFile();
        if (!textureFile.empty())
        {
            LoadTextureFromFile(textureFile.c_str());
        }
    }
}

bool D3D12Resources::SetTextureFile(const wchar_t* textureFile)
{
    if (!m_d3dDevice || !m_commandQueue || !textureFile || !*textureFile)
    {
        return false;
    }

    return LoadTextureFromFile(textureFile);
}

void D3D12Resources::Clear()
{
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    const float clearColor[] = {0.05f, 0.10f, 0.18f, 1.0f};
    const auto rtvHandle = GetCurrentRenderTargetView();
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
}

void D3D12Resources::DrawWaveform(const float* left,
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
                                  const CustomShapeDrawCommand* customShapes,
                                  size_t customShapeCount,
                                  const CustomWaveVertex* customWaveVertices,
                                  size_t customWaveVertexCount,
                                  const CustomWaveDrawCommand* customWaveDraws,
                                  size_t customWaveDrawCount,
                                  const TextureWarpVertex* textureWarpVertices,
                                  size_t textureWarpVertexCount)
{
    if (!left || !right || !m_mappedWaveformVertices || !m_waveformPipelineState || !m_waveformAdditivePipelineState || !m_solidPipelineState || !m_solidAdditivePipelineState || !m_waveformRootSignature)
    {
        Clear();
        return;
    }

    const UINT count = std::min<UINT>(static_cast<UINT>(sampleCount), c_maxWaveformVertices / 6);
    if (count < 2)
    {
        Clear();
        return;
    }

    const float energy = std::clamp((bass + mids + treble) / 6.0f, 0.0f, 1.0f);
    decay = std::clamp(decay, 0.70f, 1.0f);
    zoom = std::clamp(zoom, 0.5f, 2.0f);
    motionCenterX = std::clamp(motionCenterX, -10.0f, 10.0f);
    motionCenterY = std::clamp(motionCenterY, -10.0f, 10.0f);
    motionDX = std::clamp(motionDX, -10.0f, 10.0f);
    motionDY = std::clamp(motionDY, -10.0f, 10.0f);
    motionStretchX = fabsf(motionStretchX) < 0.001f ? 1.0f : std::clamp(motionStretchX, -100.0f, 100.0f);
    motionStretchY = fabsf(motionStretchY) < 0.001f ? 1.0f : std::clamp(motionStretchY, -100.0f, 100.0f);
    motionWarp = std::clamp(motionWarp, -10.0f, 10.0f);
    echoAlpha = std::clamp(echoAlpha, 0.0f, 1.0f);
    echoZoom = std::clamp(echoZoom, 0.001f, 1000.0f);
    echoOrientation = ((echoOrientation % 4) + 4) % 4;
    postGamma = std::clamp(postGamma, 0.0f, 8.0f);
    postShaderAmount = std::clamp(postShaderAmount, 0.0f, 1.0f);
    if (waveBrighten)
    {
        const float maxChannel = std::max({waveR, waveG, waveB});
        if (maxChannel > 0.01f)
        {
            waveR /= maxChannel;
            waveG /= maxChannel;
            waveB /= maxChannel;
        }
    }
    else
    {
        const float maxChannel = std::max({waveR, waveG, waveB});
        if (maxChannel > 0.01f && maxChannel < 0.55f)
        {
            const float lift = 0.55f / maxChannel;
            waveR *= lift;
            waveG *= lift;
            waveB *= lift;
        }
    }
    const float bgLift = (1.0f - decay) * 0.45f;
    const float bg[] = {bgLift + energy * 0.035f, bgLift * 0.65f + mids * 0.018f, bgLift * 0.40f + treble * 0.025f, 1.0f};
    const float centerX = std::clamp((waveX - 0.5f) * 1.85f, -0.90f, 0.90f);
    const float centerY = std::clamp((0.5f - waveY) * 1.55f, -0.78f, 0.78f);
    const float c = cosf(rot);
    const float s = sinf(rot);
    const float pixelX = 2.0f / static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const float pixelY = 2.0f / static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    const UINT previousFeedbackIndex = 1u - m_feedbackIndex;
    const bool canDrawTexturedShapes = m_feedbackReady[previousFeedbackIndex] &&
                                       m_feedbackSrvHeap &&
                                       m_textureAlphaPipelineState &&
                                       m_textureAdditivePipelineState &&
                                       m_textureRootSignature &&
                                       m_mappedTextureVertices;
    const bool postProcessActive = m_postProcessTexture &&
                                   m_postProcessSrvHeap &&
                                   m_postProcessPipelineState &&
                                   m_postProcessRootSignature &&
                                   m_mappedTextureVertices &&
                                   IsPostProcessEnabled() &&
                                   (fabsf(postGamma - 1.0f) > 0.001f ||
                                    postInvert ||
                                    postBrighten ||
                                    postDarken ||
                                    postSolarize ||
                                   postShaderAmount > 0.001f);
    const bool hasTextureWarpMesh = textureWarpVertices && textureWarpVertexCount >= 3;

    waveMode = ((waveMode % 8) + 8) % 8;
    if ((waveMode == 0 || waveMode == 1 || waveMode == 4) && (waveParam < -1.0f || waveParam > 1.0f))
    {
        waveParam = waveParam * 0.5f + 0.5f;
        waveParam -= floorf(waveParam);
        waveParam = fabsf(waveParam) * 2.0f - 1.0f;
    }
    else
    {
        waveParam = std::clamp(waveParam, -1.0f, 1.0f);
    }

    const UINT sourceCount = std::min<UINT>(count, static_cast<UINT>(sampleCount));
    UINT activeCount = sourceCount;
    UINT visibleSegments = 1;
    const float timeSeconds = static_cast<float>(GetTickCount64() % 600000ULL) * 0.001f;
    const float waveScaleClamped = std::clamp(waveScale, 0.10f, 5.0f);
    const float responsiveScale = std::clamp(0.85f + bass * 0.10f + mids * 0.05f, 0.75f, 1.35f);
    const float baseR = std::clamp(waveR + bass * 0.08f, 0.0f, 1.0f);
    const float baseG = std::clamp(waveG + mids * 0.08f, 0.0f, 1.0f);
    const float baseB = std::clamp(waveB + treble * 0.08f, 0.0f, 1.0f);
    const float baseA = std::clamp(waveA, 0.05f, 1.0f);

    auto sampleAt = [&](const float* source, UINT index) {
        return std::clamp(source[std::min(index, sourceCount - 1)], -1.0f, 1.0f);
    };

    auto writePoint = [&](UINT segment, UINT index, float localX, float localY, float r, float g, float b) {
        const UINT groupCount = activeCount * visibleSegments;
        const UINT baseIndex = segment * activeCount + index;
        localX /= zoom;
        localY /= zoom;
        const float x = centerX + localX * c - localY * s;
        const float y = centerY + localX * s + localY * c;

        WaveformVertex& vertex = m_mappedWaveformVertices[baseIndex];
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.color[0] = r;
        vertex.color[1] = g;
        vertex.color[2] = b;
        vertex.color[3] = baseA;

        WaveformVertex& backing = m_mappedWaveformVertices[groupCount + baseIndex];
        backing.position[0] = x + pixelX;
        backing.position[1] = y + pixelY;
        backing.color[0] = 0.0f;
        backing.color[1] = 0.0f;
        backing.color[2] = 0.0f;
        backing.color[3] = 1.0f;

        WaveformVertex& highlight = m_mappedWaveformVertices[2 * groupCount + baseIndex];
        highlight.position[0] = x;
        highlight.position[1] = y;
        highlight.color[0] = std::clamp(r * 0.65f + 0.35f, 0.0f, 1.0f);
        highlight.color[1] = std::clamp(g * 0.65f + 0.35f, 0.0f, 1.0f);
        highlight.color[2] = std::clamp(b * 0.65f + 0.35f, 0.0f, 1.0f);
        highlight.color[3] = 1.0f;

        WaveformVertex& thick = m_mappedWaveformVertices[3 * groupCount + baseIndex];
        thick.position[0] = x - pixelX;
        thick.position[1] = y + pixelY;
        thick.color[0] = r;
        thick.color[1] = g;
        thick.color[2] = b;
        thick.color[3] = baseA;
    };

    if (waveMode == 0)
    {
        activeCount = std::min<UINT>(sourceCount / 2, 240u);
        const UINT sampleOffset = sourceCount > activeCount * 2 ? (sourceCount - activeCount * 2) / 2 : 0;
        for (UINT i = 0; i < activeCount; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(activeCount - 1);
            float radius = 0.5f + 0.4f * sampleAt(right, i + sampleOffset) + waveParam;
            if (i < activeCount / 10)
            {
                const float mix = 0.5f - 0.5f * cosf((static_cast<float>(i) / (static_cast<float>(activeCount) * 0.1f)) * 3.1415927f);
                const float radius2 = 0.5f + 0.4f * sampleAt(right, i + activeCount + sampleOffset) + waveParam;
                radius = radius2 * (1.0f - mix) + radius * mix;
            }

            radius = std::clamp(radius * 0.72f * waveScaleClamped * responsiveScale, 0.03f, 1.55f);
            const float angle = t * 6.2831853f + timeSeconds * 0.2f;
            writePoint(0, i, radius * cosf(angle), radius * sinf(angle), baseR, baseG, baseB);
        }
    }
    else if (waveMode == 1)
    {
        activeCount = std::min<UINT>(sourceCount > 33 ? sourceCount / 2 : sourceCount, 240u);
        for (UINT i = 0; i < activeCount; ++i)
        {
            const float radius = std::clamp((0.53f + 0.43f * sampleAt(right, i) + waveParam) * 0.72f * waveScaleClamped * responsiveScale, 0.03f, 1.55f);
            const float angle = sampleAt(left, i + 32) * (1.57f + treble * 0.08f) + timeSeconds * 2.3f;
            writePoint(0, i, radius * cosf(angle), radius * sinf(angle), baseR, baseG, baseB);
        }
    }
    else if (waveMode == 2 || waveMode == 3)
    {
        activeCount = std::min<UINT>(sourceCount > 33 ? sourceCount - 33 : sourceCount, 480u);
        const float xyScale = (waveMode == 3 ? 0.86f : 0.66f) * responsiveScale;
        for (UINT i = 0; i < activeCount; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(activeCount - 1);
            const float wobble = waveMode == 3 ? sinf(t * 18.849556f + timeSeconds * 0.8f) * 0.05f * treble : 0.0f;
            writePoint(0, i, sampleAt(right, i) * xyScale * waveScaleClamped + wobble, -sampleAt(left, i + 32) * xyScale * waveScaleClamped, baseR, baseG, baseB);
        }
    }
    else if (waveMode == 4)
    {
        activeCount = std::min<UINT>(sourceCount > 26 ? sourceCount - 26 : sourceCount, 480u);
        const float momentum = 0.45f + 0.5f * (waveParam * 0.5f + 0.5f);
        const float direct = 1.0f - momentum;
        float previousX[2]{};
        float previousY[2]{};
        for (UINT i = 0; i < activeCount; ++i)
        {
            float x = -0.92f + 1.84f * (static_cast<float>(i) / static_cast<float>(activeCount - 1));
            float y = -sampleAt(left, i) * 0.42f * waveScaleClamped * responsiveScale;
            x += sampleAt(right, i + 25) * 0.32f * waveScaleClamped * responsiveScale;
            if (i > 1)
            {
                x = x * direct + momentum * (previousX[0] * 2.0f - previousX[1]);
                y = y * direct + momentum * (previousY[0] * 2.0f - previousY[1]);
            }
            previousX[1] = previousX[0];
            previousY[1] = previousY[0];
            previousX[0] = x;
            previousY[0] = y;
            writePoint(0, i, x, y, baseR, baseG, baseB);
        }
    }
    else if (waveMode == 5)
    {
        activeCount = std::min<UINT>(sourceCount > 33 ? sourceCount - 33 : sourceCount, 480u);
        const float rotC = cosf(timeSeconds * 0.3f);
        const float rotS = sinf(timeSeconds * 0.3f);
        for (UINT i = 0; i < activeCount; ++i)
        {
            const float x0 = sampleAt(right, i) * sampleAt(left, i + 32) + sampleAt(left, i) * sampleAt(right, i + 32);
            const float y0 = sampleAt(right, i) * sampleAt(right, i) - sampleAt(left, i + 32) * sampleAt(left, i + 32);
            writePoint(0, i, (x0 * rotC - y0 * rotS) * 0.95f * waveScaleClamped * responsiveScale, (x0 * rotS + y0 * rotC) * 0.95f * waveScaleClamped * responsiveScale, baseR, baseG, baseB);
        }
    }
    else
    {
        activeCount = std::min<UINT>(sourceCount > 33 ? sourceCount / 2 : sourceCount, 240u);
        visibleSegments = waveMode == 7 ? 2u : 1u;
        const float angle = 1.57f * waveParam;
        const float dx = cosf(angle);
        const float dy = sinf(angle);
        const float perpX = cosf(angle + 1.57f);
        const float perpY = sinf(angle + 1.57f);
        const float sep = waveMode == 7 ? std::clamp(powf(waveY * 0.5f + 0.5f, 2.0f) * 0.46f + std::abs(waveParam) * 0.12f, 0.08f, 0.62f) : 0.0f;
        for (UINT i = 0; i < activeCount; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(activeCount - 1);
            const float lineX = -dx + dx * 2.0f * t;
            const float lineY = -dy + dy * 2.0f * t;
            const float leftSample = sampleAt(left, i) * 0.38f * waveScaleClamped * responsiveScale + sep;
            writePoint(0, i, lineX + perpX * leftSample, lineY + perpY * leftSample, baseR, baseG, baseB);
            if (visibleSegments == 2)
            {
                const float rightSample = sampleAt(right, i) * 0.38f * waveScaleClamped * responsiveScale - sep;
                writePoint(1, i, lineX + perpX * rightSample, lineY + perpY * rightSample, std::clamp(baseR * 0.75f, 0.0f, 1.0f), std::clamp(baseG * 0.85f, 0.0f, 1.0f), std::clamp(baseB * 1.15f, 0.0f, 1.0f));
            }
        }
    }

    if (activeCount < 2)
    {
        Clear();
        return;
    }

    const UINT drawGroupCount = activeCount * visibleSegments;
    const UINT lineVertexCount = drawGroupCount * 4;

    if (lineVertexCount > c_maxWaveformVertices)
    {
        Clear();
        return;
    }

    UINT dotVertexStart = lineVertexCount;
    UINT dotVertexCount = 0;
    if (waveUseDots)
    {
        const float dotScale = waveThick ? 3.25f : 2.15f;
        const float dotX = pixelX * dotScale;
        const float dotY = pixelY * dotScale;

        auto writeDotVertex = [&](float x, float y, const WaveformVertex& source) {
            WaveformVertex& vertex = m_mappedWaveformVertices[dotVertexStart + dotVertexCount++];
            vertex.position[0] = x;
            vertex.position[1] = y;
            vertex.color[0] = std::clamp(source.color[0] * 0.72f + 0.28f, 0.0f, 1.0f);
            vertex.color[1] = std::clamp(source.color[1] * 0.72f + 0.28f, 0.0f, 1.0f);
            vertex.color[2] = std::clamp(source.color[2] * 0.72f + 0.28f, 0.0f, 1.0f);
            vertex.color[3] = 1.0f;
        };

        for (UINT i = 0; i < drawGroupCount && dotVertexStart + dotVertexCount + 4 <= c_maxWaveformVertices; ++i)
        {
            const WaveformVertex& source = m_mappedWaveformVertices[i];
            const float x = source.position[0];
            const float y = source.position[1];
            writeDotVertex(x - dotX, y, source);
            writeDotVertex(x + dotX, y, source);
            writeDotVertex(x, y - dotY, source);
            writeDotVertex(x, y + dotY, source);
        }
    }

    UINT customShapeTriangleStart = dotVertexStart + dotVertexCount;
    UINT customShapeTriangleCount = 0;
    UINT customShapeLineStart = customShapeTriangleStart;
    UINT customShapeLineCount = 0;

    auto writeCustomShapeVertex = [&](UINT start, UINT& count, float x, float y, float r, float g, float b, float a) {
        if (start + count >= c_maxWaveformVertices)
        {
            return false;
        }

        WaveformVertex& vertex = m_mappedWaveformVertices[start + count++];
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.color[0] = std::clamp(r, 0.0f, 1.0f);
        vertex.color[1] = std::clamp(g, 0.0f, 1.0f);
        vertex.color[2] = std::clamp(b, 0.0f, 1.0f);
        vertex.color[3] = std::clamp(a, 0.0f, 1.0f);
        return true;
    };

    auto writeCustomShapeTriangle = [&](UINT start,
                                        UINT& count,
                                        float x0,
                                        float y0,
                                        float r0,
                                        float g0,
                                        float b0,
                                        float a0,
                                        float x1,
                                        float y1,
                                        float r1,
                                        float g1,
                                        float b1,
                                        float a1,
                                        float x2,
                                        float y2,
                                        float r2,
                                        float g2,
                                        float b2,
                                        float a2) {
        const UINT before = count;
        if (!writeCustomShapeVertex(start, count, x0, y0, r0, g0, b0, a0) ||
            !writeCustomShapeVertex(start, count, x1, y1, r1, g1, b1, a1) ||
            !writeCustomShapeVertex(start, count, x2, y2, r2, g2, b2, a2))
        {
            count = before;
        }
    };

    auto writeCustomShapeLine = [&](UINT start, UINT& count, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
        const UINT before = count;
        if (!writeCustomShapeVertex(start, count, x0, y0, r, g, b, a) ||
            !writeCustomShapeVertex(start, count, x1, y1, r, g, b, a))
        {
            count = before;
        }
    };

    if (customShapes && customShapeCount > 0)
    {
        const float width = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
        const float height = static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
        const float aspectY = width > height ? height / width : 1.0f;
        customShapeCount = std::min<size_t>(customShapeCount, 64);

        for (size_t shapeIndex = 0; shapeIndex < customShapeCount; ++shapeIndex)
        {
            const CustomShapeDrawCommand& shape = customShapes[shapeIndex];
            if (shape.textured && canDrawTexturedShapes)
            {
                continue;
            }

            const int sides = std::clamp(shape.sides, 3, 100);
            const float centerShapeX = std::clamp(shape.x, -1000.0f, 1000.0f) * 2.0f - 1.0f;
            const float centerShapeY = std::clamp(shape.y, -1000.0f, 1000.0f) * -2.0f + 1.0f;
            const float radius = std::clamp(shape.radius, 0.0f, 1000.0f);
            const float angleBase = shape.angle + 3.1415927f * 0.25f;

            float ringX[101]{};
            float ringY[101]{};
            for (int side = 0; side < sides; ++side)
            {
                const float t = static_cast<float>(side) / static_cast<float>(sides);
                const float angle = t * 6.2831853f + angleBase;
                ringX[side] = centerShapeX + radius * cosf(angle) * aspectY;
                ringY[side] = centerShapeY + radius * sinf(angle);
            }

            for (int side = 0; side < sides; ++side)
            {
                const int next = (side + 1) % sides;
                writeCustomShapeTriangle(customShapeTriangleStart,
                                         customShapeTriangleCount,
                                         centerShapeX,
                                         centerShapeY,
                                         shape.r,
                                         shape.g,
                                         shape.b,
                                         shape.a,
                                         ringX[side],
                                         ringY[side],
                                         shape.r2,
                                         shape.g2,
                                         shape.b2,
                                         shape.a2,
                                         ringX[next],
                                         ringY[next],
                                         shape.r2,
                                         shape.g2,
                                         shape.b2,
                                         shape.a2);
            }
        }

        customShapeLineStart = customShapeTriangleStart + customShapeTriangleCount;

        for (size_t shapeIndex = 0; shapeIndex < customShapeCount; ++shapeIndex)
        {
            const CustomShapeDrawCommand& shape = customShapes[shapeIndex];
            if (shape.borderA > 0.001f)
            {
                const int sides = std::clamp(shape.sides, 3, 100);
                const float centerShapeX = std::clamp(shape.x, -1000.0f, 1000.0f) * 2.0f - 1.0f;
                const float centerShapeY = std::clamp(shape.y, -1000.0f, 1000.0f) * -2.0f + 1.0f;
                const float radius = std::clamp(shape.radius, 0.0f, 1000.0f);
                const float angleBase = shape.angle + 3.1415927f * 0.25f;
                float ringX[101]{};
                float ringY[101]{};
                for (int side = 0; side < sides; ++side)
                {
                    const float t = static_cast<float>(side) / static_cast<float>(sides);
                    const float angle = t * 6.2831853f + angleBase;
                    ringX[side] = centerShapeX + radius * cosf(angle) * aspectY;
                    ringY[side] = centerShapeY + radius * sinf(angle);
                }

                const int passes = shape.thickBorder ? 4 : 1;
                for (int pass = 0; pass < passes; ++pass)
                {
                    const float offsetX = (pass == 1 || pass == 3) ? (pass == 1 ? pixelX : -pixelX) : 0.0f;
                    const float offsetY = pass == 2 ? pixelY : 0.0f;
                    for (int side = 0; side < sides; ++side)
                    {
                        const int next = (side + 1) % sides;
                        writeCustomShapeLine(customShapeLineStart,
                                             customShapeLineCount,
                                             ringX[side] + offsetX,
                                             ringY[side] + offsetY,
                                             ringX[next] + offsetX,
                                             ringY[next] + offsetY,
                                             shape.borderR,
                                             shape.borderG,
                                             shape.borderB,
                                             shape.borderA);
                    }
                }
            }
        }
    }

    UINT overlayVertexStart = customShapeLineStart + customShapeLineCount;
    UINT overlayVertexCount = 0;
    auto writeOverlayVertex = [&](float x, float y, float r, float g, float b, float a) {
        if (overlayVertexStart + overlayVertexCount >= c_maxWaveformVertices)
        {
            return false;
        }
        WaveformVertex& vertex = m_mappedWaveformVertices[overlayVertexStart + overlayVertexCount++];
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.color[0] = std::clamp(r, 0.0f, 1.0f);
        vertex.color[1] = std::clamp(g, 0.0f, 1.0f);
        vertex.color[2] = std::clamp(b, 0.0f, 1.0f);
        vertex.color[3] = std::clamp(a, 0.0f, 1.0f);
        return true;
    };
    auto writeOverlayTriangle = [&](float x0,
                                    float y0,
                                    float r0,
                                    float g0,
                                    float b0,
                                    float a0,
                                    float x1,
                                    float y1,
                                    float r1,
                                    float g1,
                                    float b1,
                                    float a1,
                                    float x2,
                                    float y2,
                                    float r2,
                                    float g2,
                                    float b2,
                                    float a2) {
        const UINT before = overlayVertexCount;
        if (!writeOverlayVertex(x0, y0, r0, g0, b0, a0) ||
            !writeOverlayVertex(x1, y1, r1, g1, b1, a1) ||
            !writeOverlayVertex(x2, y2, r2, g2, b2, a2))
        {
            overlayVertexCount = before;
        }
    };
    auto writeOverlayQuad = [&](float leftEdge, float bottomEdge, float rightEdge, float topEdge, float r, float g, float b, float a) {
        writeOverlayTriangle(leftEdge, topEdge, r, g, b, a, rightEdge, topEdge, r, g, b, a, leftEdge, bottomEdge, r, g, b, a);
        writeOverlayTriangle(leftEdge, bottomEdge, r, g, b, a, rightEdge, topEdge, r, g, b, a, rightEdge, bottomEdge, r, g, b, a);
    };

    if (darkenCenter)
    {
        const float width = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
        const float height = static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
        const float aspectY = width > height ? height / width : 1.0f;
        const float halfX = 0.05f * aspectY;
        const float halfY = 0.05f;
        const float alpha = 3.0f / 32.0f;
        writeOverlayTriangle(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, alpha, -halfX, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -halfY, 0.0f, 0.0f, 0.0f, 0.0f);
        writeOverlayTriangle(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, alpha, 0.0f, -halfY, 0.0f, 0.0f, 0.0f, 0.0f, halfX, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        writeOverlayTriangle(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, alpha, halfX, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, halfY, 0.0f, 0.0f, 0.0f, 0.0f);
        writeOverlayTriangle(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, alpha, 0.0f, halfY, 0.0f, 0.0f, 0.0f, 0.0f, -halfX, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    auto writeBorder = [&](float outerRadius, float innerRadius, float r, float g, float b, float a) {
        outerRadius = std::clamp(outerRadius, 0.0f, 1.0f);
        innerRadius = std::clamp(innerRadius, 0.0f, outerRadius);
        if (a <= 0.001f || outerRadius <= innerRadius)
        {
            return;
        }
        writeOverlayQuad(-outerRadius, innerRadius, outerRadius, outerRadius, r, g, b, a);
        writeOverlayQuad(-outerRadius, -outerRadius, outerRadius, -innerRadius, r, g, b, a);
        writeOverlayQuad(-outerRadius, -innerRadius, -innerRadius, innerRadius, r, g, b, a);
        writeOverlayQuad(innerRadius, -innerRadius, outerRadius, innerRadius, r, g, b, a);
    };

    outerBorderSize = std::clamp(outerBorderSize, 0.0f, 1.0f);
    innerBorderSize = std::clamp(innerBorderSize, 0.0f, 1.0f);
    writeBorder(1.0f, 1.0f - outerBorderSize, outerBorderR, outerBorderG, outerBorderB, outerBorderA);
    writeBorder(1.0f - outerBorderSize, 1.0f - outerBorderSize - innerBorderSize, innerBorderR, innerBorderG, innerBorderB, innerBorderA);

    UINT motionVectorVertexStart = overlayVertexStart + overlayVertexCount;
    UINT motionVectorVertexCount = 0;
    if (motionVectorA >= 0.001f)
    {
        int vectorColumns = std::clamp(static_cast<int>(motionVectorX), 0, 64);
        int vectorRows = std::clamp(static_cast<int>(motionVectorY), 0, 48);
        float gridBiasX = motionVectorX - static_cast<float>(vectorColumns);
        float gridBiasY = motionVectorY - static_cast<float>(vectorRows);
        gridBiasX = std::clamp(gridBiasX, 0.0f, 1.0f);
        gridBiasY = std::clamp(gridBiasY, 0.0f, 1.0f);

        auto wrapUnit = [](float value) {
            value -= floorf(value);
            if (value < 0.0f)
            {
                value += 1.0f;
            }
            return value;
        };

        const float offsetX = wrapUnit(motionVectorDX);
        const float offsetY = wrapUnit(motionVectorDY);
        const float lengthMultiplier = std::clamp(motionVectorLength, 0.0f, 10.0f);
        const float minimumLength = std::max(pixelX, pixelY) * 2.5f;
        const float directionBiasX = std::clamp(motionVectorDX, -2.0f, 2.0f) * 0.030f;
        const float directionBiasY = std::clamp(motionVectorDY, -2.0f, 2.0f) * 0.030f;
        const float zoomDrift = std::clamp(zoom - 1.0f, -2.0f, 2.0f) * 0.050f;
        const float rotDrift = std::clamp(rot, -6.2831853f, 6.2831853f) * 0.025f;
        const float pulseDrift = 0.006f + energy * 0.012f;
        const float vectorR = std::clamp(motionVectorR, 0.0f, 1.0f);
        const float vectorG = std::clamp(motionVectorG, 0.0f, 1.0f);
        const float vectorB = std::clamp(motionVectorB, 0.0f, 1.0f);
        const float vectorA = std::clamp(motionVectorA, 0.0f, 1.0f);

        auto writeMotionVectorVertex = [&](float x, float y) {
            if (motionVectorVertexStart + motionVectorVertexCount >= c_maxWaveformVertices)
            {
                return false;
            }
            WaveformVertex& vertex = m_mappedWaveformVertices[motionVectorVertexStart + motionVectorVertexCount++];
            vertex.position[0] = x;
            vertex.position[1] = y;
            vertex.color[0] = vectorR;
            vertex.color[1] = vectorG;
            vertex.color[2] = vectorB;
            vertex.color[3] = vectorA;
            return true;
        };

        if (vectorColumns > 0 && vectorRows > 0)
        {
            for (int y = 0; y < vectorRows; ++y)
            {
                const float fyDenom = static_cast<float>(vectorRows) + gridBiasY + 0.25f - 1.0f;
                if (fabsf(fyDenom) < 0.0001f)
                {
                    continue;
                }

                float fy = (static_cast<float>(y) + 0.25f) / fyDenom;
                fy -= offsetY;
                if (fy <= 0.0001f || fy >= 0.9999f)
                {
                    continue;
                }

                for (int x = 0; x < vectorColumns; ++x)
                {
                    const float fxDenom = static_cast<float>(vectorColumns) + gridBiasX + 0.25f - 1.0f;
                    if (fabsf(fxDenom) < 0.0001f)
                    {
                        continue;
                    }

                    float fx = (static_cast<float>(x) + 0.25f) / fxDenom;
                    fx += offsetX;
                    if (fx <= 0.0001f || fx >= 0.9999f)
                    {
                        continue;
                    }

                    const float startX = fx * 2.0f - 1.0f;
                    const float startY = 1.0f - fy * 2.0f;
                    float endDX = (startX * zoomDrift - startY * rotDrift + directionBiasX + startX * pulseDrift) * lengthMultiplier;
                    float endDY = (startY * zoomDrift + startX * rotDrift - directionBiasY + startY * pulseDrift) * lengthMultiplier;
                    float length = sqrtf(endDX * endDX + endDY * endDY);

                    if (length < minimumLength)
                    {
                        if (length > 0.00000001f)
                        {
                            const float boost = minimumLength / length;
                            endDX *= boost;
                            endDY *= boost;
                        }
                        else
                        {
                            endDX = minimumLength;
                            endDY = minimumLength;
                        }
                    }

                    const UINT before = motionVectorVertexCount;
                    if (!writeMotionVectorVertex(startX, startY) ||
                        !writeMotionVectorVertex(startX + endDX, startY + endDY))
                    {
                        motionVectorVertexCount = before;
                        break;
                    }
                }
            }
        }
    }

    UINT customWaveVertexStart = motionVectorVertexStart + motionVectorVertexCount;
    UINT customWaveVertexCopiedCount = 0;
    if (customWaveVertices && customWaveVertexCount > 0 && customWaveDraws && customWaveDrawCount > 0 && customWaveVertexStart < c_maxWaveformVertices)
    {
        customWaveVertexCopiedCount = static_cast<UINT>(std::min<size_t>(customWaveVertexCount, c_maxWaveformVertices - customWaveVertexStart));
        for (UINT i = 0; i < customWaveVertexCopiedCount; ++i)
        {
            const CustomWaveVertex& source = customWaveVertices[i];
            WaveformVertex& vertex = m_mappedWaveformVertices[customWaveVertexStart + i];
            vertex.position[0] = source.x;
            vertex.position[1] = source.y;
            vertex.color[0] = std::clamp(source.r, 0.0f, 1.0f);
            vertex.color[1] = std::clamp(source.g, 0.0f, 1.0f);
            vertex.color[2] = std::clamp(source.b, 0.0f, 1.0f);
            vertex.color[3] = std::clamp(source.a, 0.0f, 1.0f);
        }
    }

    MaybeCycleTexture();

    ID3D12PipelineState* waveformPipelineState = waveAdditive ? m_waveformAdditivePipelineState.Get() : m_waveformPipelineState.Get();
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), waveformPipelineState));

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    const auto rtvHandle = GetCurrentRenderTargetView();
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    m_commandList->ClearRenderTargetView(rtvHandle, bg, 0, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    viewport.Height = static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor{};
    scissor.right = static_cast<LONG>(viewport.Width);
    scissor.bottom = static_cast<LONG>(viewport.Height);

    m_textureVertexCursor = 0;
    m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    if (m_feedbackReady[previousFeedbackIndex] && m_feedbackSrvHeap)
    {
        auto feedbackSrv = m_feedbackSrvHeap->GetGPUDescriptorHandleForHeapStart();
        feedbackSrv.ptr += static_cast<SIZE_T>(previousFeedbackIndex) * m_feedbackSrvDescriptorSize;
        const float feedbackAlphaScale = echoAlpha > 0.001f ? std::clamp(echoAlpha, 0.08f, 0.95f) : 0.72f;
        const float feedbackZoomBias = echoAlpha > 0.001f ? std::clamp(1.0f / echoZoom, 0.20f, 2.0f) : 0.95f;
        const bool flipU = (echoOrientation % 2) != 0;
        const bool flipV = echoOrientation >= 2;
        if (hasTextureWarpMesh)
        {
            DrawTextureMeshFromSrv(feedbackSrv, m_feedbackSrvHeap.Get(), textureWarpVertices, textureWarpVertexCount, bass, mids, treble, decay);
        }
        else
        {
            DrawTextureQuadFromSrv(feedbackSrv, m_feedbackSrvHeap.Get(), bass, mids, treble, decay, zoom, rot, feedbackAlphaScale, feedbackZoomBias, 0.04f, flipU, flipV, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp);
        }
    }

    if (hasTextureWarpMesh && m_texture && m_srvHeap && m_texturePipelineState && m_textureRootSignature)
    {
        DrawTextureMeshFromSrv(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), m_srvHeap.Get(), textureWarpVertices, textureWarpVertexCount, bass, mids, treble, decay);
    }
    else
    {
        DrawTextureQuad(bass, mids, treble, decay, zoom, rot, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp);
    }

    if (canDrawTexturedShapes)
    {
        auto feedbackSrv = m_feedbackSrvHeap->GetGPUDescriptorHandleForHeapStart();
        feedbackSrv.ptr += static_cast<SIZE_T>(previousFeedbackIndex) * m_feedbackSrvDescriptorSize;
        DrawTexturedCustomShapesFromSrv(feedbackSrv, m_feedbackSrvHeap.Get(), customShapes, customShapeCount);
    }

    if (customShapeTriangleCount > 0)
    {
        m_commandList->SetPipelineState(m_solidPipelineState.Get());
        m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->DrawInstanced(customShapeTriangleCount, 1, customShapeTriangleStart, 0);
    }
    if (customShapeLineCount > 0)
    {
        m_commandList->SetPipelineState(m_waveformPipelineState.Get());
        m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        m_commandList->DrawInstanced(customShapeLineCount, 1, customShapeLineStart, 0);
    }
    if (customWaveVertexCopiedCount > 0 && customWaveDraws && customWaveDrawCount > 0)
    {
        ID3D12PipelineState* activeCustomWavePipelineState = nullptr;
        D3D12_PRIMITIVE_TOPOLOGY activeCustomWaveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);

        for (size_t drawIndex = 0; drawIndex < customWaveDrawCount; ++drawIndex)
        {
            const CustomWaveDrawCommand& draw = customWaveDraws[drawIndex];
            if (draw.vertexCount == 0 || draw.vertexOffset + draw.vertexCount > customWaveVertexCopiedCount)
            {
                continue;
            }

            ID3D12PipelineState* nextPipelineState = draw.additive ?
                (draw.triangleList ? m_solidAdditivePipelineState.Get() : m_waveformAdditivePipelineState.Get()) :
                (draw.triangleList ? m_solidPipelineState.Get() : m_waveformPipelineState.Get());
            const D3D12_PRIMITIVE_TOPOLOGY nextTopology = draw.triangleList ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D_PRIMITIVE_TOPOLOGY_LINELIST;

            if (activeCustomWavePipelineState != nextPipelineState)
            {
                m_commandList->SetPipelineState(nextPipelineState);
                activeCustomWavePipelineState = nextPipelineState;
            }
            if (activeCustomWaveTopology != nextTopology)
            {
                m_commandList->IASetPrimitiveTopology(nextTopology);
                activeCustomWaveTopology = nextTopology;
            }

            m_commandList->DrawInstanced(static_cast<UINT>(draw.vertexCount), 1, customWaveVertexStart + static_cast<UINT>(draw.vertexOffset), 0);
        }
    }

    if (motionVectorVertexCount > 0)
    {
        m_commandList->SetPipelineState(m_waveformPipelineState.Get());
        m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        m_commandList->DrawInstanced(motionVectorVertexCount, 1, motionVectorVertexStart, 0);
    }

    m_commandList->SetPipelineState(waveformPipelineState);
    m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
    m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);
    if (waveUseDots && dotVertexCount > 0)
    {
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        m_commandList->DrawInstanced(dotVertexCount, 1, dotVertexStart, 0);
    }
    else
    {
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
        for (UINT segment = 0; segment < visibleSegments; ++segment)
        {
            m_commandList->DrawInstanced(activeCount, 1, drawGroupCount + segment * activeCount, 0);
        }
        if (waveThick)
        {
            for (UINT segment = 0; segment < visibleSegments; ++segment)
            {
                m_commandList->DrawInstanced(activeCount, 1, 3 * drawGroupCount + segment * activeCount, 0);
            }
        }
        for (UINT segment = 0; segment < visibleSegments; ++segment)
        {
            m_commandList->DrawInstanced(activeCount, 1, segment * activeCount, 0);
        }
        for (UINT segment = 0; segment < visibleSegments; ++segment)
        {
            m_commandList->DrawInstanced(activeCount, 1, 2 * drawGroupCount + segment * activeCount, 0);
        }
    }
    if (overlayVertexCount > 0)
    {
        m_commandList->SetPipelineState(m_solidPipelineState.Get());
        m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->DrawInstanced(overlayVertexCount, 1, overlayVertexStart, 0);
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_commandList->ResourceBarrier(1, &barrier);

    if (postProcessActive && CopyBackBufferToPostProcessSource())
    {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_commandList->ResourceBarrier(1, &barrier);

        DrawPostProcessFromSrv(m_postProcessSrvHeap->GetGPUDescriptorHandleForHeapStart(),
                               m_postProcessSrvHeap.Get(),
                               postGamma,
                               postBrighten,
                               postDarken,
                               postSolarize,
                               postInvert,
                               postShaderAmount);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_commandList->ResourceBarrier(1, &barrier);
    }

    CopyBackBufferToFeedback(m_feedbackIndex);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
}

void D3D12Resources::Present()
{
    const UINT presentFlags = (m_options & DeviceResources::c_AllowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const UINT syncInterval = (m_options & DeviceResources::c_AllowTearing) ? 0u : 1u;
    ThrowIfFailed(m_swapChain->Present(syncInterval, presentFlags));
    MoveToNextFrame();
}

void D3D12Resources::CreateFactory()
{
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(m_dxgiFactory.ReleaseAndGetAddressOf())));
}

void D3D12Resources::GetHardwareAdapter(IDXGIAdapter1** adapter)
{
    *adapter = nullptr;

    ComPtr<IDXGIAdapter1> currentAdapter;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(m_dxgiFactory.As(&factory6)))
    {
        for (UINT index = 0; SUCCEEDED(factory6->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(currentAdapter.ReleaseAndGetAddressOf()))); ++index)
        {
            DXGI_ADAPTER_DESC1 desc{};
            ThrowIfFailed(currentAdapter->GetDesc1(&desc));
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && SUCCEEDED(D3D12CreateDevice(currentAdapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            {
                *adapter = currentAdapter.Detach();
                return;
            }
        }
    }

    for (UINT index = 0; SUCCEEDED(m_dxgiFactory->EnumAdapters1(index, currentAdapter.ReleaseAndGetAddressOf())); ++index)
    {
        DXGI_ADAPTER_DESC1 desc{};
        ThrowIfFailed(currentAdapter->GetDesc1(&desc));
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && SUCCEEDED(D3D12CreateDevice(currentAdapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
        {
            *adapter = currentAdapter.Detach();
            return;
        }
    }
}

void D3D12Resources::MoveToNextFrame()
{
    const UINT64 currentFenceValue = m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFenceValue));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

void D3D12Resources::WaitForGpu()
{
    if (!m_commandQueue || !m_fence || !m_fenceEvent)
    {
        return;
    }

    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]));
    ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
    WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    ++m_fenceValues[m_frameIndex];
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Resources::GetCurrentRenderTargetView() const noexcept
{
    auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    return handle;
}

void D3D12Resources::CreateTextureResources()
{
    static constexpr char shaderSource[] = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

struct VSInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return tex0.Sample(samp0, input.uv) * input.color;
}
)";

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> errorBlob;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ThrowIfFailed(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, vertexShader.GetAddressOf(), errorBlob.GetAddressOf()));
    errorBlob.Reset();
    ThrowIfFailed(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, pixelShader.GetAddressOf(), errorBlob.GetAddressOf()));

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(m_srvHeap.ReleaseAndGetAddressOf())));

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameter.DescriptorTable.NumDescriptorRanges = 1;
    rootParameter.DescriptorTable.pDescriptorRanges = &range;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = 1;
    rootSignatureDesc.pParameters = &rootParameter;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), errorBlob.ReleaseAndGetAddressOf()));
    ThrowIfFailed(m_d3dDevice->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(m_textureRootSignature.ReleaseAndGetAddressOf())));

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 2, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout = {inputElements, static_cast<UINT>(std::size(inputElements))};
    psoDesc.pRootSignature = m_textureRootSignature.Get();
    psoDesc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    psoDesc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_backBufferFormat;
    psoDesc.SampleDesc.Count = 1;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_texturePipelineState.ReleaseAndGetAddressOf())));

    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_textureAlphaPipelineState.ReleaseAndGetAddressOf())));

    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_textureAdditivePipelineState.ReleaseAndGetAddressOf())));

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(TextureVertex) * c_maxTextureVertices;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &bufferDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_textureVertexBuffer.ReleaseAndGetAddressOf())));
    ThrowIfFailed(m_textureVertexBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedTextureVertices)));

    const TextureVertex vertices[] = {
        {{-1.0f, 1.0f}, {0.0f, 0.0f}, {0.55f, 0.55f, 0.55f, 0.32f}},
        {{1.0f, 1.0f}, {1.0f, 0.0f}, {0.55f, 0.55f, 0.55f, 0.32f}},
        {{-1.0f, -1.0f}, {0.0f, 1.0f}, {0.55f, 0.55f, 0.55f, 0.32f}},
        {{1.0f, -1.0f}, {1.0f, 1.0f}, {0.55f, 0.55f, 0.55f, 0.32f}},
    };
    memcpy(m_mappedTextureVertices, vertices, sizeof(vertices));

    m_textureVertexBufferView.BufferLocation = m_textureVertexBuffer->GetGPUVirtualAddress();
    m_textureVertexBufferView.StrideInBytes = sizeof(TextureVertex);
    m_textureVertexBufferView.SizeInBytes = sizeof(TextureVertex) * c_maxTextureVertices;
}

bool D3D12Resources::LoadTextureFromFile(const wchar_t* textureFile)
{
    if (!textureFile || !*textureFile || !m_srvHeap)
    {
        return false;
    }

    if (!_wcsicmp(m_currentTextureFile.c_str(), textureFile))
    {
        return true;
    }

    const wchar_t* ext = wcsrchr(textureFile, L'.');
    const bool loaded = ext && !_wcsicmp(ext, L".tga") ? LoadTextureFromTga(textureFile) : LoadTextureFromWic(textureFile);
    if (loaded)
    {
        m_currentTextureFile = textureFile;
        OutputDebugStringW((std::wstring(L"foo_vis_milk2 DX12 texture loaded: ") + textureFile + L"\n").c_str());
    }
    return loaded;
}

bool D3D12Resources::LoadTextureFromWic(const wchar_t* textureFile)
{
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr))
    {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(textureFile, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(factory->CreateFormatConverter(converter.GetAddressOf()));
    ThrowIfFailed(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));

    UINT width = 0;
    UINT height = 0;
    ThrowIfFailed(converter->GetSize(&width, &height));
    if (width == 0 || height == 0)
    {
        return false;
    }

    const UINT bytesPerPixel = 4;
    const UINT rowPitch = width * bytesPerPixel;
    std::vector<uint8_t> pixels(static_cast<size_t>(rowPitch) * height);
    ThrowIfFailed(converter->CopyPixels(nullptr, rowPitch, static_cast<UINT>(pixels.size()), pixels.data()));

    return UploadTextureRGBA(width, height, pixels);
}

bool D3D12Resources::LoadTextureFromTga(const wchar_t* textureFile)
{
    std::ifstream file(textureFile, std::ios::binary);
    if (!file)
    {
        return false;
    }

    uint8_t header[18]{};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!file)
    {
        return false;
    }

    const uint8_t idLength = header[0];
    const uint8_t colorMapType = header[1];
    const uint8_t imageType = header[2];
    const UINT width = static_cast<UINT>(header[12] | (header[13] << 8));
    const UINT height = static_cast<UINT>(header[14] | (header[15] << 8));
    const uint8_t bitsPerPixel = header[16];
    const uint8_t descriptor = header[17];
    const bool rle = imageType == 10;
    const bool trueColor = imageType == 2 || imageType == 10;
    const bool grayscale = imageType == 3;
    if (colorMapType != 0 || width == 0 || height == 0 || (!trueColor && !grayscale) || (bitsPerPixel != 8 && bitsPerPixel != 24 && bitsPerPixel != 32))
    {
        return false;
    }

    if (idLength > 0)
    {
        file.seekg(idLength, std::ios::cur);
        if (!file)
        {
            return false;
        }
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    const UINT bytesPerPixel = bitsPerPixel / 8;
    const bool topOrigin = (descriptor & 0x20) != 0;
    const bool rightOrigin = (descriptor & 0x10) != 0;

    auto writePixel = [&](UINT index, const uint8_t* source) {
        const UINT srcX = index % width;
        const UINT srcY = index / width;
        const UINT dstX = rightOrigin ? (width - 1 - srcX) : srcX;
        const UINT dstY = topOrigin ? srcY : (height - 1 - srcY);
        uint8_t* dest = pixels.data() + (static_cast<size_t>(dstY) * width + dstX) * 4;
        if (bitsPerPixel == 8)
        {
            dest[0] = source[0];
            dest[1] = source[0];
            dest[2] = source[0];
            dest[3] = 255;
        }
        else
        {
            dest[0] = source[2];
            dest[1] = source[1];
            dest[2] = source[0];
            dest[3] = bitsPerPixel == 32 ? source[3] : 255;
        }
    };

    UINT pixelIndex = 0;
    std::array<uint8_t, 4> pixel{};
    if (rle)
    {
        while (pixelIndex < width * height && file)
        {
            uint8_t packetHeader = 0;
            file.read(reinterpret_cast<char*>(&packetHeader), 1);
            const UINT packetCount = (packetHeader & 0x7f) + 1;
            if (packetHeader & 0x80)
            {
                file.read(reinterpret_cast<char*>(pixel.data()), bytesPerPixel);
                for (UINT i = 0; i < packetCount && pixelIndex < width * height; ++i)
                {
                    writePixel(pixelIndex++, pixel.data());
                }
            }
            else
            {
                for (UINT i = 0; i < packetCount && pixelIndex < width * height; ++i)
                {
                    file.read(reinterpret_cast<char*>(pixel.data()), bytesPerPixel);
                    writePixel(pixelIndex++, pixel.data());
                }
            }
        }
    }
    else
    {
        while (pixelIndex < width * height && file)
        {
            file.read(reinterpret_cast<char*>(pixel.data()), bytesPerPixel);
            writePixel(pixelIndex++, pixel.data());
        }
    }

    if (pixelIndex != width * height)
    {
        return false;
    }

    return UploadTextureRGBA(width, height, pixels);
}

bool D3D12Resources::UploadTextureRGBA(UINT width, UINT height, const std::vector<uint8_t>& pixels)
{
    if (width == 0 || height == 0 || pixels.size() < static_cast<size_t>(width) * height * 4)
    {
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&defaultHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &textureDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_texture.ReleaseAndGetAddressOf())));

    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    m_d3dDevice->GetCopyableFootprints(&textureDesc, 0, 1, 0, &layout, &numRows, &rowSize, &uploadSize);

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&uploadHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &uploadDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_textureUploadBuffer.ReleaseAndGetAddressOf())));

    uint8_t* mapped = nullptr;
    ThrowIfFailed(m_textureUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    const UINT sourceRowPitch = width * 4;
    for (UINT row = 0; row < height; ++row)
    {
        memcpy(mapped + layout.Offset + static_cast<size_t>(row) * layout.Footprint.RowPitch, pixels.data() + static_cast<size_t>(row) * sourceRowPitch, sourceRowPitch);
    }
    m_textureUploadBuffer->Unmap(0, nullptr);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = m_texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = m_textureUploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    WaitForGpu();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_d3dDevice->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

void D3D12Resources::CreatePostProcessResources()
{
    static constexpr char shaderSource[] = R"(
Texture2D sourceTex : register(t0);
SamplerState samp0 : register(s0);

cbuffer Effects : register(b0)
{
    float gamma;
    float brighten;
    float darken;
    float solarize;
    float invert;
    float shaderAmount;
    float pad0;
    float pad1;
};

struct VSInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.uv = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 sampleColor = sourceTex.Sample(samp0, input.uv);
    float3 color = max(sampleColor.rgb, 0.0f) * gamma;

    if (brighten > 0.5f)
    {
        color = sqrt(saturate(color));
    }
    if (darken > 0.5f)
    {
        color *= color;
    }
    if (solarize > 0.5f)
    {
        color = color * (1.0f - color) * 4.0f;
    }
    if (invert > 0.5f)
    {
        color = 1.0f - color;
    }
    if (shaderAmount > 0.001f)
    {
        float3 shifted = float3(color.r * 0.65f + color.g * 0.35f,
                               color.g * 0.65f + color.b * 0.35f,
                               color.b * 0.65f + color.r * 0.35f);
        color = lerp(color, shifted, saturate(shaderAmount));
    }

    return float4(saturate(color), sampleColor.a);
}
)";

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> errorBlob;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ThrowIfFailed(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, vertexShader.GetAddressOf(), errorBlob.GetAddressOf()));
    errorBlob.Reset();
    ThrowIfFailed(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, pixelShader.GetAddressOf(), errorBlob.GetAddressOf()));

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[2]{};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = &range;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[1].Constants.Num32BitValues = 8;
    rootParameters[1].Constants.ShaderRegister = 0;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = static_cast<UINT>(std::size(rootParameters));
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), errorBlob.ReleaseAndGetAddressOf()));
    ThrowIfFailed(m_d3dDevice->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(m_postProcessRootSignature.ReleaseAndGetAddressOf())));

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 2, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout = {inputElements, static_cast<UINT>(std::size(inputElements))};
    psoDesc.pRootSignature = m_postProcessRootSignature.Get();
    psoDesc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    psoDesc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_backBufferFormat;
    psoDesc.SampleDesc.Count = 1;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_postProcessPipelineState.ReleaseAndGetAddressOf())));
}

void D3D12Resources::CreatePostProcessTexture()
{
    if (!m_d3dDevice)
    {
        return;
    }

    const UINT width = std::max<UINT>(static_cast<UINT>(m_outputSize.right - m_outputSize.left), 1u);
    const UINT height = std::max<UINT>(static_cast<UINT>(m_outputSize.bottom - m_outputSize.top), 1u);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(m_postProcessSrvHeap.ReleaseAndGetAddressOf())));

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = m_backBufferFormat;
    textureDesc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &textureDesc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_postProcessTexture.ReleaseAndGetAddressOf())));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_d3dDevice->CreateShaderResourceView(m_postProcessTexture.Get(), &srvDesc, m_postProcessSrvHeap->GetCPUDescriptorHandleForHeapStart());
}

void D3D12Resources::RefreshTextureFileList()
{
    static constexpr const wchar_t* masks[] = {L"*.jpg", L"*.jpeg", L"*.png", L"*.bmp", L"*.gif", L"*.jfif", L"*.dds", L"*.tga"};
    m_textureFiles.clear();
    m_textureCycleIndex = 0;

    if (m_textureDirectory.empty())
    {
        return;
    }

    for (const wchar_t* mask : masks)
    {
        WIN32_FIND_DATAW findData{};
        const std::wstring query = m_textureDirectory + mask;
        HANDLE find = FindFirstFileW(query.c_str(), &findData);
        if (find == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                m_textureFiles.push_back(m_textureDirectory + findData.cFileName);
            }
        } while (FindNextFileW(find, &findData));

        FindClose(find);
    }
}

bool D3D12Resources::IsPostProcessEnabled() const
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_POSTPROCESS", value, static_cast<DWORD>(std::size(value))) > 0 && wcscmp(value, L"0") != 0;
}

bool D3D12Resources::IsTextureCyclingEnabled() const
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_TEXTURE_CYCLE", value, static_cast<DWORD>(std::size(value))) > 0 && wcscmp(value, L"0") != 0;
}

DWORD D3D12Resources::GetTextureCycleIntervalMs() const
{
    wchar_t value[16]{};
    if (GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_TEXTURE_CYCLE_MS", value, static_cast<DWORD>(std::size(value))) > 0)
    {
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(value, &end, 10);
        if (end && *end == 0 && parsed >= 500 && parsed <= 60000)
        {
            return static_cast<DWORD>(parsed);
        }
    }

    return 3000;
}

void D3D12Resources::MaybeCycleTexture()
{
    if (!IsTextureCyclingEnabled() || m_textureFiles.empty())
    {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (m_lastTextureCycleTick != 0 && now - m_lastTextureCycleTick < GetTextureCycleIntervalMs())
    {
        return;
    }

    m_lastTextureCycleTick = now;
    const size_t attempts = m_textureFiles.size();
    for (size_t i = 0; i < attempts; ++i)
    {
        const std::wstring textureFile = m_textureFiles[m_textureCycleIndex % m_textureFiles.size()];
        ++m_textureCycleIndex;
        if (LoadTextureFromFile(textureFile.c_str()))
        {
            return;
        }
    }
}

void D3D12Resources::CreateFeedbackResources()
{
    if (!m_d3dDevice)
    {
        return;
    }

    const UINT width = std::max<UINT>(static_cast<UINT>(m_outputSize.right - m_outputSize.left), 1u);
    const UINT height = std::max<UINT>(static_cast<UINT>(m_outputSize.bottom - m_outputSize.top), 1u);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = 2;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(m_feedbackSrvHeap.ReleaseAndGetAddressOf())));
    m_feedbackSrvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = m_backBufferFormat;
    textureDesc.SampleDesc.Count = 1;

    auto srvHandle = m_feedbackSrvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; ++i)
    {
        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                           D3D12_HEAP_FLAG_NONE,
                                                           &textureDesc,
                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                           nullptr,
                                                           IID_PPV_ARGS(m_feedbackTextures[i].ReleaseAndGetAddressOf())));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_d3dDevice->CreateShaderResourceView(m_feedbackTextures[i].Get(), &srvDesc, srvHandle);
        srvHandle.ptr += m_feedbackSrvDescriptorSize;
    }
}

void D3D12Resources::CopyBackBufferToFeedback(UINT feedbackIndex)
{
    if (feedbackIndex >= 2 || !m_feedbackTextures[feedbackIndex])
    {
        return;
    }

    D3D12_RESOURCE_BARRIER feedbackBarrier{};
    feedbackBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    feedbackBarrier.Transition.pResource = m_feedbackTextures[feedbackIndex].Get();
    feedbackBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    feedbackBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    feedbackBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &feedbackBarrier);

    m_commandList->CopyResource(m_feedbackTextures[feedbackIndex].Get(), m_renderTargets[m_frameIndex].Get());

    feedbackBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    feedbackBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &feedbackBarrier);

    m_feedbackReady[feedbackIndex] = true;
    m_feedbackIndex = 1u - feedbackIndex;
}

bool D3D12Resources::CopyBackBufferToPostProcessSource()
{
    if (!m_postProcessTexture || !m_renderTargets[m_frameIndex])
    {
        return false;
    }

    D3D12_RESOURCE_BARRIER postProcessBarrier{};
    postProcessBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postProcessBarrier.Transition.pResource = m_postProcessTexture.Get();
    postProcessBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    postProcessBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    postProcessBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &postProcessBarrier);

    m_commandList->CopyResource(m_postProcessTexture.Get(), m_renderTargets[m_frameIndex].Get());

    postProcessBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    postProcessBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &postProcessBarrier);
    return true;
}

std::wstring D3D12Resources::PickTextureFile() const
{
    static constexpr const wchar_t* masks[] = {L"*.jpg", L"*.jpeg", L"*.png", L"*.bmp", L"*.gif", L"*.jfif", L"*.dds", L"*.tga"};
    if (m_textureDirectory.empty())
    {
        return {};
    }

    for (const wchar_t* mask : masks)
    {
        WIN32_FIND_DATAW findData{};
        const std::wstring query = m_textureDirectory + mask;
        HANDLE find = FindFirstFileW(query.c_str(), &findData);
        if (find == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                std::wstring result = m_textureDirectory + findData.cFileName;
                FindClose(find);
                return result;
            }
        } while (FindNextFileW(find, &findData));

        FindClose(find);
    }

    return {};
}

void D3D12Resources::DrawTextureQuad(float bass,
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
                                     float motionWarp)
{
    if (!m_texture || !m_srvHeap || !m_texturePipelineState || !m_textureRootSignature)
    {
        return;
    }

    DrawTextureQuadFromSrv(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), m_srvHeap.Get(), bass, mids, treble, decay, zoom, rot, 1.0f, 1.0f, 0.0f, false, false, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp);
}

void D3D12Resources::DrawTextureQuadFromSrv(D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
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
                                            float motionWarp)
{
    if (!descriptorHeap || !m_texturePipelineState || !m_textureRootSignature || !m_mappedTextureVertices)
    {
        return;
    }

    if (m_textureVertexCursor + 4 > c_maxTextureVertices)
    {
        return;
    }

    const UINT vertexStart = m_textureVertexCursor;
    m_textureVertexCursor += 4;
    const float energy = std::clamp((bass + mids + treble) / 6.0f, 0.0f, 1.0f);
    const float textureZoom = std::clamp((1.0f / std::max(zoom + bass * 0.04f, 0.25f)) * zoomBias, 0.55f, 1.75f);
    const float angle = rot * 0.35f + mids * 0.035f + angleBias;
    const float c = cosf(angle);
    const float s = sinf(angle);
    const float warpTime = static_cast<float>(GetTickCount64() % 600000ULL) * 0.001f;
    const float alpha = std::clamp((0.22f + energy * 0.28f + (1.0f - std::clamp(decay, 0.70f, 1.0f)) * 0.6f) * alphaScale, 0.05f, 0.80f);
    const float tintR = std::clamp(0.55f + bass * 0.12f, 0.0f, 1.0f);
    const float tintG = std::clamp(0.55f + mids * 0.10f, 0.0f, 1.0f);
    const float tintB = std::clamp(0.55f + treble * 0.12f, 0.0f, 1.0f);

    auto setVertex = [&](UINT index, float x, float y) {
        TextureVertex& vertex = m_mappedTextureVertices[vertexStart + index];
        vertex.position[0] = x;
        vertex.position[1] = y;
        float u = 0.5f + x * 0.5f * textureZoom;
        float v = 0.5f + y * 0.5f * textureZoom;

        u = (u - motionCenterX) / motionStretchX + motionCenterX;
        v = (v - motionCenterY) / motionStretchY + motionCenterY;

        if (fabsf(motionWarp) > 0.001f)
        {
            const float warpAmount = motionWarp * 0.0035f;
            u += warpAmount * sinf(warpTime * 0.333f + x * 7.1f - y * 5.3f);
            v += warpAmount * cosf(warpTime * 0.375f - x * 4.7f - y * 6.2f);
            u += warpAmount * cosf(warpTime * 0.753f - x * 3.3f + y * 8.1f);
            v += warpAmount * sinf(warpTime * 0.825f + x * 8.8f + y * 2.9f);
        }

        const float u2 = u - motionCenterX;
        const float v2 = v - motionCenterY;
        u = u2 * c - v2 * s + motionCenterX;
        v = u2 * s + v2 * c + motionCenterY;
        u -= motionDX;
        v -= motionDY;
        vertex.uv[0] = flipU ? 1.0f - u : u;
        vertex.uv[1] = flipV ? 1.0f - v : v;
        vertex.color[0] = tintR;
        vertex.color[1] = tintG;
        vertex.color[2] = tintB;
        vertex.color[3] = alpha;
    };

    setVertex(0, -1.0f, 1.0f);
    setVertex(1, 1.0f, 1.0f);
    setVertex(2, -1.0f, -1.0f);
    setVertex(3, 1.0f, -1.0f);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetPipelineState(m_texturePipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_textureRootSignature.Get());
    m_commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
    m_commandList->IASetVertexBuffers(0, 1, &m_textureVertexBufferView);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_commandList->DrawInstanced(4, 1, vertexStart, 0);
}

void D3D12Resources::DrawTexturedCustomShapesFromSrv(D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
                                                     ID3D12DescriptorHeap* descriptorHeap,
                                                     const CustomShapeDrawCommand* customShapes,
                                                     size_t customShapeCount)
{
    if (!descriptorHeap || !customShapes || customShapeCount == 0 || !m_mappedTextureVertices || !m_textureAlphaPipelineState || !m_textureAdditivePipelineState || !m_textureRootSignature)
    {
        return;
    }

    const float width = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const float height = static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    const float aspectY = width > height ? height / width : 1.0f;
    customShapeCount = std::min<size_t>(customShapeCount, 64);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetGraphicsRootSignature(m_textureRootSignature.Get());
    m_commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
    m_commandList->IASetVertexBuffers(0, 1, &m_textureVertexBufferView);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12PipelineState* activePipelineState = nullptr;
    auto setVertex = [&](UINT index, float x, float y, float u, float v, float r, float g, float b, float a) {
        TextureVertex& vertex = m_mappedTextureVertices[index];
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.uv[0] = u;
        vertex.uv[1] = v;
        vertex.color[0] = std::clamp(r, 0.0f, 1.0f);
        vertex.color[1] = std::clamp(g, 0.0f, 1.0f);
        vertex.color[2] = std::clamp(b, 0.0f, 1.0f);
        vertex.color[3] = std::clamp(a, 0.0f, 1.0f);
    };

    for (size_t shapeIndex = 0; shapeIndex < customShapeCount; ++shapeIndex)
    {
        const CustomShapeDrawCommand& shape = customShapes[shapeIndex];
        if (!shape.textured || (shape.a <= 0.001f && shape.a2 <= 0.001f))
        {
            continue;
        }

        const int sides = std::clamp(shape.sides, 3, 100);
        const UINT vertexCount = static_cast<UINT>(sides * 3);
        if (m_textureVertexCursor + vertexCount > c_maxTextureVertices)
        {
            break;
        }

        ID3D12PipelineState* pipelineState = shape.additive ? m_textureAdditivePipelineState.Get() : m_textureAlphaPipelineState.Get();
        if (activePipelineState != pipelineState)
        {
            m_commandList->SetPipelineState(pipelineState);
            activePipelineState = pipelineState;
        }

        const UINT vertexStart = m_textureVertexCursor;
        m_textureVertexCursor += vertexCount;
        const float centerX = std::clamp(shape.x, -1000.0f, 1000.0f) * 2.0f - 1.0f;
        const float centerY = std::clamp(shape.y, -1000.0f, 1000.0f) * -2.0f + 1.0f;
        const float radius = std::clamp(shape.radius, 0.0f, 1000.0f);
        const float shapeAngleBase = shape.angle + 3.1415927f * 0.25f;
        const float textureAngleBase = shape.texAngle + 3.1415927f * 0.25f;
        const float textureZoom = std::clamp(fabsf(shape.texZoom), 0.001f, 1000.0f);
        UINT writeIndex = vertexStart;

        auto ringPoint = [&](int side, float& x, float& y, float& u, float& v) {
            const float t = static_cast<float>(side) / static_cast<float>(sides);
            const float shapeAngle = t * 6.2831853f + shapeAngleBase;
            const float textureAngle = t * 6.2831853f + textureAngleBase;
            x = centerX + radius * cosf(shapeAngle) * aspectY;
            y = centerY + radius * sinf(shapeAngle);
            u = 0.5f + 0.5f * cosf(textureAngle) * aspectY / textureZoom;
            v = 0.5f + 0.5f * sinf(textureAngle) / textureZoom;
        };

        for (int side = 0; side < sides; ++side)
        {
            const int next = (side + 1) % sides;
            float x0 = 0.0f;
            float y0 = 0.0f;
            float u0 = 0.0f;
            float v0 = 0.0f;
            float x1 = 0.0f;
            float y1 = 0.0f;
            float u1 = 0.0f;
            float v1 = 0.0f;
            ringPoint(side, x0, y0, u0, v0);
            ringPoint(next, x1, y1, u1, v1);
            setVertex(writeIndex++, centerX, centerY, 0.5f, 0.5f, shape.r, shape.g, shape.b, shape.a);
            setVertex(writeIndex++, x0, y0, u0, v0, shape.r2, shape.g2, shape.b2, shape.a2);
            setVertex(writeIndex++, x1, y1, u1, v1, shape.r2, shape.g2, shape.b2, shape.a2);
        }

        m_commandList->DrawInstanced(vertexCount, 1, vertexStart, 0);
    }
}

void D3D12Resources::DrawTextureMeshFromSrv(D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
                                            ID3D12DescriptorHeap* descriptorHeap,
                                            const TextureWarpVertex* vertices,
                                            size_t vertexCount,
                                            float bass,
                                            float mids,
                                            float treble,
                                            float decay)
{
    if (!descriptorHeap || !vertices || vertexCount < 3 || !m_texturePipelineState || !m_textureRootSignature || !m_mappedTextureVertices)
    {
        return;
    }

    const UINT copiedVertexCount = static_cast<UINT>(std::min<size_t>(vertexCount, c_maxTextureVertices - m_textureVertexCursor));
    if (copiedVertexCount < 3)
    {
        return;
    }

    const UINT vertexStart = m_textureVertexCursor;
    m_textureVertexCursor += copiedVertexCount;

    const float energy = std::clamp((bass + mids + treble) / 6.0f, 0.0f, 1.0f);
    const float alpha = std::clamp(0.32f + energy * 0.22f + (1.0f - std::clamp(decay, 0.70f, 1.0f)) * 0.55f, 0.08f, 0.85f);
    const float tintR = std::clamp(0.62f + bass * 0.10f, 0.0f, 1.0f);
    const float tintG = std::clamp(0.62f + mids * 0.08f, 0.0f, 1.0f);
    const float tintB = std::clamp(0.62f + treble * 0.10f, 0.0f, 1.0f);

    for (UINT index = 0; index < copiedVertexCount; ++index)
    {
        const TextureWarpVertex& source = vertices[index];
        TextureVertex& vertex = m_mappedTextureVertices[vertexStart + index];
        vertex.position[0] = source.x;
        vertex.position[1] = source.y;
        vertex.uv[0] = source.u;
        vertex.uv[1] = source.v;
        vertex.color[0] = std::clamp(source.r * tintR, 0.0f, 1.0f);
        vertex.color[1] = std::clamp(source.g * tintG, 0.0f, 1.0f);
        vertex.color[2] = std::clamp(source.b * tintB, 0.0f, 1.0f);
        vertex.color[3] = std::clamp(source.a * alpha, 0.0f, 1.0f);
    }

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetPipelineState(m_texturePipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_textureRootSignature.Get());
    m_commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
    m_commandList->IASetVertexBuffers(0, 1, &m_textureVertexBufferView);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(copiedVertexCount - copiedVertexCount % 3u, 1, vertexStart, 0);
}

void D3D12Resources::DrawPostProcessFromSrv(D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
                                            ID3D12DescriptorHeap* descriptorHeap,
                                            float gamma,
                                            bool brighten,
                                            bool darken,
                                            bool solarize,
                                            bool invert,
                                            float shaderAmount)
{
    if (!descriptorHeap || !m_mappedTextureVertices || !m_postProcessPipelineState || !m_postProcessRootSignature)
    {
        return;
    }

    if (m_textureVertexCursor + 4 > c_maxTextureVertices)
    {
        return;
    }

    const UINT vertexStart = m_textureVertexCursor;
    m_textureVertexCursor += 4;
    const TextureVertex vertices[] = {
        {{-1.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{1.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{-1.0f, -1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{1.0f, -1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    };
    memcpy(m_mappedTextureVertices + vertexStart, vertices, sizeof(vertices));

    const float constants[8] = {
        std::clamp(gamma, 0.0f, 8.0f),
        brighten ? 1.0f : 0.0f,
        darken ? 1.0f : 0.0f,
        solarize ? 1.0f : 0.0f,
        invert ? 1.0f : 0.0f,
        std::clamp(shaderAmount, 0.0f, 1.0f),
        0.0f,
        0.0f,
    };

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetPipelineState(m_postProcessPipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_postProcessRootSignature.Get());
    m_commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
    m_commandList->SetGraphicsRoot32BitConstants(1, static_cast<UINT>(std::size(constants)), constants, 0);
    m_commandList->IASetVertexBuffers(0, 1, &m_textureVertexBufferView);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_commandList->DrawInstanced(4, 1, vertexStart, 0);
}

void D3D12Resources::CreateWaveformResources()
{
    static constexpr char shaderSource[] = R"(
struct VSInput
{
    float2 position : POSITION;
    float4 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
)";

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> errorBlob;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, vertexShader.GetAddressOf(), errorBlob.GetAddressOf());
    ThrowIfFailed(hr);
    errorBlob.Reset();
    hr = D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, pixelShader.GetAddressOf(), errorBlob.GetAddressOf());
    ThrowIfFailed(hr);

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), errorBlob.ReleaseAndGetAddressOf()));
    ThrowIfFailed(m_d3dDevice->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(m_waveformRootSignature.ReleaseAndGetAddressOf())));

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 2, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout = {inputElements, static_cast<UINT>(std::size(inputElements))};
    psoDesc.pRootSignature = m_waveformRootSignature.Get();
    psoDesc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    psoDesc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_backBufferFormat;
    psoDesc.SampleDesc.Count = 1;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_waveformPipelineState.ReleaseAndGetAddressOf())));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC additivePsoDesc = psoDesc;
    additivePsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    additivePsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&additivePsoDesc, IID_PPV_ARGS(m_waveformAdditivePipelineState.ReleaseAndGetAddressOf())));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC solidPsoDesc = psoDesc;
    solidPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&solidPsoDesc, IID_PPV_ARGS(m_solidPipelineState.ReleaseAndGetAddressOf())));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC solidAdditivePsoDesc = solidPsoDesc;
    solidAdditivePsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    solidAdditivePsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&solidAdditivePsoDesc, IID_PPV_ARGS(m_solidAdditivePipelineState.ReleaseAndGetAddressOf())));

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(WaveformVertex) * c_maxWaveformVertices;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &bufferDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_waveformVertexBuffer.ReleaseAndGetAddressOf())));
    ThrowIfFailed(m_waveformVertexBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedWaveformVertices)));

    m_waveformVertexBufferView.BufferLocation = m_waveformVertexBuffer->GetGPUVirtualAddress();
    m_waveformVertexBufferView.StrideInBytes = sizeof(WaveformVertex);
    m_waveformVertexBufferView.SizeInBytes = sizeof(WaveformVertex) * c_maxWaveformVertices;
}
} // namespace DX
