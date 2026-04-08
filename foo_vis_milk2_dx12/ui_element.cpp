/*
 * ui_element.cpp - Experimental DX12 status panel implementation.
 */

#include "pch.h"
#include "ui_element.h"

#include <chrono>

namespace
{
int milkdrop_dx12_ui_element::OnCreate(LPCREATESTRUCT)
{
    initialize_backend_status();
    ensure_render_child_window();
    return 0;
}

void milkdrop_dx12_ui_element::OnDestroy()
{
    if (m_render_window != nullptr)
    {
        ::DestroyWindow(m_render_window);
        m_render_window = nullptr;
    }
    m_probe.reset();
    m_backend.reset();
}

void milkdrop_dx12_ui_element::OnSize(UINT, CSize)
{
    if (m_render_window != nullptr)
    {
        CRect client;
        GetClientRect(&client);
        ::MoveWindow(m_render_window, 0, 0, client.Width(), client.Height(), TRUE);
    }

    if (m_renderer_ready && m_backend != nullptr)
    {
        CRect client;
        GetClientRect(&client);
        std::wstring error;
        const UINT width = client.Width() > 0 ? static_cast<UINT>(client.Width()) : 0u;
        const UINT height = client.Height() > 0 ? static_cast<UINT>(client.Height()) : 0u;
        if (!m_backend->resize({width, height}, error))
        {
            m_runtime_error = error;
            m_renderer_ready = false;
            Invalidate();
            return;
        }
        m_backend->render_frame(0.0f, error);
    }
    else
    {
        Invalidate();
    }
}

BOOL milkdrop_dx12_ui_element::OnEraseBkgnd(CDCHandle)
{
    return TRUE;
}

void milkdrop_dx12_ui_element::OnPaint(CDCHandle)
{
    CPaintDC dc(*this);
    if (ensure_renderer_ready())
    {
        std::wstring error;
        if (!m_backend->render_frame(0.0f, error))
        {
            m_runtime_error = error;
            m_renderer_ready = false;
        }
        return;
    }

    CRect client;
    GetClientRect(&client);
    dc.FillSolidRect(&client, RGB(24, 24, 24));
    dc.Draw3dRect(&client, RGB(48, 48, 48), RGB(48, 48, 48));
    dc.SetTextColor(RGB(230, 230, 230));
    dc.SetBkMode(TRANSPARENT);

    CRect textRect = client;
    textRect.DeflateRect(12, 12);
    const auto status = build_status_text();
    dc.DrawText(status.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

void milkdrop_dx12_ui_element::initialize_backend_status()
{
    m_backend = milkdrop::dx12::make_dx12_backend();
    m_probe = m_backend->probe();
    m_runtime_error.clear();
    m_renderer_ready = false;
}

std::wstring milkdrop_dx12_ui_element::build_status_text() const
{
    std::wstring text;
    text += L"MilkDrop 2 DX12 Experimental\n\n";
    text += L"Status: this component is a separate DX12 probe and scaffold.\n";
    text += L"Rendering parity with the DX11 component is not implemented yet.\n\n";

    if (!m_probe.has_value())
    {
        text += L"DX12 probe has not run.";
        return text;
    }

    const auto& probe = *m_probe;
    text += L"Factory: ";
    text += probe.factory_created ? L"ok\n" : L"failed\n";
    text += L"Adapter: ";
    text += probe.adapter_selected ? probe.adapter_name + L"\n" : L"not selected\n";
    text += L"Device: ";
    text += probe.device_created ? L"ok\n" : L"failed\n";
    text += L"Feature level: ";
    text += probe.feature_level.empty() ? L"n/a\n" : probe.feature_level + L"\n";
    text += L"Command queue: ";
    text += probe.command_queue_created ? L"ok\n" : L"failed\n";
    text += L"Tearing support: ";
    text += probe.tearing_supported ? L"yes\n" : L"no\n";
    text += L"\n";
    text += probe.status_message;
    if (!m_runtime_error.empty())
    {
        text += L"\n\nRuntime error: ";
        text += m_runtime_error;
    }
    return text;
}

bool milkdrop_dx12_ui_element::ensure_renderer_ready()
{
    if (m_renderer_ready || !m_probe.has_value() || !m_probe->device_created || m_backend == nullptr)
        return m_renderer_ready;

    const HWND renderWindow = ensure_render_child_window();
    if (renderWindow == nullptr)
    {
        m_runtime_error = L"Could not create DX12 child render window.";
        return false;
    }

    CRect client;
    GetClientRect(&client);
    const UINT width = client.Width() > 0 ? static_cast<UINT>(client.Width()) : 0u;
    const UINT height = client.Height() > 0 ? static_cast<UINT>(client.Height()) : 0u;
    if (width == 0 || height == 0)
        return false;

    std::wstring error;
    if (!m_backend->initialize_window(renderWindow, {width, height}, error))
    {
        m_runtime_error = error;
        return false;
    }

    m_renderer_ready = true;
    m_runtime_error.clear();
    m_backend->render_frame(0.0f, error);
    return true;
}

HWND milkdrop_dx12_ui_element::ensure_render_child_window()
{
    if (m_render_window != nullptr && ::IsWindow(m_render_window))
        return m_render_window;

    CRect client;
    GetClientRect(&client);
    m_render_window = ::CreateWindowExW(0,
                                        L"STATIC",
                                        L"",
                                        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                        0,
                                        0,
                                        client.Width(),
                                        client.Height(),
                                        m_hWnd,
                                        nullptr,
                                        core_api::get_my_instance(),
                                        nullptr);
    return m_render_window;
}

static service_factory_single_t<ui_element_milkdrop_dx12> g_ui_element_factory_dx12;
} // namespace
