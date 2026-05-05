/*
 * d3d12resources.cpp - Minimal Direct3D 12 device and swap chain path.
 */

#include "pch.h"
#include "d3d12resources.h"
#include "deviceresources.h"

#include <wincodec.h>

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
                                  float rot)
{
    if (!left || !right || !m_mappedWaveformVertices || !m_waveformPipelineState || !m_waveformRootSignature)
    {
        Clear();
        return;
    }

    const UINT count = std::min<UINT>(static_cast<UINT>(sampleCount), c_maxWaveformVertices / 2);
    if (count < 2)
    {
        Clear();
        return;
    }

    const float energy = std::clamp((bass + mids + treble) / 6.0f, 0.0f, 1.0f);
    decay = std::clamp(decay, 0.70f, 1.0f);
    zoom = std::clamp(zoom, 0.5f, 2.0f);
    const float bgLift = (1.0f - decay) * 0.45f;
    const float bg[] = {bgLift + energy * 0.035f, bgLift * 0.65f + mids * 0.018f, bgLift * 0.40f + treble * 0.025f, 1.0f};
    const float centerX = std::clamp((waveX - 0.5f) * 1.6f, -0.75f, 0.75f);
    const float centerY = std::clamp((0.5f - waveY) * 1.3f, -0.65f, 0.65f);
    const float amp = std::clamp(waveScale, 0.10f, 5.0f) * 0.16f;
    const float c = cosf(rot);
    const float s = sinf(rot);

    for (UINT channel = 0; channel < 2; ++channel)
    {
        const float* source = channel == 0 ? left : right;
        const float laneY = channel == 0 ? 0.18f : -0.18f;
        const float r = std::clamp(waveR * (channel == 0 ? 1.15f : 0.75f) + bass * 0.08f, 0.0f, 1.0f);
        const float g = std::clamp(waveG * (channel == 0 ? 1.00f : 0.80f) + mids * 0.08f, 0.0f, 1.0f);
        const float b = std::clamp(waveB * (channel == 0 ? 0.90f : 1.15f) + treble * 0.08f, 0.0f, 1.0f);
        for (UINT i = 0; i < count; ++i)
        {
            const float localX = (-0.92f + (1.84f * static_cast<float>(i) / static_cast<float>(count - 1))) / zoom;
            const float localY = laneY + std::clamp(source[i], -1.0f, 1.0f) * amp;
            WaveformVertex& vertex = m_mappedWaveformVertices[channel * count + i];
            vertex.position[0] = centerX + localX * c - localY * s;
            vertex.position[1] = centerY + localX * s + localY * c;
            vertex.color[0] = r;
            vertex.color[1] = g;
            vertex.color[2] = b;
            vertex.color[3] = std::clamp(waveA, 0.05f, 1.0f);
        }
    }

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), m_waveformPipelineState.Get()));

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

    m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);
    DrawTextureQuad();

    m_commandList->SetPipelineState(m_waveformPipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
    m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
    m_commandList->DrawInstanced(count, 1, 0, 0);
    m_commandList->DrawInstanced(count, 1, count, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
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
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_backBufferFormat;
    psoDesc.SampleDesc.Count = 1;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_texturePipelineState.ReleaseAndGetAddressOf())));

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(TextureVertex) * 4;
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
    m_textureVertexBufferView.SizeInBytes = sizeof(vertices);
}

bool D3D12Resources::LoadTextureFromFile(const wchar_t* textureFile)
{
    if (!textureFile || !*textureFile || !m_srvHeap)
    {
        return false;
    }

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
    for (UINT row = 0; row < height; ++row)
    {
        memcpy(mapped + layout.Offset + static_cast<size_t>(row) * layout.Footprint.RowPitch, pixels.data() + static_cast<size_t>(row) * rowPitch, rowPitch);
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

std::wstring D3D12Resources::PickTextureFile() const
{
    static constexpr const wchar_t* masks[] = {L"*.jpg", L"*.jpeg", L"*.png", L"*.bmp", L"*.gif", L"*.jfif"};
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

void D3D12Resources::DrawTextureQuad()
{
    if (!m_texture || !m_srvHeap || !m_texturePipelineState || !m_textureRootSignature)
    {
        return;
    }

    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetPipelineState(m_texturePipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_textureRootSignature.Get());
    m_commandList->SetGraphicsRootDescriptorTable(0, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->IASetVertexBuffers(0, 1, &m_textureVertexBufferView);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_commandList->DrawInstanced(4, 1, 0, 0);
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
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_backBufferFormat;
    psoDesc.SampleDesc.Count = 1;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_waveformPipelineState.ReleaseAndGetAddressOf())));

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
