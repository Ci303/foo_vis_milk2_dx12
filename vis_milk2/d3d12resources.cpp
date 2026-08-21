/*
 * d3d12resources.cpp - Minimal Direct3D 12 device and swap chain path.
 */

#include "pch.h"
#include "d3d12resources.h"
#include "deviceresources.h"

#include <array>
#include <wincodec.h>
#include <fstream>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace DX
{
namespace
{
constexpr UINT c_postProcessConstantBufferSize = 4096;

void StoreTextureRadAng(float radAng[2], float u, float v) noexcept
{
    const float du = u - 0.5f;
    const float dv = v - 0.5f;
    radAng[0] = sqrtf(du * du + dv * dv);
    radAng[1] = atan2f(dv, du);
}

void StoreTextureRadAng(float radAng[2], float rad, float ang, float u, float v) noexcept
{
    if (std::isfinite(rad) && std::isfinite(ang))
    {
        radAng[0] = rad;
        radAng[1] = ang;
        return;
    }

    StoreTextureRadAng(radAng, u, v);
}

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

float NoiseCubicInterpolate(float y0, float y1, float y2, float y3, float t) noexcept
{
    const float t2 = t * t;
    const float a0 = y3 - y2 - y0 + y1;
    const float a1 = y0 - y1 - a0;
    const float a2 = y2 - y0;
    return a0 * t * t2 + a1 * t2 + a2 * t + y1;
}

uint32_t NoiseCubicInterpolateRgba(uint32_t y0, uint32_t y1, uint32_t y2, uint32_t y3, float t) noexcept
{
    uint32_t result = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8)
    {
        float value = NoiseCubicInterpolate(((y0 >> shift) & 0xffu) / 255.0f,
                                            ((y1 >> shift) & 0xffu) / 255.0f,
                                            ((y2 >> shift) & 0xffu) / 255.0f,
                                            ((y3 >> shift) & 0xffu) / 255.0f,
                                            t);
        value = std::clamp(value, 0.0f, 1.0f);
        result |= static_cast<uint32_t>(value * 255.0f) << shift;
    }
    return result;
}

uint32_t NextNoiseRandom(uint32_t& state) noexcept
{
    state += 0x9e3779b9u;
    uint32_t value = state;
    value = (value ^ (value >> 16)) * 0x7feb352du;
    value = (value ^ (value >> 15)) * 0x846ca68bu;
    return value ^ (value >> 16);
}

uint32_t MakeNoiseColor(uint32_t& state, UINT zoomFactor) noexcept
{
    const uint32_t range = zoomFactor > 1 ? 216u : 256u;
    uint32_t color = 0;
    color |= ((NextNoiseRandom(state) % range) + range / 2u) << 24;
    color |= ((NextNoiseRandom(state) % range) + range / 2u) << 16;
    color |= ((NextNoiseRandom(state) % range) + range / 2u) << 8;
    color |= ((NextNoiseRandom(state) % range) + range / 2u);
    return color;
}

bool IsD3D12TruthyLogValue(const wchar_t* value) noexcept
{
    return value && value[0] != L'\0' && wcscmp(value, L"0") != 0 && _wcsicmp(value, L"false") != 0;
}

bool ResolveD3D12LogPath(wchar_t* logPath, size_t logPathCount) noexcept
{
    if (!logPath || logPathCount == 0)
        return false;

    logPath[0] = L'\0';

    wchar_t logValue[MAX_PATH]{};
    const DWORD valueLength = GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_LOG", logValue, static_cast<DWORD>(std::size(logValue)));
    if (valueLength >= std::size(logValue))
        return false;
    if (valueLength > 0 && !IsD3D12TruthyLogValue(logValue))
        return false;

    const bool useDefaultPath =
        valueLength == 0 ||
        wcscmp(logValue, L"1") == 0 ||
        _wcsicmp(logValue, L"true") == 0;
    if (!useDefaultPath)
    {
        wcscpy_s(logPath, logPathCount, logValue);
        return true;
    }

    wchar_t devValue[8]{};
    const DWORD devValueLength = GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_DEV", devValue, static_cast<DWORD>(std::size(devValue)));
    if (valueLength == 0 && (devValueLength == 0 || !IsD3D12TruthyLogValue(devValue)))
        return false;

    wchar_t exePath[MAX_PATH]{};
    const DWORD exeLength = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (exeLength == 0 || exeLength >= std::size(exePath))
        return false;

    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash)
        return false;
    *(lastSlash + 1) = L'\0';

    wcscpy_s(logPath, logPathCount, exePath);
    wcscat_s(logPath, logPathCount, L"profile\\");
    CreateDirectoryW(logPath, nullptr);
    wcscat_s(logPath, logPathCount, L"foo_vis_milk2_dx12.log");
    return true;
}

void WriteD3D12LogLine(const wchar_t* message)
{
    wchar_t logPath[MAX_PATH]{};
    if (!ResolveD3D12LogPath(logPath, std::size(logPath)))
        return;

    HANDLE file = CreateFileW(logPath,
                              FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t wideLine[2048]{};
    swprintf_s(wideLine,
               L"%04u-%02u-%02u %02u:%02u:%02u.%03u d3d12resources %ls\r\n",
               now.wYear,
               now.wMonth,
               now.wDay,
               now.wHour,
               now.wMinute,
               now.wSecond,
               now.wMilliseconds,
               message ? message : L"");

    char utf8Line[4096]{};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wideLine, -1, utf8Line, static_cast<int>(std::size(utf8Line)), nullptr, nullptr);
    if (bytes > 1)
    {
        DWORD written = 0;
        WriteFile(file, utf8Line, static_cast<DWORD>(bytes - 1), &written, nullptr);
    }
    CloseHandle(file);
}

const wchar_t* ShaderInputTypeName(D3D_SHADER_INPUT_TYPE type) noexcept
{
    switch (type)
    {
        case D3D_SIT_CBUFFER:
            return L"cbuffer";
        case D3D_SIT_TBUFFER:
            return L"tbuffer";
        case D3D_SIT_TEXTURE:
            return L"texture";
        case D3D_SIT_SAMPLER:
            return L"sampler";
        case D3D_SIT_UAV_RWTYPED:
            return L"uav_rwtyped";
        case D3D_SIT_STRUCTURED:
            return L"structured";
        case D3D_SIT_UAV_RWSTRUCTURED:
            return L"uav_rwstructured";
        case D3D_SIT_BYTEADDRESS:
            return L"byteaddress";
        case D3D_SIT_UAV_RWBYTEADDRESS:
            return L"uav_rwbyteaddress";
        case D3D_SIT_UAV_APPEND_STRUCTURED:
            return L"uav_append";
        case D3D_SIT_UAV_CONSUME_STRUCTURED:
            return L"uav_consume";
        case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
            return L"uav_rwstructured_counter";
        default:
            return L"other";
    }
}

void LogShaderSignatureParameter(const wchar_t* label,
                                 const wchar_t* direction,
                                 UINT index,
                                 const D3D11_SIGNATURE_PARAMETER_DESC& parameter)
{
    wchar_t semantic[128]{};
    MultiByteToWideChar(CP_UTF8, 0, parameter.SemanticName ? parameter.SemanticName : "", -1, semantic, static_cast<int>(std::size(semantic)));

    wchar_t line[512]{};
    swprintf_s(line,
               L"%ls %ls%u sem=%ls%u reg=%u sys=%u type=%u mask=0x%X rw=0x%X",
               label,
               direction,
               index,
               semantic,
               parameter.SemanticIndex,
               parameter.Register,
               static_cast<unsigned int>(parameter.SystemValueType),
               static_cast<unsigned int>(parameter.ComponentType),
               parameter.Mask,
               parameter.ReadWriteMask);
    WriteD3D12LogLine(line);
}

void LogD3D12ShaderReflection(const wchar_t* label, const void* bytecode, size_t bytecodeSize)
{
    if (!label || !bytecode || bytecodeSize == 0)
    {
        return;
    }

    ComPtr<ID3D11ShaderReflection> reflection;
    const HRESULT hr = D3DReflect(bytecode, bytecodeSize, IID_ID3D11ShaderReflection, reinterpret_cast<void**>(reflection.GetAddressOf()));
    if (FAILED(hr) || !reflection)
    {
        wchar_t line[192]{};
        swprintf_s(line, L"%ls reflection failed hr=0x%08X bytes=%zu", label, static_cast<unsigned int>(hr), bytecodeSize);
        WriteD3D12LogLine(line);
        return;
    }

    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc)))
    {
        WriteD3D12LogLine(L"shader reflection GetDesc failed");
        return;
    }

    wchar_t line[512]{};
    swprintf_s(line,
               L"%ls reflection inputs=%u outputs=%u resources=%u cbuffers=%u instr=%u",
               label,
               desc.InputParameters,
               desc.OutputParameters,
               desc.BoundResources,
               desc.ConstantBuffers,
               desc.InstructionCount);
    WriteD3D12LogLine(line);

    for (UINT index = 0; index < desc.InputParameters; ++index)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        if (SUCCEEDED(reflection->GetInputParameterDesc(index, &parameter)))
        {
            LogShaderSignatureParameter(label, L"in", index, parameter);
        }
    }

    for (UINT index = 0; index < desc.OutputParameters; ++index)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        if (SUCCEEDED(reflection->GetOutputParameterDesc(index, &parameter)))
        {
            LogShaderSignatureParameter(label, L"out", index, parameter);
        }
    }

    for (UINT index = 0; index < desc.BoundResources; ++index)
    {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDesc(index, &binding)))
        {
            continue;
        }
        wchar_t name[160]{};
        MultiByteToWideChar(CP_UTF8, 0, binding.Name ? binding.Name : "", -1, name, static_cast<int>(std::size(name)));
        swprintf_s(line,
                   L"%ls resource%u name=%ls type=%ls bind=%u count=%u",
                   label,
                   index,
                   name,
                   ShaderInputTypeName(binding.Type),
                   binding.BindPoint,
                   binding.BindCount);
        WriteD3D12LogLine(line);
    }

    for (UINT index = 0; index < desc.ConstantBuffers; ++index)
    {
        ID3D11ShaderReflectionConstantBuffer* buffer = reflection->GetConstantBufferByIndex(index);
        if (!buffer)
        {
            continue;
        }
        D3D11_SHADER_BUFFER_DESC bufferDesc{};
        if (FAILED(buffer->GetDesc(&bufferDesc)))
        {
            continue;
        }
        wchar_t name[160]{};
        MultiByteToWideChar(CP_UTF8, 0, bufferDesc.Name ? bufferDesc.Name : "", -1, name, static_cast<int>(std::size(name)));
        swprintf_s(line,
                   L"%ls cbuffer%u name=%ls type=%u vars=%u size=%u",
                   label,
                   index,
                   name,
                   static_cast<unsigned int>(bufferDesc.Type),
                   bufferDesc.Variables,
                   bufferDesc.Size);
        WriteD3D12LogLine(line);
    }
}

void PackNoiseRgba(const std::vector<uint32_t>& src, std::vector<uint8_t>& dst)
{
    dst.resize(src.size() * 4);
    for (size_t index = 0; index < src.size(); ++index)
    {
        const uint32_t value = src[index];
        uint8_t* pixel = dst.data() + index * 4;
        pixel[0] = static_cast<uint8_t>(value & 0xffu);
        pixel[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
        pixel[2] = static_cast<uint8_t>((value >> 16) & 0xffu);
        pixel[3] = static_cast<uint8_t>((value >> 24) & 0xffu);
    }
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
        if (m_textOverlayVertexBuffers[i] && m_mappedTextOverlayVertexBuffers[i])
        {
            m_textOverlayVertexBuffers[i]->Unmap(0, nullptr);
            m_mappedTextOverlayVertexBuffers[i] = nullptr;
        }
    }
    m_mappedTextOverlayVertices = nullptr;

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
    if (m_blurConstantBuffer && m_mappedBlurConstantBuffer)
    {
        m_blurConstantBuffer->Unmap(0, nullptr);
        m_mappedBlurConstantBuffer = nullptr;
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
    m_feedbackScratchTexture.Reset();
    m_postProcessTexture.Reset();
    for (auto& blurTexture : m_blurTextures)
    {
        blurTexture.Reset();
    }
    m_blurRtvHeap.Reset();
    m_blurPassSrvHeap.Reset();
    m_blurTexturesPrimed = false;
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
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

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
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

    if (!WaitForGpu(1000))
        return false;
    CaptureBackBufferForResume();
    {
        wchar_t logLine[192]{};
        swprintf_s(logLine, L"window swap target=%dx%d captured_resume=%d", width, height, m_resumeFeedbackReady ? 1 : 0);
        WriteD3D12LogLine(logLine);
    }
    for (auto& renderTarget : m_renderTargets)
    {
        renderTarget.Reset();
    }
    for (auto& feedbackTexture : m_feedbackTextures)
    {
        feedbackTexture.Reset();
    }
    m_feedbackScratchTexture.Reset();
    m_postProcessTexture.Reset();
    for (auto& blurTexture : m_blurTextures)
    {
        blurTexture.Reset();
    }
    m_blurRtvHeap.Reset();
    m_blurPassSrvHeap.Reset();
    m_blurTexturesPrimed = false;
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
    m_standaloneTextureOverride = false;
    m_textureDirectory = textureDirectory ? textureDirectory : L"";
    if (!m_textureDirectory.empty() && m_textureDirectory.back() != L'\\' && m_textureDirectory.back() != L'/')
    {
        m_textureDirectory.push_back(L'\\');
    }

    if (m_d3dDevice && m_commandQueue)
    {
        RefreshTextureFileList();
        if (IsTextureCyclingEnabled())
        {
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
        else
        {
            ClearTextureSlots();
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
    return SetTextureFilesInternal(textureFiles, textureFileCount, false, false);
}

void D3D12Resources::ClearTextureFiles()
{
    m_presetTextureOverride = false;
    m_standaloneTextureOverride = false;
    ClearTextureSlots();
}

bool D3D12Resources::SetPresetTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount)
{
    return SetTextureFilesInternal(textureFiles, textureFileCount, true, false);
}

bool D3D12Resources::SetStandaloneTextureFiles(const wchar_t* const* textureFiles, size_t textureFileCount)
{
    return SetTextureFilesInternal(textureFiles, textureFileCount, false, true);
}

void D3D12Resources::ClearPresetTextureOverride()
{
    if (!m_presetTextureOverride && !m_standaloneTextureOverride)
    {
        return;
    }

    m_presetTextureOverride = false;
    m_standaloneTextureOverride = false;
    if (!m_d3dDevice || !m_commandQueue)
    {
        return;
    }

    if (!IsTextureCyclingEnabled())
    {
        ClearTextureSlots();
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
    else
    {
        ClearTextureSlots();
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

bool D3D12Resources::SetPresetWarpShader(const void* bytecode, size_t bytecodeSize)
{
    m_presetWarpPipelineState.Reset();
    m_presetWarpShaderBytecode.clear();

    if (!bytecode || bytecodeSize == 0)
    {
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    m_presetWarpShaderBytecode.assign(bytes, bytes + bytecodeSize);
    return CreatePresetWarpPipeline();
}

void D3D12Resources::ClearPresetWarpShader()
{
    m_presetWarpPipelineState.Reset();
    m_presetWarpShaderBytecode.clear();
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

void D3D12Resources::SetPresetShaderRuntimeConstants(float presetTime,
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
    auto clean = [](float value, float fallback) {
        return std::isfinite(value) ? value : fallback;
    };

    m_presetShaderPresetTime = clean(presetTime, 0.0f);
    m_presetShaderGlobalTime = clean(globalTime, m_presetShaderPresetTime);
    m_presetShaderFps = clean(fps, 0.0f);
    m_presetShaderFrame = clean(frame, 0.0f);
    m_presetShaderProgress = std::clamp(clean(progress, 0.0f), 0.0f, 1.0f);
    m_presetShaderCanvasWidth = std::max(clean(canvasWidth, 1.0f), 1.0f);
    m_presetShaderCanvasHeight = std::max(clean(canvasHeight, 1.0f), 1.0f);
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

bool D3D12Resources::SetTextureFilesInternal(const wchar_t* const* textureFiles,
                                             size_t textureFileCount,
                                             bool presetTextureOverride,
                                             bool standaloneTextureOverride)
{
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

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
        m_presetTextureOverride = false;
        m_standaloneTextureOverride = false;
        ClearTextureSlots();
        return false;
    }

    for (UINT slot = loadedCount; slot < c_maxTextureLayers; ++slot)
    {
        m_textureSlots[slot].texture.Reset();
        m_textureSlots[slot].uploadBuffer.Reset();
        m_textureSlots[slot].file.clear();
        m_textureSlots[slot].width = 0;
        m_textureSlots[slot].height = 0;
    }
    m_activeTextureLayerCount = loadedCount;
    m_presetTextureOverride = presetTextureOverride;
    m_standaloneTextureOverride = standaloneTextureOverride && !presetTextureOverride;
    RefreshPostProcessTextureSrvs();
    return true;
}

void D3D12Resources::ClearTextureSlots()
{
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

    for (UINT slot = 0; slot < c_maxTextureLayers; ++slot)
    {
        m_textureSlots[slot].texture.Reset();
        m_textureSlots[slot].uploadBuffer.Reset();
        m_textureSlots[slot].file.clear();
        m_textureSlots[slot].width = 0;
        m_textureSlots[slot].height = 0;
    }
    m_activeTextureLayerCount = 0;
    RefreshPostProcessTextureSrvs();
}

void D3D12Resources::Clear()
{
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

    if (!m_commandList || !m_commandQueue || !m_renderTargets[m_frameIndex] || !m_commandAllocators[m_frameIndex])
    {
        return;
    }

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
                                  const CustomShapeDrawCommand* customShapes,
                                  size_t customShapeCount,
                                  const CustomWaveVertex* customWaveVertices,
                                  size_t customWaveVertexCount,
                                  const CustomWaveDrawCommand* customWaveDraws,
                                  size_t customWaveDrawCount,
                                  const TextureWarpVertex* textureWarpVertices,
                                  size_t textureWarpVertexCount,
                                  int textureWarpGridX,
                                  int textureWarpGridY,
                                  const TextureWarpVertex* compositeVertices,
                                  size_t compositeVertexCount)
{
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

    (void)spectrumRight;
    m_mappedWaveformVertices = m_mappedWaveformVertexBuffers[m_frameIndex];
    m_waveformVertexBufferView = m_waveformVertexBufferViews[m_frameIndex];
    m_mappedTextOverlayVertices = m_mappedTextOverlayVertexBuffers[m_frameIndex];
    m_textOverlayVertexBufferView = m_textOverlayVertexBufferViews[m_frameIndex];
    m_mappedTextureVertices = m_mappedTextureVertexBuffers[m_frameIndex];
    m_textureVertexBufferView = m_textureVertexBufferViews[m_frameIndex];

    if (!left || !right || !m_commandList || !m_commandQueue || !m_commandAllocators[m_frameIndex] || !m_renderTargets[m_frameIndex] || !m_mappedWaveformVertices || !m_waveformPipelineState || !m_waveformAdditivePipelineState || !m_pointPipelineState || !m_pointAdditivePipelineState || !m_solidPipelineState || !m_solidAdditivePipelineState || !m_waveformRootSignature)
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
    const bool hasVideoEcho = echoAlpha > 0.001f;
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
    const UINT outputPixelWidth = static_cast<UINT>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const UINT outputPixelHeight = static_cast<UINT>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    const float waveAspectX = outputPixelHeight > outputPixelWidth ?
        static_cast<float>(outputPixelWidth) / static_cast<float>(outputPixelHeight) :
        1.0f;
    const float waveAspectY = outputPixelWidth > outputPixelHeight ?
        static_cast<float>(outputPixelHeight) / static_cast<float>(outputPixelWidth) :
        1.0f;
    const bool resumeFeedbackUsable = !suppressVisualFeedback && m_resumeFeedbackReady && IsResumeFeedbackCompatible(outputPixelWidth, outputPixelHeight);
    const bool feedbackSourceReady = !suppressVisualFeedback && (m_feedbackReady[previousFeedbackIndex] || resumeFeedbackUsable);
    const bool canDrawTexturedShapes = !suppressVisualFeedback &&
                                       m_feedbackReady[previousFeedbackIndex] &&
                                       m_feedbackSrvHeap &&
                                       m_textureAlphaPipelineState &&
                                       m_textureAdditivePipelineState &&
                                       m_textureRootSignature &&
                                       m_mappedTextureVertices;
    const bool postProcessResourcesReady = m_postProcessTexture &&
                                           m_postProcessSrvHeap &&
                                           m_postProcessPipelineState &&
                                           m_postProcessRootSignature &&
                                           m_mappedTextureVertices &&
                                           IsPostProcessEnabled();
    const bool presetCompositeActive = m_presetCompositePipelineState != nullptr;
    const bool postProcessActive = postProcessResourcesReady &&
                                   (presetCompositeActive ||
                                    fabsf(postGamma - 1.0f) > 0.001f ||
                                    postInvert ||
                                    postBrighten ||
                                    postDarken ||
                                    postSolarize ||
                                    hasVideoEcho ||
                                    postShaderAmount > 0.001f ||
                                    activePostBlurAmount > 0.001f);
    const bool blurTexturesActive = postProcessResourcesReady &&
                                    m_blurHorizontalPipelineState &&
                                    m_blurVerticalPipelineState &&
                                    m_blurRtvHeap &&
                                    m_blurPassSrvHeap &&
                                    (m_presetWarpPipelineState || m_presetCompositePipelineState || postProcessActive);
    const bool hasTextureWarpMesh = textureWarpVertices && textureWarpVertexCount >= 3;
    const bool legacyFixedPipeline = !m_presetWarpPipelineState.Get() && !m_presetCompositePipelineState.Get() && hasTextureWarpMesh;
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
        localX *= waveAspectY;
        localY *= waveAspectX;
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
        const UINT circularSampleCount = std::max<UINT>(2u, std::min<UINT>(sourceCount / 2, 288u));
        activeCount = circularSampleCount + 1u;
        const UINT sampleOffset = sourceCount > circularSampleCount * 2 ? (sourceCount - circularSampleCount * 2) / 2 : 0;
        for (UINT i = 0; i < activeCount; ++i)
        {
            const UINT sampleIndex = i == circularSampleCount ? 0u : i;
            const float t = static_cast<float>(sampleIndex) / static_cast<float>(circularSampleCount - 1);
            float radius = 0.5f + 0.4f * sampleAt(right, sampleIndex + sampleOffset) + waveParam;
            if (sampleIndex < circularSampleCount / 10)
            {
                const float mix = 0.5f - 0.5f * cosf((static_cast<float>(sampleIndex) / (static_cast<float>(circularSampleCount) * 0.1f)) * 3.1415927f);
                const float radius2 = 0.5f + 0.4f * sampleAt(right, sampleIndex + circularSampleCount + sampleOffset) + waveParam;
                radius = radius2 * (1.0f - mix) + radius * mix;
            }

            radius = std::clamp(radius * responsiveScale, -2.0f, 2.0f);
            const float angle = t * 6.2831853f + timeSeconds * 0.2f;
            writePoint(0, i, radius * cosf(angle), radius * sinf(angle), baseR, baseG, baseB);
        }
    }
    else if (waveMode == 1)
    {
        activeCount = std::min<UINT>(sourceCount > 33 ? sourceCount / 2 : sourceCount, 288u);
        for (UINT i = 0; i < activeCount; ++i)
        {
            const float radius = std::clamp((0.53f + 0.43f * sampleAt(right, i) + waveParam) * responsiveScale, -2.0f, 2.0f);
            const float angle = sampleAt(left, i + 32) * 1.57f + timeSeconds * 2.3f;
            writePoint(0, i, radius * cosf(angle), radius * sinf(angle), baseR, baseG, baseB);
        }
    }
    else if (waveMode == 2 || waveMode == 3)
    {
        activeCount = sourceCount > 33 ? sourceCount - 33 : sourceCount;
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
        activeCount = sourceCount > 33 ? sourceCount - 33 : sourceCount;
        const float rotC = cosf(timeSeconds * 0.3f);
        const float rotS = sinf(timeSeconds * 0.3f);
        for (UINT i = 0; i < activeCount; ++i)
        {
            const float x0 = sampleAt(right, i) * sampleAt(left, i + 32) + sampleAt(left, i) * sampleAt(right, i + 32);
            const float y0 = sampleAt(right, i) * sampleAt(right, i) - sampleAt(left, i + 32) * sampleAt(left, i + 32);
            writePoint(0, i, (x0 * rotC - y0 * rotS) * responsiveScale, -(x0 * rotS + y0 * rotC) * responsiveScale, baseR, baseG, baseB);
        }
    }
    else
    {
        activeCount = waveMode == 8 ? 256u : std::min<UINT>(sourceCount > 33 ? sourceCount / 2 : sourceCount, 288u);
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
            -(centerX * sinf(angle + 1.57f) - dy * 3.0f),
            -(centerX * sinf(angle + 1.57f) + dy * 3.0f),
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
        const float perpY = sinf(clippedAngle + 1.57f);
        const float legacyWavePosY = waveY * 2.0f - 1.0f;
        const float sep = waveMode == 7 ? powf(legacyWavePosY * 0.5f + 0.5f, 2.0f) : 0.0f;
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
        bool texturedFill = false;
        size_t shapeIndex = 0;
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
        customShapeCount = std::min<size_t>(customShapeCount, c_maxCustomShapeCommands);

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

            if (skipSolidFill)
            {
                customShapeDrawBatches.push_back({0, 0, shape.additive, true, true, shapeIndex});
            }
            else
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
                customShapeDrawBatches.push_back({fillStart, fillCount, shape.additive, true, false, shapeIndex});
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
                customShapeDrawBatches.push_back({borderStart, borderCount, shape.additive, false, false, shapeIndex});
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
    ClearBlurTexturesIfNeeded();

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    viewport.Height = static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor{};
    scissor.right = static_cast<LONG>(viewport.Width);
    scissor.bottom = static_cast<LONG>(viewport.Height);

    auto copyRenderTargetToPostProcessSourceAndResume = [&]() -> bool {
        barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);

        const bool copied = CopyBackBufferToPostProcessSource();

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_commandList->ResourceBarrier(1, &barrier);

        m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
        return copied;
    };

    auto copyShaderSourceToPostProcessSource = [&](ID3D12Resource* sourceTexture) -> bool {
        if (!sourceTexture || !m_postProcessTexture)
        {
            return false;
        }

        D3D12_RESOURCE_BARRIER copyBarriers[2]{};
        copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[0].Transition.pResource = sourceTexture;
        copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[1].Transition.pResource = m_postProcessTexture.Get();
        copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        copyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(static_cast<UINT>(std::size(copyBarriers)), copyBarriers);

        m_commandList->CopyResource(m_postProcessTexture.Get(), sourceTexture);

        copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_commandList->ResourceBarrier(static_cast<UINT>(std::size(copyBarriers)), copyBarriers);
        return true;
    };

    auto restoreCurrentRenderTarget = [&]() {
        m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
    };

    auto drawMotionVectors = [&]() {
        if (motionVectorVertexCount == 0)
        {
            return;
        }

        m_commandList->SetPipelineState(m_waveformPipelineState.Get());
        m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        m_commandList->DrawInstanced(motionVectorVertexCount, 1, motionVectorVertexStart, 0);
    };

    D3D12_GPU_DESCRIPTOR_HANDLE feedbackSourceSrv{};
    if (m_feedbackSrvHeap)
    {
        feedbackSourceSrv = m_feedbackSrvHeap->GetGPUDescriptorHandleForHeapStart();
        feedbackSourceSrv.ptr += static_cast<SIZE_T>(previousFeedbackIndex) * m_feedbackSrvDescriptorSize;
    }
    ID3D12DescriptorHeap* feedbackSourceHeap = m_feedbackSrvHeap.Get();
    ID3D12Resource* feedbackSourceTexture = m_feedbackTextures[previousFeedbackIndex].Get();
    bool motionVectorsPrewarped = false;

    if (motionVectorVertexCount > 0 &&
        m_feedbackReady[previousFeedbackIndex] &&
        m_feedbackTextures[previousFeedbackIndex] &&
        m_feedbackScratchTexture &&
        m_feedbackSrvHeap)
    {
        D3D12_RESOURCE_BARRIER copyToBackBufferBarriers[2]{};
        copyToBackBufferBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyToBackBufferBarriers[0].Transition.pResource = m_renderTargets[m_frameIndex].Get();
        copyToBackBufferBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        copyToBackBufferBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        copyToBackBufferBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyToBackBufferBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyToBackBufferBarriers[1].Transition.pResource = m_feedbackTextures[previousFeedbackIndex].Get();
        copyToBackBufferBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        copyToBackBufferBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyToBackBufferBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(static_cast<UINT>(std::size(copyToBackBufferBarriers)), copyToBackBufferBarriers);

        m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(), m_feedbackTextures[previousFeedbackIndex].Get());

        copyToBackBufferBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        copyToBackBufferBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        copyToBackBufferBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyToBackBufferBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_commandList->ResourceBarrier(static_cast<UINT>(std::size(copyToBackBufferBarriers)), copyToBackBufferBarriers);

        restoreCurrentRenderTarget();
        drawMotionVectors();

        D3D12_RESOURCE_BARRIER copyToScratchBarriers[2]{};
        copyToScratchBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyToScratchBarriers[0].Transition.pResource = m_renderTargets[m_frameIndex].Get();
        copyToScratchBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        copyToScratchBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyToScratchBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyToScratchBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyToScratchBarriers[1].Transition.pResource = m_feedbackScratchTexture.Get();
        copyToScratchBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        copyToScratchBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        copyToScratchBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(static_cast<UINT>(std::size(copyToScratchBarriers)), copyToScratchBarriers);

        m_commandList->CopyResource(m_feedbackScratchTexture.Get(), m_renderTargets[m_frameIndex].Get());

        copyToScratchBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyToScratchBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        copyToScratchBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        copyToScratchBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_commandList->ResourceBarrier(static_cast<UINT>(std::size(copyToScratchBarriers)), copyToScratchBarriers);

        restoreCurrentRenderTarget();
        m_commandList->ClearRenderTargetView(rtvHandle, bg, 0, nullptr);

        feedbackSourceSrv = m_feedbackSrvHeap->GetGPUDescriptorHandleForHeapStart();
        feedbackSourceSrv.ptr += static_cast<SIZE_T>(c_feedbackScratchSrvIndex) * m_feedbackSrvDescriptorSize;
        feedbackSourceHeap = m_feedbackSrvHeap.Get();
        feedbackSourceTexture = m_feedbackScratchTexture.Get();
        motionVectorsPrewarped = true;
    }

    m_textureVertexCursor = 0;
    m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    if (!suppressVisualFeedback && m_feedbackReady[previousFeedbackIndex] && m_feedbackSrvHeap)
    {
        const float baseFeedbackAlpha = 1.0f;

        if (hasTextureWarpMesh && baseFeedbackAlpha > 0.001f)
        {
            DrawTextureMeshFromSrv(feedbackSourceSrv,
                                   feedbackSourceHeap,
                                   textureWarpVertices,
                                   textureWarpVertexCount,
                                   bass,
                                   mids,
                                   treble,
                                   decay,
                                   baseFeedbackAlpha,
                                   true,
                                   false,
                                   true,
                                   feedbackSourceTexture,
                                   textureWrap);
        }
        else if (!hasTextureWarpMesh && baseFeedbackAlpha > 0.001f)
        {
            DrawTextureQuadFromSrv(feedbackSourceSrv, feedbackSourceHeap, bass, mids, treble, decay, zoom, rot, baseFeedbackAlpha, 1.0f, 0.0f, false, false, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, true, false, textureWrap);
        }
    }
    else if (!suppressVisualFeedback && resumeFeedbackUsable && m_feedbackSrvHeap)
    {
        auto resumeSrv = m_feedbackSrvHeap->GetGPUDescriptorHandleForHeapStart();
        resumeSrv.ptr += static_cast<SIZE_T>(c_resumeFeedbackSrvIndex) * m_feedbackSrvDescriptorSize;
        DrawTextureQuadFromSrv(resumeSrv, m_feedbackSrvHeap.Get(), bass, mids, treble, decay, zoom, rot, 1.0f, 1.0f, 0.0f, false, false, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, true, false, textureWrap);
    }

    const bool drawStandaloneTextureLayers = !m_presetTextureOverride && m_activeTextureLayerCount > 0;
    const bool presetBackgroundLayer = drawStandaloneTextureLayers && m_standaloneTextureOverride;
    const float standaloneEnergy = std::clamp(bass * 0.52f + mids * 0.30f + treble * 0.18f, 0.0f, 3.0f);
    const float standalonePulse = std::clamp(standaloneEnergy - 0.70f, 0.0f, 1.60f);
    const float legacyBackgroundAlpha = feedbackSourceReady ? std::clamp((1.0f - decay) * 0.30f, 0.0f, 0.018f) : 1.0f;
    const float liveBackgroundAlpha = legacyFixedPipeline && presetBackgroundLayer ?
        legacyBackgroundAlpha :
        (feedbackSourceReady ?
             (presetBackgroundLayer ?
                  std::clamp(0.070f + standalonePulse * 0.030f + (1.0f - decay) * 0.18f, 0.055f, 0.18f) :
                  std::clamp(0.16f + standalonePulse * 0.045f + (1.0f - decay) * 0.30f, 0.14f, 0.32f)) :
             1.0f);
    const float liveExtraLayerAlphaScale = legacyFixedPipeline && presetBackgroundLayer ?
        (feedbackSourceReady ? 0.08f : 1.0f) :
        (feedbackSourceReady ? (presetBackgroundLayer ? 0.35f : 0.55f) : 1.0f);
    if (drawStandaloneTextureLayers && hasTextureWarpMesh && m_srvHeap && m_texturePipelineState && m_textureRootSignature)
    {
        for (UINT layer = 0; layer < m_activeTextureLayerCount && layer < c_maxTextureLayers; ++layer)
        {
            if (!m_textureSlots[layer].texture)
            {
                continue;
            }

            auto textureSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            textureSrv.ptr += static_cast<SIZE_T>(layer) * m_srvDescriptorSize;
            const float layerAlpha = layer == 0 ? liveBackgroundAlpha : std::clamp((0.34f * liveExtraLayerAlphaScale) / static_cast<float>(layer + 1), 0.05f, 0.34f);
            DrawTextureMeshFromSrv(textureSrv, m_srvHeap.Get(), textureWarpVertices, textureWarpVertexCount, bass, mids, treble, decay, layerAlpha, false, false, false, nullptr, textureWrap);
        }
    }
    else if (drawStandaloneTextureLayers)
    {
        DrawTextureQuad(bass, mids, treble, decay, zoom, rot, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, liveBackgroundAlpha, liveExtraLayerAlphaScale, textureWrap);
    }

    // Legacy MilkDrop binds sampler_main/blur sources for preset composite
    // shaders to the previous virtual screen, not the just-rendered target.
    // The current warped scene is still saved below as next frame's feedback.
    const bool presetCompositeUsesFeedbackSource = presetCompositeActive &&
                                                  feedbackSourceTexture &&
                                                  m_feedbackReady[previousFeedbackIndex];
    const bool copiedLegacyBlurSource = blurTexturesActive &&
                                        (presetCompositeUsesFeedbackSource ?
                                             copyShaderSourceToPostProcessSource(feedbackSourceTexture) :
                                             copyRenderTargetToPostProcessSourceAndResume());
    if (copiedLegacyBlurSource)
    {
        RenderBlurTextures(postBlurEdgeDarken);
        m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
    }

    if (!customShapeDrawBatches.empty())
    {
        ID3D12PipelineState* activeCustomShapePipelineState = nullptr;
        D3D12_PRIMITIVE_TOPOLOGY activeCustomShapeTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        bool waveformShapePipelineBound = false;
        auto bindWaveformShapePipeline = [&]() {
            if (!waveformShapePipelineBound)
            {
                m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
                m_commandList->IASetVertexBuffers(0, 1, &m_waveformVertexBufferView);
                activeCustomShapePipelineState = nullptr;
                activeCustomShapeTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
                waveformShapePipelineBound = true;
            }
        };

        for (const ShapeDrawBatch& draw : customShapeDrawBatches)
        {
            if (draw.texturedFill)
            {
                if (canDrawTexturedShapes && customShapes && draw.shapeIndex < customShapeCount)
                {
                    DrawTexturedCustomShapesFromSrv(feedbackSourceSrv, feedbackSourceHeap, &customShapes[draw.shapeIndex], 1);
                    waveformShapePipelineBound = false;
                }
                continue;
            }

            if (draw.count == 0)
            {
                continue;
            }

            bindWaveformShapePipeline();
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

            const bool pointList = draw.topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            ID3D12PipelineState* nextPipelineState = draw.additive ?
                (draw.triangleList ? m_solidAdditivePipelineState.Get() : (pointList ? m_pointAdditivePipelineState.Get() : m_waveformAdditivePipelineState.Get())) :
                (draw.triangleList ? m_solidPipelineState.Get() : (pointList ? m_pointPipelineState.Get() : m_waveformPipelineState.Get()));
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

    if (motionVectorVertexCount > 0 && !motionVectorsPrewarped)
    {
        drawMotionVectors();
    }

    m_commandList->SetPipelineState(waveUseDots ? (waveAdditive ? m_pointAdditivePipelineState.Get() : m_pointPipelineState.Get()) : waveformPipelineState);
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

    // Match the legacy path: feedback stores the raw warped scene. Video echo,
    // gamma, hue and invert-style filters are display-only and must not recurse.
    if (suppressVisualFeedback)
    {
        m_feedbackReady[0] = false;
        m_feedbackReady[1] = false;
        m_feedbackIndex = 0;
    }
    else
    {
        CopyBackBufferToFeedback(m_feedbackIndex);
    }

    const bool copiedPostProcessSource = postProcessActive &&
                                         (presetCompositeUsesFeedbackSource && copiedLegacyBlurSource ?
                                              true :
                                              CopyBackBufferToPostProcessSource());
    if (!copiedLegacyBlurSource && copiedPostProcessSource && blurTexturesActive)
    {
        RenderBlurTextures(postBlurEdgeDarken);
    }

    if (postProcessActive && copiedPostProcessSource)
    {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_commandList->ResourceBarrier(1, &barrier);
        m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);

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
                               postBlurEdgeDarken,
                               echoAlpha,
                               echoZoom,
                               echoOrientation,
                               compositeVertices,
                               compositeVertexCount);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_commandList->ResourceBarrier(1, &barrier);
    }

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
        if (!m_mappedTextOverlayVertices || textOverlayVertexCount >= c_maxTextOverlayVertices)
        {
            return false;
        }

        WaveformVertex& vertex = m_mappedTextOverlayVertices[textOverlayVertexCount++];
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
            const bool drawShadow = shadowOffset > 0.0f;
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
                    if (drawShadow)
                    {
                        writeTextQuad(px + shadowOffset,
                                      py + shadowOffset,
                                      px + cell + shadowOffset,
                                      py + cell + shadowOffset,
                                      0.0f,
                                      0.0f,
                                      0.0f,
                                      a * 0.55f);
                    }
                    writeTextQuad(px, py, px + cell, py + cell, r, g, b, a);
                }
            }
        }
    };
    auto appendTextLine = [&](const std::wstring& text, float x, float y, float maxWidth, float r, float g, float b, float a) {
        appendTextLineScaled(text, x, y, maxWidth, textCell, textShadowOffset, r, g, b, a);
    };
    auto wrapTextBlock = [](const std::wstring& text, size_t maxChars) {
        std::vector<std::wstring> lines;
        size_t start = 0;
        maxChars = std::max<size_t>(1, maxChars);
        while (start <= text.size())
        {
            const size_t end = text.find(L'\n', start);
            std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }

            while (line.size() > maxChars)
            {
                size_t split = line.rfind(L' ', maxChars);
                if (split == std::wstring::npos || split == 0)
                {
                    split = maxChars;
                }
                lines.push_back(line.substr(0, split));
                line.erase(0, split);
                while (!line.empty() && line.front() == L' ')
                {
                    line.erase(0, 1);
                }
            }
            lines.push_back(line);

            if (end == std::wstring::npos)
            {
                break;
            }
            start = end + 1;
        }
        return lines;
    };
    auto appendTextBlock = [&](const std::wstring& text, float x, float y, float maxWidth, float r, float g, float b, float a) {
        size_t start = 0;
        float lineY = y;
        while (start <= text.size())
        {
            const size_t end = text.find(L'\n', start);
            std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }
            appendTextLine(line, x, lineY, maxWidth, r, g, b, a);
            if (end == std::wstring::npos)
            {
                break;
            }
            lineY += glyphHeight + textCell * 2.0f;
            start = end + 1;
        }
    };
    auto appendRightTextBlock = [&](const std::wstring& text, float right, float y, float maxWidth, float r, float g, float b, float a) {
        size_t start = 0;
        float lineY = y;
        while (start <= text.size())
        {
            const size_t end = text.find(L'\n', start);
            std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }

            const size_t chars = std::min(line.size(), static_cast<size_t>(std::max<float>(1.0f, floorf(maxWidth / glyphAdvance))));
            const float textWidth = static_cast<float>(chars) * glyphAdvance;
            appendTextLine(line, std::max(textMargin, right - textWidth), lineY, maxWidth, r, g, b, a);

            if (end == std::wstring::npos)
            {
                break;
            }
            lineY += glyphHeight + textCell * 2.0f;
            start = end + 1;
        }
    };
    auto appendHelpPanel = [&](const std::wstring& text, float x, float y, float maxWidth) {
        const float helpCell = std::clamp(floorf(textReferenceSize / 380.0f + 0.5f), 1.0f, 4.0f);
        const float helpAdvance = 6.0f * helpCell;
        const float helpHeight = 7.0f * helpCell;
        const float helpLineStep = helpHeight + helpCell;
        const float helpShadowOffset = 0.0f;
        const float panelPad = std::clamp(helpCell * 3.0f, 5.0f, 14.0f);

        std::vector<std::wstring> rawLines;
        size_t start = 0;
        while (start <= text.size())
        {
            const size_t end = text.find(L'\n', start);
            std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }
            rawLines.push_back(line);
            if (end == std::wstring::npos)
            {
                break;
            }
            start = end + 1;
        }

        if (rawLines.empty())
        {
            return;
        }

        bool hasColumns = false;
        for (const auto& line : rawLines)
        {
            hasColumns = hasColumns || line.find(L'\t') != std::wstring::npos;
        }

        const float panelWidth = hasColumns ? maxWidth : std::min(maxWidth, static_cast<float>(rawLines.front().size()) * helpAdvance + panelPad * 2.0f);
        const float contentWidth = std::max(helpAdvance, panelWidth - panelPad * 2.0f);
        const float columnGap = hasColumns ? std::clamp(helpAdvance * 3.0f, 12.0f, 42.0f) : 0.0f;
        const float columnWidth = hasColumns ? std::max(helpAdvance, (contentWidth - columnGap) * 0.5f) : contentWidth;
        const size_t fullMaxChars = static_cast<size_t>(std::max<float>(1.0f, floorf(contentWidth / helpAdvance)));
        const size_t columnMaxChars = static_cast<size_t>(std::max<float>(1.0f, floorf(columnWidth / helpAdvance)));

        struct HelpPanelRow
        {
            std::wstring left;
            std::wstring right;
            bool columns = false;
            bool heading = false;
            bool gapBefore = false;
            bool gapAfter = false;
        };
        std::vector<HelpPanelRow> rows;
        rows.reserve(rawLines.size() + 8);

        for (size_t lineIndex = 0; lineIndex < rawLines.size(); ++lineIndex)
        {
            const std::wstring& rawLine = rawLines[lineIndex];
            const size_t tab = rawLine.find(L'\t');
            if (tab != std::wstring::npos)
            {
                const bool sectionHeading = lineIndex == 1 ||
                                            rawLine.find(L"MESSAGES") != std::wstring::npos ||
                                            (rawLine.find(L"PRESET") != std::wstring::npos && rawLine.find(L"TWEAKS") != std::wstring::npos);
                std::vector<std::wstring> leftLines = wrapTextBlock(rawLine.substr(0, tab), columnMaxChars);
                std::vector<std::wstring> rightLines = wrapTextBlock(rawLine.substr(tab + 1), columnMaxChars);
                const size_t pairRows = std::max(leftLines.size(), rightLines.size());
                for (size_t rowIndex = 0; rowIndex < pairRows; ++rowIndex)
                {
                    HelpPanelRow row{};
                    row.columns = true;
                    row.heading = sectionHeading;
                    row.gapBefore = sectionHeading && rowIndex == 0 && lineIndex > 1;
                    if (rowIndex < leftLines.size())
                    {
                        row.left = leftLines[rowIndex];
                    }
                    if (rowIndex < rightLines.size())
                    {
                        row.right = rightLines[rowIndex];
                    }
                    row.gapAfter = sectionHeading && rowIndex + 1 == pairRows;
                    rows.push_back(std::move(row));
                }
            }
            else
            {
                std::vector<std::wstring> lineRows = wrapTextBlock(rawLine, fullMaxChars);
                const bool sectionHeading = lineIndex == 0;
                for (size_t rowIndex = 0; rowIndex < lineRows.size(); ++rowIndex)
                {
                    HelpPanelRow row{};
                    row.left = std::move(lineRows[rowIndex]);
                    row.heading = sectionHeading;
                    row.gapAfter = sectionHeading && rowIndex + 1 == lineRows.size();
                    rows.push_back(std::move(row));
                }
            }
        }

        const float headingGap = helpLineStep;
        float rowsHeight = rows.empty() ? 0.0f : -helpCell * 2.0f;
        for (const HelpPanelRow& row : rows)
        {
            if (row.gapBefore)
            {
                rowsHeight += headingGap;
            }
            rowsHeight += helpLineStep;
            if (row.gapAfter)
            {
                rowsHeight += headingGap;
            }
        }
        const float panelHeight = rowsHeight + panelPad * 2.0f;
        const float panelRight = std::min(outputWidth - textMargin, x + panelWidth);
        const float panelBottom = std::min(outputHeight - textMargin, y + panelHeight);
        const float border = std::max(1.0f, helpCell);

        writeTextQuad(x, y, panelRight, panelBottom, 0.0f, 0.0f, 0.0f, 0.84f);
        writeTextQuad(x, y, panelRight, y + border, 0.45f, 0.78f, 1.0f, 0.48f);
        writeTextQuad(x, panelBottom - border, panelRight, panelBottom, 0.45f, 0.78f, 1.0f, 0.34f);
        writeTextQuad(x, y, x + border, panelBottom, 0.45f, 0.78f, 1.0f, 0.34f);
        writeTextQuad(panelRight - border, y, panelRight, panelBottom, 0.45f, 0.78f, 1.0f, 0.34f);

        float lineY = y + panelPad;
        for (const HelpPanelRow& row : rows)
        {
            if (row.gapBefore)
            {
                lineY += headingGap;
            }
            if (lineY + helpHeight > panelBottom - panelPad * 0.5f)
            {
                break;
            }
            const float r = row.heading ? 0.96f : 0.78f;
            const float g = row.heading ? 0.98f : 0.92f;
            const float b = row.heading ? 1.0f : 0.98f;
            const float a = row.heading ? 0.96f : 0.90f;
            if (row.columns)
            {
                appendTextLineScaled(row.left, x + panelPad, lineY, columnWidth, helpCell, helpShadowOffset, r, g, b, a);
                appendTextLineScaled(row.right, x + panelPad + columnWidth + columnGap, lineY, columnWidth, helpCell, helpShadowOffset, r, g, b, a);
            }
            else
            {
                appendTextLineScaled(row.left, x + panelPad, lineY, panelRight - x - panelPad * 2.0f, helpCell, helpShadowOffset, r, g, b, a);
            }
            lineY += helpLineStep;
            if (row.gapAfter)
            {
                lineY += headingGap;
            }
        }
    };

    if (!m_overlayTopLeft.empty())
    {
        const float maxWidth = m_overlayTopRight.empty() ? outputWidth - textMargin * 2.0f : outputWidth * 0.62f;
        appendTextLine(m_overlayTopLeft, textMargin, textMargin, maxWidth, 0.92f, 0.96f, 1.0f, 0.88f);
    }
    if (!m_overlayDebugLine.empty())
    {
        const bool isHelpPanel = m_overlayDebugLine.find(L'\n') != std::wstring::npos;
        if (isHelpPanel)
        {
            const float panelTop = textMargin + glyphHeight + textCell * 3.0f;
            appendHelpPanel(m_overlayDebugLine, textMargin, panelTop, outputWidth - textMargin * 2.0f);
        }
        else
        {
            appendTextBlock(m_overlayDebugLine, textMargin, textMargin + glyphHeight + textCell * 2.0f, outputWidth - textMargin * 2.0f, 0.72f, 0.92f, 1.0f, 0.82f);
        }
    }
    if (!m_overlayTopRight.empty())
    {
        const float maxWidth = outputWidth * 0.35f;
        appendRightTextBlock(m_overlayTopRight, outputWidth - textMargin, textMargin, maxWidth, 0.86f, 1.0f, 0.72f, 0.88f);
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
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
        m_commandList->SetPipelineState(m_solidPipelineState.Get());
        m_commandList->SetGraphicsRootSignature(m_waveformRootSignature.Get());
        m_commandList->IASetVertexBuffers(0, 1, &m_textOverlayVertexBufferView);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->DrawInstanced(textOverlayVertexCount, 1, 0, 0);

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
    // Foobar owns frame pacing through the UI element timer. Present with
    // sync interval 0 so DXGI does not silently clamp "Unlimited" or caps
    // above monitor refresh back to vsync.
    const UINT syncInterval = 0u;
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
SamplerState sampWrap : register(s0);
SamplerState sampClamp : register(s1);

struct VSInput
{
    float2 position : POSITION;
    float4 uv : TEXCOORD0;
    float2 radAng : TEXCOORD1;
    float4 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 uv : TEXCOORD0;
    float4 color : COLOR;
    float2 radAng : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    output.radAng = input.radAng;
    return output;
}

float4 PSMainWrap(PSInput input) : SV_TARGET
{
    return tex0.Sample(sampWrap, input.uv.xy) * input.color;
}

float4 PSMainClamp(PSInput input) : SV_TARGET
{
    return tex0.Sample(sampClamp, input.uv.xy) * input.color;
}
)";

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShaderWrap;
    ComPtr<ID3DBlob> pixelShaderClamp;
    ComPtr<ID3DBlob> errorBlob;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ThrowIfFailed(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, vertexShader.GetAddressOf(), errorBlob.GetAddressOf()));
    errorBlob.Reset();
    ThrowIfFailed(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "PSMainWrap", "ps_5_0", compileFlags, 0, pixelShaderWrap.GetAddressOf(), errorBlob.GetAddressOf()));
    errorBlob.Reset();
    ThrowIfFailed(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "PSMainClamp", "ps_5_0", compileFlags, 0, pixelShaderClamp.GetAddressOf(), errorBlob.GetAddressOf()));
    m_textureVertexShaderBytecode.assign(static_cast<const uint8_t*>(vertexShader->GetBufferPointer()),
                                         static_cast<const uint8_t*>(vertexShader->GetBufferPointer()) + vertexShader->GetBufferSize());

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

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (UINT index = 0; index < static_cast<UINT>(std::size(samplers)); ++index)
    {
        samplers[index].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[index].AddressU = index == 0 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[index].AddressV = samplers[index].AddressU;
        samplers[index].AddressW = samplers[index].AddressU;
        samplers[index].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[index].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[index].ShaderRegister = index;
        samplers[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = 1;
    rootSignatureDesc.pParameters = &rootParameter;
    rootSignatureDesc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    rootSignatureDesc.pStaticSamplers = samplers;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), errorBlob.ReleaseAndGetAddressOf()));
    ThrowIfFailed(m_d3dDevice->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(m_textureRootSignature.ReleaseAndGetAddressOf())));

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(TextureVertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, radAng), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(TextureVertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout = {inputElements, static_cast<UINT>(std::size(inputElements))};
    psoDesc.pRootSignature = m_textureRootSignature.Get();
    psoDesc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
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
    auto createTexturePipeline = [&](ID3DBlob* pixelShader, bool alphaBlend, bool additive, ID3D12PipelineState** pipelineState) {
        psoDesc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        psoDesc.BlendState = {};
        auto& renderTargetBlend = psoDesc.BlendState.RenderTarget[0];
        renderTargetBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (alphaBlend)
        {
            renderTargetBlend.BlendEnable = TRUE;
            renderTargetBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            renderTargetBlend.DestBlend = additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
            renderTargetBlend.BlendOp = D3D12_BLEND_OP_ADD;
            renderTargetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
            renderTargetBlend.DestBlendAlpha = additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
            renderTargetBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineState)));
    };

    createTexturePipeline(pixelShaderWrap.Get(), false, false, m_texturePipelineState.ReleaseAndGetAddressOf());
    createTexturePipeline(pixelShaderClamp.Get(), false, false, m_textureClampPipelineState.ReleaseAndGetAddressOf());
    createTexturePipeline(pixelShaderWrap.Get(), true, false, m_textureAlphaPipelineState.ReleaseAndGetAddressOf());
    createTexturePipeline(pixelShaderClamp.Get(), true, false, m_textureAlphaClampPipelineState.ReleaseAndGetAddressOf());
    createTexturePipeline(pixelShaderWrap.Get(), true, true, m_textureAdditivePipelineState.ReleaseAndGetAddressOf());
    createTexturePipeline(pixelShaderClamp.Get(), true, true, m_textureAdditiveClampPipelineState.ReleaseAndGetAddressOf());

    const TextureVertex vertices[] = {
        {{-1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.70710678f, -2.35619449f}, {0.55f, 0.55f, 0.55f, 0.32f}},
        {{1.0f, 1.0f}, {1.0f, 0.0f, 1.0f, 0.0f}, {0.70710678f, -0.78539816f}, {0.55f, 0.55f, 0.55f, 0.32f}},
        {{-1.0f, -1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.70710678f, 2.35619449f}, {0.55f, 0.55f, 0.55f, 0.32f}},
        {{1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.70710678f, 0.78539816f}, {0.55f, 0.55f, 0.55f, 0.32f}},
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

    CreateNoiseTextures();
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
        OutputDebugStringW((std::wstring(L"foo_vis_milk2 DX12 texture loaded: ") + textureFile + L"\n").c_str());
        wchar_t logLine[1024]{};
        swprintf_s(logLine, L"texture slot %u loaded %ux%u \"%ls\"", slotIndex, slot.width, slot.height, textureFile);
        WriteD3D12LogLine(logLine);
    }
    else
    {
        slot.texture.Reset();
        slot.uploadBuffer.Reset();
        slot.file.clear();
        slot.width = 0;
        slot.height = 0;
        wchar_t logLine[1024]{};
        swprintf_s(logLine, L"texture slot %u failed \"%ls\"", slotIndex, textureFile);
        WriteD3D12LogLine(logLine);
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
    if (!UploadTextureSlotData(slot, width, height, format, pixels, pixelsSize, sourceRowPitch, sourceRowCount))
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    auto srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(slotIndex) * m_srvDescriptorSize;
    m_d3dDevice->CreateShaderResourceView(slot.texture.Get(), &srvDesc, srvHandle);
    RefreshPostProcessTextureSrvs();
    return true;
}

bool D3D12Resources::UploadTextureSlotData(TextureSlot& slot,
                                           UINT width,
                                           UINT height,
                                           DXGI_FORMAT format,
                                           const uint8_t* pixels,
                                           size_t pixelsSize,
                                           UINT sourceRowPitch,
                                           UINT sourceRowCount)
{
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

    if (width == 0 || height == 0 || !pixels || pixelsSize == 0 || sourceRowPitch == 0 || sourceRowCount == 0)
    {
        return false;
    }
    if (!m_d3dDevice || !m_commandQueue || !m_commandList || !m_commandAllocators[m_frameIndex])
    {
        return false;
    }

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
    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&defaultHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &textureDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));

    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    m_d3dDevice->GetCopyableFootprints(&textureDesc, 0, 1, 0, &layout, &numRows, &rowSize, &uploadSize);
    if (uploadSize == 0 || numRows == 0 || rowSize == 0 || layout.Footprint.RowPitch == 0 ||
        sourceRowCount > numRows || rowSize > sourceRowPitch || rowSize > layout.Footprint.RowPitch ||
        rowSize > static_cast<UINT64>(std::numeric_limits<size_t>::max()))
    {
        return false;
    }

    const UINT64 destEnd = layout.Offset +
                           (static_cast<UINT64>(sourceRowCount - 1) * layout.Footprint.RowPitch) +
                           rowSize;
    if (destEnd < layout.Offset || destEnd > uploadSize)
    {
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
                                                       IID_PPV_ARGS(uploadBuffer.ReleaseAndGetAddressOf())));

    if (sourceRowPitch > std::numeric_limits<size_t>::max() / sourceRowCount ||
        pixelsSize < static_cast<size_t>(sourceRowPitch) * sourceRowCount)
    {
        return false;
    }

    uint8_t* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    const size_t copyRowPitch = static_cast<size_t>(rowSize);
    for (UINT row = 0; row < sourceRowCount; ++row)
    {
        memcpy(mapped + layout.Offset + static_cast<size_t>(row) * layout.Footprint.RowPitch, pixels + static_cast<size_t>(row) * sourceRowPitch, copyRowPitch);
    }
    uploadBuffer->Unmap(0, nullptr);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    if (!WaitForGpu())
    {
        return false;
    }

    slot.texture = texture;
    slot.uploadBuffer.Reset();
    slot.width = width;
    slot.height = height;
    return true;
}

bool D3D12Resources::CreateNoiseTextures()
{
    if (!m_d3dDevice || !m_commandQueue)
    {
        return false;
    }

    bool ok = true;
    const auto create2D = [&](UINT index, UINT size, UINT zoom, UINT seed, const wchar_t* name) {
        try
        {
            const bool created = CreateNoiseTexture(index, size, zoom, seed);
            OutputDebugStringW((std::wstring(L"foo_vis_milk2 DX12 2D noise ") + name + (created ? L" created\n" : L" failed\n")).c_str());
            return created;
        }
        catch (const std::exception& ex)
        {
            OutputDebugStringA("foo_vis_milk2 DX12 2D noise creation exception: ");
            OutputDebugStringA(ex.what());
            OutputDebugStringA("\n");
            return false;
        }
    };
    const auto create3D = [&](UINT index, UINT size, UINT zoom, UINT seed, const wchar_t* name) {
        try
        {
            const bool created = CreateNoiseVolumeTexture(index, size, zoom, seed);
            OutputDebugStringW((std::wstring(L"foo_vis_milk2 DX12 3D noise ") + name + (created ? L" created\n" : L" failed\n")).c_str());
            return created;
        }
        catch (const std::exception& ex)
        {
            OutputDebugStringA("foo_vis_milk2 DX12 3D noise creation exception: ");
            OutputDebugStringA(ex.what());
            OutputDebugStringA("\n");
            return false;
        }
    };

    ok = create2D(0, 256, 1, 0x4275u, L"noise_lq") && ok;
    ok = create2D(1, 32, 1, 0x5f19u, L"noise_lq_lite") && ok;
    ok = create2D(2, 256, 4, 0x91bdu, L"noise_mq") && ok;
    ok = create2D(3, 256, 8, 0xc343u, L"noise_hq") && ok;
    ok = create3D(0, 32, 1, 0x6aa1u, L"noisevol_lq") && ok;
    ok = create3D(1, 32, 4, 0xbd21u, L"noisevol_hq") && ok;
    return ok;
}

bool D3D12Resources::CreateNoiseTexture(UINT noiseIndex, UINT size, UINT zoomFactor, UINT seed)
{
    if (noiseIndex >= c_noiseTextureCount || size == 0)
    {
        return false;
    }

    std::vector<uint32_t> noise(static_cast<size_t>(size) * size);
    for (UINT y = 0; y < size; ++y)
    {
        uint32_t rowState = seed ^ (0x6d2b79f5u * (y + 1u));
        for (UINT x = 0; x < size; ++x)
        {
            noise[static_cast<size_t>(y) * size + x] = MakeNoiseColor(rowState, zoomFactor);
        }
        for (UINT x = 0; x < size; ++x)
        {
            const UINT x1 = (NextNoiseRandom(rowState) ^ (seed + y * 0x9e3779b9u)) % size;
            const UINT x2 = (NextNoiseRandom(rowState) ^ (seed + y * 0x85ebca6bu)) % size;
            std::swap(noise[static_cast<size_t>(y) * size + x1], noise[static_cast<size_t>(y) * size + x2]);
        }
    }

    if (zoomFactor > 1)
    {
        for (UINT y = 0; y < size; y += zoomFactor)
        {
            for (UINT x = 0; x < size; ++x)
            {
                if (x % zoomFactor)
                {
                    const UINT baseX = (x / zoomFactor) * zoomFactor + size;
                    const size_t base = static_cast<size_t>(y) * size;
                    const uint32_t y0 = noise[base + ((baseX - zoomFactor) % size)];
                    const uint32_t y1 = noise[base + (baseX % size)];
                    const uint32_t y2 = noise[base + ((baseX + zoomFactor) % size)];
                    const uint32_t y3 = noise[base + ((baseX + zoomFactor * 2u) % size)];
                    noise[base + x] = NoiseCubicInterpolateRgba(y0, y1, y2, y3, static_cast<float>(x % zoomFactor) / static_cast<float>(zoomFactor));
                }
            }
        }

        for (UINT x = 0; x < size; ++x)
        {
            for (UINT y = 0; y < size; ++y)
            {
                if (y % zoomFactor)
                {
                    const UINT baseY = (y / zoomFactor) * zoomFactor + size;
                    const uint32_t y0 = noise[static_cast<size_t>((baseY - zoomFactor) % size) * size + x];
                    const uint32_t y1 = noise[static_cast<size_t>(baseY % size) * size + x];
                    const uint32_t y2 = noise[static_cast<size_t>((baseY + zoomFactor) % size) * size + x];
                    const uint32_t y3 = noise[static_cast<size_t>((baseY + zoomFactor * 2u) % size) * size + x];
                    noise[static_cast<size_t>(y) * size + x] = NoiseCubicInterpolateRgba(y0, y1, y2, y3, static_cast<float>(y % zoomFactor) / static_cast<float>(zoomFactor));
                }
            }
        }
    }

    std::vector<uint8_t> pixels;
    PackNoiseRgba(noise, pixels);
    return UploadTextureSlotData(m_noiseTextureSlots[noiseIndex],
                                 size,
                                 size,
                                 DXGI_FORMAT_R8G8B8A8_UNORM,
                                 pixels.data(),
                                 pixels.size(),
                                 size * 4,
                                 size);
}

bool D3D12Resources::CreateNoiseVolumeTexture(UINT noiseIndex, UINT size, UINT zoomFactor, UINT seed)
{
    if (noiseIndex >= c_noiseVolumeTextureCount || size == 0)
    {
        return false;
    }

    std::vector<uint32_t> noise(static_cast<size_t>(size) * size * size);
    for (UINT z = 0; z < size; ++z)
    {
        for (UINT y = 0; y < size; ++y)
        {
            uint32_t rowState = seed ^ (0x6d2b79f5u * (y + 1u)) ^ (0x27d4eb2du * (z + 1u));
            for (UINT x = 0; x < size; ++x)
            {
                noise[(static_cast<size_t>(z) * size * size) + (static_cast<size_t>(y) * size) + x] = MakeNoiseColor(rowState, zoomFactor);
            }
            const size_t rowBase = (static_cast<size_t>(z) * size * size) + static_cast<size_t>(y) * size;
            for (UINT x = 0; x < size; ++x)
            {
                const UINT x1 = (NextNoiseRandom(rowState) ^ (seed + z * 0x9e3779b9u + y)) % size;
                const UINT x2 = (NextNoiseRandom(rowState) ^ (seed + z * 0x85ebca6bu + y)) % size;
                std::swap(noise[rowBase + x1], noise[rowBase + x2]);
            }
        }
    }

    if (zoomFactor > 1)
    {
        const size_t slicePitch = static_cast<size_t>(size) * size;

        for (UINT z = 0; z < size; z += zoomFactor)
        {
            for (UINT y = 0; y < size; y += zoomFactor)
            {
                for (UINT x = 0; x < size; ++x)
                {
                    if (x % zoomFactor)
                    {
                        const UINT baseX = (x / zoomFactor) * zoomFactor + size;
                        const size_t base = static_cast<size_t>(z) * slicePitch + static_cast<size_t>(y) * size;
                        const uint32_t y0 = noise[base + ((baseX - zoomFactor) % size)];
                        const uint32_t y1 = noise[base + (baseX % size)];
                        const uint32_t y2 = noise[base + ((baseX + zoomFactor) % size)];
                        const uint32_t y3 = noise[base + ((baseX + zoomFactor * 2u) % size)];
                        noise[base + x] = NoiseCubicInterpolateRgba(y0, y1, y2, y3, static_cast<float>(x % zoomFactor) / static_cast<float>(zoomFactor));
                    }
                }
            }
        }

        for (UINT z = 0; z < size; z += zoomFactor)
        {
            for (UINT x = 0; x < size; ++x)
            {
                for (UINT y = 0; y < size; ++y)
                {
                    if (y % zoomFactor)
                    {
                        const UINT baseY = (y / zoomFactor) * zoomFactor + size;
                        const size_t sliceBase = static_cast<size_t>(z) * slicePitch;
                        const uint32_t y0 = noise[sliceBase + static_cast<size_t>((baseY - zoomFactor) % size) * size + x];
                        const uint32_t y1 = noise[sliceBase + static_cast<size_t>(baseY % size) * size + x];
                        const uint32_t y2 = noise[sliceBase + static_cast<size_t>((baseY + zoomFactor) % size) * size + x];
                        const uint32_t y3 = noise[sliceBase + static_cast<size_t>((baseY + zoomFactor * 2u) % size) * size + x];
                        noise[sliceBase + static_cast<size_t>(y) * size + x] = NoiseCubicInterpolateRgba(y0, y1, y2, y3, static_cast<float>(y % zoomFactor) / static_cast<float>(zoomFactor));
                    }
                }
            }
        }

        for (UINT x = 0; x < size; ++x)
        {
            for (UINT y = 0; y < size; ++y)
            {
                for (UINT z = 0; z < size; ++z)
                {
                    if (z % zoomFactor)
                    {
                        const UINT baseZ = (z / zoomFactor) * zoomFactor + size;
                        const size_t rowOffset = static_cast<size_t>(y) * size + x;
                        const uint32_t y0 = noise[static_cast<size_t>((baseZ - zoomFactor) % size) * slicePitch + rowOffset];
                        const uint32_t y1 = noise[static_cast<size_t>(baseZ % size) * slicePitch + rowOffset];
                        const uint32_t y2 = noise[static_cast<size_t>((baseZ + zoomFactor) % size) * slicePitch + rowOffset];
                        const uint32_t y3 = noise[static_cast<size_t>((baseZ + zoomFactor * 2u) % size) * slicePitch + rowOffset];
                        noise[static_cast<size_t>(z) * slicePitch + rowOffset] = NoiseCubicInterpolateRgba(y0, y1, y2, y3, static_cast<float>(z % zoomFactor) / static_cast<float>(zoomFactor));
                    }
                }
            }
        }
    }

    std::vector<uint8_t> pixels;
    PackNoiseRgba(noise, pixels);
    return UploadTextureVolumeSlotData(m_noiseVolumeTextureSlots[noiseIndex],
                                       size,
                                       size,
                                       size,
                                       DXGI_FORMAT_R8G8B8A8_UNORM,
                                       pixels.data(),
                                       pixels.size(),
                                       size * 4,
                                       size,
                                       size);
}

bool D3D12Resources::UploadTextureVolumeSlotData(TextureSlot& slot,
                                                 UINT width,
                                                 UINT height,
                                                 UINT depth,
                                                 DXGI_FORMAT format,
                                                 const uint8_t* pixels,
                                                 size_t pixelsSize,
                                                 UINT sourceRowPitch,
                                                 UINT sourceRowCount,
                                                 UINT sourceDepthCount)
{
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

    if (width == 0 || height == 0 || depth == 0 || !pixels || pixelsSize == 0 || sourceRowPitch == 0 || sourceRowCount == 0 || sourceDepthCount == 0 ||
        depth > static_cast<UINT>(std::numeric_limits<UINT16>::max()))
    {
        return false;
    }
    if (!m_d3dDevice || !m_commandQueue || !m_commandList || !m_commandAllocators[m_frameIndex])
    {
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = static_cast<UINT16>(depth);
    textureDesc.MipLevels = 1;
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&defaultHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &textureDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));

    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    m_d3dDevice->GetCopyableFootprints(&textureDesc, 0, 1, 0, &layout, &numRows, &rowSize, &uploadSize);
    if (uploadSize == 0 || numRows == 0 || rowSize == 0 || layout.Footprint.RowPitch == 0 || layout.Footprint.Depth == 0 ||
        sourceRowCount > numRows || sourceDepthCount > layout.Footprint.Depth ||
        rowSize > sourceRowPitch || rowSize > layout.Footprint.RowPitch ||
        rowSize > static_cast<UINT64>(std::numeric_limits<size_t>::max()))
    {
        return false;
    }

    const UINT64 destSlicePitch = static_cast<UINT64>(layout.Footprint.RowPitch) * numRows;
    if (numRows != 0 && destSlicePitch / numRows != layout.Footprint.RowPitch)
    {
        return false;
    }

    const UINT64 destEnd = layout.Offset +
                           (static_cast<UINT64>(sourceDepthCount - 1) * destSlicePitch) +
                           (static_cast<UINT64>(sourceRowCount - 1) * layout.Footprint.RowPitch) +
                           rowSize;
    if (destEnd < layout.Offset || destEnd > uploadSize)
    {
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
                                                       IID_PPV_ARGS(uploadBuffer.ReleaseAndGetAddressOf())));

    if (sourceRowPitch > std::numeric_limits<size_t>::max() / sourceRowCount)
    {
        return false;
    }
    const size_t sourceSlicePitch = static_cast<size_t>(sourceRowPitch) * sourceRowCount;
    if (sourceSlicePitch > std::numeric_limits<size_t>::max() / sourceDepthCount ||
        pixelsSize < sourceSlicePitch * sourceDepthCount)
    {
        return false;
    }

    uint8_t* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    const size_t copyRowPitch = static_cast<size_t>(rowSize);
    const size_t destSlicePitchSize = static_cast<size_t>(destSlicePitch);
    for (UINT slice = 0; slice < sourceDepthCount; ++slice)
    {
        for (UINT row = 0; row < sourceRowCount; ++row)
        {
            memcpy(mapped + layout.Offset + static_cast<size_t>(slice) * destSlicePitchSize + static_cast<size_t>(row) * layout.Footprint.RowPitch,
                   pixels + static_cast<size_t>(slice) * sourceSlicePitch + static_cast<size_t>(row) * sourceRowPitch,
                   copyRowPitch);
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    if (!WaitForGpu())
    {
        return false;
    }

    slot.texture = texture;
    slot.uploadBuffer.Reset();
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
    float echoAlpha;
    float echoZoom;
    float3 hueTopRight;
    float echoOrientation;
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
    float4 uv : TEXCOORD0;
    float2 radAng : TEXCOORD1;
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
    output.uv = input.uv.xy;
    output.color = input.color;
    output.radAng = input.radAng;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 sampleColor = sourceTex.Sample(samp0, input.uv);
    float3 color = max(sampleColor.rgb, 0.0f);

    if (echoAlpha > 0.001f)
    {
        float2 echoUv = 0.5f + (input.uv - 0.5f) / max(abs(echoZoom), 0.001f);
        int orientation = ((int)round(echoOrientation)) & 3;
        if ((orientation & 1) != 0)
        {
            echoUv.x = 1.0f - echoUv.x;
        }
        if ((orientation & 2) != 0)
        {
            echoUv.y = 1.0f - echoUv.y;
        }
        color = color * (1.0f - saturate(echoAlpha)) + sourceTex.Sample(samp0, echoUv).rgb * saturate(echoAlpha);
    }

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

    static constexpr char blurShaderSource[] = R"(
Texture2D sourceTex : register(t0);
SamplerState samp0 : register(s0);

cbuffer BlurConstants : register(b0)
{
    float4 srcTexSize;
    float4 horizontalWeights;
    float4 horizontalOffsets;
    float4 horizontalScaleBiasDiv;
    float4 verticalWeightsOffsets;
    float4 verticalDivEdge;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
    float2 radAng : TEXCOORD1;
};

float4 PSBlurHorizontal(PSInput input) : SV_TARGET
{
    float2 uv = input.uv + srcTexSize.zw * float2(1.0f, 1.0f);
    float3 blur =
        (sourceTex.Sample(samp0, uv + float2(horizontalOffsets.x * srcTexSize.z, 0.0f)).rgb +
         sourceTex.Sample(samp0, uv - float2(horizontalOffsets.x * srcTexSize.z, 0.0f)).rgb) * horizontalWeights.x +
        (sourceTex.Sample(samp0, uv + float2(horizontalOffsets.y * srcTexSize.z, 0.0f)).rgb +
         sourceTex.Sample(samp0, uv - float2(horizontalOffsets.y * srcTexSize.z, 0.0f)).rgb) * horizontalWeights.y +
        (sourceTex.Sample(samp0, uv + float2(horizontalOffsets.z * srcTexSize.z, 0.0f)).rgb +
         sourceTex.Sample(samp0, uv - float2(horizontalOffsets.z * srcTexSize.z, 0.0f)).rgb) * horizontalWeights.z +
        (sourceTex.Sample(samp0, uv + float2(horizontalOffsets.w * srcTexSize.z, 0.0f)).rgb +
         sourceTex.Sample(samp0, uv - float2(horizontalOffsets.w * srcTexSize.z, 0.0f)).rgb) * horizontalWeights.w;
    blur *= horizontalScaleBiasDiv.z;
    blur = blur * horizontalScaleBiasDiv.x + horizontalScaleBiasDiv.y;
    return float4(blur, 1.0f);
}

float4 PSBlurVertical(PSInput input) : SV_TARGET
{
    float2 uv = input.uv + srcTexSize.zw * float2(1.0f, 0.0f);
    float2 weights = verticalWeightsOffsets.xy;
    float2 offsets = verticalWeightsOffsets.zw;
    float3 blur =
        (sourceTex.Sample(samp0, uv + float2(0.0f, offsets.x * srcTexSize.w)).rgb +
         sourceTex.Sample(samp0, uv - float2(0.0f, offsets.x * srcTexSize.w)).rgb) * weights.x +
        (sourceTex.Sample(samp0, uv + float2(0.0f, offsets.y * srcTexSize.w)).rgb +
         sourceTex.Sample(samp0, uv - float2(0.0f, offsets.y * srcTexSize.w)).rgb) * weights.y;
    blur *= verticalDivEdge.x;

    float edge = min(min(input.uv.x, input.uv.y), 1.0f - max(input.uv.x, input.uv.y));
    edge = sqrt(max(edge, 0.0f));
    float edgeScale = verticalDivEdge.y + verticalDivEdge.z * saturate(edge * verticalDivEdge.w);
    blur *= edgeScale;
    return float4(blur, 1.0f);
}
)";

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> blurHorizontalShader;
    ComPtr<ID3DBlob> blurVerticalShader;
    ComPtr<ID3DBlob> errorBlob;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ThrowIfFailed(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, vertexShader.GetAddressOf(), errorBlob.GetAddressOf()));
    errorBlob.Reset();
    ThrowIfFailed(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, pixelShader.GetAddressOf(), errorBlob.GetAddressOf()));
    errorBlob.Reset();
    ThrowIfFailed(D3DCompile(blurShaderSource, sizeof(blurShaderSource), nullptr, nullptr, nullptr, "PSBlurHorizontal", "ps_5_0", compileFlags, 0, blurHorizontalShader.GetAddressOf(), errorBlob.GetAddressOf()));
    errorBlob.Reset();
    ThrowIfFailed(D3DCompile(blurShaderSource, sizeof(blurShaderSource), nullptr, nullptr, nullptr, "PSBlurVertical", "ps_5_0", compileFlags, 0, blurVerticalShader.GetAddressOf(), errorBlob.GetAddressOf()));
    m_postProcessVertexShaderBytecode.assign(static_cast<const uint8_t*>(vertexShader->GetBufferPointer()),
                                             static_cast<const uint8_t*>(vertexShader->GetBufferPointer()) + vertexShader->GetBufferSize());

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = c_shaderSrvCount;
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

    D3D12_STATIC_SAMPLER_DESC samplers[c_shaderStaticSamplerCount]{};
    auto setStaticSampler = [&](UINT samplerIndex,
                                D3D12_FILTER filter,
                                D3D12_TEXTURE_ADDRESS_MODE addressMode) {
        samplers[samplerIndex].Filter = filter;
        samplers[samplerIndex].AddressU = addressMode;
        samplers[samplerIndex].AddressV = addressMode;
        samplers[samplerIndex].AddressW = addressMode;
        samplers[samplerIndex].ShaderRegister = samplerIndex;
        samplers[samplerIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    };
    setStaticSampler(c_samplerLinearWrap, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    setStaticSampler(c_samplerLinearClamp, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    setStaticSampler(c_samplerPointWrap, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    setStaticSampler(c_samplerPointClamp, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

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
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(TextureVertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, radAng), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(TextureVertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
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

    psoDesc.PS = {blurHorizontalShader->GetBufferPointer(), blurHorizontalShader->GetBufferSize()};
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_blurHorizontalPipelineState.ReleaseAndGetAddressOf())));

    psoDesc.PS = {blurVerticalShader->GetBufferPointer(), blurVerticalShader->GetBufferSize()};
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_blurVerticalPipelineState.ReleaseAndGetAddressOf())));

    if (!m_presetWarpShaderBytecode.empty())
    {
        CreatePresetWarpPipeline();
    }

    if (!m_presetCompositeShaderBytecode.empty())
    {
        CreatePresetCompositePipeline();
    }

    if (m_postProcessConstantBuffer && m_mappedPostProcessConstantBuffer)
    {
        m_postProcessConstantBuffer->Unmap(0, nullptr);
    }
    if (m_blurConstantBuffer && m_mappedBlurConstantBuffer)
    {
        m_blurConstantBuffer->Unmap(0, nullptr);
    }
    m_mappedPostProcessConstantBuffer = nullptr;
    m_mappedBlurConstantBuffer = nullptr;
    m_postProcessConstantBuffer.Reset();
    m_blurConstantBuffer.Reset();

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

    constantBufferDesc.Width = c_postProcessConstantBufferSize * c_blurRenderTextureCount;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&uploadHeap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &constantBufferDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_blurConstantBuffer.ReleaseAndGetAddressOf())));
    ThrowIfFailed(m_blurConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedBlurConstantBuffer)));
}

bool D3D12Resources::CreatePresetWarpPipeline()
{
    m_presetWarpPipelineState.Reset();

    if (!m_d3dDevice || !m_postProcessRootSignature || m_textureVertexShaderBytecode.empty() || m_presetWarpShaderBytecode.empty())
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(TextureVertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, radAng), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(TextureVertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout = {inputElements, static_cast<UINT>(std::size(inputElements))};
    psoDesc.pRootSignature = m_postProcessRootSignature.Get();
    psoDesc.VS = {m_textureVertexShaderBytecode.data(), m_textureVertexShaderBytecode.size()};
    psoDesc.PS = {m_presetWarpShaderBytecode.data(), m_presetWarpShaderBytecode.size()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_backBufferFormat;
    psoDesc.SampleDesc.Count = 1;

    const HRESULT hr = m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_presetWarpPipelineState.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        wchar_t logLine[256]{};
        swprintf_s(logLine,
                   L"preset warp PSO create failed hr=0x%08X vs_bytes=%zu ps_bytes=%zu",
                   static_cast<unsigned int>(hr),
                   m_textureVertexShaderBytecode.size(),
                   m_presetWarpShaderBytecode.size());
        WriteD3D12LogLine(logLine);
        LogD3D12ShaderReflection(L"preset warp VS", m_textureVertexShaderBytecode.data(), m_textureVertexShaderBytecode.size());
        LogD3D12ShaderReflection(L"preset warp PS", m_presetWarpShaderBytecode.data(), m_presetWarpShaderBytecode.size());
        return false;
    }
    WriteD3D12LogLine(L"preset warp PSO installed");
    return true;
}

bool D3D12Resources::CreatePresetCompositePipeline()
{
    m_presetCompositePipelineState.Reset();

    if (!m_d3dDevice || !m_postProcessRootSignature || m_postProcessVertexShaderBytecode.empty() || m_presetCompositeShaderBytecode.empty())
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(TextureVertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, radAng), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(TextureVertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
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

    const HRESULT hr = m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_presetCompositePipelineState.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        wchar_t logLine[256]{};
        swprintf_s(logLine,
                   L"preset composite PSO create failed hr=0x%08X vs_bytes=%zu ps_bytes=%zu",
                   static_cast<unsigned int>(hr),
                   m_postProcessVertexShaderBytecode.size(),
                   m_presetCompositeShaderBytecode.size());
        WriteD3D12LogLine(logLine);
        LogD3D12ShaderReflection(L"preset composite VS", m_postProcessVertexShaderBytecode.data(), m_postProcessVertexShaderBytecode.size());
        LogD3D12ShaderReflection(L"preset composite PS", m_presetCompositeShaderBytecode.data(), m_presetCompositeShaderBytecode.size());
        return false;
    }
    WriteD3D12LogLine(L"preset composite PSO installed");
    return true;
}

void D3D12Resources::UpdatePresetShaderConstantBuffer()
{
    if (!m_mappedPostProcessConstantBuffer)
    {
        return;
    }

    const float width = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const float height = static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    const float safeWidth = std::max(width, 1.0f);
    const float safeHeight = std::max(height, 1.0f);
    const float aspectX = safeHeight > safeWidth ? safeWidth / safeHeight : 1.0f;
    const float aspectY = safeWidth > safeHeight ? safeHeight / safeWidth : 1.0f;
    const float bassAvg = (m_presetShaderBass + m_presetShaderMids + m_presetShaderTreble) * (1.0f / 3.0f);
    const float bassAttAvg = (m_presetShaderBassAtt + m_presetShaderMidsAtt + m_presetShaderTrebleAtt) * (1.0f / 3.0f);
    const float presetTime = std::isfinite(m_presetShaderPresetTime) ? m_presetShaderPresetTime : 0.0f;
    const float globalTime = std::isfinite(m_presetShaderGlobalTime) ? m_presetShaderGlobalTime : presetTime;
    const float canvasWidth = std::max(m_presetShaderCanvasWidth, 1.0f);
    const float canvasHeight = std::max(m_presetShaderCanvasHeight, 1.0f);

    std::array<float, 512> constants{};
    auto write4 = [&](size_t offset, float x, float y, float z, float w) {
        constants[offset + 0] = x;
        constants[offset + 1] = y;
        constants[offset + 2] = z;
        constants[offset + 3] = w;
    };
    auto writeTextureLayerTexsizes = [&]() {
        static constexpr size_t firstLayerTexsizeOffset = 384;
        for (UINT layer = 0; layer < c_maxTextureLayers; ++layer)
        {
            const TextureSlot& slot = m_textureSlots[layer];
            const float texWidth = static_cast<float>(std::max<UINT>(slot.width, 1u));
            const float texHeight = static_cast<float>(std::max<UINT>(slot.height, 1u));
            write4(firstLayerTexsizeOffset + static_cast<size_t>(layer) * 4,
                   texWidth,
                   texHeight,
                   1.0f / texWidth,
                   1.0f / texHeight);
        }
    };
    auto writeBlurTexsizes = [&]() {
        static constexpr size_t firstBlurTexsizeOffset = 448;
        static constexpr UINT visibleBlurIndices[c_visibleBlurTextureCount] = {1, 3, 5};
        for (UINT blurIndex = 0; blurIndex < c_visibleBlurTextureCount; ++blurIndex)
        {
            const UINT textureIndex = visibleBlurIndices[blurIndex];
            const float texWidth = static_cast<float>(std::max<UINT>(m_blurTextureWidths[textureIndex], 1u));
            const float texHeight = static_cast<float>(std::max<UINT>(m_blurTextureHeights[textureIndex], 1u));
            write4(firstBlurTexsizeOffset + static_cast<size_t>(blurIndex) * 4,
                   texWidth,
                   texHeight,
                   1.0f / texWidth,
                   1.0f / texHeight);
        }
    };

    write4(0, m_presetShaderRandFrame[0], m_presetShaderRandFrame[1], m_presetShaderRandFrame[2], m_presetShaderRandFrame[3]);
    write4(4, m_presetShaderRandPreset[0], m_presetShaderRandPreset[1], m_presetShaderRandPreset[2], m_presetShaderRandPreset[3]);
    write4(8, aspectX, aspectY, aspectX > 0.0001f ? 1.0f / aspectX : 1.0f, aspectY > 0.0001f ? 1.0f / aspectY : 1.0f);
    write4(12, 0.0f, 0.0f, 0.0f, 0.0f);
    write4(16, presetTime, m_presetShaderFps, m_presetShaderFrame, m_presetShaderProgress);
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
    write4(36, canvasWidth, canvasHeight, 1.0f / canvasWidth, 1.0f / canvasHeight);
    write4(40, 0.5f + 0.5f * cosf(globalTime * 0.329f + 1.2f),
               0.5f + 0.5f * cosf(globalTime * 1.293f + 3.9f),
               0.5f + 0.5f * cosf(globalTime * 5.070f + 2.5f),
               0.5f + 0.5f * cosf(globalTime * 20.051f + 5.4f));
    write4(44, 0.5f + 0.5f * sinf(globalTime * 0.329f + 1.2f),
               0.5f + 0.5f * sinf(globalTime * 1.293f + 3.9f),
               0.5f + 0.5f * sinf(globalTime * 5.070f + 2.5f),
               0.5f + 0.5f * sinf(globalTime * 20.051f + 5.4f));
    write4(48, 0.5f + 0.5f * cosf(globalTime * 0.0050f + 2.7f),
               0.5f + 0.5f * cosf(globalTime * 0.0085f + 5.3f),
               0.5f + 0.5f * cosf(globalTime * 0.0133f + 4.5f),
               0.5f + 0.5f * cosf(globalTime * 0.0217f + 3.8f));
    write4(52, 0.5f + 0.5f * sinf(globalTime * 0.0050f + 2.7f),
               0.5f + 0.5f * sinf(globalTime * 0.0085f + 5.3f),
               0.5f + 0.5f * sinf(globalTime * 0.0133f + 4.5f),
               0.5f + 0.5f * sinf(globalTime * 0.0217f + 3.8f));
    const float mipX = log2f(safeWidth);
    const float mipY = log2f(safeHeight);
    write4(56, mipX, mipY, (mipX + mipY) * 0.5f, 0.0f);
    write4(60, m_presetShaderBlurMin[1], m_presetShaderBlurMax[1], m_presetShaderBlurMin[2], m_presetShaderBlurMax[2]);
    std::copy_n(m_presetShaderQ, std::size(m_presetShaderQ), constants.data() + 64);
    std::copy_n(m_presetShaderRotMatrices, std::size(m_presetShaderRotMatrices), constants.data() + 96);
    writeTextureLayerTexsizes();
    writeBlurTexsizes();

    memcpy(m_mappedPostProcessConstantBuffer, constants.data(), sizeof(constants));
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
    srvHeapDesc.NumDescriptors = c_shaderSrvCount;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(m_postProcessSrvHeap.ReleaseAndGetAddressOf())));
    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(m_warpShaderSrvHeap.ReleaseAndGetAddressOf())));

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

    D3D12_DESCRIPTOR_HEAP_DESC blurRtvHeapDesc{};
    blurRtvHeapDesc.NumDescriptors = c_blurRenderTextureCount;
    blurRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&blurRtvHeapDesc, IID_PPV_ARGS(m_blurRtvHeap.ReleaseAndGetAddressOf())));

    D3D12_DESCRIPTOR_HEAP_DESC blurPassSrvHeapDesc{};
    blurPassSrvHeapDesc.NumDescriptors = c_blurRenderTextureCount * c_shaderSrvCount;
    blurPassSrvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    blurPassSrvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&blurPassSrvHeapDesc, IID_PPV_ARGS(m_blurPassSrvHeap.ReleaseAndGetAddressOf())));

    D3D12_CLEAR_VALUE blurClearValue{};
    blurClearValue.Format = m_backBufferFormat;
    blurClearValue.Color[3] = 1.0f;

    UINT blurSourceWidth = width;
    UINT blurSourceHeight = height;
    const UINT maxBlurDimension = std::max(blurSourceWidth, blurSourceHeight);
    if (maxBlurDimension > c_maxLegacyBlurSourceDimension)
    {
        blurSourceWidth = std::max<UINT>(
            16u,
            static_cast<UINT>((static_cast<uint64_t>(blurSourceWidth) * c_maxLegacyBlurSourceDimension +
                               (maxBlurDimension / 2u)) /
                              maxBlurDimension));
        blurSourceHeight = std::max<UINT>(
            16u,
            static_cast<UINT>((static_cast<uint64_t>(blurSourceHeight) * c_maxLegacyBlurSourceDimension +
                               (maxBlurDimension / 2u)) /
                              maxBlurDimension));
    }

    UINT blurWidth = blurSourceWidth;
    UINT blurHeight = blurSourceHeight;
    auto blurRtvHandle = m_blurRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT blurIndex = 0; blurIndex < c_blurRenderTextureCount; ++blurIndex)
    {
        if ((blurIndex & 1u) == 0 || blurIndex < 2)
        {
            blurWidth = std::max<UINT>(16u, blurWidth / 2u);
            blurHeight = std::max<UINT>(16u, blurHeight / 2u);
        }

        m_blurTextureWidths[blurIndex] = ((blurWidth + 15u) / 16u) * 16u;
        m_blurTextureHeights[blurIndex] = ((blurHeight + 3u) / 4u) * 4u;

        D3D12_RESOURCE_DESC blurDesc = textureDesc;
        blurDesc.Width = m_blurTextureWidths[blurIndex];
        blurDesc.Height = m_blurTextureHeights[blurIndex];
        blurDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                           D3D12_HEAP_FLAG_NONE,
                                                           &blurDesc,
                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                           &blurClearValue,
                                                           IID_PPV_ARGS(m_blurTextures[blurIndex].ReleaseAndGetAddressOf())));
        m_d3dDevice->CreateRenderTargetView(m_blurTextures[blurIndex].Get(), nullptr, blurRtvHandle);
        blurRtvHandle.ptr += m_rtvDescriptorSize;
    }
    m_blurTexturesPrimed = false;
    {
        wchar_t logLine[192]{};
        swprintf_s(logLine,
                   L"postprocess resources output=%ux%u blur_source=%ux%u blur1=%ux%u blur2=%ux%u blur3=%ux%u",
                   width,
                   height,
                   blurSourceWidth,
                   blurSourceHeight,
                   m_blurTextureWidths[1],
                   m_blurTextureHeights[1],
                   m_blurTextureWidths[3],
                   m_blurTextureHeights[3],
                   m_blurTextureWidths[5],
                   m_blurTextureHeights[5]);
        WriteD3D12LogLine(logLine);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_d3dDevice->CreateShaderResourceView(m_postProcessTexture.Get(), &srvDesc, m_postProcessSrvHeap->GetCPUDescriptorHandleForHeapStart());
    m_d3dDevice->CreateShaderResourceView(m_postProcessTexture.Get(), &srvDesc, m_warpShaderSrvHeap->GetCPUDescriptorHandleForHeapStart());

    auto blurPassSrvHandle = m_blurPassSrvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT blurIndex = 0; blurIndex < c_blurRenderTextureCount; ++blurIndex)
    {
        ID3D12Resource* sourceTexture = blurIndex == 0 ? m_postProcessTexture.Get() : m_blurTextures[blurIndex - 1].Get();
        const D3D12_RESOURCE_DESC sourceDesc = sourceTexture->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC sourceSrvDesc{};
        sourceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sourceSrvDesc.Format = sourceDesc.Format;
        sourceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sourceSrvDesc.Texture2D.MipLevels = 1;
        m_d3dDevice->CreateShaderResourceView(sourceTexture, &sourceSrvDesc, blurPassSrvHandle);
        blurPassSrvHandle.ptr += m_srvDescriptorSize;

        for (UINT unusedDescriptor = 1; unusedDescriptor < c_shaderSrvCount; ++unusedDescriptor)
        {
            m_d3dDevice->CreateShaderResourceView(nullptr, &srvDesc, blurPassSrvHandle);
            blurPassSrvHandle.ptr += m_srvDescriptorSize;
        }
    }
    RefreshPostProcessTextureSrvs();
}

void D3D12Resources::RefreshPostProcessTextureSrvs()
{
    RefreshShaderTextureLayerSrvs(m_postProcessSrvHeap.Get());
    RefreshShaderTextureLayerSrvs(m_warpShaderSrvHeap.Get());
}

void D3D12Resources::RefreshShaderTextureLayerSrvs(ID3D12DescriptorHeap* descriptorHeap)
{
    if (!m_d3dDevice || !descriptorHeap || m_srvDescriptorSize == 0)
    {
        return;
    }

    auto srvHandle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
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

    for (UINT noiseIndex = 0; noiseIndex < c_noiseTextureCount; ++noiseIndex)
    {
        ID3D12Resource* texture = m_noiseTextureSlots[noiseIndex].texture.Get();
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

    for (UINT noiseIndex = 0; noiseIndex < c_noiseVolumeTextureCount; ++noiseIndex)
    {
        ID3D12Resource* texture = m_noiseVolumeTextureSlots[noiseIndex].texture.Get();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srvDesc.Texture3D.MipLevels = 1;

        if (texture)
        {
            const D3D12_RESOURCE_DESC desc = texture->GetDesc();
            srvDesc.Format = desc.Format;
            m_d3dDevice->CreateShaderResourceView(texture, &srvDesc, srvHandle);
        }
        else
        {
            m_d3dDevice->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);
        }
        srvHandle.ptr += m_srvDescriptorSize;
    }

    static constexpr UINT visibleBlurIndices[c_visibleBlurTextureCount] = {1, 3, 5};
    for (UINT blurIndex = 0; blurIndex < c_visibleBlurTextureCount; ++blurIndex)
    {
        ID3D12Resource* texture = m_blurTextures[visibleBlurIndices[blurIndex]].Get();
        if (!texture)
        {
            texture = m_postProcessTexture.Get();
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = m_backBufferFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        if (texture)
        {
            const D3D12_RESOURCE_DESC desc = texture->GetDesc();
            srvDesc.Format = desc.Format;
            m_d3dDevice->CreateShaderResourceView(texture, &srvDesc, srvHandle);
        }
        else
        {
            m_d3dDevice->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);
        }
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
    const DWORD valueLength = GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_POSTPROCESS", value, static_cast<DWORD>(std::size(value)));
    if (valueLength == 0)
    {
        return true;
    }
    return wcscmp(value, L"0") != 0;
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
    if (m_presetTextureOverride || m_standaloneTextureOverride || !IsTextureCyclingEnabled() || m_textureFiles.empty())
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
    srvHeapDesc.NumDescriptors = c_feedbackSrvCount;
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

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &textureDesc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                       nullptr,
                                                       IID_PPV_ARGS(m_feedbackScratchTexture.ReleaseAndGetAddressOf())));
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_d3dDevice->CreateShaderResourceView(m_feedbackScratchTexture.Get(), &srvDesc, srvHandle);
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
        SeedFeedbackTexturesFromResume();
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

bool D3D12Resources::SeedFeedbackTexturesFromResume()
{
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

    if (!m_resumeFeedbackReady ||
        !m_resumeFeedbackTexture ||
        !m_feedbackTextures[0] ||
        !m_feedbackTextures[1] ||
        !m_commandQueue ||
        !m_fence)
    {
        return false;
    }

    const UINT outputWidth = static_cast<UINT>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const UINT outputHeight = static_cast<UINT>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    if (!IsResumeFeedbackCompatible(outputWidth, outputHeight) || !WaitForGpu(1000))
    {
        wchar_t logLine[192]{};
        swprintf_s(logLine, L"resume feedback seed skipped compatible=0 output=%ux%u", outputWidth, outputHeight);
        WriteD3D12LogLine(logLine);
        return false;
    }

    try
    {
        ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
        ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

        D3D12_RESOURCE_BARRIER barriers[3]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = m_resumeFeedbackTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        for (UINT i = 0; i < 2; ++i)
        {
            barriers[i + 1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i + 1].Transition.pResource = m_feedbackTextures[i].Get();
            barriers[i + 1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[i + 1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[i + 1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        m_commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

        m_commandList->CopyResource(m_feedbackTextures[0].Get(), m_resumeFeedbackTexture.Get());
        m_commandList->CopyResource(m_feedbackTextures[1].Get(), m_resumeFeedbackTexture.Get());

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        for (UINT i = 0; i < 2; ++i)
        {
            barriers[i + 1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[i + 1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        m_commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

        ThrowIfFailed(m_commandList->Close());
        ID3D12CommandList* commandLists[] = {m_commandList.Get()};
        m_commandQueue->ExecuteCommandLists(1, commandLists);
        if (!WaitForGpu(1000))
        {
            m_feedbackReady[0] = false;
            m_feedbackReady[1] = false;
            WriteD3D12LogLine(L"resume feedback seed failed waiting for GPU");
            return false;
        }
    }
    catch (...)
    {
        m_feedbackReady[0] = false;
        m_feedbackReady[1] = false;
        WriteD3D12LogLine(L"resume feedback seed failed with exception");
        return false;
    }

    m_feedbackReady[0] = true;
    m_feedbackReady[1] = true;
    m_feedbackIndex = 0;
    {
        wchar_t logLine[192]{};
        swprintf_s(logLine, L"resume feedback seeded both ping-pong buffers size=%ux%u", outputWidth, outputHeight);
        WriteD3D12LogLine(logLine);
    }
    return true;
}

bool D3D12Resources::CaptureBackBufferForResume()
{
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

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
    {
        wchar_t logLine[192]{};
        swprintf_s(logLine, L"captured GPU resume feedback size=%llux%u", sourceDesc.Width, sourceDesc.Height);
        WriteD3D12LogLine(logLine);
    }
    return true;
}

bool D3D12Resources::CaptureCurrentFrame(std::vector<uint8_t>* pixels, UINT* width, UINT* height)
{
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

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
    if (readbackSize == 0 || numRows == 0 || rowSize == 0 ||
        sourceDesc.Width > std::numeric_limits<UINT>::max() ||
        sourceDesc.Height > std::numeric_limits<UINT>::max() ||
        rowSize > std::numeric_limits<UINT>::max() ||
        layout.Offset > readbackSize ||
        readbackSize > static_cast<UINT64>(std::numeric_limits<SIZE_T>::max()))
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
    if (sourceRowPitch > std::numeric_limits<size_t>::max() / capturedHeight)
    {
        return false;
    }
    std::vector<uint8_t> captured(static_cast<size_t>(capturedHeight) * sourceRowPitch);

    const uint8_t* mapped = nullptr;
    const SIZE_T readOffset = static_cast<SIZE_T>(layout.Offset);
    D3D12_RANGE readRange{readOffset, static_cast<SIZE_T>(readbackSize)};
    ThrowIfFailed(readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped))));
    for (UINT row = 0; row < capturedHeight; ++row)
    {
        memcpy(captured.data() + static_cast<size_t>(row) * sourceRowPitch,
               mapped + readOffset + static_cast<size_t>(row) * layout.Footprint.RowPitch,
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
    std::lock_guard<std::recursive_mutex> commandLock(m_commandMutex);

    ClearResumeFeedback();

    if (width == 0 || height == 0 || pixels.empty() || !m_d3dDevice || !m_commandQueue || !m_fence)
    {
        return false;
    }

    const UINT outputWidth = static_cast<UINT>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const UINT outputHeight = static_cast<UINT>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));
    const UINT bytesPerPixel = 4;
    const UINT sourceRowPitch = width * bytesPerPixel;
    if (pixels.size() < static_cast<size_t>(sourceRowPitch) * height)
    {
        return false;
    }

    UINT uploadWidth = width;
    UINT uploadHeight = height;
    const uint8_t* uploadPixels = pixels.data();
    std::vector<uint8_t> resizedPixels;
    if (width != outputWidth || height != outputHeight)
    {
        uploadWidth = outputWidth;
        uploadHeight = outputHeight;
        resizedPixels.resize(static_cast<size_t>(uploadWidth) * uploadHeight * bytesPerPixel);
        {
            wchar_t logLine[192]{};
            swprintf_s(logLine, L"resizing CPU resume feedback %ux%u -> %ux%u", width, height, uploadWidth, uploadHeight);
            WriteD3D12LogLine(logLine);
        }

        const float scaleX = static_cast<float>(width) / static_cast<float>(uploadWidth);
        const float scaleY = static_cast<float>(height) / static_cast<float>(uploadHeight);
        for (UINT y = 0; y < uploadHeight; ++y)
        {
            const float srcY = std::clamp((static_cast<float>(y) + 0.5f) * scaleY - 0.5f, 0.0f, static_cast<float>(height - 1));
            const UINT y0 = static_cast<UINT>(srcY);
            const UINT y1 = std::min<UINT>(y0 + 1, height - 1);
            const float fy = srcY - static_cast<float>(y0);

            for (UINT x = 0; x < uploadWidth; ++x)
            {
                const float srcX = std::clamp((static_cast<float>(x) + 0.5f) * scaleX - 0.5f, 0.0f, static_cast<float>(width - 1));
                const UINT x0 = static_cast<UINT>(srcX);
                const UINT x1 = std::min<UINT>(x0 + 1, width - 1);
                const float fx = srcX - static_cast<float>(x0);

                const uint8_t* c00 = pixels.data() + static_cast<size_t>(y0) * sourceRowPitch + static_cast<size_t>(x0) * bytesPerPixel;
                const uint8_t* c10 = pixels.data() + static_cast<size_t>(y0) * sourceRowPitch + static_cast<size_t>(x1) * bytesPerPixel;
                const uint8_t* c01 = pixels.data() + static_cast<size_t>(y1) * sourceRowPitch + static_cast<size_t>(x0) * bytesPerPixel;
                const uint8_t* c11 = pixels.data() + static_cast<size_t>(y1) * sourceRowPitch + static_cast<size_t>(x1) * bytesPerPixel;
                uint8_t* dst = resizedPixels.data() + (static_cast<size_t>(y) * uploadWidth + x) * bytesPerPixel;

                for (UINT channel = 0; channel < bytesPerPixel; ++channel)
                {
                    const float top = static_cast<float>(c00[channel]) + (static_cast<float>(c10[channel]) - static_cast<float>(c00[channel])) * fx;
                    const float bottom = static_cast<float>(c01[channel]) + (static_cast<float>(c11[channel]) - static_cast<float>(c01[channel])) * fx;
                    dst[channel] = static_cast<uint8_t>(std::clamp(top + (bottom - top) * fy, 0.0f, 255.0f) + 0.5f);
                }
            }
        }
        uploadPixels = resizedPixels.data();
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = uploadWidth;
    textureDesc.Height = uploadHeight;
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
    const UINT uploadRowPitch = uploadWidth * bytesPerPixel;
    for (UINT row = 0; row < uploadHeight; ++row)
    {
        memcpy(mapped + layout.Offset + static_cast<size_t>(row) * layout.Footprint.RowPitch,
               uploadPixels + static_cast<size_t>(row) * uploadRowPitch,
               uploadRowPitch);
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
        srvHandle.ptr += static_cast<SIZE_T>(c_resumeFeedbackSrvIndex) * m_feedbackSrvDescriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_d3dDevice->CreateShaderResourceView(m_resumeFeedbackTexture.Get(), &srvDesc, srvHandle);
    }

    m_resumeFeedbackReady = true;
    SeedFeedbackTexturesFromResume();
    {
        wchar_t logLine[192]{};
        swprintf_s(logLine, L"installed CPU resume feedback size=%ux%u source=%ux%u", uploadWidth, uploadHeight, width, height);
        WriteD3D12LogLine(logLine);
    }
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

void D3D12Resources::ClearBlurTexturesIfNeeded()
{
    if (m_blurTexturesPrimed || !m_blurRtvHeap)
    {
        return;
    }

    const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    auto rtvHandle = m_blurRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT blurIndex = 0; blurIndex < c_blurRenderTextureCount; ++blurIndex)
    {
        ID3D12Resource* blurTexture = m_blurTextures[blurIndex].Get();
        if (!blurTexture)
        {
            rtvHandle.ptr += m_rtvDescriptorSize;
            continue;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = blurTexture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);

        m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_commandList->ResourceBarrier(1, &barrier);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    m_blurTexturesPrimed = true;
}

bool D3D12Resources::RenderBlurTextures(float blurEdgeDarken)
{
    if (!m_blurRtvHeap ||
        !m_blurPassSrvHeap ||
        !m_blurHorizontalPipelineState ||
        !m_blurVerticalPipelineState ||
        !m_postProcessRootSignature ||
        !m_blurConstantBuffer ||
        !m_mappedBlurConstantBuffer ||
        !m_mappedTextureVertices)
    {
        return false;
    }

    for (UINT blurIndex = 0; blurIndex < c_blurRenderTextureCount; ++blurIndex)
    {
        if (!m_blurTextures[blurIndex])
        {
            return false;
        }
    }

    if (m_textureVertexCursor + c_blurRenderTextureCount * 4u > c_maxTextureVertices)
    {
        return false;
    }

    const float weights[8] = {4.0f, 3.8f, 3.5f, 2.9f, 1.9f, 1.2f, 0.7f, 0.3f};
    auto safeRange = [](float minValue, float maxValue) {
        minValue = std::isfinite(minValue) ? minValue : 0.0f;
        maxValue = std::isfinite(maxValue) ? maxValue : 1.0f;
        if (maxValue - minValue < 0.00001f)
        {
            const float center = (minValue + maxValue) * 0.5f;
            minValue = center - 0.000005f;
            maxValue = center + 0.000005f;
        }
        return std::array<float, 2>{minValue, maxValue};
    };

    const auto range0 = safeRange(m_presetShaderBlurMin[0], m_presetShaderBlurMax[0]);
    const auto range1 = safeRange(m_presetShaderBlurMin[1], m_presetShaderBlurMax[1]);
    const auto range2 = safeRange(m_presetShaderBlurMin[2], m_presetShaderBlurMax[2]);
    const float blurMin[3] = {range0[0], range1[0], range2[0]};
    const float blurMax[3] = {range0[1], range1[1], range2[1]};

    float fscale[3]{};
    float fbias[3]{};
    fscale[0] = 1.0f / (blurMax[0] - blurMin[0]);
    fbias[0] = -blurMin[0] * fscale[0];
    float tempMin = (blurMin[1] - blurMin[0]) / (blurMax[0] - blurMin[0]);
    float tempMax = (blurMax[1] - blurMin[0]) / (blurMax[0] - blurMin[0]);
    if (tempMax - tempMin < 0.00001f)
    {
        const float center = (tempMin + tempMax) * 0.5f;
        tempMin = center - 0.000005f;
        tempMax = center + 0.000005f;
    }
    fscale[1] = 1.0f / (tempMax - tempMin);
    fbias[1] = -tempMin * fscale[1];
    tempMin = (blurMin[2] - blurMin[1]) / (blurMax[1] - blurMin[1]);
    tempMax = (blurMax[2] - blurMin[1]) / (blurMax[1] - blurMin[1]);
    if (tempMax - tempMin < 0.00001f)
    {
        const float center = (tempMin + tempMax) * 0.5f;
        tempMin = center - 0.000005f;
        tempMax = center + 0.000005f;
    }
    fscale[2] = 1.0f / (tempMax - tempMin);
    fbias[2] = -tempMin * fscale[2];

    const UINT outputWidth = std::max<UINT>(static_cast<UINT>(m_outputSize.right - m_outputSize.left), 1u);
    const UINT outputHeight = std::max<UINT>(static_cast<UINT>(m_outputSize.bottom - m_outputSize.top), 1u);
    auto rtvHandle = m_blurRtvHeap->GetCPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap* heaps[] = {m_blurPassSrvHeap.Get()};

    for (UINT blurIndex = 0; blurIndex < c_blurRenderTextureCount; ++blurIndex)
    {
        const UINT sourceWidth = blurIndex == 0 ? outputWidth : m_blurTextureWidths[blurIndex - 1];
        const UINT sourceHeight = blurIndex == 0 ? outputHeight : m_blurTextureHeights[blurIndex - 1];
        const UINT targetWidth = std::max<UINT>(m_blurTextureWidths[blurIndex], 1u);
        const UINT targetHeight = std::max<UINT>(m_blurTextureHeights[blurIndex], 1u);
        const UINT blurGroup = std::min<UINT>(blurIndex / 2u, 2u);

        std::array<float, 512> constants{};
        constants[0] = static_cast<float>(sourceWidth);
        constants[1] = static_cast<float>(sourceHeight);
        constants[2] = 1.0f / static_cast<float>(sourceWidth);
        constants[3] = 1.0f / static_cast<float>(sourceHeight);

        ID3D12PipelineState* pipelineState = nullptr;
        if ((blurIndex & 1u) == 0)
        {
            const float w1 = weights[0] + weights[1];
            const float w2 = weights[2] + weights[3];
            const float w3 = weights[4] + weights[5];
            const float w4 = weights[6] + weights[7];
            constants[4] = w1;
            constants[5] = w2;
            constants[6] = w3;
            constants[7] = w4;
            constants[8] = 2.0f * weights[1] / w1;
            constants[9] = 2.0f + 2.0f * weights[3] / w2;
            constants[10] = 4.0f + 2.0f * weights[5] / w3;
            constants[11] = 6.0f + 2.0f * weights[7] / w4;
            constants[12] = fscale[blurGroup];
            constants[13] = fbias[blurGroup];
            constants[14] = 0.5f / (w1 + w2 + w3 + w4);
            pipelineState = m_blurHorizontalPipelineState.Get();
        }
        else
        {
            const float w1 = weights[0] + weights[1] + weights[2] + weights[3];
            const float w2 = weights[4] + weights[5] + weights[6] + weights[7];
            constants[16] = w1;
            constants[17] = w2;
            constants[18] = 2.0f * ((weights[2] + weights[3]) / w1);
            constants[19] = 2.0f + 2.0f * ((weights[6] + weights[7]) / w2);
            constants[20] = 1.0f / ((w1 + w2) * 2.0f);
            constants[21] = blurIndex == 1 ? 1.0f - std::clamp(blurEdgeDarken, 0.0f, 1.0f) : 1.0f;
            constants[22] = blurIndex == 1 ? std::clamp(blurEdgeDarken, 0.0f, 1.0f) : 0.0f;
            constants[23] = 5.0f;
            pipelineState = m_blurVerticalPipelineState.Get();
        }
        const SIZE_T blurConstantOffset = static_cast<SIZE_T>(blurIndex) * c_postProcessConstantBufferSize;
        memcpy(m_mappedBlurConstantBuffer + blurConstantOffset, constants.data(), sizeof(constants));

        const UINT vertexStart = m_textureVertexCursor;
        m_textureVertexCursor += 4;
        const TextureVertex vertices[] = {
            {{-1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.70710678f, -2.35619449f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{1.0f, 1.0f}, {1.0f, 0.0f, 1.0f, 0.0f}, {0.70710678f, -0.78539816f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{-1.0f, -1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.70710678f, 2.35619449f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.70710678f, 0.78539816f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        };
        memcpy(m_mappedTextureVertices + vertexStart, vertices, sizeof(vertices));

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_blurTextures[blurIndex].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);

        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(targetWidth);
        viewport.Height = static_cast<float>(targetHeight);
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor{};
        scissor.right = static_cast<LONG>(targetWidth);
        scissor.bottom = static_cast<LONG>(targetHeight);

        auto targetRtv = rtvHandle;
        targetRtv.ptr += static_cast<SIZE_T>(blurIndex) * m_rtvDescriptorSize;
        m_commandList->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_commandList->SetPipelineState(pipelineState);
        m_commandList->SetGraphicsRootSignature(m_postProcessRootSignature.Get());
        auto srvHandle = m_blurPassSrvHeap->GetGPUDescriptorHandleForHeapStart();
        srvHandle.ptr += static_cast<SIZE_T>(blurIndex) * static_cast<SIZE_T>(c_shaderSrvCount) * m_srvDescriptorSize;
        m_commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
        m_commandList->SetGraphicsRootConstantBufferView(1, m_blurConstantBuffer->GetGPUVirtualAddress() + blurConstantOffset);
        m_commandList->IASetVertexBuffers(0, 1, &m_textureVertexBufferView);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_commandList->DrawInstanced(4, 1, vertexStart, 0);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_commandList->ResourceBarrier(1, &barrier);
    }

    m_blurTexturesPrimed = true;
    return true;
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
                                     float motionWarp,
                                     float baseLayerAlpha,
                                     float extraLayerAlphaScale,
                                     bool textureWrap)
{
    if (!m_srvHeap || !m_texturePipelineState || !m_textureClampPipelineState || !m_textureRootSignature)
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
        const float layerAlpha = layer == 0 ? std::clamp(baseLayerAlpha, 0.0f, 1.0f) : std::clamp((0.42f * extraLayerAlphaScale) / static_cast<float>(layer + 1), 0.05f, 0.42f);
        const float layerZoom = 1.0f + static_cast<float>(layer) * 0.08f;
        const float layerAngle = static_cast<float>(layer) * 0.11f;
        DrawTextureQuadFromSrv(srvHandle, m_srvHeap.Get(), bass, mids, treble, decay, zoom, rot, layerAlpha, layerZoom, layerAngle, false, false, motionCenterX, motionCenterY, motionDX, motionDY, motionStretchX, motionStretchY, motionWarp, false, false, textureWrap);
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
                                            bool additive,
                                            bool textureWrap)
{
    ID3D12PipelineState* pipelineState = additive ?
        (textureWrap ? m_textureAdditivePipelineState.Get() : m_textureAdditiveClampPipelineState.Get()) :
        (textureWrap ? m_textureAlphaPipelineState.Get() : m_textureAlphaClampPipelineState.Get());
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
    const bool reactiveStandaloneTexture = !applyDecayTint && !additive;
    const float cleanBass = std::clamp(bass, 0.0f, 3.0f);
    const float cleanMids = std::clamp(mids, 0.0f, 3.0f);
    const float cleanTreble = std::clamp(treble, 0.0f, 3.0f);
    const float audioEnergy = std::clamp(cleanBass * 0.52f + cleanMids * 0.30f + cleanTreble * 0.18f, 0.0f, 3.0f);
    const float audioPulse = reactiveStandaloneTexture ? std::clamp(audioEnergy - 0.70f, 0.0f, 1.60f) : 0.0f;
    const float textureZoom = std::clamp((1.0f / std::max(zoom, 0.25f)) * zoomBias * (1.0f + audioPulse * 0.035f), 0.55f, 1.85f);
    const float angle = rot * 0.35f + angleBias + (reactiveStandaloneTexture ? (cleanTreble - cleanMids) * 0.010f : 0.0f);
    const float c = cosf(angle);
    const float s = sinf(angle);
    const float warpTime = static_cast<float>(GetTickCount64() % 600000ULL) * 0.001f;
    const float alpha = std::clamp(alphaScale, 0.0f, 1.0f);
    const float tint = applyDecayTint ? std::clamp(decay, 0.0f, 1.0f) : std::clamp(0.88f + audioPulse * 0.08f, 0.88f, 1.0f);
    const float reactiveWarpAmount = reactiveStandaloneTexture ? (0.0025f + audioPulse * 0.010f + cleanTreble * 0.002f) : 0.0f;

    auto setVertex = [&](UINT index, float x, float y) {
        TextureVertex& vertex = m_mappedTextureVertices[vertexStart + index];
        vertex.position[0] = x;
        vertex.position[1] = y;
        float u = 0.5f + x * 0.5f * textureZoom;
        float v = 0.5f + y * 0.5f * textureZoom;

        u = (u - motionCenterX) / motionStretchX + motionCenterX;
        v = (v - motionCenterY) / motionStretchY + motionCenterY;

        const float warpAmount = motionWarp * 0.0035f + reactiveWarpAmount;
        if (fabsf(warpAmount) > 0.0001f)
        {
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
        vertex.uv[2] = vertex.uv[0];
        vertex.uv[3] = vertex.uv[1];
        StoreTextureRadAng(vertex.radAng, vertex.uv[0], vertex.uv[1]);
        vertex.color[0] = tint;
        vertex.color[1] = tint;
        vertex.color[2] = tint;
        vertex.color[3] = std::clamp(alpha * (reactiveStandaloneTexture ? (0.86f + audioPulse * 0.10f) : 1.0f), 0.0f, 1.0f);
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
    customShapeCount = std::min<size_t>(customShapeCount, c_maxCustomShapeCommands);

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
        vertex.uv[2] = u;
        vertex.uv[3] = v;
        StoreTextureRadAng(vertex.radAng, u, v);
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
                                            bool additive,
                                            bool usePresetWarpShader,
                                            ID3D12Resource* shaderSourceTexture,
                                            bool textureWrap)
{
    const bool useWarpShader =
        usePresetWarpShader &&
        !additive &&
        m_presetWarpPipelineState &&
        m_warpShaderSrvHeap &&
        shaderSourceTexture &&
        m_postProcessRootSignature &&
        m_postProcessConstantBuffer &&
        m_mappedPostProcessConstantBuffer;
    ID3D12PipelineState* fixedPipelineState = additive ?
        (textureWrap ? m_textureAdditivePipelineState.Get() : m_textureAdditiveClampPipelineState.Get()) :
        (textureWrap ? m_textureAlphaPipelineState.Get() : m_textureAlphaClampPipelineState.Get());
    ID3D12PipelineState* pipelineState = useWarpShader ? m_presetWarpPipelineState.Get() : fixedPipelineState;
    ID3D12RootSignature* rootSignature = useWarpShader ? m_postProcessRootSignature.Get() : m_textureRootSignature.Get();
    if (!descriptorHeap || !vertices || vertexCount < 3 || !pipelineState || !rootSignature || !m_mappedTextureVertices)
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

    const bool reactiveStandaloneTexture = !applyDecayTint && !additive && !useWarpShader;
    const float cleanBass = std::clamp(bass, 0.0f, 3.0f);
    const float cleanMids = std::clamp(mids, 0.0f, 3.0f);
    const float cleanTreble = std::clamp(treble, 0.0f, 3.0f);
    const float audioEnergy = std::clamp(cleanBass * 0.52f + cleanMids * 0.30f + cleanTreble * 0.18f, 0.0f, 3.0f);
    const float audioPulse = reactiveStandaloneTexture ? std::clamp(audioEnergy - 0.70f, 0.0f, 1.60f) : 0.0f;
    const float warpTime = static_cast<float>(GetTickCount64() % 600000ULL) * 0.001f;
    const float reactiveWarpAmount = reactiveStandaloneTexture ? (0.0015f + audioPulse * 0.006f + cleanTreble * 0.0015f) : 0.0f;
    const float alpha = std::clamp(alphaScale, 0.0f, 1.0f);
    const float tint = applyDecayTint ? std::clamp(decay, 0.0f, 1.0f) : std::clamp(0.90f + audioPulse * 0.06f, 0.90f, 1.0f);
    const float alphaPulse = reactiveStandaloneTexture ? std::clamp(0.86f + audioPulse * 0.10f, 0.86f, 1.0f) : 1.0f;

    for (UINT index = 0; index < copiedVertexCount; ++index)
    {
        const TextureWarpVertex& source = vertices[index];
        TextureVertex& vertex = m_mappedTextureVertices[vertexStart + index];
        vertex.position[0] = source.x;
        vertex.position[1] = source.y;
        float u = source.u;
        float v = source.v;
        if (reactiveWarpAmount > 0.0001f)
        {
            u += reactiveWarpAmount * sinf(warpTime * 0.47f + source.x * 5.9f - source.y * 3.1f);
            v += reactiveWarpAmount * cosf(warpTime * 0.53f - source.x * 4.3f + source.y * 6.7f);
        }
        vertex.uv[0] = u;
        vertex.uv[1] = v;
        vertex.uv[2] = std::isfinite(source.uOrig) ? source.uOrig : source.u;
        vertex.uv[3] = std::isfinite(source.vOrig) ? source.vOrig : source.v;
        StoreTextureRadAng(vertex.radAng, source.rad, source.ang, u, v);
        vertex.color[0] = std::clamp(source.r * tint, 0.0f, 1.0f);
        vertex.color[1] = std::clamp(source.g * tint, 0.0f, 1.0f);
        vertex.color[2] = std::clamp(source.b * tint, 0.0f, 1.0f);
        vertex.color[3] = std::clamp(source.a * alpha * alphaPulse, 0.0f, 1.0f);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE activeSrvHandle = srvHandle;
    ID3D12DescriptorHeap* activeDescriptorHeap = descriptorHeap;
    if (useWarpShader)
    {
        const D3D12_RESOURCE_DESC sourceDesc = shaderSourceTexture->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = sourceDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_d3dDevice->CreateShaderResourceView(shaderSourceTexture, &srvDesc, m_warpShaderSrvHeap->GetCPUDescriptorHandleForHeapStart());
        activeDescriptorHeap = m_warpShaderSrvHeap.Get();
        activeSrvHandle = m_warpShaderSrvHeap->GetGPUDescriptorHandleForHeapStart();
    }

    ID3D12DescriptorHeap* heaps[] = {activeDescriptorHeap};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetPipelineState(pipelineState);
    m_commandList->SetGraphicsRootSignature(rootSignature);
    m_commandList->SetGraphicsRootDescriptorTable(0, activeSrvHandle);
    if (useWarpShader)
    {
        UpdatePresetShaderConstantBuffer();
        m_commandList->SetGraphicsRootConstantBufferView(1, m_postProcessConstantBuffer->GetGPUVirtualAddress());
    }
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
                                            float blurEdgeDarken,
                                            float echoAlpha,
                                            float echoZoom,
                                            int echoOrientation,
                                            const TextureWarpVertex* compositeVertices,
                                            size_t compositeVertexCount)
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

    const float width = static_cast<float>(std::max<long>(m_outputSize.right - m_outputSize.left, 1));
    const float height = static_cast<float>(std::max<long>(m_outputSize.bottom - m_outputSize.top, 1));

    std::array<float, 512> constants{};
    const bool usePresetComposite = m_presetCompositePipelineState != nullptr;
    const bool useCompositeMesh = usePresetComposite && compositeVertices && compositeVertexCount >= 3;
    const size_t requestedVertexCount = useCompositeMesh ? compositeVertexCount : 4;
    const UINT copiedVertexCount = static_cast<UINT>(std::min<size_t>(requestedVertexCount, c_maxTextureVertices - m_textureVertexCursor));
    if ((!useCompositeMesh && copiedVertexCount < 4) || (useCompositeMesh && copiedVertexCount < 3))
    {
        return;
    }

    const UINT vertexStart = m_textureVertexCursor;
    m_textureVertexCursor += copiedVertexCount;
    if (usePresetComposite)
    {
        static ULONGLONG s_lastPresetCompositeDrawLogTick = 0;
        const ULONGLONG nowTick = GetTickCount64();
        if (nowTick - s_lastPresetCompositeDrawLogTick >= 1000)
        {
            s_lastPresetCompositeDrawLogTick = nowTick;
            wchar_t logLine[1536]{};
            swprintf_s(logLine,
                       L"preset composite draw use_mesh=%d requested=%llu copied=%u vertex_start=%u output=%ldx%ld q3=%.4f q4=%.4f q10=%.4f q11=%.6f q12=%.6f "
                       L"blur1=%ux%u blur2=%ux%u blur3=%ux%u",
                       useCompositeMesh ? 1 : 0,
                       static_cast<unsigned long long>(requestedVertexCount),
                       copiedVertexCount,
                       vertexStart,
                       m_outputSize.right - m_outputSize.left,
                       m_outputSize.bottom - m_outputSize.top,
                       m_presetShaderQ[2],
                       m_presetShaderQ[3],
                       m_presetShaderQ[9],
                       m_presetShaderQ[10],
                       m_presetShaderQ[11],
                       m_blurTextureWidths[1],
                       m_blurTextureHeights[1],
                       m_blurTextureWidths[3],
                       m_blurTextureHeights[3],
                       m_blurTextureWidths[5],
                       m_blurTextureHeights[5]);
            WriteD3D12LogLine(logLine);
        }
    }
    if (useCompositeMesh)
    {
        for (UINT index = 0; index < copiedVertexCount; ++index)
        {
            const TextureWarpVertex& source = compositeVertices[index];
            TextureVertex& vertex = m_mappedTextureVertices[vertexStart + index];
            vertex.position[0] = source.x;
            vertex.position[1] = source.y;
            vertex.uv[0] = source.u;
            vertex.uv[1] = source.v;
            vertex.uv[2] = std::isfinite(source.uOrig) ? source.uOrig : source.u;
            vertex.uv[3] = std::isfinite(source.vOrig) ? source.vOrig : source.v;
            StoreTextureRadAng(vertex.radAng, source.rad, source.ang, source.u, source.v);
            vertex.color[0] = std::clamp(source.r, 0.0f, 1.0f);
            vertex.color[1] = std::clamp(source.g, 0.0f, 1.0f);
            vertex.color[2] = std::clamp(source.b, 0.0f, 1.0f);
            vertex.color[3] = std::clamp(source.a, 0.0f, 1.0f);
        }
    }
    else
    {
        const TextureVertex vertices[] = {
            {{-1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.70710678f, -2.35619449f}, {hueColors[3], hueColors[4], hueColors[5], 1.0f}},
            {{1.0f, 1.0f}, {1.0f, 0.0f, 1.0f, 0.0f}, {0.70710678f, -0.78539816f}, {hueColors[0], hueColors[1], hueColors[2], 1.0f}},
            {{-1.0f, -1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.70710678f, 2.35619449f}, {hueColors[9], hueColors[10], hueColors[11], 1.0f}},
            {{1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.70710678f, 0.78539816f}, {hueColors[6], hueColors[7], hueColors[8], 1.0f}},
        };
        memcpy(m_mappedTextureVertices + vertexStart, vertices, sizeof(vertices));
    }
    if (usePresetComposite)
    {
        auto write4 = [&](size_t offset, float x, float y, float z, float w) {
            constants[offset + 0] = x;
            constants[offset + 1] = y;
            constants[offset + 2] = z;
            constants[offset + 3] = w;
        };
        auto writeTextureLayerTexsizes = [&]() {
            static constexpr size_t firstLayerTexsizeOffset = 384;
            for (UINT layer = 0; layer < c_maxTextureLayers; ++layer)
            {
                const TextureSlot& slot = m_textureSlots[layer];
                const float texWidth = static_cast<float>(std::max<UINT>(slot.width, 1u));
                const float texHeight = static_cast<float>(std::max<UINT>(slot.height, 1u));
                write4(firstLayerTexsizeOffset + static_cast<size_t>(layer) * 4,
                       texWidth,
                       texHeight,
                       1.0f / texWidth,
                       1.0f / texHeight);
            }
        };
        auto writeBlurTexsizes = [&]() {
            static constexpr size_t firstBlurTexsizeOffset = 448;
            static constexpr UINT visibleBlurIndices[c_visibleBlurTextureCount] = {1, 3, 5};
            for (UINT blurIndex = 0; blurIndex < c_visibleBlurTextureCount; ++blurIndex)
            {
                const UINT textureIndex = visibleBlurIndices[blurIndex];
                const float texWidth = static_cast<float>(std::max<UINT>(m_blurTextureWidths[textureIndex], 1u));
                const float texHeight = static_cast<float>(std::max<UINT>(m_blurTextureHeights[textureIndex], 1u));
                write4(firstBlurTexsizeOffset + static_cast<size_t>(blurIndex) * 4,
                       texWidth,
                       texHeight,
                       1.0f / texWidth,
                       1.0f / texHeight);
            }
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
        const float presetTime = clean(m_presetShaderPresetTime, 0.0f);
        const float globalTime = clean(m_presetShaderGlobalTime, presetTime);
        const float canvasWidth = std::max(clean(m_presetShaderCanvasWidth, 1.0f), 1.0f);
        const float canvasHeight = std::max(clean(m_presetShaderCanvasHeight, 1.0f), 1.0f);

        write4(0, m_presetShaderRandFrame[0], m_presetShaderRandFrame[1], m_presetShaderRandFrame[2], m_presetShaderRandFrame[3]);
        write4(4, m_presetShaderRandPreset[0], m_presetShaderRandPreset[1], m_presetShaderRandPreset[2], m_presetShaderRandPreset[3]);
        write4(8, aspectX, aspectY, aspectX > 0.0001f ? 1.0f / aspectX : 1.0f, aspectY > 0.0001f ? 1.0f / aspectY : 1.0f);
        write4(12, 0.0f, 0.0f, 0.0f, 0.0f);
        write4(16, presetTime, m_presetShaderFps, m_presetShaderFrame, m_presetShaderProgress);
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
        write4(36, canvasWidth, canvasHeight, 1.0f / canvasWidth, 1.0f / canvasHeight);
        write4(40, 0.5f + 0.5f * cosf(globalTime * 0.329f + 1.2f),
                   0.5f + 0.5f * cosf(globalTime * 1.293f + 3.9f),
                   0.5f + 0.5f * cosf(globalTime * 5.070f + 2.5f),
                   0.5f + 0.5f * cosf(globalTime * 20.051f + 5.4f));
        write4(44, 0.5f + 0.5f * sinf(globalTime * 0.329f + 1.2f),
                   0.5f + 0.5f * sinf(globalTime * 1.293f + 3.9f),
                   0.5f + 0.5f * sinf(globalTime * 5.070f + 2.5f),
                   0.5f + 0.5f * sinf(globalTime * 20.051f + 5.4f));
        write4(48, 0.5f + 0.5f * cosf(globalTime * 0.0050f + 2.7f),
                   0.5f + 0.5f * cosf(globalTime * 0.0085f + 5.3f),
                   0.5f + 0.5f * cosf(globalTime * 0.0133f + 4.5f),
                   0.5f + 0.5f * cosf(globalTime * 0.0217f + 3.8f));
        write4(52, 0.5f + 0.5f * sinf(globalTime * 0.0050f + 2.7f),
                   0.5f + 0.5f * sinf(globalTime * 0.0085f + 5.3f),
                   0.5f + 0.5f * sinf(globalTime * 0.0133f + 4.5f),
                   0.5f + 0.5f * sinf(globalTime * 0.0217f + 3.8f));
        const float mipX = log2f(safeWidth);
        const float mipY = log2f(safeHeight);
        write4(56, mipX, mipY, (mipX + mipY) * 0.5f, 0.0f);
        write4(60, m_presetShaderBlurMin[1], m_presetShaderBlurMax[1], m_presetShaderBlurMin[2], m_presetShaderBlurMax[2]);
        std::copy_n(m_presetShaderQ, std::size(m_presetShaderQ), constants.data() + 64);
        std::copy_n(m_presetShaderRotMatrices, std::size(m_presetShaderRotMatrices), constants.data() + 96);
        writeTextureLayerTexsizes();
        writeBlurTexsizes();
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
        constants[10] = std::clamp(echoAlpha, 0.0f, 1.0f);
        constants[11] = std::clamp(echoZoom, 0.001f, 1000.0f);
        constants[12] = hueColors[0];
        constants[13] = hueColors[1];
        constants[14] = hueColors[2];
        constants[15] = static_cast<float>(((echoOrientation % 4) + 4) % 4);
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
    m_commandList->IASetPrimitiveTopology(useCompositeMesh ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_commandList->DrawInstanced(useCompositeMesh ? copiedVertexCount - copiedVertexCount % 3u : 4u, 1, vertexStart, 0);
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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pointPsoDesc = psoDesc;
    pointPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&pointPsoDesc, IID_PPV_ARGS(m_pointPipelineState.ReleaseAndGetAddressOf())));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pointAdditivePsoDesc = pointPsoDesc;
    pointAdditivePsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    pointAdditivePsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&pointAdditivePsoDesc, IID_PPV_ARGS(m_pointAdditivePipelineState.ReleaseAndGetAddressOf())));

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

    bufferDesc.Width = sizeof(WaveformVertex) * c_maxTextOverlayVertices;
    for (UINT i = 0; i < m_backBufferCount; ++i)
    {
        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps,
                                                           D3D12_HEAP_FLAG_NONE,
                                                           &bufferDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                                           nullptr,
                                                           IID_PPV_ARGS(m_textOverlayVertexBuffers[i].ReleaseAndGetAddressOf())));
        ThrowIfFailed(m_textOverlayVertexBuffers[i]->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedTextOverlayVertexBuffers[i])));

        m_textOverlayVertexBufferViews[i].BufferLocation = m_textOverlayVertexBuffers[i]->GetGPUVirtualAddress();
        m_textOverlayVertexBufferViews[i].StrideInBytes = sizeof(WaveformVertex);
        m_textOverlayVertexBufferViews[i].SizeInBytes = sizeof(WaveformVertex) * c_maxTextOverlayVertices;
    }

    m_mappedTextOverlayVertices = m_mappedTextOverlayVertexBuffers[m_frameIndex];
    m_textOverlayVertexBufferView = m_textOverlayVertexBufferViews[m_frameIndex];
}
} // namespace DX
