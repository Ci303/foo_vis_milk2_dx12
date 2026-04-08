/*
 * render_backend_dx12.cpp - Experimental DX12 backend probe implementation.
 */

#include "pch.h"
#include "render_backend.h"

namespace milkdrop::dx12
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT g_frame_count = 2;

std::wstring to_wstring(HRESULT hr)
{
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

std::wstring feature_level_to_string(D3D_FEATURE_LEVEL level)
{
    switch (level)
    {
        case D3D_FEATURE_LEVEL_12_2:
            return L"12_2";
        case D3D_FEATURE_LEVEL_12_1:
            return L"12_1";
        case D3D_FEATURE_LEVEL_12_0:
            return L"12_0";
        case D3D_FEATURE_LEVEL_11_1:
            return L"11_1";
        case D3D_FEATURE_LEVEL_11_0:
            return L"11_0";
        default:
            return L"unknown";
    }
}

class render_backend_dx12 final : public render_backend
{
  public:
    backend_probe_result probe() override
    {
        backend_probe_result result;

        UINT factoryFlags = 0;
#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.ReleaseAndGetAddressOf()))))
        {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif

        ComPtr<IDXGIFactory6> factory;
        HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            result.status_message = L"CreateDXGIFactory2 failed: " + to_wstring(hr);
            return result;
        }
        result.factory_created = true;

        BOOL allowTearing = FALSE;
        hr = factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
        result.tearing_supported = SUCCEEDED(hr) && allowTearing;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;; ++index)
        {
            DXGI_ADAPTER_DESC1 desc{};
            hr = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf()));
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr))
            {
                result.status_message = L"EnumAdapterByGpuPreference failed: " + to_wstring(hr);
                return result;
            }

            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                continue;

            result.adapter_name = desc.Description;
            result.adapter_selected = true;
            break;
        }

        if (!result.adapter_selected)
        {
            result.status_message = L"No hardware DXGI adapter supporting the DX12 path was selected.";
            return result;
        }

        static constexpr D3D_FEATURE_LEVEL requestedLevels[] = {
            D3D_FEATURE_LEVEL_12_2,
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        ComPtr<ID3D12Device> device;
        D3D_FEATURE_LEVEL chosenLevel = D3D_FEATURE_LEVEL_11_0;
        bool createdDevice = false;
        for (const auto level : requestedLevels)
        {
            hr = D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(device.ReleaseAndGetAddressOf()));
            if (SUCCEEDED(hr))
            {
                chosenLevel = level;
                createdDevice = true;
                break;
            }
        }

        if (!createdDevice)
        {
            result.status_message = L"D3D12CreateDevice failed on the selected adapter: " + to_wstring(hr);
            return result;
        }
        result.device_created = true;
        result.feature_level = feature_level_to_string(chosenLevel);

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        ComPtr<ID3D12CommandQueue> queue;
        hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            result.status_message = L"CreateCommandQueue failed: " + to_wstring(hr);
            return result;
        }
        result.command_queue_created = true;

        result.status_message = L"DX12 backend probe succeeded.";
        return result;
    }

    bool initialize_window(HWND window, viewport_size size, std::wstring& error) override
    {
        m_window = window;
        m_size = size;

        if (!create_device_objects(error))
            return false;
        if (!create_swap_chain(error))
            return false;
        if (!create_render_targets(error))
            return false;
        if (!create_command_objects(error))
            return false;
        if (!create_sync_objects(error))
            return false;

        return true;
    }

    bool resize(viewport_size size, std::wstring& error) override
    {
        if (m_swap_chain == nullptr || size.width == 0 || size.height == 0)
            return true;

        wait_for_gpu();
        release_render_targets();

        HRESULT hr = m_swap_chain->ResizeBuffers(g_frame_count, size.width, size.height, m_swap_chain_format, m_swap_chain_flags);
        if (FAILED(hr))
        {
            error = L"ResizeBuffers failed: " + to_wstring(hr);
            return false;
        }

        m_frame_index = m_swap_chain->GetCurrentBackBufferIndex();
        m_size = size;
        return create_render_targets(error);
    }

    bool render_frame(float time_seconds, std::wstring& error) override
    {
        if (m_swap_chain == nullptr)
        {
            error = L"DX12 swap chain is not initialized.";
            return false;
        }

        HRESULT hr = m_command_allocators[m_frame_index]->Reset();
        if (FAILED(hr))
        {
            error = L"Command allocator reset failed: " + to_wstring(hr);
            return false;
        }

        hr = m_command_list->Reset(m_command_allocators[m_frame_index].Get(), nullptr);
        if (FAILED(hr))
        {
            error = L"Command list reset failed: " + to_wstring(hr);
            return false;
        }

        D3D12_RESOURCE_BARRIER barrierBegin{};
        barrierBegin.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrierBegin.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrierBegin.Transition.pResource = m_render_targets[m_frame_index].Get();
        barrierBegin.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierBegin.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrierBegin.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_command_list->ResourceBarrier(1, &barrierBegin);

        const float red = 0.15f + 0.1f * sinf(time_seconds * 0.9f);
        const float green = 0.08f + 0.06f * sinf(time_seconds * 1.3f + 1.0f);
        const float blue = 0.22f + 0.10f * sinf(time_seconds * 0.7f + 2.0f);
        const float clearColor[] = {red, green, blue, 1.0f};

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += static_cast<SIZE_T>(m_frame_index) * m_rtv_descriptor_size;

        m_command_list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(m_size.width);
        viewport.Height = static_cast<float>(m_size.height);
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor{};
        scissor.right = static_cast<LONG>(m_size.width);
        scissor.bottom = static_cast<LONG>(m_size.height);
        m_command_list->RSSetViewports(1, &viewport);
        m_command_list->RSSetScissorRects(1, &scissor);
        m_command_list->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        D3D12_RESOURCE_BARRIER barrierEnd = barrierBegin;
        barrierEnd.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrierEnd.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_command_list->ResourceBarrier(1, &barrierEnd);

        hr = m_command_list->Close();
        if (FAILED(hr))
        {
            error = L"Command list close failed: " + to_wstring(hr);
            return false;
        }

        ID3D12CommandList* lists[] = {m_command_list.Get()};
        m_command_queue->ExecuteCommandLists(1, lists);

        hr = m_swap_chain->Present(0, 0);
        if (FAILED(hr))
        {
            error = L"Present failed: " + to_wstring(hr);
            return false;
        }

        move_to_next_frame();
        return true;
    }

    ~render_backend_dx12() override
    {
        wait_for_gpu();
        if (m_fence_event)
            CloseHandle(m_fence_event);
    }

  private:
    bool create_device_objects(std::wstring& error)
    {
        UINT factoryFlags = 0;
#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.ReleaseAndGetAddressOf()))))
        {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif

        HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(m_factory.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            error = L"CreateDXGIFactory2 failed: " + to_wstring(hr);
            return false;
        }

        BOOL allowTearing = FALSE;
        hr = m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
        m_tearing_supported = SUCCEEDED(hr) && allowTearing;
        m_swap_chain_flags = 0u;

        for (UINT index = 0;; ++index)
        {
            DXGI_ADAPTER_DESC1 desc{};
            hr = m_factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(m_adapter.ReleaseAndGetAddressOf()));
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr))
            {
                error = L"EnumAdapterByGpuPreference failed: " + to_wstring(hr);
                return false;
            }

            m_adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                continue;

            hr = D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(m_device.ReleaseAndGetAddressOf()));
            if (SUCCEEDED(hr))
                break;
        }

        if (m_device == nullptr)
        {
            error = L"Could not create a D3D12 device for the target window.";
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_command_queue.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            error = L"CreateCommandQueue failed: " + to_wstring(hr);
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = g_frame_count;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(m_rtv_heap.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            error = L"CreateDescriptorHeap failed: " + to_wstring(hr);
            return false;
        }

        m_rtv_descriptor_size = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        return true;
    }

    bool create_swap_chain(std::wstring& error)
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = m_size.width;
        desc.Height = m_size.height;
        desc.Format = m_swap_chain_format;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = g_frame_count;
        desc.SampleDesc.Count = 1;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = m_swap_chain_flags;

        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr = m_factory->CreateSwapChainForHwnd(m_command_queue.Get(), m_window, &desc, nullptr, nullptr, swapChain1.ReleaseAndGetAddressOf());
        if (FAILED(hr))
        {
            error = L"CreateSwapChainForHwnd failed: " + to_wstring(hr);
            return false;
        }

        hr = m_factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(hr))
        {
            error = L"MakeWindowAssociation failed: " + to_wstring(hr);
            return false;
        }

        hr = swapChain1.As(&m_swap_chain);
        if (FAILED(hr))
        {
            error = L"Swap chain upgrade to IDXGISwapChain3 failed: " + to_wstring(hr);
            return false;
        }

        m_frame_index = m_swap_chain->GetCurrentBackBufferIndex();
        return true;
    }

    bool create_render_targets(std::wstring& error)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < g_frame_count; ++i)
        {
            HRESULT hr = m_swap_chain->GetBuffer(i, IID_PPV_ARGS(m_render_targets[i].ReleaseAndGetAddressOf()));
            if (FAILED(hr))
            {
                error = L"GetBuffer failed: " + to_wstring(hr);
                return false;
            }
            m_device->CreateRenderTargetView(m_render_targets[i].Get(), nullptr, handle);
            handle.ptr += m_rtv_descriptor_size;
        }
        return true;
    }

    bool create_command_objects(std::wstring& error)
    {
        for (UINT i = 0; i < g_frame_count; ++i)
        {
            HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_command_allocators[i].ReleaseAndGetAddressOf()));
            if (FAILED(hr))
            {
                error = L"CreateCommandAllocator failed: " + to_wstring(hr);
                return false;
            }
        }

        HRESULT hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_command_allocators[0].Get(), nullptr, IID_PPV_ARGS(m_command_list.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            error = L"CreateCommandList failed: " + to_wstring(hr);
            return false;
        }

        hr = m_command_list->Close();
        if (FAILED(hr))
        {
            error = L"Initial command list close failed: " + to_wstring(hr);
            return false;
        }
        return true;
    }

    bool create_sync_objects(std::wstring& error)
    {
        HRESULT hr = m_device->CreateFence(m_fence_values[m_frame_index], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            error = L"CreateFence failed: " + to_wstring(hr);
            return false;
        }

        m_fence_values[m_frame_index]++;
        m_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_fence_event == nullptr)
        {
            error = L"CreateEvent failed.";
            return false;
        }
        return true;
    }

    void release_render_targets()
    {
        for (auto& target : m_render_targets)
            target.Reset();
    }

    void wait_for_gpu()
    {
        if (m_command_queue == nullptr || m_fence == nullptr || m_fence_event == nullptr)
            return;

        const UINT64 value = m_fence_values[m_frame_index];
        if (FAILED(m_command_queue->Signal(m_fence.Get(), value)))
            return;
        if (FAILED(m_fence->SetEventOnCompletion(value, m_fence_event)))
            return;
        WaitForSingleObjectEx(m_fence_event, INFINITE, FALSE);
        m_fence_values[m_frame_index]++;
    }

    void move_to_next_frame()
    {
        const UINT64 currentFenceValue = m_fence_values[m_frame_index];
        m_command_queue->Signal(m_fence.Get(), currentFenceValue);

        m_frame_index = m_swap_chain->GetCurrentBackBufferIndex();
        if (m_fence->GetCompletedValue() < m_fence_values[m_frame_index])
        {
            m_fence->SetEventOnCompletion(m_fence_values[m_frame_index], m_fence_event);
            WaitForSingleObjectEx(m_fence_event, INFINITE, FALSE);
        }

        m_fence_values[m_frame_index] = currentFenceValue + 1;
    }

    HWND m_window = nullptr;
    viewport_size m_size{};
    bool m_tearing_supported = false;
    DXGI_FORMAT m_swap_chain_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT m_swap_chain_flags = 0;
    UINT m_frame_index = 0;
    UINT m_rtv_descriptor_size = 0;

    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_command_queue;
    ComPtr<IDXGISwapChain3> m_swap_chain;
    ComPtr<ID3D12DescriptorHeap> m_rtv_heap;
    ComPtr<ID3D12Resource> m_render_targets[g_frame_count];
    ComPtr<ID3D12CommandAllocator> m_command_allocators[g_frame_count];
    ComPtr<ID3D12GraphicsCommandList> m_command_list;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fence_values[g_frame_count] = {};
    HANDLE m_fence_event = nullptr;
};
} // namespace

std::unique_ptr<render_backend> make_dx12_backend()
{
    return std::make_unique<render_backend_dx12>();
}
} // namespace milkdrop::dx12
