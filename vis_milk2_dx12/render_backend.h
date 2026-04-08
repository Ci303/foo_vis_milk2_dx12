/*
 * render_backend.h - Backend-neutral renderer probe interface for DX12 work.
 */

#pragma once

#include "pch.h"

namespace milkdrop::dx12
{
struct viewport_size
{
    UINT width = 0;
    UINT height = 0;
};

struct backend_probe_result
{
    bool factory_created = false;
    bool adapter_selected = false;
    bool device_created = false;
    bool command_queue_created = false;
    bool tearing_supported = false;
    std::wstring adapter_name;
    std::wstring feature_level;
    std::wstring status_message;
};

class render_backend
{
  public:
    virtual ~render_backend() = default;
    virtual backend_probe_result probe() = 0;
    virtual bool initialize_window(HWND window, viewport_size size, std::wstring& error) = 0;
    virtual bool resize(viewport_size size, std::wstring& error) = 0;
    virtual bool render_frame(float time_seconds, std::wstring& error) = 0;
};

std::unique_ptr<render_backend> make_dx12_backend();
} // namespace milkdrop::dx12
