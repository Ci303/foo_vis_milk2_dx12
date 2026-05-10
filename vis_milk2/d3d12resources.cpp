/*
 * d3d12resources.cpp - Minimal Direct3D 12 device and swap chain path.
 */

#include "pch.h"
#include "d3d12resources.h"
#include "deviceresources.h"

#include <array>
#include <wincodec.h>
#include <fstream>

using Microsoft::WRL::ComPtr;

namespace DX
{
namespace
{
constexpr UINT c_postProcessConstantBufferSize = 4096;

constexpr uint32_t MakeFourCC(char a, char b, char c, char d) noexcept
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

uint32_t ReadLe32(const uint8_t* data) noexcept
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

std::array<uint8_t, 7> GlyphRows(wchar_t ch) noexcept
{
    switch (towupper(ch))
    {
        case L'0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
        case L'1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case L'2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
        case L'3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
        case L'4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
        case L'5': return {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
        case L'6': return {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
        case L'7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        case L'8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
        case L'9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
        case L'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case L'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
        case L'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
        case L'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
        case L'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
        case L'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
        case L'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
        case L'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case L'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case L'J': return {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
        case L'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        case L'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        case L'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
        case L'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
        case L'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case L'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
        case L'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
        case L'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
        case L'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
        case L'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case L'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case L'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
        case L'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
        case L'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
        case L'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
        case L'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
        case L'.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
        case L',': return {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08};
        case L':': return {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
        case L'-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
        case L'_': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
        case L'/': return {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
        case L'\\': return {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01};
        case L'(': return {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
        case L')': return {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
        case L'[': return {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E};
        case L']': return {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E};
        case L'+': return {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
        case L'!': return {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
        case L'?': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
        case L' ': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        default: return {0x1F, 0x11, 0x15, 0x15, 0x15, 0x11, 0x1F};
    }
}
} // namespace

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
        WaitForGpu(1000);
    }

    for (UINT i = 0; i < c_maxBackBufferCount; ++i)
    {
        if (m_waveformVertexBuffers[i] && m_mappedWaveformVertexBuffers[i])
        {
            m_waveformVertexBuffers[i]->Unmap(0, nullptr);
            m_mappedWaveformVertexBuffers[i] = nullptr;
        }
    }
    m_mappedWaveformVertices = nullptr;

    for (UINT i = 0; i < c_maxBackBufferCount; ++i)
    {
        if (m_textureVertexBuffers[i] && m_mappedTextureVertexBuffers[i])
        {
            m_textureVertexBuffers[i]->Unmap(0, nullptr);
            m_mappedTextureVertexBuffers[i] = nullptr;
        }
    }
    m_mappedTextureVertices = nullptr;

    if (m_postProcessConstantBuffer && m_mappedPostProcessConstantBuffer)
    {
        m_postProcessConstantBuffer->Unmap(0, nullptr);
        m_mappedPostProcessConstantBuffer = nullptr;
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

    if (!WaitForGpu(1000))
    {
        throw std::runtime_error("Timed out waiting for the D3D12 GPU before resizing");
    }
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

    if (!WaitForGpu(1000))
    {
        return false;
    }

    m_outputSize = newRc;
    CreateWindowSizeDependentResources();
    return true;
}

bool D3D12Resources::WindowSwap(HWND window, int width, int height)
{
    if (!WaitForGpu(1000))
        return false;
    CaptureBackBufferForResume();
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
    m_swapChain.Reset();

    SetWindow(window, width, height);
    CreateWindowSizeDependentResources();
    return true;
}

std::vector<std::wstring> D3D12Resources::GetActiveTextureFiles() const
{
    std::vector<std::wstring> textureFiles;
    textureFiles.reserve(m_activeTextureLayerCount);
    for (UINT slot = 0; slot < m_activeTextureLayerCount && slot < c_maxTextureLayers; ++slot)
    {
        if (!m_textureSlots[slot].file.empty())
            textureFiles.push_back(m_textureSlots[slot].file);
    }
    return textureFiles;
}

void D3D12Resources::SetTextureDirectory(const wchar_t* textureDirectory)
{
    m_presetTextureOverride = false;
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
            SetTextureFile(textureFile.c_str());
            if (!m_textureFiles.empty())
            {
                m_textureCycleIndex = (m_textureCycleIndex + 1) % m_textureFiles.size();
            }
        }
    }
}

bool D3D12Resources::SetTextureFile(const wchar_t* textureFile)
{
    if (!m_d3dDevice || !m_commandQueue || !textureFile || !*textureFile)
    {
        return false;
    }

    const wchar_t* textureFiles[] = {textureFile};
    return SetTextureFiles(textureFiles, std::size(textureFiles));
}

bool D3D12Resources::SetTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount)
{
    return SetTextureFilesInternal(textureFiles, textureFileCount, false);
}

bool D3D12Resources::SetPresetTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount)
{
    return SetTextureFilesInternal(textureFiles, textureFileCount, true);
}

void D3D12Resources::ClearPresetTextureOverride()
{
    if (!m_presetTextureOverride)
    {
        return;
    }

    m_presetTextureOverride = false;
    if (!m_d3dDevice || !m_commandQueue)
    {
        return;
    }

    const std::wstring textureFile = PickTextureFile();
    if (!textureFile.empty())
    {
        SetTextureFile(textureFile.c_str());
        if (!m_textureFiles.empty())
        {
            m_textureCycleIndex = (m_textureCycleIndex + 1) % m_textureFiles.size();
        }
    }
}

void D3D12Resources::ResetVisualHistory()
{
    if (!m_swapChain || !m_commandQueue || !m_fence)
    {
        return;
    }

    WaitForGpu(1000);
    m_feedbackReady[0] = false;
    m_feedbackReady[1] = false;
    m_feedbackIndex = 0;
    m_lastTextureCycleTick = GetTickCount64();

    const UINT buffersToPrime = std::max<UINT>(m_backBufferCount, 1u);
    for (UINT i = 0; i < buffersToPrime; ++i)
    {
        Clear();
        Present();
    }
}

void D3D12Resources::SetTextOverlay(const wchar_t* topLeft,
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
    m_overlayTopLeft = topLeft ? topLeft : L"";
    m_overlayTopRight = topRight ? topRight : L"";
    m_overlayBottomLeft = bottomLeft ? bottomLeft : L"";
    m_overlayDebugLine = debugLine ? debugLine : L"";
    m_overlayCenterText = centerText ? centerText : L"";
    m_overlayCenterX = std::clamp(centerX, 0.0f, 1.0f);
    m_overlayCenterY = std::clamp(centerY, 0.0f, 1.0f);
    m_overlayCenterScale = std::clamp(centerScale, 0.35f, 8.0f);
    m_overlayCenterR = std::clamp(centerR, 0.0f, 1.0f);
    m_overlayCenterG = std::clamp(centerG, 0.0f, 1.0f);
    m_overlayCenterB = std::clamp(centerB, 0.0f, 1.0f);
    m_overlayCenterA = std::clamp(centerA, 0.0f, 1.0f);
}

bool D3D12Resources::SetPresetCompositeShader(const void* bytecode, size_t bytecodeSize)
{
    m_presetCompositePipelineState.Reset();
    m_presetCompositeShaderBytecode.clear();

    if (!bytecode || bytecodeSize == 0)
    {
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    m_presetCompositeShaderBytecode.assign(bytes, bytes + bytecodeSize);
    return CreatePresetCompositePipeline();
}

void D3D12Resources::ClearPresetCompositeShader()
{
    m_presetCompositePipelineState.Reset();
    m_presetCompositeShaderBytecode.clear();
}

void D3D12Resources::SetPresetShaderRuntimeConstants(float time,
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
                                                     const float* rotMatrices)
{
    auto clean = [](float value, float fallback) {
        return std::isfinite(value) ? value : fallback;
    };

    m_presetShaderTime = clean(time, 0.0f);
    m_presetShaderFps = clean(fps, 0.0f);
    m_presetShaderFrame = clean(frame, 0.0f);
    m_presetShaderProgress = std::clamp(clean(progress, 0.0f), 0.0f, 1.0f);
    m_presetShaderBass = clean(bass, 0.0f);
    m_presetShaderMids = clean(mids, 0.0f);
    m_presetShaderTreble = clean(treble, 0.0f);
    m_presetShaderBassAtt = clean(bassAtt, m_presetShaderBass);
    m_presetShaderMidsAtt = clean(midsAtt, m_presetShaderMids);
    m_presetShaderTrebleAtt = clean(trebleAtt, m_presetShaderTreble);

    for (size_t i = 0; i < std::size(m_presetShaderQ); ++i)
    {
        m_presetShaderQ[i] = qValues ? clean(qValues[i], 0.0f) : 0.0f;
    }
    for (size_t i = 0; i < std::size(m_presetShaderRandFrame); ++i)
    {
        m_presetShaderRandFrame[i] = randFrame ? clean(randFrame[i], 0.0f) : 0.0f;
        m_presetShaderRandPreset[i] = randPreset ? clean(randPreset[i], 0.0f) : 0.0f;
    }
    for (size_t i = 0; i < std::size(m_presetShaderBlurMin); ++i)
    {
        m_presetShaderBlurMin[i] = blurMin ? clean(blurMin[i], 0.0f) : 0.0f;
        m_presetShaderBlurMax[i] = blurMax ? clean(blurMax[i], 1.0f) : 1.0f;
    }
    for (size_t i = 0; i < std::size(m_presetShaderRotMatrices); ++i)
    {
        m_presetShaderRotMatrices[i] = rotMatrices ? clean(rotMatrices[i], 0.0f) : 0.0f;
    }
}

RECT D3D12Resources::GetPresentationSize() const noexcept
{
    RECT result = m_outputSize;
    RECT clientRect{};
    if (m_window && GetClientRect(m_window, &clientRect))
    {
        const long clientWidth = clientRect.right - clientRect.left;
        const long clientHeight = clientRect.bottom - clientRect.top;
        if (clientWidth > 0 && clientHeight > 0)
        {
            result.left = 0;
            result.top = 0;
            result.right = clientWidth;
            result.bottom = clientHeight;
        }
    }

    result.right = std::max<long>(result.right - result.left, 1);
    result.bottom = std::max<long>(result.bottom - result.top, 1);
    result.left = 0;
    result.top = 0;
    return result;
}

bool D3D12Resources::SetTextureFilesInternal(const wchar_t* const* textureFiles, size_t textureFileCount, bool presetTextureOverride)
{
    if (!m_d3dDevice || !m_commandQueue || !textureFiles || textureFileCount == 0)
    {
        return false;
    }

    WaitForGpu();

    UINT loadedCount = 0;
    for (size_t index = 0; index < textureFileCount && loadedCount < c_maxTextureLayers; ++index)
    {
        const wchar_t* textureFile = textureFiles[index];
        if (!textureFile || !*textureFile)
        {
            continue;
        }

        bool duplicate = false;
        for (UINT existing = 0; existing < loadedCount; ++existing)
        {
            if (!_wcsicmp(m_textureSlots[existing].file.c_str(), textureFile))
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }

        if (LoadTextureFromFile(textureFile, loadedCount))
        {
            ++loadedCount;
        }
    }

    if (loadedCount == 0)
    {
        if (presetTextureOverride)
        {
            m_presetTextureOverride = false;
        }
        return false;
    }

    for (UINT slot = loadedCount; slot < c_maxTextureLayers; ++slot)
    {
        m_textureSlots[slot].texture.Reset();
        m_textureSlots[slot].uploadBuffer.Reset();
        m_textureSlots[slot].file.clear();
    }
    m_activeTextureLayerCount = loadedCount;
    m_currentTextureFile = m_textureSlots[0].file;
    m_presetTextureOverride = presetTextureOverride;
    return true;
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
                                  const CustomShapeDrawCommand* customShapes,
                                  size_t customShapeCount,
                                  const CustomWaveVertex* customWaveVertices,
                                  size_t customWaveVertexCount,
                                  const CustomWaveDrawCommand* customWaveDraws,
                                  size_t customWaveDrawCount,
                                  const TextureWarpVertex* textureWarpVertices,
                                  size_t textureWarpVertexCount,
                                  int textureWarpGridX,
                                  int textureWarpGridY)
{
    (void)spectrumRight;
    m_mappedWaveformVertices = m_mappedWaveformVertexBuffers[m_frameIndex];
    m_waveformVertexBufferView = m_waveformVertexBufferViews[m_frameIndex];
    m_mappedTextureVertices = m_mappedTextureVertexBuffers[m_frameIndex];
    m_textureVertexBufferView = m_textureVertexBufferViews[m_frameIndex];

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
    postBlurAmount = std::clamp(postBlurAmount, 0.0f, 1.0f);
    postBlurEdgeDarken = std::clamp(postBlurEdgeDarken, 0.0f, 1.0f);
    const float forcedPostBlurAmount = IsPostProcessBlurEnabled() ? GetPostProcessBlurAmount() : 0.0f;
    const float activePostBlurAmount = std::max(postBlurAmount, forcedPostBlurAmount);
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
    const float bgLift = (1.0f - decay) * 0.015f;
    const float bg[] = {bgLift, bgLift, bgLift, 1.0f};
    const float centerX = std::clamp(waveX * 2.0f - 1.0f, -1.10f, 1.10f);
    const float centerY = std::clamp(1.0f - waveY * 2.0f, -1.10f, 1.10f);
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
                                    postShaderAmount > 0.001f ||
                                    activePostBlurAmount > 0.001f);
    const bool hasTextureWarpMesh = textureWarpVertices && textureWarpVertexCount >= 3;
    const bool hasTextureWarpGrid = textureWarpVertices &&
                                    textureWarpGridX > 0 &&
                                    textureWarpGridY > 0 &&
                                    textureWarpVertexCount >= static_cast<size_t>(textureWarpGridX) * static_cast<size_t>(textureWarpGridY) * 6;

    waveMode = ((waveMode % 9) + 9) % 9;
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
    std::vector<float> smoothedLeft;
    std::vector<float> smoothedRight;
    waveSmoothing = std::clamp(waveSmoothing, 0.0f, 0.9f);
    if (waveSmoothing > 0.001f)
    {
        smoothedLeft.resize(sourceCount);
        smoothedRight.resize(sourceCount);
        const float direct = 1.0f - waveSmoothing;
        smoothedLeft[0] = std::clamp(left[0], -1.0f, 1.0f);
        smoothedRight[0] = std::clamp(right[0], -1.0f, 1.0f);
        for (UINT i = 1; i < sourceCount; ++i)
        {
            smoothedLeft[i] = std::clamp(left[i], -1.0f, 1.0f) * direct + smoothedLeft[i - 1] * waveSmoothing;
            smoothedRight[i] = std::clamp(right[i], -1.0f, 1.0f) * direct + smoothedRight[i - 1] * waveSmoothing;
        }
        left = smoothedLeft.data();
        right = smoothedRight.data();
    }
    UINT activeCount = sourceCount;
    UINT visibleSegments = 1;
    const float timeSeconds = static_cast<float>(GetTickCount64() % 600000ULL) * 0.001f;
    (void)waveScale;
    const float responsiveScale = 1.0f;
    const float baseR = std::clamp(waveR, 0.0f, 1.0f);
    const float baseG = std::clamp(waveG, 0.0f, 1.0f);
    const float baseB = std::clamp(waveB, 0.0f, 1.0f);
    float effectiveWaveA = waveA;
    const float outputWidthForAlpha = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    auto sizeAlphaScale = [&](float scale256, float scale512, float scale1024, float scale2048) {
        if (outputWidthForAlpha >= 1536.0f)
            return scale2048;
        if (outputWidthForAlpha >= 768.0f)
            return scale1024;
        if (outputWidthForAlpha >= 384.0f)
            return scale512;
        return scale256;
    };
    if (waveMode == 1)
    {
        effectiveWaveA *= 1.25f;
    }
    else if (waveMode == 2 || waveMode == 5)
    {
        effectiveWaveA *= sizeAlphaScale(0.07f, 0.09f, 0.11f, 0.13f);
    }
    else if (waveMode == 3)
    {
        effectiveWaveA = sizeAlphaScale(0.075f, 0.150f, 0.220f, 0.330f) * 1.3f * std::pow(std::max(treble, 0.0f), 2.0f);
    }
    effectiveWaveA *= std::clamp(waveAlphaVolumeScale, 0.0f, 4.0f);
    const float baseA = std::clamp(effectiveWaveA, 0.0f, 1.0f);

    auto sampleAt = [&](const float* source, UINT index) {
        return std::clamp(source[std::min(index, sourceCount - 1)], -1.0f, 1.0f);
    };
    auto spectrumAt = [&](const float* source, UINT index) {
        if (!source)
        {
            return 0.000001f;
        }
        return std::max(0.000001f, source[std::min<UINT>(index, 511u)]);
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

        WaveformVertex& pass1 = m_mappedWaveformVertices[groupCount + baseIndex];
        pass1 = vertex;
        pass1.position[0] = x + pixelX;

        WaveformVertex& pass2 = m_mappedWaveformVertices[2 * groupCount + baseIndex];
        pass2 = vertex;
        pass2.position[0] = x + pixelX;
        pass2.position[1] = y + pixelY;

        WaveformVertex& pass3 = m_mappedWaveformVertices[3 * groupCount + baseIndex];
        pass3 = vertex;
        pass3.position[1] = y + pixelY;
    };
    auto writeScreenPoint = [&](UINT segment, UINT index, float x, float y, float r, float g, float b) {
        const UINT groupCount = activeCount * visibleSegments;
        const UINT baseIndex = segment * activeCount + index;

        WaveformVertex& vertex = m_mappedWaveformVertices[baseIndex];
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.color[0] = r;
        vertex.color[1] = g;
        vertex.color[2] = b;
        vertex.color[3] = baseA;

        WaveformVertex& pass1 = m_mappedWaveformVertices[groupCount + baseIndex];
        pass1 = vertex;
        pass1.position[0] = x + pixelX;

        WaveformVertex& pass2 = m_mappedWaveformVertices[2 * groupCount + baseIndex];
        pass2 = vertex;
        pass2.position[0] = x + pixelX;
        pass2.position[1] = y + pixelY;

        WaveformVertex& pass3 = m_mappedWaveformVertices[3 * groupCount + baseIndex];
        pass3 = vertex;
        pass3.position[1] = y + pixelY;
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

            radius = std::clamp(radius * responsiveScale, -2.0f, 2.0f);
            const float angle = t * 6.2831853f + timeSeconds * 0.2f;
            writePoint(0, i, radius * cosf(angle), radius * sinf(angle), baseR, baseG, baseB);
        }
    }
    else if (waveMode == 1)
    {
        activeCount = std::min<UINT>(sourceCount > 33 ? sourceCount / 2 : sourceCount, 240u);
        for (UINT i = 0; i < activeCount; ++i)
        {
            const float radius = std::clamp((0.53f + 0.43f * sampleAt(right, i) + waveParam) * responsiveScale, -2.0f, 2.0f);
            const float angle = sampleAt(left, i + 32) * 1.57f + timeSeconds * 2.3f;
            writePoint(0, i, radius * cosf(angle), radius * sinf(angle), baseR, baseG, baseB);
        }
    }
    else if (waveMode == 2 || waveMode == 3)
    {
        activeCount = std::min<UINT>(sourceCount > 33 ? sourceCount - 33 : sourceCount, 480u);
        const float xyScale = responsiveScale;
        for (UINT i = 0; i < activeCount; ++i)
        {
            writePoint(0, i, sampleAt(right, i) * xyScale, -sampleAt(left, i + 32) * xyScale, baseR, baseG, baseB);
        }
    }
    else if (waveMode == 4)
    {
        activeCount = std::min<UINT>(sourceCount > 26 ? sourceCount - 26 : sourceCount, 480u);
        const UINT sampleOffset = sourceCount > activeCount ? (sourceCount - activeCount) / 2 : 0;
        const float momentum = 0.45f + 0.5f * (waveParam * 0.5f + 0.5f);
        const float direct = 1.0f - momentum;
        float previousX[2]{};
        float previousY[2]{};
        for (UINT i = 0; i < activeCount; ++i)
        {
            float x = -0.92f + 1.84f * (static_cast<float>(i) / static_cast<float>(activeCount - 1));
            float y = -sampleAt(left, i + sampleOffset) * 0.47f * responsiveScale;
            x += sampleAt(right, i + 25 + sampleOffset) * 0.44f * responsiveScale;
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
            writePoint(0, i, (x0 * rotC - y0 * rotS) * responsiveScale, (x0 * rotS + y0 * rotC) * responsiveScale, baseR, baseG, baseB);
        }
    }
    else
    {
        activeCount = waveMode == 8 ? 256u : std::min<UINT>(sourceCount > 33 ? sourceCount / 2 : sourceCount, 240u);
        visibleSegments = waveMode == 7 ? 2u : 1u;
        const UINT sampleOffset = (waveMode == 8 || sourceCount <= activeCount) ? 0 : (sourceCount - activeCount) / 2;
        const float angle = 1.57f * waveParam;
        float dx = cosf(angle);
        float dy = sinf(angle);
        float edgeX[2] = {
            centerX * cosf(angle + 1.57f) - dx * 3.0f,
            centerX * cosf(angle + 1.57f) + dx * 3.0f,
        };
        float edgeY[2] = {
            -centerX * sinf(angle + 1.57f) + dy * 3.0f,
            -centerX * sinf(angle + 1.57f) - dy * 3.0f,
        };
        for (int edge = 0; edge < 2; ++edge)
        {
            for (int plane = 0; plane < 4; ++plane)
            {
                float t = 0.0f;
                bool clip = false;
                switch (plane)
                {
                    case 0:
                        if (edgeX[edge] > 1.1f)
                        {
                            t = (1.1f - edgeX[1 - edge]) / (edgeX[edge] - edgeX[1 - edge]);
                            clip = true;
                        }
                        break;
                    case 1:
                        if (edgeX[edge] < -1.1f)
                        {
                            t = (-1.1f - edgeX[1 - edge]) / (edgeX[edge] - edgeX[1 - edge]);
                            clip = true;
                        }
                        break;
                    case 2:
                        if (edgeY[edge] > 1.1f)
                        {
                            t = (1.1f - edgeY[1 - edge]) / (edgeY[edge] - edgeY[1 - edge]);
                            clip = true;
                        }
                        break;
                    case 3:
                        if (edgeY[edge] < -1.1f)
                        {
                            t = (-1.1f - edgeY[1 - edge]) / (edgeY[edge] - edgeY[1 - edge]);
                            clip = true;
                        }
                        break;
                }

                if (clip)
                {
                    const float spanX = edgeX[edge] - edgeX[1 - edge];
                    const float spanY = edgeY[edge] - edgeY[1 - edge];
                    edgeX[edge] = edgeX[1 - edge] + spanX * t;
                    edgeY[edge] = edgeY[1 - edge] + spanY * t;
                }
            }
        }

        dx = (edgeX[1] - edgeX[0]) / static_cast<float>(activeCount);
        dy = (edgeY[1] - edgeY[0]) / static_cast<float>(activeCount);
        const float clippedAngle = atan2f(dy, dx);
        const float perpX = cosf(clippedAngle + 1.57f);
        const float perpY = -sinf(clippedAngle + 1.57f);
        const float sep = waveMode == 7 ? powf(std::clamp(waveY, -1000.0f, 1000.0f), 2.0f) : 0.0f;
        for (UINT i = 0; i < activeCount; ++i)
        {
            const float lineX = edgeX[0] + dx * static_cast<float>(i);
            const float lineY = edgeY[0] + dy * static_cast<float>(i);
            const float sourceSample = waveMode == 8 ?
                0.1f * logf(spectrumAt(spectrumLeft, i * 2) + spectrumAt(spectrumLeft, i * 2 + 1)) :
                sampleAt(left, i + sampleOffset) * 0.25f * responsiveScale;
            const float leftSample = sourceSample + sep;
            writeScreenPoint(0, i, lineX + perpX * leftSample, lineY + perpY * leftSample, baseR, baseG, baseB);
            if (visibleSegments == 2)
            {
                const float rightSample = sampleAt(right, i + sampleOffset) * 0.25f * responsiveScale - sep;
                writeScreenPoint(1, i, lineX + perpX * rightSample, lineY + perpY * rightSample, std::clamp(baseR * 0.75f, 0.0f, 1.0f), std::clamp(baseG * 0.85f, 0.0f, 1.0f), std::clamp(baseB * 1.15f, 0.0f, 1.0f));
            }
        }
    }

    if (activeCount < 2)
    {
        Clear();
        return;
    }

    const UINT originalActiveCount = activeCount;
    const UINT originalDrawGroupCount = originalActiveCount * visibleSegments;
    const UINT smoothedActiveCount = originalActiveCount * 2u - 1u;
    if (smoothedActiveCount * visibleSegments * 4u <= c_maxWaveformVertices)
    {
        std::vector<WaveformVertex> sourceVertices(originalDrawGroupCount);
        for (UINT i = 0; i < originalDrawGroupCount; ++i)
        {
            sourceVertices[i] = m_mappedWaveformVertices[i];
        }

        auto writeSmoothedWaveVertex = [&](UINT segment, UINT index, const WaveformVertex& source) {
            const UINT groupCount = smoothedActiveCount * visibleSegments;
            const UINT baseIndex = segment * smoothedActiveCount + index;
            const float x = source.position[0];
            const float y = source.position[1];
            const float r = source.color[0];
            const float g = source.color[1];
            const float b = source.color[2];
            const float a = source.color[3];

            WaveformVertex& vertex = m_mappedWaveformVertices[baseIndex];
            vertex = source;

            (void)r;
            (void)g;
            (void)b;
            (void)a;

            WaveformVertex& pass1 = m_mappedWaveformVertices[groupCount + baseIndex];
            pass1 = vertex;
            pass1.position[0] = x + pixelX;

            WaveformVertex& pass2 = m_mappedWaveformVertices[2 * groupCount + baseIndex];
            pass2 = vertex;
            pass2.position[0] = x + pixelX;
            pass2.position[1] = y + pixelY;

            WaveformVertex& pass3 = m_mappedWaveformVertices[3 * groupCount + baseIndex];
            pass3 = vertex;
            pass3.position[1] = y + pixelY;
        };

        const float c1 = -0.15f;
        const float c2 = 1.15f;
        const float c3 = 1.15f;
        const float c4 = -0.15f;
        const float invSum = 1.0f / (c1 + c2 + c3 + c4);
        for (UINT segment = 0; segment < visibleSegments; ++segment)
        {
            UINT outputIndex = 0;
            UINT below = 0;
            UINT above2 = 1;
            const UINT sourceBase = segment * originalActiveCount;
            for (UINT i = 0; i + 1 < originalActiveCount; ++i)
            {
                const UINT above = above2;
                above2 = std::min(originalActiveCount - 1u, i + 2u);
                const WaveformVertex& current = sourceVertices[sourceBase + i];
                writeSmoothedWaveVertex(segment, outputIndex++, current);

                WaveformVertex smoothed = current;
                smoothed.position[0] = (c1 * sourceVertices[sourceBase + below].position[0] +
                                        c2 * sourceVertices[sourceBase + i].position[0] +
                                        c3 * sourceVertices[sourceBase + above].position[0] +
                                        c4 * sourceVertices[sourceBase + above2].position[0]) *
                                       invSum;
                smoothed.position[1] = (c1 * sourceVertices[sourceBase + below].position[1] +
                                        c2 * sourceVertices[sourceBase + i].position[1] +
                                        c3 * sourceVertices[sourceBase + above].position[1] +
                                        c4 * sourceVertices[sourceBase + above2].position[1]) *
                                       invSum;
                writeSmoothedWaveVertex(segment, outputIndex++, smoothed);
                below = i;
            }
            writeSmoothedWaveVertex(segment, outputIndex, sourceVertices[sourceBase + originalActiveCount - 1u]);
        }
        activeCount = smoothedActiveCount;
    }

    const UINT drawGroupCount = activeCount * visibleSegments;
    const UINT lineVertexCount = drawGroupCount * 4;

    if (lineVertexCount > c_maxWaveformVertices)
    {
        Clear();
        return;
    }

    struct ShapeDrawBatch
    {
        UINT start = 0;
        UINT count = 0;
        bool additive = false;
        bool triangleList = false;
    };

    UINT customShapeVertexStart = lineVertexCount;
    UINT customShapeVertexCount = 0;
    std::vector<ShapeDrawBatch> customShapeDrawBatches;
    customShapeDrawBatches.reserve(128);

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
        customShapeCount = std::min<size_t>(customShapeCount, 256);

        for (size_t shapeIndex = 0; shapeIndex < customShapeCount; ++shapeIndex)
        {
            const CustomShapeDrawCommand& shape = customShapes[shapeIndex];
            const bool skipSolidFill = shape.textured && canDrawTexturedShapes;

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

            if (!skipSolidFill)
            {
                for (int side = 0; side < sides; ++side)
                {
                    const int next = (side + 1) % sides;
                    writeCustomShapeTriangle(customShapeVertexStart,
                                             customShapeVertexCount,
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

                const UINT fillStart = customShapeVertexStart;
                const UINT fillCount = customShapeVertexCount;
                customShapeDrawBatches.push_back({fillStart, fillCount, shape.additive, true});
                customShapeVertexStart += customShapeVertexCount;
                customShapeVertexCount = 0;
            }

            if (shape.borderA > 0.001f)
            {
                const int passes = shape.thickBorder ? 4 : 1;
                for (int pass = 0; pass < passes; ++pass)
                {
                    const float offsetX = (pass == 1 || pass == 2) ? pixelX : 0.0f;
                    const float offsetY = (pass == 2 || pass == 3) ? pixelY : 0.0f;
                    for (int side = 0; side < sides; ++side)
                    {
                        const int next = (side + 1) % sides;
                        writeCustomShapeLine(customShapeVertexStart,
                                             customShapeVertexCount,
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
                const UINT borderStart = customShapeVertexStart;
                const UINT borderCount = customShapeVertexCount;
                customShapeDrawBatches.push_back({borderStart, borderCount, shape.additive, false});
                customShapeVertexStart += customShapeVertexCount;
                customShapeVertexCount = 0;
            }
        }
    }

    UINT overlayVertexStart = customShapeVertexStart + customShapeVertexCount;
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
        float fractionalColumns = motionVectorX - static_cast<float>(vectorColumns);
        float fractionalRows = motionVectorY - static_cast<float>(vectorRows);
        fractionalColumns = std::clamp(fractionalColumns, 0.0f, 1.0f);
        fractionalRows = std::clamp(fractionalRows, 0.0f, 1.0f);

        const float offsetX = motionVectorDX;
        const float offsetY = motionVectorDY;
        const float lengthMultiplier = std::clamp(motionVectorLength, 0.0f, 10.0f);
        const float minimumLength = pixelX * 0.5f;
        const float vectorR = std::clamp(motionVectorR, 0.0f, 1.0f);
        const float vectorG = std::clamp(motionVectorG, 0.0f, 1.0f);
        const float vectorB = std::clamp(motionVectorB, 0.0f, 1.0f);
        const float vectorA = std::clamp(motionVectorA, 0.0f, 1.0f);

        auto reversePropagatePoint = [&](float fx, float fy, float& fx2, float& fy2) {
            if (!hasTextureWarpGrid)
            {
                return false;
            }

            const int x0 = static_cast<int>(fx * static_cast<float>(textureWarpGridX));
            const int y0 = static_cast<int>(fy * static_cast<float>(textureWarpGridY));
            const float localX = fx * static_cast<float>(textureWarpGridX) - static_cast<float>(x0);
            const float localY = fy * static_cast<float>(textureWarpGridY) - static_cast<float>(y0);

            if (x0 < 0 || y0 < 0 || x0 >= textureWarpGridX || y0 >= textureWarpGridY)
            {
                return false;
            }

            const size_t cellBase = (static_cast<size_t>(y0) * static_cast<size_t>(textureWarpGridX) + static_cast<size_t>(x0)) * 6;
            if (cellBase + 5 >= textureWarpVertexCount)
            {
                return false;
            }

            const TextureWarpVertex& topLeft = textureWarpVertices[cellBase + 0];
            const TextureWarpVertex& topRight = textureWarpVertices[cellBase + 1];
            const TextureWarpVertex& bottomLeft = textureWarpVertices[cellBase + 2];
            const TextureWarpVertex& bottomRight = textureWarpVertices[cellBase + 5];

            const float topU = topLeft.u * (1.0f - localX) + topRight.u * localX;
            const float bottomU = bottomLeft.u * (1.0f - localX) + bottomRight.u * localX;
            const float topV = topLeft.v * (1.0f - localX) + topRight.v * localX;
            const float bottomV = bottomLeft.v * (1.0f - localX) + bottomRight.v * localX;

            fx2 = topU * (1.0f - localY) + bottomU * localY;
            const float tv = topV * (1.0f - localY) + bottomV * localY;
            fy2 = 1.0f - tv;
            return std::isfinite(fx2) && std::isfinite(fy2);
        };

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
            const float fyDenom = static_cast<float>(vectorRows) + fractionalRows + 0.25f - 1.0f;
            const float fxDenom = static_cast<float>(vectorColumns) + fractionalColumns + 0.25f - 1.0f;
            if (fabsf(fyDenom) < 0.0001f || fabsf(fxDenom) < 0.0001f)
            {
                vectorColumns = 0;
                vectorRows = 0;
            }
        }

        if (vectorColumns > 0 && vectorRows > 0)
        {
            for (int y = 0; y < vectorRows; ++y)
            {
                const float fyDenom = static_cast<float>(vectorRows) + fractionalRows + 0.25f - 1.0f;
                float fy = (static_cast<float>(y) + 0.25f) / fyDenom;
                fy -= offsetY;
                if (fy <= 0.0001f || fy >= 0.9999f)
                {
                    continue;
                }

                for (int x = 0; x < vectorColumns; ++x)
                {
                    const float fxDenom = static_cast<float>(vectorColumns) + fractionalColumns + 0.25f - 1.0f;
                    float fx = (static_cast<float>(x) + 0.25f) / fxDenom;
                    fx += offsetX;
                    if (fx <= 0.0001f || fx >= 0.9999f)
                    {
                        continue;
                    }

                    float fx2 = fx;
                    float fy2 = fy;
                    if (!reversePropagatePoint(fx, fy, fx2, fy2))
                    {
                        fx2 = fx + minimumLength;
                        fy2 = fy + minimumLength;
                    }

                    float endpointDX = (fx2 - fx) * lengthMultiplier;
                    float endpointDY = (fy2 - fy) * lengthMultiplier;
                    const float length = sqrtf(endpointDX * endpointDX + endpointDY * endpointDY);
                    if (length < minimumLength)
                    {
                        if (length > 0.00000001f)
                        {
                            const float boost = minimumLength / length;
                            endpointDX *= boost;
                            endpointDY *= boost;
                        }
                        else
                        {
                            endpointDX = minimumLength;
                            endpointDY = minimumLength;
                        }
                    }

                    fx2 = fx + endpointDX;
                    fy2 = fy + endpointDY;

                    const float startX = fx * 2.0f - 1.0f;
                    const float startY = 1.0f - fy * 2.0f;
                    const float endX = fx2 * 2.0f - 1.0f;
                    const float endY = 1.0f - fy2 * 2.0f;

                    const UINT before = motionVectorVertexCount;
                    if (!writeMotionVectorVertex(startX, startY) ||
                        !writeMotionVectorVertex(endX, endY))
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
        const bool hasVideoEcho = echoAlpha > 0.001f;
        const float baseFeedbackAlpha = hasVideoEcho ? 1.0f - echoAlpha : 1.0f;
        const float echoFeedbackAlpha = echoAlpha;
        const float echoFeedbackZoomBias = std::clamp(1.0f / echoZoom, 0.20f, 2.0f);
        const bool echoFlipU = (echoOrientation % 2) != 0;
        const bool echoFlipV = echoOrientation >= 2;

        if (hasTextureWarpMesh && baseFeedbackAlpha > 0.001f)
        {
            DrawTextureMeshFromSrv(feedbackSrv, m_feedbackSrvHeap.Get(), textureWarpVertices, textureWarpVertexCount, bass, mids, treble, decay, baseFeedbackAlpha, true);
        }
        else if (!hasTextureWarpMesh && baseFeedbackAlpha > 0.001f)
        {
            DrawTextureQuadFromSrv(feedbackSrv, m_feedbackSrvHeap.Get(), bass, mids, treble, decay, zoom, rot, baseFeedbackAlpha, 1.0f, 0.0f, false, false, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, true);
        }

        if (hasVideoEcho && echoFeedbackAlpha > 0.001f)
        {
            DrawTextureQuadFromSrv(feedbackSrv, m_feedbackSrvHeap.Get(), bass, mids, treble, decay, zoom, rot, echoFeedbackAlpha, echoFeedbackZoomBias, 0.04f, echoFlipU, echoFlipV, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, true, true);
        }
    }
    else if (m_resumeFeedbackReady && m_feedbackSrvHeap &&
             IsResumeFeedbackCompatible(static_cast<UINT>(std::max<long>(m_outputSize.right - m_outputSize.left, 1)),
                                        static_cast<UINT>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1))))
    {
        auto resumeSrv = m_feedbackSrvHeap->GetGPUDescriptorHandleForHeapStart();
        resumeSrv.ptr += static_cast<SIZE_T>(2) * m_feedbackSrvDescriptorSize;
        DrawTextureQuadFromSrv(resumeSrv, m_feedbackSrvHeap.Get(), bass, mids, treble, decay, zoom, rot, 1.0f, 1.0f, 0.0f, false, false, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, true);
    }

    if (hasTextureWarpMesh && m_srvHeap && m_texturePipelineState && m_textureRootSignature)
    {
        for (UINT layer = 0; layer < m_activeTextureLayerCount && layer < c_maxTextureLayers; ++layer)
        {
            if (!m_textureSlots[layer].texture)
            {
                continue;
            }

            auto textureSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            textureSrv.ptr += static_cast<SIZE_T>(layer) * m_srvDescriptorSize;
            const float layerAlpha = layer == 0 ? 1.0f : std::clamp(0.34f / static_cast<float>(layer + 1), 0.08f, 0.34f);
            DrawTextureMeshFromSrv(textureSrv, m_srvHeap.Get(), textureWarpVertices, textureWarpVertexCount, bass, mids, treble, decay, layerAlpha, false);
        }
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

    if (!customShapeDrawBatches.empty())
    {
        ID3D12PipelineState* activeCustomShapePipelineState = nullptr;
        D3D12_PRIMITIVE_TOPOLOGY activeCustomShapeTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);

        for (const ShapeDrawBatch& draw : customShapeDrawBatches)
        {
            if (draw.count == 0)
            {
                continue;
            }

            ID3D12PipelineState* nextPipelineState = draw.additive ?
                (draw.triangleList ? m_solidAdditivePipelineState.Get() : m_waveformAdditivePipelineState.Get()) :
                (draw.triangleList ? m_solidPipelineState.Get() : m_waveformPipelineState.Get());
            const D3D12_PRIMITIVE_TOPOLOGY nextTopology = draw.triangleList ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D_PRIMITIVE_TOPOLOGY_LINELIST;

            if (activeCustomShapePipelineState != nextPipelineState)
            {
                m_commandList->SetPipelineState(nextPipelineState);
                activeCustomShapePipelineState = nextPipelineState;
            }
            if (activeCustomShapeTopology != nextTopology)
            {
                m_commandList->IASetPrimitiveTopology(nextTopology);
                activeCustomShapeTopology = nextTopology;
            }

            m_commandList->DrawInstanced(draw.count, 1, draw.start, 0);
        }
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
            const D3D12_PRIMITIVE_TOPOLOGY nextTopology = draw.topology;

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
    const UINT waveformPassCount = ((waveThick || waveUseDots) && viewport.Width >= 512.0f) ? 4u : 1u;
    if (waveUseDots)
    {
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        for (UINT pass = 0; pass < waveformPassCount; ++pass)
        {
            for (UINT segment = 0; segment < visibleSegments; ++segment)
            {
                m_commandList->DrawInstanced(activeCount, 1, pass * drawGroupCount + segment * activeCount, 0);
            }
        }
    }
    else
    {
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
        for (UINT pass = 0; pass < waveformPassCount; ++pass)
        {
            for (UINT segment = 0; segment < visibleSegments; ++segment)
            {
                m_commandList->DrawInstanced(activeCount, 1, pass * drawGroupCount + segment * activeCount, 0);
            }
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
                               postShaderAmount,
                               postHueShaderColors,
                               activePostBlurAmount,
                               postBlurEdgeDarken);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_commandList->ResourceBarrier(1, &barrier);
    }

    CopyBackBufferToFeedback(m_feedbackIndex);

    UINT textOverlayVertexStart = customWaveVertexStart + customWaveVertexCopiedCount;
    UINT textOverlayVertexCount = 0;
    const float outputWidth = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const float outputHeight = static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    const float textReferenceSize = std::min(outputWidth, outputHeight);
    const float textScale = static_cast<float>(std::clamp(static_cast<int>(floorf(textReferenceSize / 180.0f + 0.5f)), 2, 8));
    const float textCell = textScale;
    const float glyphAdvance = 6.0f * textCell;
    const float glyphHeight = 7.0f * textCell;
    const float textMargin = std::clamp(textCell * 2.0f, 4.0f, 20.0f);
    const float textShadowOffset = std::max(1.0f, floorf(textCell * 0.33f + 0.5f));

    auto writeTextVertex = [&](float pixelXPos, float pixelYPos, float r, float g, float b, float a) {
        if (textOverlayVertexStart + textOverlayVertexCount >= c_maxWaveformVertices)
        {
            return false;
        }

        WaveformVertex& vertex = m_mappedWaveformVertices[textOverlayVertexStart + textOverlayVertexCount++];
        vertex.position[0] = pixelXPos / outputWidth * 2.0f - 1.0f;
        vertex.position[1] = 1.0f - pixelYPos / outputHeight * 2.0f;
        vertex.color[0] = r;
        vertex.color[1] = g;
        vertex.color[2] = b;
        vertex.color[3] = a;
        return true;
    };

    auto writeTextQuad = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
        const UINT before = textOverlayVertexCount;
        if (!writeTextVertex(x0, y0, r, g, b, a) ||
            !writeTextVertex(x1, y0, r, g, b, a) ||
            !writeTextVertex(x0, y1, r, g, b, a) ||
            !writeTextVertex(x0, y1, r, g, b, a) ||
            !writeTextVertex(x1, y0, r, g, b, a) ||
            !writeTextVertex(x1, y1, r, g, b, a))
        {
            textOverlayVertexCount = before;
        }
    };

    auto appendTextLineScaled = [&](const std::wstring& text,
                                    float x,
                                    float y,
                                    float maxWidth,
                                    float cell,
                                    float shadowOffset,
                                    float r,
                                    float g,
                                    float b,
                                    float a) {
        const float advance = 6.0f * cell;
        if (text.empty() || maxWidth <= advance || cell <= 0.0f || a <= 0.001f)
        {
            return;
        }

        const size_t maxChars = static_cast<size_t>(std::max<float>(1.0f, floorf(maxWidth / advance)));
        const size_t charCount = std::min(text.size(), maxChars);
        for (size_t charIndex = 0; charIndex < charCount; ++charIndex)
        {
            const auto rows = GlyphRows(text[charIndex]);
            const float charX = x + static_cast<float>(charIndex) * advance;
            for (size_t row = 0; row < rows.size(); ++row)
            {
                for (int col = 0; col < 5; ++col)
                {
                    if ((rows[row] & (1 << (4 - col))) == 0)
                    {
                        continue;
                    }

                    const float px = charX + static_cast<float>(col) * cell;
                    const float py = y + static_cast<float>(row) * cell;
                    writeTextQuad(px + shadowOffset,
                                  py + shadowOffset,
                                  px + cell + shadowOffset,
                                  py + cell + shadowOffset,
                                  0.0f,
                                  0.0f,
                                  0.0f,
                                  a * 0.55f);
                    writeTextQuad(px, py, px + cell, py + cell, r, g, b, a);
                }
            }
        }
    };
    auto appendTextLine = [&](const std::wstring& text, float x, float y, float maxWidth, float r, float g, float b, float a) {
        appendTextLineScaled(text, x, y, maxWidth, textCell, textShadowOffset, r, g, b, a);
    };

    if (!m_overlayTopLeft.empty())
    {
        const float maxWidth = m_overlayTopRight.empty() ? outputWidth - textMargin * 2.0f : outputWidth * 0.62f;
        appendTextLine(m_overlayTopLeft, textMargin, textMargin, maxWidth, 0.92f, 0.96f, 1.0f, 0.88f);
    }
    if (!m_overlayDebugLine.empty())
    {
        appendTextLine(m_overlayDebugLine, textMargin, textMargin + glyphHeight + textCell * 2.0f, outputWidth - textMargin * 2.0f, 0.72f, 0.92f, 1.0f, 0.82f);
    }
    if (!m_overlayTopRight.empty())
    {
        const float maxWidth = outputWidth * 0.35f;
        const size_t chars = std::min(m_overlayTopRight.size(), static_cast<size_t>(std::max<float>(1.0f, floorf(maxWidth / glyphAdvance))));
        const float textWidth = static_cast<float>(chars) * glyphAdvance;
        appendTextLine(m_overlayTopRight, std::max(textMargin, outputWidth - textWidth - textMargin), textMargin, maxWidth, 0.86f, 1.0f, 0.72f, 0.88f);
    }
    if (!m_overlayBottomLeft.empty())
    {
        appendTextLine(m_overlayBottomLeft, textMargin, std::max(textMargin, outputHeight - glyphHeight - textMargin), outputWidth - textMargin * 2.0f, 0.95f, 0.92f, 0.78f, 0.86f);
    }
    if (!m_overlayCenterText.empty() && m_overlayCenterA > 0.001f)
    {
        const float centerCell = std::clamp(textCell * m_overlayCenterScale, 1.0f, std::max(1.0f, textReferenceSize / 10.0f));
        const float centerAdvance = 6.0f * centerCell;
        const float maxWidth = outputWidth * 0.90f;
        const size_t chars = std::min(m_overlayCenterText.size(), static_cast<size_t>(std::max<float>(1.0f, floorf(maxWidth / centerAdvance))));
        const float textWidth = static_cast<float>(chars) * centerAdvance;
        const float centerHeight = 7.0f * centerCell;
        const float x = std::clamp(m_overlayCenterX * outputWidth - textWidth * 0.5f, textMargin, std::max(textMargin, outputWidth - textWidth - textMargin));
        const float y = std::clamp(m_overlayCenterY * outputHeight - centerHeight * 0.5f, textMargin, std::max(textMargin, outputHeight - centerHeight - textMargin));
        appendTextLineScaled(m_overlayCenterText,
                             x,
                             y,
                             maxWidth,
                             centerCell,
                             std::max(1.0f, floorf(centerCell * 0.18f + 0.5f)),
                             m_overlayCenterR,
                             m_overlayCenterG,
                             m_overlayCenterB,
                             m_overlayCenterA);
    }

    if (textOverlayVertexCount > 0)
    {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_commandList->ResourceBarrier(1, &barrier);

        m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        m_commandList->SetPipelineState(m_solidPipelineState.Get());
        m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->DrawInstanced(textOverlayVertexCount, 1, textOverlayVertexStart, 0);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_commandList->ResourceBarrier(1, &barrier);
    }
    else
    {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_commandList->ResourceBarrier(1, &barrier);
    }

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

bool D3D12Resources::WaitForGpu(DWORD timeoutMs)
{
    if (!m_commandQueue || !m_fence || !m_fenceEvent)
    {
        return true;
    }

    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]));
    ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
    const DWORD waitResult = WaitForSingleObjectEx(m_fenceEvent, timeoutMs, FALSE);
    if (waitResult != WAIT_OBJECT_0)
    {
        return false;
    }
    ++m_fenceValues[m_frameIndex];
    return true;
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
    srvHeapDesc.NumDescriptors = c_maxTextureLayers;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(m_srvHeap.ReleaseAndGetAddressOf())));
    m_srvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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

    const TextureVertex vertices[] = {
        {{-1.0f, 1.0f}, {0.0f, 0.0f}, {0.55f, 0.55f, 0.55f, 0.32f}},
        {{1.0f, 1.0f}, {1.0f, 0.0f}, {0.55f, 0.55f, 0.55f, 0.32f}},
        {{-1.0f, -1.0f}, {0.0f, 1.0f}, {0.55f, 0.55f, 0.55f, 0.32f}},
        {{1.0f, -1.0f}, {1.0f, 1.0f}, {0.55f, 0.55f, 0.55f, 0.32f}},
    };

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

    D3D12_RANGE readRange{0, 0};
    for (UINT i = 0; i < m_backBufferCount; ++i)
    {
        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                           D3D12_HEAP_FLAG_NONE,
                                                           &bufferDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                                           nullptr,
                                                           IID_PPV_ARGS(m_textureVertexBuffers[i].ReleaseAndGetAddressOf())));
        ThrowIfFailed(m_textureVertexBuffers[i]->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedTextureVertexBuffers[i])));
        memcpy(m_mappedTextureVertexBuffers[i], vertices, sizeof(vertices));

        m_textureVertexBufferViews[i].BufferLocation = m_textureVertexBuffers[i]->GetGPUVirtualAddress();
        m_textureVertexBufferViews[i].StrideInBytes = sizeof(TextureVertex);
        m_textureVertexBufferViews[i].SizeInBytes = sizeof(TextureVertex) * c_maxTextureVertices;
    }

    m_mappedTextureVertices = m_mappedTextureVertexBuffers[m_frameIndex];
    m_textureVertexBufferView = m_textureVertexBufferViews[m_frameIndex];
}

bool D3D12Resources::LoadTextureFromFile(const wchar_t* textureFile, UINT slotIndex)
{
    if (!textureFile || !*textureFile || !m_srvHeap || slotIndex >= c_maxTextureLayers)
    {
        return false;
    }

    TextureSlot& slot = m_textureSlots[slotIndex];
    if (!_wcsicmp(slot.file.c_str(), textureFile) && slot.texture)
    {
        return true;
    }

    const wchar_t* ext = wcsrchr(textureFile, L'.');
    bool loaded = false;
    if (ext && !_wcsicmp(ext, L".dds"))
    {
        loaded = LoadTextureFromDds(textureFile, slotIndex);
    }
    else if (ext && !_wcsicmp(ext, L".tga"))
    {
        loaded = LoadTextureFromTga(textureFile, slotIndex);
    }
    else
    {
        loaded = LoadTextureFromWic(textureFile, slotIndex);
    }
    if (loaded)
    {
        slot.file = textureFile;
        if (slotIndex == 0)
        {
            m_currentTextureFile = textureFile;
        }
        OutputDebugStringW((std::wstring(L"foo_vis_milk2 DX12 texture loaded: ") + textureFile + L"\n").c_str());
    }
    return loaded;
}

bool D3D12Resources::LoadTextureFromWic(const wchar_t* textureFile, UINT slotIndex)
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

    return UploadTextureRGBA(width, height, pixels, slotIndex);
}

bool D3D12Resources::LoadTextureFromTga(const wchar_t* textureFile, UINT slotIndex)
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

    return UploadTextureRGBA(width, height, pixels, slotIndex);
}

bool D3D12Resources::LoadTextureFromDds(const wchar_t* textureFile, UINT slotIndex)
{
    std::ifstream file(textureFile, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return false;
    }

    const std::streamoff fileSize = file.tellg();
    if (fileSize < 128)
    {
        return false;
    }

    std::vector<uint8_t> data(static_cast<size_t>(fileSize));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    if (!file)
    {
        return false;
    }

    if (ReadLe32(data.data()) != MakeFourCC('D', 'D', 'S', ' ') || ReadLe32(data.data() + 4) != 124)
    {
        return false;
    }

    const UINT height = ReadLe32(data.data() + 12);
    const UINT width = ReadLe32(data.data() + 16);
    const uint8_t* pixelFormat = data.data() + 76;
    if (width == 0 || height == 0 || ReadLe32(pixelFormat) != 32)
    {
        return false;
    }

    static constexpr uint32_t ddsFourCC = 0x4;
    static constexpr uint32_t ddsRgb = 0x40;
    static constexpr uint32_t ddsAlphaPixels = 0x1;

    const uint32_t pixelFormatFlags = ReadLe32(pixelFormat + 4);
    const uint32_t fourCC = ReadLe32(pixelFormat + 8);
    const uint32_t rgbBitCount = ReadLe32(pixelFormat + 12);
    const uint32_t rMask = ReadLe32(pixelFormat + 16);
    const uint32_t gMask = ReadLe32(pixelFormat + 20);
    const uint32_t bMask = ReadLe32(pixelFormat + 24);
    const uint32_t aMask = ReadLe32(pixelFormat + 28);
    const uint8_t* source = data.data() + 128;
    const size_t sourceSize = data.size() - 128;

    if (pixelFormatFlags & ddsFourCC)
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT bytesPerBlock = 0;
        switch (fourCC)
        {
            case MakeFourCC('D', 'X', 'T', '1'):
                format = DXGI_FORMAT_BC1_UNORM;
                bytesPerBlock = 8;
                break;
            case MakeFourCC('D', 'X', 'T', '3'):
                format = DXGI_FORMAT_BC2_UNORM;
                bytesPerBlock = 16;
                break;
            case MakeFourCC('D', 'X', 'T', '5'):
                format = DXGI_FORMAT_BC3_UNORM;
                bytesPerBlock = 16;
                break;
            default:
                return false;
        }

        const UINT blockColumns = std::max<UINT>(1, (width + 3) / 4);
        const UINT blockRows = std::max<UINT>(1, (height + 3) / 4);
        const UINT sourceRowPitch = blockColumns * bytesPerBlock;
        const size_t requiredSize = static_cast<size_t>(sourceRowPitch) * blockRows;
        if (sourceSize < requiredSize)
        {
            return false;
        }

        return UploadTextureData(width, height, format, source, requiredSize, sourceRowPitch, blockRows, slotIndex);
    }

    if ((pixelFormatFlags & ddsRgb) == 0 ||
        rMask != 0x00ff0000 ||
        gMask != 0x0000ff00 ||
        bMask != 0x000000ff ||
        ((pixelFormatFlags & ddsAlphaPixels) && aMask != 0xff000000))
    {
        return false;
    }

    const UINT sourceBytesPerPixel = rgbBitCount / 8;
    if (sourceBytesPerPixel != 3 && sourceBytesPerPixel != 4)
    {
        return false;
    }

    const UINT sourceRowPitch = width * sourceBytesPerPixel;
    const size_t requiredSize = static_cast<size_t>(sourceRowPitch) * height;
    if (sourceSize < requiredSize)
    {
        return false;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y)
    {
        const uint8_t* srcRow = source + static_cast<size_t>(y) * sourceRowPitch;
        uint8_t* dstRow = pixels.data() + static_cast<size_t>(y) * width * 4;
        for (UINT x = 0; x < width; ++x)
        {
            const uint8_t* src = srcRow + static_cast<size_t>(x) * sourceBytesPerPixel;
            uint8_t* dst = dstRow + static_cast<size_t>(x) * 4;
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = sourceBytesPerPixel == 4 ? src[3] : 255;
        }
    }

    return UploadTextureRGBA(width, height, pixels, slotIndex);
}

bool D3D12Resources::UploadTextureRGBA(UINT width, UINT height, const std::vector<uint8_t>& pixels, UINT slotIndex)
{
    if (width == 0 || height == 0 || pixels.size() < static_cast<size_t>(width) * height * 4 || slotIndex >= c_maxTextureLayers)
    {
        return false;
    }

    return UploadTextureData(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.data(), pixels.size(), width * 4, height, slotIndex);
}

bool D3D12Resources::UploadTextureData(UINT width,
                                       UINT height,
                                       DXGI_FORMAT format,
                                       const uint8_t* pixels,
                                       size_t pixelsSize,
                                       UINT sourceRowPitch,
                                       UINT sourceRowCount,
                                       UINT slotIndex)
{
    if (width == 0 || height == 0 || !pixels || pixelsSize == 0 || sourceRowPitch == 0 || sourceRowCount == 0 || slotIndex >= c_maxTextureLayers)
    {
        return false;
    }

    TextureSlot& slot = m_textureSlots[slotIndex];

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&defaultHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &textureDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       IID_PPV_ARGS(slot.texture.ReleaseAndGetAddressOf())));

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
                                                       IID_PPV_ARGS(slot.uploadBuffer.ReleaseAndGetAddressOf())));

    uint8_t* mapped = nullptr;
    ThrowIfFailed(slot.uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    if (pixelsSize < static_cast<size_t>(sourceRowPitch) * sourceRowCount)
    {
        slot.uploadBuffer->Unmap(0, nullptr);
        return false;
    }
    const size_t copyRowPitch = std::min<size_t>(sourceRowPitch, layout.Footprint.RowPitch);
    for (UINT row = 0; row < sourceRowCount; ++row)
    {
        memcpy(mapped + layout.Offset + static_cast<size_t>(row) * layout.Footprint.RowPitch, pixels + static_cast<size_t>(row) * sourceRowPitch, copyRowPitch);
    }
    slot.uploadBuffer->Unmap(0, nullptr);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = slot.texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = slot.uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = slot.texture.Get();
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
    auto srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(slotIndex) * m_srvDescriptorSize;
    m_d3dDevice->CreateShaderResourceView(slot.texture.Get(), &srvDesc, srvHandle);
    RefreshPostProcessTextureSrvs();
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
    float blurAmount;
    float texelX;
    float texelY;
    float blurEdgeDarken;
    float3 hueTopRight;
    float huePad0;
    float3 hueTopLeft;
    float huePad1;
    float3 hueBottomRight;
    float huePad2;
    float3 hueBottomLeft;
    float huePad3;
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
    float4 color : COLOR;
    float2 radAng : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    float2 uvFromCenter = input.uv - float2(0.5f, 0.5f);
    output.radAng = float2(length(uvFromCenter), atan2(uvFromCenter.y, uvFromCenter.x));
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 sampleColor = sourceTex.Sample(samp0, input.uv);
    float3 color = max(sampleColor.rgb, 0.0f);

    if (blurAmount > 0.001f)
    {
        float2 texel = float2(texelX, texelY);
        float3 blur =
            color * 0.36f +
            sourceTex.Sample(samp0, input.uv + float2(texel.x, 0.0f)).rgb * 0.12f +
            sourceTex.Sample(samp0, input.uv - float2(texel.x, 0.0f)).rgb * 0.12f +
            sourceTex.Sample(samp0, input.uv + float2(0.0f, texel.y)).rgb * 0.12f +
            sourceTex.Sample(samp0, input.uv - float2(0.0f, texel.y)).rgb * 0.12f +
            sourceTex.Sample(samp0, input.uv + texel).rgb * 0.04f +
            sourceTex.Sample(samp0, input.uv - texel).rgb * 0.04f +
            sourceTex.Sample(samp0, input.uv + float2(texel.x, -texel.y)).rgb * 0.04f +
            sourceTex.Sample(samp0, input.uv + float2(-texel.x, texel.y)).rgb * 0.04f;
        color = lerp(color, blur, saturate(blurAmount));

        float edge = max(abs(input.uv.x * 2.0f - 1.0f), abs(input.uv.y * 2.0f - 1.0f));
        float edgeFade = saturate((edge - 0.65f) / 0.35f);
        color *= 1.0f - edgeFade * saturate(blurEdgeDarken) * saturate(blurAmount);
    }

    color *= gamma;

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
        float x = saturate(input.uv.x);
        float y = saturate(1.0f - input.uv.y);
        float3 hue =
            hueTopRight * x * y +
            hueTopLeft * (1.0f - x) * y +
            hueBottomRight * x * (1.0f - y) +
            hueBottomLeft * (1.0f - x) * (1.0f - y);
        color *= lerp(float3(1.0f, 1.0f, 1.0f), hue, saturate(shaderAmount));
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
    m_postProcessVertexShaderBytecode.assign(static_cast<const uint8_t*>(vertexShader->GetBufferPointer()),
                                             static_cast<const uint8_t*>(vertexShader->GetBufferPointer()) + vertexShader->GetBufferSize());

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1 + c_maxTextureLayers;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[2]{};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = &range;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[1].Descriptor.ShaderRegister = 0;
    rootParameters[1].Descriptor.RegisterSpace = 0;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[1 + c_maxTextureLayers]{};
    for (UINT samplerIndex = 0; samplerIndex < static_cast<UINT>(std::size(samplers)); ++samplerIndex)
    {
        samplers[samplerIndex].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[samplerIndex].AddressU = samplerIndex == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[samplerIndex].AddressV = samplerIndex == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[samplerIndex].AddressW = samplerIndex == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[samplerIndex].ShaderRegister = samplerIndex;
        samplers[samplerIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = static_cast<UINT>(std::size(rootParameters));
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    rootSignatureDesc.pStaticSamplers = samplers;
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

    if (!m_presetCompositeShaderBytecode.empty())
    {
        CreatePresetCompositePipeline();
    }

    if (m_postProcessConstantBuffer && m_mappedPostProcessConstantBuffer)
    {
        m_postProcessConstantBuffer->Unmap(0, nullptr);
    }
    m_mappedPostProcessConstantBuffer = nullptr;
    m_postProcessConstantBuffer.Reset();

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC constantBufferDesc{};
    constantBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    constantBufferDesc.Width = c_postProcessConstantBufferSize;
    constantBufferDesc.Height = 1;
    constantBufferDesc.DepthOrArraySize = 1;
    constantBufferDesc.MipLevels = 1;
    constantBufferDesc.SampleDesc.Count = 1;
    constantBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&uploadHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &constantBufferDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_postProcessConstantBuffer.ReleaseAndGetAddressOf())));
    ThrowIfFailed(m_postProcessConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedPostProcessConstantBuffer)));
}

bool D3D12Resources::CreatePresetCompositePipeline()
{
    m_presetCompositePipelineState.Reset();

    if (!m_d3dDevice || !m_postProcessRootSignature || m_postProcessVertexShaderBytecode.empty() || m_presetCompositeShaderBytecode.empty())
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 2, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout = {inputElements, static_cast<UINT>(std::size(inputElements))};
    psoDesc.pRootSignature = m_postProcessRootSignature.Get();
    psoDesc.VS = {m_postProcessVertexShaderBytecode.data(), m_postProcessVertexShaderBytecode.size()};
    psoDesc.PS = {m_presetCompositeShaderBytecode.data(), m_presetCompositeShaderBytecode.size()};
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

    return SUCCEEDED(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_presetCompositePipelineState.ReleaseAndGetAddressOf())));
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
    srvHeapDesc.NumDescriptors = 1 + c_maxTextureLayers;
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
    RefreshPostProcessTextureSrvs();
}

void D3D12Resources::RefreshPostProcessTextureSrvs()
{
    if (!m_d3dDevice || !m_postProcessSrvHeap || m_srvDescriptorSize == 0)
    {
        return;
    }

    auto srvHandle = m_postProcessSrvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += m_srvDescriptorSize;

    for (UINT slot = 0; slot < c_maxTextureLayers; ++slot)
    {
        ID3D12Resource* texture = m_textureSlots[slot].texture.Get();
        if (!texture)
        {
            texture = m_postProcessTexture.Get();
        }
        if (!texture)
        {
            srvHandle.ptr += m_srvDescriptorSize;
            continue;
        }

        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_d3dDevice->CreateShaderResourceView(texture, &srvDesc, srvHandle);
        srvHandle.ptr += m_srvDescriptorSize;
    }
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

    if (!m_textureFiles.empty())
    {
        m_textureCycleIndex = static_cast<size_t>(GetTickCount64() % m_textureFiles.size());
    }
}

bool D3D12Resources::IsPostProcessEnabled() const
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_POSTPROCESS", value, static_cast<DWORD>(std::size(value))) > 0 && wcscmp(value, L"0") != 0;
}

bool D3D12Resources::IsPostProcessBlurEnabled() const
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_BLUR", value, static_cast<DWORD>(std::size(value))) > 0 && wcscmp(value, L"0") != 0;
}

float D3D12Resources::GetPostProcessBlurAmount() const
{
    wchar_t value[16]{};
    if (GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_BLUR_AMOUNT", value, static_cast<DWORD>(std::size(value))) > 0)
    {
        wchar_t* end = nullptr;
        const float parsed = wcstof(value, &end);
        if (end && *end == 0 && parsed >= 0.0f && parsed <= 1.0f)
        {
            return parsed;
        }
    }

    return 0.18f;
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
    if (m_presetTextureOverride || !IsTextureCyclingEnabled() || m_textureFiles.empty())
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
        if (SetTextureFile(textureFile.c_str()))
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
    srvHeapDesc.NumDescriptors = 3;
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

    if (m_resumeFeedbackReady && !IsResumeFeedbackCompatible(width, height))
    {
        ClearResumeFeedback();
    }

    if (m_resumeFeedbackReady && m_resumeFeedbackTexture)
    {
        D3D12_RESOURCE_DESC resumeDesc = m_resumeFeedbackTexture->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = resumeDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_d3dDevice->CreateShaderResourceView(m_resumeFeedbackTexture.Get(), &srvDesc, srvHandle);
    }
}

bool D3D12Resources::IsResumeFeedbackCompatible(UINT width, UINT height) const
{
    if (!m_resumeFeedbackReady || !m_resumeFeedbackTexture)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC desc = m_resumeFeedbackTexture->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc.Width == width &&
           desc.Height == height &&
           desc.Format == m_backBufferFormat;
}

void D3D12Resources::ClearResumeFeedback()
{
    m_resumeFeedbackReady = false;
    m_resumeFeedbackTexture.Reset();
}

bool D3D12Resources::CaptureBackBufferForResume()
{
    ClearResumeFeedback();

    if (!m_d3dDevice || !m_commandQueue || !m_fence || !m_renderTargets[m_frameIndex])
    {
        return false;
    }

    D3D12_RESOURCE_DESC sourceDesc = m_renderTargets[m_frameIndex]->GetDesc();
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &sourceDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_resumeFeedbackTexture.ReleaseAndGetAddressOf())));

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    D3D12_RESOURCE_BARRIER sourceBarrier{};
    sourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sourceBarrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    sourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    sourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    sourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &sourceBarrier);

    m_commandList->CopyResource(m_resumeFeedbackTexture.Get(), m_renderTargets[m_frameIndex].Get());

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0] = sourceBarrier;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = m_resumeFeedbackTexture.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(static_cast<UINT>(std::size(commandLists)), commandLists);
    if (!WaitForGpu(1000))
    {
        m_resumeFeedbackTexture.Reset();
        return false;
    }

    m_resumeFeedbackReady = true;
    return true;
}

bool D3D12Resources::CaptureCurrentFrame(std::vector<uint8_t>* pixels, UINT* width, UINT* height)
{
    if (!pixels || !width || !height)
    {
        return false;
    }

    pixels->clear();
    *width = 0;
    *height = 0;

    if (!m_d3dDevice || !m_commandQueue || !m_fence || !m_renderTargets[m_frameIndex])
    {
        return false;
    }

    if (!WaitForGpu(1000))
    {
        return false;
    }

    D3D12_RESOURCE_DESC sourceDesc = m_renderTargets[m_frameIndex]->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.Width == 0 || sourceDesc.Height == 0)
    {
        return false;
    }

    UINT64 readbackSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    m_d3dDevice->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &layout, &numRows, &rowSize, &readbackSize);
    if (readbackSize == 0 || numRows == 0 || rowSize == 0)
    {
        return false;
    }

    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = readbackSize;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    ComPtr<ID3D12Resource> readbackBuffer;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&readbackHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &readbackDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       IID_PPV_ARGS(readbackBuffer.GetAddressOf())));

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    D3D12_RESOURCE_BARRIER sourceBarrier{};
    sourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sourceBarrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    sourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    sourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    sourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &sourceBarrier);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readbackBuffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = m_renderTargets[m_frameIndex].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    sourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    sourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &sourceBarrier);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    if (!WaitForGpu(1000))
    {
        return false;
    }

    const UINT capturedWidth = static_cast<UINT>(sourceDesc.Width);
    const UINT capturedHeight = static_cast<UINT>(sourceDesc.Height);
    const UINT sourceRowPitch = static_cast<UINT>(rowSize);
    std::vector<uint8_t> captured(static_cast<size_t>(capturedHeight) * sourceRowPitch);

    const uint8_t* mapped = nullptr;
    D3D12_RANGE readRange{layout.Offset, layout.Offset + readbackSize};
    ThrowIfFailed(readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped))));
    for (UINT row = 0; row < capturedHeight; ++row)
    {
        memcpy(captured.data() + static_cast<size_t>(row) * sourceRowPitch,
               mapped + layout.Offset + static_cast<size_t>(row) * layout.Footprint.RowPitch,
               sourceRowPitch);
    }
    D3D12_RANGE writtenRange{0, 0};
    readbackBuffer->Unmap(0, &writtenRange);

    *width = capturedWidth;
    *height = capturedHeight;
    *pixels = std::move(captured);
    return true;
}

bool D3D12Resources::SetResumeFeedbackFromFrame(UINT width, UINT height, const std::vector<uint8_t>& pixels)
{
    ClearResumeFeedback();

    if (width == 0 || height == 0 || pixels.empty() || !m_d3dDevice || !m_commandQueue || !m_fence)
    {
        return false;
    }

    const UINT outputWidth = static_cast<UINT>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const UINT outputHeight = static_cast<UINT>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    if (width != outputWidth || height != outputHeight)
    {
        return false;
    }

    const UINT sourceRowPitch = width * 4;
    if (pixels.size() < static_cast<size_t>(sourceRowPitch) * height)
    {
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = m_backBufferFormat;
    textureDesc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&defaultHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &textureDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_resumeFeedbackTexture.ReleaseAndGetAddressOf())));

    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    m_d3dDevice->GetCopyableFootprints(&textureDesc, 0, 1, 0, &layout, &numRows, &rowSize, &uploadSize);
    if (uploadSize == 0 || numRows == 0)
    {
        m_resumeFeedbackTexture.Reset();
        return false;
    }

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
    ComPtr<ID3D12Resource> uploadBuffer;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&uploadHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &uploadDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(uploadBuffer.GetAddressOf())));

    uint8_t* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (UINT row = 0; row < height; ++row)
    {
        memcpy(mapped + layout.Offset + static_cast<size_t>(row) * layout.Footprint.RowPitch,
               pixels.data() + static_cast<size_t>(row) * sourceRowPitch,
               sourceRowPitch);
    }
    uploadBuffer->Unmap(0, nullptr);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = m_resumeFeedbackTexture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_resumeFeedbackTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    if (!WaitForGpu(1000))
    {
        m_resumeFeedbackTexture.Reset();
        return false;
    }

    if (m_feedbackSrvHeap)
    {
        auto srvHandle = m_feedbackSrvHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandle.ptr += static_cast<SIZE_T>(2) * m_feedbackSrvDescriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_d3dDevice->CreateShaderResourceView(m_resumeFeedbackTexture.Get(), &srvDesc, srvHandle);
    }

    m_resumeFeedbackReady = true;
    return true;
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
    m_resumeFeedbackReady = false;
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
    if (!m_textureFiles.empty())
    {
        return m_textureFiles[m_textureCycleIndex % m_textureFiles.size()];
    }

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
    if (!m_srvHeap || !m_texturePipelineState || !m_textureRootSignature)
    {
        return;
    }

    for (UINT layer = 0; layer < m_activeTextureLayerCount && layer < c_maxTextureLayers; ++layer)
    {
        if (!m_textureSlots[layer].texture)
        {
            continue;
        }

        auto srvHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        srvHandle.ptr += static_cast<SIZE_T>(layer) * m_srvDescriptorSize;
        const float layerAlpha = layer == 0 ? 1.0f : std::clamp(0.42f / static_cast<float>(layer + 1), 0.10f, 0.42f);
        const float layerZoom = 1.0f + static_cast<float>(layer) * 0.08f;
        const float layerAngle = static_cast<float>(layer) * 0.11f;
        DrawTextureQuadFromSrv(srvHandle, m_srvHeap.Get(), bass, mids, treble, decay, zoom, rot, layerAlpha, layerZoom, layerAngle, false, false, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, false);
    }
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
                                            float motionWarp,
                                            bool applyDecayTint,
                                            bool additive)
{
    ID3D12PipelineState* pipelineState = additive ? m_textureAdditivePipelineState.Get() : m_textureAlphaPipelineState.Get();
    if (!descriptorHeap || !pipelineState || !m_textureRootSignature || !m_mappedTextureVertices)
    {
        return;
    }

    if (m_textureVertexCursor + 4 > c_maxTextureVertices)
    {
        return;
    }

    const UINT vertexStart = m_textureVertexCursor;
    m_textureVertexCursor += 4;
    (void)bass;
    (void)mids;
    (void)treble;
    const float textureZoom = std::clamp((1.0f / std::max(zoom, 0.25f)) * zoomBias, 0.55f, 1.75f);
    const float angle = rot * 0.35f + angleBias;
    const float c = cosf(angle);
    const float s = sinf(angle);
    const float warpTime = static_cast<float>(GetTickCount64() % 600000ULL) * 0.001f;
    const float alpha = std::clamp(alphaScale, 0.0f, 1.0f);
    const float tint = applyDecayTint ? std::clamp(decay, 0.0f, 1.0f) : 1.0f;

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
        vertex.color[0] = tint;
        vertex.color[1] = tint;
        vertex.color[2] = tint;
        vertex.color[3] = alpha;
    };

    setVertex(0, -1.0f, 1.0f);
    setVertex(1, 1.0f, 1.0f);
    setVertex(2, -1.0f, -1.0f);
    setVertex(3, 1.0f, -1.0f);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetPipelineState(pipelineState);
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
    customShapeCount = std::min<size_t>(customShapeCount, 256);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetGraphicsRootSignature(m_textureRootSignature.Get());
    m_commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
    m_commandList->IASetVertexBuffers(0, 1, &m_textureVertexBufferView);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

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
        const UINT vertexCount = static_cast<UINT>(sides + 2);
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

        setVertex(writeIndex++, centerX, centerY, 0.5f, 0.5f, shape.r, shape.g, shape.b, shape.a);
        for (int side = 0; side < sides; ++side)
        {
            float x0 = 0.0f;
            float y0 = 0.0f;
            float u0 = 0.0f;
            float v0 = 0.0f;
            ringPoint(side, x0, y0, u0, v0);
            setVertex(writeIndex++, x0, y0, u0, v0, shape.r2, shape.g2, shape.b2, shape.a2);
        }
        setVertex(writeIndex++, m_mappedTextureVertices[vertexStart + 1].position[0],
                  m_mappedTextureVertices[vertexStart + 1].position[1],
                  m_mappedTextureVertices[vertexStart + 1].uv[0],
                  m_mappedTextureVertices[vertexStart + 1].uv[1],
                  shape.r2,
                  shape.g2,
                  shape.b2,
                  shape.a2);

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
                                            float decay,
                                            float alphaScale,
                                            bool applyDecayTint,
                                            bool additive)
{
    ID3D12PipelineState* pipelineState = additive ? m_textureAdditivePipelineState.Get() : m_textureAlphaPipelineState.Get();
    if (!descriptorHeap || !vertices || vertexCount < 3 || !pipelineState || !m_textureRootSignature || !m_mappedTextureVertices)
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

    (void)bass;
    (void)mids;
    (void)treble;
    const float alpha = std::clamp(alphaScale, 0.0f, 1.0f);
    const float tint = applyDecayTint ? std::clamp(decay, 0.0f, 1.0f) : 1.0f;

    for (UINT index = 0; index < copiedVertexCount; ++index)
    {
        const TextureWarpVertex& source = vertices[index];
        TextureVertex& vertex = m_mappedTextureVertices[vertexStart + index];
        vertex.position[0] = source.x;
        vertex.position[1] = source.y;
        vertex.uv[0] = source.u;
        vertex.uv[1] = source.v;
        vertex.color[0] = std::clamp(source.r * tint, 0.0f, 1.0f);
        vertex.color[1] = std::clamp(source.g * tint, 0.0f, 1.0f);
        vertex.color[2] = std::clamp(source.b * tint, 0.0f, 1.0f);
        vertex.color[3] = std::clamp(source.a * alpha, 0.0f, 1.0f);
    }

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetPipelineState(pipelineState);
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
                                            float shaderAmount,
                                            const float* hueShaderColors,
                                            float blurAmount,
                                            float blurEdgeDarken)
{
    if (!descriptorHeap ||
        !m_mappedTextureVertices ||
        !m_postProcessPipelineState ||
        !m_postProcessRootSignature ||
        !m_postProcessConstantBuffer ||
        !m_mappedPostProcessConstantBuffer)
    {
        return;
    }

    if (m_textureVertexCursor + 4 > c_maxTextureVertices)
    {
        return;
    }

    float hueColors[12] = {
        1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f,
    };
    if (hueShaderColors)
    {
        std::copy_n(hueShaderColors, std::size(hueColors), hueColors);
    }

    const UINT vertexStart = m_textureVertexCursor;
    m_textureVertexCursor += 4;
    const TextureVertex vertices[] = {
        {{-1.0f, 1.0f}, {0.0f, 0.0f}, {hueColors[3], hueColors[4], hueColors[5], 1.0f}},
        {{1.0f, 1.0f}, {1.0f, 0.0f}, {hueColors[0], hueColors[1], hueColors[2], 1.0f}},
        {{-1.0f, -1.0f}, {0.0f, 1.0f}, {hueColors[9], hueColors[10], hueColors[11], 1.0f}},
        {{1.0f, -1.0f}, {1.0f, 1.0f}, {hueColors[6], hueColors[7], hueColors[8], 1.0f}},
    };
    memcpy(m_mappedTextureVertices + vertexStart, vertices, sizeof(vertices));

    const float width = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const float height = static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));

    std::array<float, 512> constants{};
    const bool usePresetComposite = m_presetCompositePipelineState != nullptr;
    if (usePresetComposite)
    {
        auto write4 = [&](size_t offset, float x, float y, float z, float w) {
            constants[offset + 0] = x;
            constants[offset + 1] = y;
            constants[offset + 2] = z;
            constants[offset + 3] = w;
        };
        auto clean = [](float value, float fallback) {
            return std::isfinite(value) ? value : fallback;
        };

        const float safeWidth = std::max(width, 1.0f);
        const float safeHeight = std::max(height, 1.0f);
        const float aspectX = safeHeight > safeWidth ? safeWidth / safeHeight : 1.0f;
        const float aspectY = safeWidth > safeHeight ? safeHeight / safeWidth : 1.0f;
        const float bassAvg = (m_presetShaderBass + m_presetShaderMids + m_presetShaderTreble) * (1.0f / 3.0f);
        const float bassAttAvg = (m_presetShaderBassAtt + m_presetShaderMidsAtt + m_presetShaderTrebleAtt) * (1.0f / 3.0f);
        const float time = clean(m_presetShaderTime, 0.0f);

        write4(0, m_presetShaderRandFrame[0], m_presetShaderRandFrame[1], m_presetShaderRandFrame[2], m_presetShaderRandFrame[3]);
        write4(4, m_presetShaderRandPreset[0], m_presetShaderRandPreset[1], m_presetShaderRandPreset[2], m_presetShaderRandPreset[3]);
        write4(8, aspectX, aspectY, aspectX > 0.0001f ? 1.0f / aspectX : 1.0f, aspectY > 0.0001f ? 1.0f / aspectY : 1.0f);
        write4(12, 0.0f, 0.0f, 0.0f, 0.0f);
        write4(16, time, m_presetShaderFps, m_presetShaderFrame, m_presetShaderProgress);
        write4(20, m_presetShaderBass, m_presetShaderMids, m_presetShaderTreble, bassAvg);
        write4(24, m_presetShaderBassAtt, m_presetShaderMidsAtt, m_presetShaderTrebleAtt, bassAttAvg);
        write4(28,
               m_presetShaderBlurMax[0] - m_presetShaderBlurMin[0],
               m_presetShaderBlurMin[0],
               m_presetShaderBlurMax[1] - m_presetShaderBlurMin[1],
               m_presetShaderBlurMin[1]);
        write4(32,
               m_presetShaderBlurMax[2] - m_presetShaderBlurMin[2],
               m_presetShaderBlurMin[2],
               m_presetShaderBlurMin[0],
               m_presetShaderBlurMax[0]);
        write4(36, safeWidth, safeHeight, 1.0f / safeWidth, 1.0f / safeHeight);
        write4(40, 0.5f + 0.5f * cosf(time * 0.329f + 1.2f),
                   0.5f + 0.5f * cosf(time * 1.293f + 3.9f),
                   0.5f + 0.5f * cosf(time * 5.070f + 2.5f),
                   0.5f + 0.5f * cosf(time * 20.051f + 5.4f));
        write4(44, 0.5f + 0.5f * sinf(time * 0.329f + 1.2f),
                   0.5f + 0.5f * sinf(time * 1.293f + 3.9f),
                   0.5f + 0.5f * sinf(time * 5.070f + 2.5f),
                   0.5f + 0.5f * sinf(time * 20.051f + 5.4f));
        write4(48, 0.5f + 0.5f * cosf(time * 0.0050f + 2.7f),
                   0.5f + 0.5f * cosf(time * 0.0085f + 5.3f),
                   0.5f + 0.5f * cosf(time * 0.0133f + 4.5f),
                   0.5f + 0.5f * cosf(time * 0.0217f + 3.8f));
        write4(52, 0.5f + 0.5f * sinf(time * 0.0050f + 2.7f),
                   0.5f + 0.5f * sinf(time * 0.0085f + 5.3f),
                   0.5f + 0.5f * sinf(time * 0.0133f + 4.5f),
                   0.5f + 0.5f * sinf(time * 0.0217f + 3.8f));
        const float mipX = log2f(safeWidth);
        const float mipY = log2f(safeHeight);
        write4(56, mipX, mipY, (mipX + mipY) * 0.5f, 0.0f);
        write4(60, m_presetShaderBlurMin[1], m_presetShaderBlurMax[1], m_presetShaderBlurMin[2], m_presetShaderBlurMax[2]);
        std::copy_n(m_presetShaderQ, std::size(m_presetShaderQ), constants.data() + 64);
        std::copy_n(m_presetShaderRotMatrices, std::size(m_presetShaderRotMatrices), constants.data() + 96);
    }
    else
    {
        constants[0] = std::clamp(gamma, 0.0f, 8.0f);
        constants[1] = brighten ? 1.0f : 0.0f;
        constants[2] = darken ? 1.0f : 0.0f;
        constants[3] = solarize ? 1.0f : 0.0f;
        constants[4] = invert ? 1.0f : 0.0f;
        constants[5] = std::clamp(shaderAmount, 0.0f, 1.0f);
        constants[6] = std::clamp(blurAmount, 0.0f, 1.0f);
        constants[7] = 1.0f / width;
        constants[8] = 1.0f / height;
        constants[9] = std::clamp(blurEdgeDarken, 0.0f, 1.0f);
        constants[12] = hueColors[0];
        constants[13] = hueColors[1];
        constants[14] = hueColors[2];
        constants[16] = hueColors[3];
        constants[17] = hueColors[4];
        constants[18] = hueColors[5];
        constants[20] = hueColors[6];
        constants[21] = hueColors[7];
        constants[22] = hueColors[8];
        constants[24] = hueColors[9];
        constants[25] = hueColors[10];
        constants[26] = hueColors[11];
    }
    memcpy(m_mappedPostProcessConstantBuffer, constants.data(), sizeof(constants));

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetPipelineState(usePresetComposite ? m_presetCompositePipelineState.Get() : m_postProcessPipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_postProcessRootSignature.Get());
    m_commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
    m_commandList->SetGraphicsRootConstantBufferView(1, m_postProcessConstantBuffer->GetGPUVirtualAddress());
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

    D3D12_RANGE readRange{0, 0};
    for (UINT i = 0; i < m_backBufferCount; ++i)
    {
        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                           D3D12_HEAP_FLAG_NONE,
                                                           &bufferDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                                           nullptr,
                                                           IID_PPV_ARGS(m_waveformVertexBuffers[i].ReleaseAndGetAddressOf())));
        ThrowIfFailed(m_waveformVertexBuffers[i]->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedWaveformVertexBuffers[i])));

        m_waveformVertexBufferViews[i].BufferLocation = m_waveformVertexBuffers[i]->GetGPUVirtualAddress();
        m_waveformVertexBufferViews[i].StrideInBytes = sizeof(WaveformVertex);
        m_waveformVertexBufferViews[i].SizeInBytes = sizeof(WaveformVertex) * c_maxWaveformVertices;
    }

    m_mappedWaveformVertices = m_mappedWaveformVertexBuffers[m_frameIndex];
    m_waveformVertexBufferView = m_waveformVertexBufferViews[m_frameIndex];
}
} // namespace DX
