/*
 * ui_element.h - Experimental DX12 status panel for foobar2000.
 */

#pragma once

namespace
{
static const GUID guid_milkdrop_dx12_ui = {0x6f8ac22e, 0x5ca4, 0x4f86, {0x8a, 0x7a, 0x20, 0xcd, 0x7a, 0x25, 0xdd, 0x91}};

class milkdrop_dx12_ui_element : public ui_element_instance, public CWindowImpl<milkdrop_dx12_ui_element>
{
  public:
    DECLARE_WND_CLASS(L"foo_vis_milk2_dx12_ui")

    explicit milkdrop_dx12_ui_element(ui_element_config::ptr, ui_element_instance_callback_ptr callback) : m_callback(callback) {}

    void initialize_window(HWND parent)
    {
        Create(parent, nullptr, L"MilkDrop 2 DX12 Experimental", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, WS_EX_STATICEDGE);
    }

    BEGIN_MSG_MAP_EX(milkdrop_dx12_ui_element)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_DESTROY(OnDestroy)
        MSG_WM_SIZE(OnSize)
        MSG_WM_ERASEBKGND(OnEraseBkgnd)
        MSG_WM_PAINT(OnPaint)
    END_MSG_MAP()

    HWND get_wnd() { return *this; }
    void set_configuration(ui_element_config::ptr) {}
    ui_element_config::ptr get_configuration() { return ui_element_config::g_create_empty(g_get_guid()); }

    static GUID g_get_guid() { return guid_milkdrop_dx12_ui; }
    static GUID g_get_subclass() { return ui_element_subclass_playback_visualisation; }
    static void g_get_name(pfc::string_base& out) { out = "MilkDrop 2 DX12 Experimental"; }
    static ui_element_config::ptr g_get_default_configuration() { return ui_element_config::g_create_empty(g_get_guid()); }
    static const char* g_get_description() { return "Experimental DirectX 12 renderer scaffold and probe."; }

  private:
    int OnCreate(LPCREATESTRUCT);
    void OnDestroy();
    void OnSize(UINT, CSize);
    BOOL OnEraseBkgnd(CDCHandle);
    void OnPaint(CDCHandle);

    void initialize_backend_status();
    std::wstring build_status_text() const;
    bool ensure_renderer_ready();
    HWND ensure_render_child_window();

  public:
    const ui_element_instance_callback_ptr m_callback;

  private:
    std::unique_ptr<milkdrop::dx12::render_backend> m_backend;
    std::optional<milkdrop::dx12::backend_probe_result> m_probe;
    bool m_renderer_ready = false;
    std::wstring m_runtime_error;
    HWND m_render_window = nullptr;
};

class ui_element_milkdrop_dx12 : public ui_element_impl_visualisation<milkdrop_dx12_ui_element>
{
};
} // namespace
