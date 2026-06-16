/*
 * ui_element.cpp - Implements the MilkDrop 2 visualization component.
 *
 * Copyright (c) 2023-2024 Jimmy Cassis
 * SPDX-License-Identifier: MPL-2.0
 */

#include "pch.h"
#include "ui_element.h"

#pragma hdrstop

#ifdef __AVX__
#error `/arch:AVX`, `/arch:AVX2` or `/arch:AVX512` compiler flag set.
#endif

//#ifdef __clang__
//#pragma clang diagnostic ignored "-Wcovered-switch-default"
//#pragma clang diagnostic ignored "-Wswitch-enum"
//#endif

CPlugin g_plugin;
HWND g_hWindow;

// Indicates to hybrid graphics systems to prefer the discrete part by default.
//extern "C" {
//    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
//    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
//}

namespace
{
uint32_t normalize_frame_limit(uint32_t max_fps) noexcept
{
    for (size_t i = 0; i < supported_max_fps_count; ++i)
    {
        if (max_fps == supported_max_fps_values[i])
            return max_fps;
    }
    return default_max_fps_fs;
}

DWORD get_refresh_interval_ms(uint32_t max_fps) noexcept
{
    max_fps = normalize_frame_limit(max_fps);
    if (max_fps == 0)
        return 1;

    return (std::max)(1L, lround(1000.0 / static_cast<double>(max_fps)));
}

bool call_winmm_timer_period(const char* procName) noexcept
{
    using TimerPeriodProc = UINT(WINAPI*)(UINT);
    static HMODULE winmm = ::LoadLibraryW(L"winmm.dll");
    if (winmm == nullptr)
        return false;

    auto proc = reinterpret_cast<TimerPeriodProc>(::GetProcAddress(winmm, procName));
    return proc != nullptr && proc(1) == 0;
}

bool is_dx12_truthy_log_value(const wchar_t* value) noexcept
{
    return value && value[0] != L'\0' && wcscmp(value, L"0") != 0 && _wcsicmp(value, L"false") != 0;
}

bool resolve_dx12_ui_log_path(wchar_t* logPath, size_t logPathCount) noexcept
{
    if (!logPath || logPathCount == 0)
        return false;

    logPath[0] = L'\0';

    wchar_t logValue[MAX_PATH] = {};
    const DWORD valueLength = GetEnvironmentVariableW(L"FOO_VIS_MILK2_DX12_LOG", logValue, static_cast<DWORD>(std::size(logValue)));
    if (valueLength >= std::size(logValue))
        return false;
    if (valueLength > 0 && !is_dx12_truthy_log_value(logValue))
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
    if (valueLength == 0 && (devValueLength == 0 || !is_dx12_truthy_log_value(devValue)))
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

void write_dx12_ui_log_line(const wchar_t* message) noexcept
{
    wchar_t logPath[MAX_PATH]{};
    if (!resolve_dx12_ui_log_path(logPath, std::size(logPath)))
        return;

    HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t line[1024]{};
    swprintf_s(line,
               L"%04u-%02u-%02u %02u:%02u:%02u.%03u ui %ls\r\n",
               st.wYear,
               st.wMonth,
               st.wDay,
               st.wHour,
               st.wMinute,
               st.wSecond,
               st.wMilliseconds,
               message ? message : L"");

    char utf8Line[2048]{};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8Line, static_cast<int>(std::size(utf8Line)), nullptr, nullptr);
    if (bytes > 1)
    {
        DWORD bytesWritten = 0;
        WriteFile(file, utf8Line, static_cast<DWORD>(bytes - 1), &bytesWritten, nullptr);
    }
    CloseHandle(file);
}

milk2_ui_element::milk2_ui_element(ui_element_config::ptr config, ui_element_instance_callback_ptr p_callback) :
    m_callback(p_callback),
    m_bMsgHandled(TRUE),
    play_callback_impl_base(flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_pause),
    playlist_callback_impl_base(flag_on_items_added | flag_on_items_reordered | flag_on_items_removed | flag_on_items_selection_change |
                                flag_on_item_focus_change | flag_on_items_modified | flag_on_playlist_activate | flag_on_playlists_reorder |
                                flag_on_playlists_removed | flag_on_playback_order_changed)
{
    m_milk2 = false;
    m_in_sizemove = false;
    m_in_suspend = false;
    m_minimized = false;
    m_focus_hotkeys_registered = false;
    m_pending_single_click = false;
    m_popout_drag_candidate = false;
    m_click_pause_confirmation_required = false;
    m_click_pause_confirmation_pending = false;
    m_blacklist_load_retries = 0;
    m_last_left_double_click_tick = 0;
    m_click_pause_confirmation_tick = 0;
    m_pending_animated_text_kind = pending_animated_text_kind::none;
    m_pending_animated_status_duration = 1.6f;
    m_pending_animated_status_fade_time = 0.35f;
    m_pending_animated_status_font = SONGTITLE_FONT;
#if defined(TIMER_TP)
    m_framePacerThread = nullptr;
    m_framePacerStopEvent = nullptr;
    m_framePacerWakeEvent = nullptr;
    LARGE_INTEGER frameTimerFrequency{};
    m_frameTimerFrequencyQpc = QueryPerformanceFrequency(&frameTimerFrequency) ? frameTimerFrequency.QuadPart : 0;
    if (m_frameTimerFrequencyQpc > 0)
    {
        m_frameIntervalQpc = (std::max)(1LL, (m_frameTimerFrequencyQpc + default_max_fps_fs / 2) / default_max_fps_fs);
    }
    else
    {
        m_frameIntervalQpc = 0;
    }
    m_nextFrameQpc = 0;
    m_renderPending = false;
    m_renderPostTick = 0;
#elif defined(TIMER_32)
    m_last_time = 0.0;
#endif
    m_refresh_interval = get_refresh_interval_ms(default_max_fps_fs);
    m_art_data = std::make_unique<artFetchData>();

    m_pwd = L".\\";
    set_configuration(config);
}

ui_element_config::ptr milk2_ui_element::g_get_default_configuration()
{
    MILK2_CONSOLE_LOG("g_get_default_configuration")

    try
    {
        ui_element_config_builder builder;
        milk2_config config;
        config.init();
        config.build(builder, false);
        return builder.finish(g_get_guid());
    }
    catch (exception_io& exc)
    {
        FB2K_console_print(core_api::get_my_file_name(), ": Exception while building default configuration data - ", exc);
        return ui_element_config::g_create_empty(g_get_guid());
    }
}

void milk2_ui_element::set_configuration(ui_element_config::ptr p_data)
{
    MILK2_CONSOLE_LOG("set_configuration")

    //LPVOID dataptr = const_cast<LPVOID>(p_data->get_data());
    //if (dataptr && p_data->get_data_size() >= 4 && static_cast<DWORD*>(dataptr)[0] == ('M' | 'I' << 8 | 'L' << 16 | 'K' << 24))
    //    s_config = p_data;
    //else
    //    s_config = g_get_default_configuration();

    if (s_milk2)
        return;

    ui_element_config_parser parser(p_data);
    s_config.parse(parser);
}

ui_element_config::ptr milk2_ui_element::get_configuration()
{
    MILK2_CONSOLE_LOG("get_configuration")

    ui_element_config_builder builder;
    s_config.build(builder, !s_in_toggle);
    return builder.finish(g_get_guid());
}

void milk2_ui_element::notify(const GUID& p_what, size_t p_param1, const void* p_param2, size_t p_param2size)
{
    UNREFERENCED_PARAMETER(p_param1);
    UNREFERENCED_PARAMETER(p_param2);
    UNREFERENCED_PARAMETER(p_param2size);

    MILK2_CONSOLE_LOG("notify")
    if (p_what == ui_element_notify_colors_changed || p_what == ui_element_notify_font_changed)
        Invalidate();
}

HWND GetRealParent(HWND hWnd)
{
    HWND hWndOwner;

    // To obtain a window's owner window, instead of using `GetParent()`,
    // use `GetWindow()` with the `GW_OWNER` flag.
    if (NULL != (hWndOwner = GetWindow(hWnd, GW_OWNER)))
        return hWndOwner;

    // Obtain the parent window and not the owner.
    return GetAncestor(hWnd, GA_PARENT);
}

int milk2_ui_element::OnCreate(LPCREATESTRUCT cs)
{
#ifndef _DEBUG
    UNREFERENCED_PARAMETER(cs);
#endif

    MILK2_CONSOLE_LOG("OnCreate0 ", cs->x, ", ", cs->y, ", ", GetWnd())
    if (!XMVerifyCPUSupport()) {
        FB2K_console_print(core_api::get_my_file_name(), ": CPU does not support mathematics intrinsics. Exiting.");
        return E_FAIL;
    }

    if (!s_milk2)
    {
        ResolvePwd();
        s_config.init();
#ifdef TIMER_TP
        if (!s_cs_initialized)
        {
            InitializeCriticalSection(&s_cs);
            s_cs_initialized = true;
        }
#endif
    }

    if (!m_milk2)
    {
        SetPwd(s_pwd);

        try
        {
            static_api_ptr_t<visualisation_manager> vis_manager;

            vis_manager->create_stream(m_vis_stream, 0);

            m_vis_stream->request_backlog(0.8);
            UpdateChannelMode();
        }
        catch (std::exception& exc)
        {
            FB2K_console_print(core_api::get_my_file_name(), ": Exception while creating visualization stream - ", exc);
        }

#if defined(TIMER_DX)
        message_loop_v2::get()->add_idle_handler(this);
#endif

        RegisterForArtwork();
    }

    HRESULT hr = S_OK;
    int w, h;
    GetDefaultSize(w, h);

    CRect r{};
    WIN32_OP_D(GetClientRect(&r))
    if (r.right - r.left > 0 && r.bottom - r.top > 0)
    {
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
    if (!Initialize(get_wnd(), w, h))
    {
        FB2K_console_print(core_api::get_my_file_name(), ": Could not initialize MilkDrop");
    }
    MILK2_CONSOLE_LOG("OnCreate1 ", r.right, ", ", r.left, ", ", r.top, ", ", r.bottom, ", ", GetWnd())

    return hr;
}

void milk2_ui_element::OnClose()
{
    MILK2_CONSOLE_LOG("OnClose ", GetWnd())
    if (s_popout)
    {
        ReturnPopoutToPanel();
        return;
    }
    //DestroyWindow();
}

void milk2_ui_element::OnDestroy()
{
    MILK2_CONSOLE_LOG("OnDestroy ", GetWnd())
    m_milk2 = false;
    UnregisterFocusHotkeys();
    KillTimer(ID_CLICK_TIMER);
    m_pending_single_click = false;
#if defined(TIMER_TP)
    m_renderPending = false;
    m_renderPostTick = 0;
    StopTimer();
    const bool csEntered = s_cs_initialized;
    if (csEntered)
        EnterCriticalSection(&s_cs);
#elif defined(TIMER_DX)
    message_loop_v2::get()->remove_idle_handler(this);
#endif
    if (m_vis_stream.is_valid())
        m_vis_stream.release();
    //DestroyMenu();
    auto manager = now_playing_album_art_notify_manager::tryGet();
    if (manager.is_valid())
        manager->remove(this);

    s_count = 0ull;

    const bool finalDestroy = !s_in_toggle;
    if (finalDestroy)
    {
        MILK2_CONSOLE_LOG("ExitVis")
        s_fullscreen = false;
        g_plugin.SetFoobarFullscreenFrameLimit(0);
        s_in_toggle = false;
        s_was_topmost = false;
        s_popout = false;
        s_popout_fullscreen = false;
        s_popout_parent = nullptr;
        s_milk2 = false;
#if defined(TIMER_32)
        KillTimer(ID_REFRESH_TIMER);
#endif
        wcscpy_s(s_config.settings.m_szPresetDir, g_plugin.GetPresetDir()); // save last "Load Preset" menu directory
        g_plugin.PluginQuit();

        HWND parent = GetRealParent(get_wnd());
        if (parent && parent != ::GetDesktopWindow())
        {
            ::SetClassLongPtr(parent, GCLP_HICON, NULL);
            ::SetClassLongPtr(parent, GCLP_HICONSM, NULL);
        }

        //PostQuitMessage(0);
    }
    else
    {
        s_in_toggle = false;
        if (!s_fullscreen && !s_popout_fullscreen && g_hWindow && g_hWindow != get_wnd() && ::IsWindow(g_hWindow))
        {
            ::PostMessage(g_hWindow, WM_MILK2_RESTORE_WINDOWED, 0, 0);
        }
    }
#ifdef TIMER_TP
    if (csEntered)
        LeaveCriticalSection(&s_cs);
    if (finalDestroy && s_cs_initialized)
    {
        DeleteCriticalSection(&s_cs);
        s_cs_initialized = false;
    }
#endif
}

void milk2_ui_element::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == ID_CLICK_TIMER)
    {
        KillTimer(ID_CLICK_TIMER);
        if (m_pending_single_click)
        {
            m_pending_single_click = false;
            const DWORD now = GetTickCount();
            if (m_last_left_double_click_tick != 0 && now - m_last_left_double_click_tick <= GetDoubleClickTime())
                return;

            TogglePlaybackFromClick();
        }
        return;
    }

    if (nIDEvent == ID_BLACKLIST_TIMER)
    {
        KillTimer(ID_BLACKLIST_TIMER);
#ifdef TIMER_TP
        if (s_cs_initialized && TryEnterCriticalSection(&s_cs) == 0)
        {
            if (++m_blacklist_load_retries <= ID_BLACKLIST_MAX_RETRIES)
                SetTimer(ID_BLACKLIST_TIMER, ID_BLACKLIST_RETRY_DELAY_MS, nullptr);
            else
                m_blacklist_load_retries = 0;
            return;
        }
#endif
        g_plugin.LoadRandomPreset(0.0f);
#ifdef TIMER_TP
        if (s_cs_initialized)
            LeaveCriticalSection(&s_cs);
#endif
        m_blacklist_load_retries = 0;
        return;
    }

    if (nIDEvent == WM_MILK2_REPAIR_WINDOWED_DX12)
    {
        ::KillTimer(get_wnd(), WM_MILK2_REPAIR_WINDOWED_DX12);
        ::PostMessage(get_wnd(), WM_MILK2_REPAIR_WINDOWED_DX12, 0, 0);
        return;
    }

    MILK2_CONSOLE_LOG_LIMIT("OnTimer ", GetWnd())
#ifdef TIMER_32
    KillTimer(ID_REFRESH_TIMER);
    InvalidateRect(NULL, TRUE);
#endif
}

void milk2_ui_element::OnPaint(CDCHandle dc)
{
    (void)dc;

    MILK2_CONSOLE_LOG_LIMIT("OnPaint ", GetWnd())
    if (m_in_sizemove && m_milk2) // foobar2000 does not enter/exit size/move
    {
        Tick();
    }
    else
    {
        PAINTSTRUCT ps;
        std::ignore = BeginPaint(&ps);
        EndPaint(&ps);
    }
#ifdef TIMER_TP
    if (m_milk2)
        StartTimer();
#endif
    ValidateRect(NULL);
#ifdef TIMER_32
    ULONGLONG now = GetTickCount64();
#endif
    if (m_vis_stream.is_valid())
    {
        BuildWaves();
#ifdef TIMER_32
        ULONGLONG next_refresh = m_last_refresh + m_refresh_interval;
        // (next_refresh < now) would break when GetTickCount() overflows
        if (static_cast<LONGLONG>(next_refresh - now) < 0)
        {
            next_refresh = now;
        }
        SetTimer(ID_REFRESH_TIMER, static_cast<UINT>(next_refresh - now));
#endif
    }
#ifdef TIMER_32
    m_last_refresh = now;
#endif
}

BOOL milk2_ui_element::OnEraseBkgnd(CDCHandle dc)
{
    (void)dc;

    MILK2_CONSOLE_LOG_LIMIT("OnEraseBkgnd ", GetWnd())
    ++s_count;

#if 0
    CRect r;
    WIN32_OP_D(GetClientRect(&r));
    CBrush brush;
    //const COLORREF rgbBlack = 0x00000000;
    WIN32_OP_D(brush.CreateSolidBrush(m_callback->query_std_color(ui_color_background)) != NULL);
    WIN32_OP_D(dc.FillRect(&r, brush));
#else
    if (!g_plugin.IsD3D12Active() && !m_milk2 && !s_fullscreen && !s_popout_fullscreen && s_milk2 && !s_in_toggle)
    {
        int w, h;
        GetDefaultSize(w, h);

        CRect r{};
        WIN32_OP_D(GetClientRect(&r))
        if (r.right - r.left > 0 && r.bottom - r.top > 0)
        {
            w = r.right - r.left;
            h = r.bottom - r.top;
        }
        if (!Initialize(get_wnd(), w, h))
        {
            FB2K_console_print(core_api::get_my_file_name(), ": Could not initialize MilkDrop");
        }
    }
#ifdef TIMER_32
    Tick();
#endif
#endif
#if 0
    FB2K_console_print(m_timer.GetFramesPerSecond(), "FPS");
#endif

    return TRUE;
}

void milk2_ui_element::OnMove(CPoint ptPos)
{
    UNREFERENCED_PARAMETER(ptPos);

    MILK2_CONSOLE_LOG("OnMove ", GetWnd())
    if (m_milk2)
    {
#ifdef TIMER_TP
        if (TryEnterCriticalSection(&s_cs) == 0)
            return;
#endif
        g_plugin.OnWindowMoved();
#ifdef TIMER_TP
        LeaveCriticalSection(&s_cs);
#endif
    }
}

void milk2_ui_element::OnSize(UINT nType, CSize size)
{
    MILK2_CONSOLE_LOG("OnSize0 ", nType, ", ", size.cx, ", ", size.cy, ", ", GetWnd())
    if (nType == SIZE_MINIMIZED)
    {
        if (!m_minimized)
        {
            m_minimized = true;
            if (!m_in_suspend && m_milk2)
                OnSuspending();
            m_in_suspend = true;
        }
    }
    else if (m_minimized)
    {
        m_minimized = false;
        if (m_in_suspend && m_milk2)
            OnResuming();
        m_in_suspend = false;
    }
    else if (!m_in_sizemove && m_milk2)
    {
        int width = size.cx;
        int height = size.cy;

        if (!width || !height)
            return;
        if (width < 128)
            width = 128;
        if (height < 128)
            height = 128;
        MILK2_CONSOLE_LOG("OnSize1 ", nType, ", ", size.cx, ", ", size.cy, ", ", GetWnd())
#ifdef TIMER_TP
        if (TryEnterCriticalSection(&s_cs) == 0)
            return;
#endif
        g_plugin.OnWindowSizeChanged(width, height);
        if (g_plugin.IsD3D12Active() && !s_fullscreen && !s_popout_fullscreen)
            ::SetTimer(get_wnd(), WM_MILK2_REPAIR_WINDOWED_DX12, 180, nullptr);
#ifdef TIMER_TP
        LeaveCriticalSection(&s_cs);
#endif
    }
}

void milk2_ui_element::OnEnterSizeMove()
{
    MILK2_CONSOLE_LOG("OnEnterSizeMove ", GetWnd())
    m_in_sizemove = true;
}

void milk2_ui_element::OnExitSizeMove()
{
    MILK2_CONSOLE_LOG("OnExitSizeMove ", GetWnd())
    m_in_sizemove = false;
    if (m_milk2)
    {
#ifdef TIMER_TP
        if (TryEnterCriticalSection(&s_cs) == 0)
            return;
#endif
        RECT rc;
        WIN32_OP_D(GetClientRect(&rc));
        const int width = std::max<int>(rc.right - rc.left, 128);
        const int height = std::max<int>(rc.bottom - rc.top, 128);
        g_plugin.OnWindowSizeChanged(width, height);
        if (g_plugin.IsD3D12Active() && !s_fullscreen && !s_popout_fullscreen)
            ::SetTimer(get_wnd(), WM_MILK2_REPAIR_WINDOWED_DX12, 180, nullptr);
#ifdef TIMER_TP
        LeaveCriticalSection(&s_cs);
#endif
    }
}

LRESULT milk2_ui_element::OnNcHitTest(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(uMsg);
    UNREFERENCED_PARAMETER(wParam);

    if (!s_popout || s_popout_fullscreen || !s_config.settings.m_bPopoutBorderless)
    {
        SetMsgHandled(FALSE);
        return 0;
    }

    RECT windowRect{};
    if (!::GetWindowRect(get_wnd(), &windowRect))
    {
        SetMsgHandled(FALSE);
        return 0;
    }

    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const int resizeBorder = 8;
    const int dragBand = 40;

    const bool left = point.x >= windowRect.left && point.x < windowRect.left + resizeBorder;
    const bool right = point.x <= windowRect.right && point.x > windowRect.right - resizeBorder;
    const bool top = point.y >= windowRect.top && point.y < windowRect.top + resizeBorder;
    const bool bottom = point.y <= windowRect.bottom && point.y > windowRect.bottom - resizeBorder;

    if (top && left)
        return HTTOPLEFT;
    if (top && right)
        return HTTOPRIGHT;
    if (bottom && left)
        return HTBOTTOMLEFT;
    if (bottom && right)
        return HTBOTTOMRIGHT;
    if (left)
        return HTLEFT;
    if (right)
        return HTRIGHT;
    if (top)
        return HTTOP;
    if (bottom)
        return HTBOTTOM;

    if (point.y >= windowRect.top && point.y < windowRect.top + dragBand)
        return HTCAPTION;

    return HTCLIENT;
}

LRESULT milk2_ui_element::OnNcLButtonDblClk(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(uMsg);
    UNREFERENCED_PARAMETER(lParam);

    if (s_popout && !s_popout_fullscreen && s_config.settings.m_bPopoutBorderless && wParam == HTCAPTION)
    {
        TogglePopoutFullscreen();
        return 0;
    }

    SetMsgHandled(FALSE);
    return 0;
}

// To avoid a 1-pixel-thick border of noise.
LRESULT milk2_ui_element::OnNcCalcSize(BOOL bCalcValidRects, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    MILK2_CONSOLE_LOG("OnNcCalcSize ", GetWnd())
    if (s_popout && !s_popout_fullscreen && !s_config.settings.m_bPopoutBorderless)
    {
        SetMsgHandled(FALSE);
        return 0;
    }
    if (bCalcValidRects == TRUE)
    {
        //auto& params = *reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        //adjust_fullscreen_client_rect(get_wnd(), params.rgrc[0]);
        return 0;
    }
    return 0;
}

BOOL milk2_ui_element::OnCopyData(CWindow wnd, PCOPYDATASTRUCT pcds)
{
    UNREFERENCED_PARAMETER(wnd);
    //typedef struct
    //{
    //    wchar_t error[1024];
    //} ErrorCopy;

    MILK2_CONSOLE_LOG("OnCopyData ", GetWnd())
    switch (pcds->dwData)
    {
        case 0x09: // PRINT STDOUT
            {
                LPCTSTR lpszString = (LPCTSTR)((ErrorCopy*)(pcds->lpData))->error;
                FB2K_console_print(core_api::get_my_file_name(), ": ", lpszString);
                break;
            }
    }

    return TRUE;
}

void milk2_ui_element::OnDisplayChange(UINT uBitsPerPixel, CSize sizeScreen)
{
    UNREFERENCED_PARAMETER(uBitsPerPixel);
    UNREFERENCED_PARAMETER(sizeScreen);

    MILK2_CONSOLE_LOG("OnDisplayChange ", GetWnd())
    if (m_milk2)
    {
        g_plugin.OnDisplayChange();
    }
}

void milk2_ui_element::OnDpiChanged(UINT nDpiX, UINT nDpiY, PRECT pRect)
{
    UNREFERENCED_PARAMETER(nDpiX);
    UNREFERENCED_PARAMETER(nDpiY);
    UNREFERENCED_PARAMETER(pRect);
}

void milk2_ui_element::OnGetMinMaxInfo(LPMINMAXINFO lpMMI)
{
    lpMMI->ptMinTrackSize.x = 150; // 320
    lpMMI->ptMinTrackSize.y = 150; // 200
}

void milk2_ui_element::OnActivateApp(BOOL bActive, DWORD dwThreadID)
{
    UNREFERENCED_PARAMETER(dwThreadID);

    MILK2_CONSOLE_LOG("OnActivateApp ", GetWnd())
    if (m_milk2)
    {
        if (bActive)
        {
            OnActivated();
        }
        else
        {
            OnDeactivated();
        }
    }
}

BOOL milk2_ui_element::OnPowerBroadcast(DWORD dwPowerEvent, DWORD_PTR dwData)
{
    UNREFERENCED_PARAMETER(dwData);

    MILK2_CONSOLE_LOG("OnPowerBroadcast ", GetWnd())
    switch (dwPowerEvent)
    {
        case PBT_APMQUERYSUSPEND:
            if (!m_in_suspend && m_milk2)
                OnSuspending();
            m_in_suspend = true;
            break;
        case PBT_APMRESUMESUSPEND:
            if (!m_minimized)
            {
                if (m_in_suspend && m_milk2)
                    OnResuming();
                m_in_suspend = false;
            }
            break;
        default:
            break;
    }

    return TRUE;
}

void milk2_ui_element::OnLButtonDblClk(UINT nFlags, CPoint point)
{
    UNREFERENCED_PARAMETER(nFlags);
    UNREFERENCED_PARAMETER(point);

    MILK2_CONSOLE_LOG("OnLButtonDblClk ", GetWnd())
    m_last_left_double_click_tick = GetTickCount();
    KillTimer(ID_CLICK_TIMER);
    m_pending_single_click = false;
    m_click_pause_confirmation_required = false;
    m_click_pause_confirmation_pending = false;
    ToggleFullScreen();
}

#pragma region Keyboard Controls
#include "keyboard.cpp"
#pragma endregion

void milk2_ui_element::OnContextMenu(CWindow wnd, CPoint point)
{
    UNREFERENCED_PARAMETER(wnd);

    MILK2_CONSOLE_LOG("OnContextMenu ", point.x, ", ", point.y, ", ", GetWnd())
    if (m_callback->is_edit_mode_enabled())
    {
        SetMsgHandled(FALSE);
        return;
    }
    //_context_menu = make_context_menu();

    // A (-1,-1) point is due to context menu key rather than right click.
    // `GetContextMenuPoint()` fixes that, returning a proper point at which the menu should be shown.
    //point = m_list.GetContextMenuPoint(point);
    CMenu menu;
    WIN32_OP_D(menu.CreatePopupMenu()); // ID_VIS_MENU
    //BOOL b = TRUE;
    //CMenu original;
    //b = menu.LoadMenu(IDR_WINDOWED_CONTEXT_MENU);
    //menu.AppendMenu(MF_STRING, menu.GetSubMenu(0), TEXT("Winamp"));
    KillTimer(ID_CLICK_TIMER);
    m_pending_single_click = false;
    m_click_pause_confirmation_required = true;
    m_click_pause_confirmation_pending = false;

    const std::wstring currentPreset = GetCurrentPreset();
    const UINT presetItemFlags = currentPreset.empty() ? (MF_STRING | MF_GRAYED) : MF_STRING;
    menu.AppendMenu(presetItemFlags, IDM_OPEN_PRESET_LOCATION, currentPreset.empty() ? TEXT("(No preset loaded)") : currentPreset.c_str());
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, IDM_NEXT_PRESET, TEXT("Next Preset"));
    menu.AppendMenu(MF_STRING, IDM_PREVIOUS_PRESET, TEXT("Previous Preset"));
    menu.AppendMenu(MF_STRING, IDM_SHUFFLE_PRESET, TEXT("Random Preset"));
    menu.AppendMenu(MF_STRING, IDM_LOAD_PRESET_FILE, TEXT("Load Preset..."));
    menu.AppendMenu(MF_STRING | (IsPresetLock() ? MF_CHECKED : 0), IDM_LOCK_PRESET, TEXT("Lock Preset"));
    menu.AppendMenu(currentPreset.empty() ? (MF_STRING | MF_GRAYED) : MF_STRING, IDM_BLACKLIST_PRESET, TEXT("Never Show Again"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING | (s_config.settings.m_bEnableDownmix ? MF_CHECKED : 0), IDM_ENABLE_DOWNMIX, TEXT("Downmix Channels"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING | (g_plugin.m_show_playlist ? MF_CHECKED : 0), IDM_SHOW_PLAYLIST, TEXT("Show Playlist"));
    //menu.AppendMenu(MF_STRING | (g_plugin.m_show_presets ? MF_CHECKED : 0), IDM_SHOW_PRESETS, TEXT("Show Presets"));
    //menu.AppendMenu(MF_STRING | (g_plugin.m_show_menu ? MF_CHECKED : 0), IDM_SHOW_MENU, TEXT("Show Menu"));
    menu.AppendMenu(MF_STRING | (s_config.settings.m_bShowAlbum && std::filesystem::exists(s_config.settings.m_szImgIniFile)
                                     ? MF_CHECKED
                                     : (std::filesystem::exists(s_config.settings.m_szImgIniFile) ? 0 : MF_DISABLED)), 
                    IDM_SHOW_ALBUM, TEXT("Show Album Art"));
    menu.AppendMenu(MF_STRING, IDM_SHOW_TITLE, TEXT("Launch Title"));
    menu.AppendMenu(MF_STRING | (s_config.settings.m_bShowFPS ? MF_CHECKED : 0), IDM_SHOW_FPS, TEXT("Show FPS"));
    menu.AppendMenu(MF_STRING | (s_config.settings.m_bShowSongTime || s_config.settings.m_bShowSongLen ? MF_CHECKED : 0), IDM_SHOW_SONG_TIME, TEXT("Show Song Time"));
    menu.AppendMenu(MF_STRING | (s_config.settings.m_bShowPresetInfo ? MF_CHECKED : 0), IDM_SHOW_PRESET_INFO, TEXT("Show Preset Info"));
    menu.AppendMenu(MF_STRING | (s_config.settings.m_bShowRating ? MF_CHECKED : 0), IDM_SHOW_RATING, TEXT("Show Rating"));
    menu.AppendMenu(MF_STRING | (s_config.settings.m_bShowShaderHelp ? MF_CHECKED : 0), IDM_SHOW_SHADER_HELP, TEXT("Show Shader Help"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING | (g_plugin.m_show_help ? MF_CHECKED : 0), IDM_SHOW_HELP, TEXT("Show Help"));
    menu.AppendMenu(MF_STRING, IDM_SHOW_PREFS, TEXT("Launch Preferences Page"));
    menu.AppendMenu(MF_SEPARATOR);
    const UINT popoutFlags = MF_STRING
        | (s_popout ? MF_CHECKED : 0)
        | ((s_fullscreen && !s_popout) ? MF_GRAYED : 0);
    menu.AppendMenu(popoutFlags, IDM_TOGGLE_POPOUT, s_popout ? TEXT("Return to Panel") : TEXT("Pop Out Window"));
    menu.AppendMenu(MF_STRING | ((s_fullscreen || s_popout_fullscreen) ? MF_CHECKED : 0), IDM_TOGGLE_FULLSCREEN, TEXT("Fullscreen"));

    //auto submenu = std::make_unique<CMenu>(menu.GetSubMenu(0));
    //b = menu.RemoveMenu(0, MF_BYPOSITION);
    //return submenu;
    //int cmd = menu.GetSubMenu(0).TrackPopupMenu(TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, point.x, point.y, *this);
    int cmd = menu.TrackPopupMenu(TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, point.x, point.y, *this);

    switch (cmd)
    {
        case IDM_OPEN_PRESET_LOCATION:
            OpenCurrentPresetLocation();
            break;
        case IDM_TOGGLE_FULLSCREEN:
            if (g_plugin.GetFrame() > 0)
                ToggleFullScreen();
            break;
        case IDM_TOGGLE_POPOUT:
            TogglePopout();
            break;
        case IDM_NEXT_PRESET:
            NextPreset();
            break;
        case IDM_PREVIOUS_PRESET:
            PrevPreset();
            break;
        case IDM_LOCK_PRESET:
            LockPreset(!IsPresetLock());
            break;
        case IDM_BLACKLIST_PRESET:
            BlacklistCurrentPreset();
            break;
        case IDM_SHUFFLE_PRESET:
            RandomPreset();
            break;
        case IDM_LOAD_PRESET_FILE:
            LoadPresetFromFile();
            break;
        case IDM_ENABLE_DOWNMIX:
            s_config.settings.m_bEnableDownmix = !s_config.settings.m_bEnableDownmix;
            UpdateChannelMode();
            s_config.persist_runtime_settings();
            break;
        case IDM_SHOW_PREFS:
            ShowPreferencesPage();
            break;
        case IDM_SHOW_HELP:
            ToggleHelp();
            break;
        case IDM_SHOW_PLAYLIST:
            TogglePlaylist();
            break;
        case IDM_SHOW_TITLE:
            LaunchSongTitle();
            break;
        case IDM_SHOW_ALBUM:
            s_config.settings.m_bShowAlbum = !s_config.settings.m_bShowAlbum;
            ShowAlbumArt();
            s_config.persist_runtime_settings();
            break;
        case IDM_SHOW_FPS:
            ToggleFps();
            break;
        case IDM_SHOW_SONG_TIME:
            ToggleSongLength();
            break;
        case IDM_SHOW_PRESET_INFO:
            TogglePresetInfo();
            break;
        case IDM_SHOW_RATING:
            ToggleRating();
            break;
        case IDM_SHOW_SHADER_HELP:
            ToggleShaderHelp();
            break;
        case IDM_QUIT:
            //g_plugin.m_exiting = 1;
            //PostMessage(WM_CLOSE, static_cast<WPARAM>(0), static_cast<LPARAM>(0));
            break;
    }

    Invalidate();
}

LRESULT milk2_ui_element::OnImeNotify(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    MILK2_CONSOLE_LOG("OnImeNotify ", GetWnd())
    if (uMsg != WM_IME_NOTIFY)
        return 1;
    if (wParam == IMN_CLOSESTATUSWINDOW)
    {
        if ((s_fullscreen || s_popout_fullscreen) && !s_in_toggle)
            ToggleFullScreen();
    }
    return 0;
}

LRESULT milk2_ui_element::OnQuit(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(uMsg);
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    MILK2_CONSOLE_LOG("OnQuit ", GetWnd())
    return 0;
}

LRESULT milk2_ui_element::OnMilk2Message(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg != WM_MILK2)
        return -1;

    if (wParam == MILK2_WPARAM_REFRESH_PRESET_LIST)
    {
        g_plugin.UpdatePresetList(false, true, true);
        return 0;
    }
    if (wParam == MILK2_WPARAM_RANDOM_PRESET)
    {
        RandomPreset(0.0f);
        return 0;
    }

    auto api = playlist_manager::get();
    if (LOBYTE(wParam) == 0x21 && HIBYTE(wParam) == 0x09)
    {
        wchar_t buf[2048], title[64];
        LoadString(core_api::get_my_instance(), LOWORD(lParam), buf, 2048);
        LoadString(core_api::get_my_instance(), HIWORD(lParam), title, 64);
        MILK2_CONSOLE_LOG("milk2 -> title: ", title, ", message: ", buf)
        return HIWORD(lParam) == IDS_MILKDROP_ERROR || HIWORD(lParam) == IDS_MILKDROP_WARNING ? 0 : 1;
    }
    else if (lParam == IPC_GETVERSION)
    {
        MILK2_CONSOLE_LOG("IPC_GETVERSION")
        //const t_core_version_data v = core_version_info_v2::get_version();
        return 1;
    }
    else if (lParam == IPC_GETVERSIONSTRING)
    {
        MILK2_CONSOLE_LOG("IPC_GETVERSIONSTRING")
        return reinterpret_cast<LRESULT>(core_version_info_v2::g_get_version_string()); // "foobar2000 v2.1.2"
        return 0;
    }
    else if (lParam == IPC_ISPLAYING)
    {
        MILK2_CONSOLE_LOG("IPC_ISPLAYING")
        if (m_playback_control->is_playing())
            return 1;
        else if (m_playback_control->is_paused())
            return 3;
        else
            return 0;
    }
    else if (lParam == IPC_SETPLAYLISTPOS)
    {
        //MILK2_CONSOLE_LOG("IPC_SETPLAYLISTPOS")
        SetSelectionSingle(static_cast<size_t>(g_plugin.m_playlist_pos));
        return static_cast<LRESULT>(wParam);
    }
    else if (lParam == IPC_GETLISTLENGTH)
    {
        //MILK2_CONSOLE_LOG("IPC_GETLISTLENGTH")
        const size_t count = api->activeplaylist_get_item_count();
        return static_cast<LRESULT>(count);
    }
    else if (lParam == IPC_GETLISTPOS)
    {
        //MILK2_CONSOLE_LOG("IPC_GETLISTPOS")
        if (m_playback_control->is_playing())
        {
            size_t playing_index = NULL, playing_playlist = NULL;
            bool valid = api->get_playing_item_location(&playing_playlist, &playing_index);
            if (valid && playing_playlist == api->get_active_playlist())
                return static_cast<LRESULT>(playing_index);
        }
        return -1;
    }
    else if (lParam == IPC_GETPLAYLISTTITLEW || lParam == IPC_GET_PLAYING_TITLE)
    {
        //MILK2_CONSOLE_LOG(IPC_GETPLAYLISTTITLEW ? "IPC_GETPLAYLISTTITLEW" : "IPC_GET_PLAYING_TITLE")
        titleformat_object::ptr title_format;
        if (lParam == IPC_GETPLAYLISTTITLEW && m_script.is_empty())
        {
            pfc::string8 pattern = pfc::utf8FromWide(s_config.settings.m_szTitleFormat);
            static_api_ptr_t<titleformat_compiler>()->compile_safe_ex(m_script, pattern);
        }
        if (lParam == IPC_GET_PLAYING_TITLE && m_title.is_empty())
        {
            pfc::string8 pattern = default_szTitleFormat;
            static_api_ptr_t<titleformat_compiler>()->compile_safe_ex(m_title, pattern);
        }
        pfc::string_formatter state;
        metadb_handle_list list;
        api->activeplaylist_get_all_items(list);
        if (list.size() == 0)
            state = ""; // no playlist
        else if (wParam == -1 || !(list.get_item(static_cast<size_t>(wParam)))->format_title(NULL, state, IPC_GETPLAYLISTTITLEW ? m_script : m_title, NULL))
            if (m_playback_control->is_playing())
                state = "Opening...";
            else
                state = "Stopped.";
        m_szBuffer = pfc::wideFromUTF8(state);
        return reinterpret_cast<LRESULT>(m_szBuffer.c_str());
    }
    else if (lParam == IPC_GETOUTPUTTIME)
    {
        //MILK2_CONSOLE_LOG("IPC_GETOUTPUTTIME")
        if (wParam == 0)
        {
            if (m_playback_control->is_playing())
                return static_cast<LRESULT>(m_playback_control->playback_get_position() * 1000);
        }
        else if (wParam == 1)
        {
            if (m_playback_control->is_playing())
                return static_cast<LRESULT>(m_playback_control->playback_get_length());
        }
        else if (wParam == 2)
        {
            if (m_playback_control->is_playing())
                return static_cast<LRESULT>(m_playback_control->playback_get_length() * 1000);
        }
        return -1;
    }
    else if (lParam == IPC_GETPLUGINDIRECTORYW)
    {
        //MILK2_CONSOLE_LOG("IPC_GETPLUGINDIRECTORYW")
        m_szBuffer = s_config.settings.m_szPluginsDirPath;
        return reinterpret_cast<LRESULT>(m_szBuffer.c_str());
    }
    else if (lParam == IPC_GETINIDIRECTORYW)
    {
        //MILK2_CONSOLE_LOG("IPC_GETINIDIRECTORYW")
        m_szBuffer = s_config.settings.m_szConfigIniFile;
        size_t p = m_szBuffer.find_last_of(L"\\");
        if (p != std::wstring::npos)
            m_szBuffer = m_szBuffer.substr(0, p + 1);
        return reinterpret_cast<LRESULT>(m_szBuffer.c_str());
    }
    else if (lParam == IPC_FETCH_ALBUMART)
    {
        MILK2_CONSOLE_LOG("IPC_FETCH_ALBUMART")
        if (m_art_file.empty())
        {
            m_art_data->imgData = m_raster.data();
            m_art_data->imgDataLen = static_cast<int>(m_raster.size());
            m_art_data->type[0] = L'j'; m_art_data->type[1] = L'p'; m_art_data->type[2] = L'g'; m_art_data->type[3] = L'\0';
            m_art_data->gracenoteFileId = nullptr;
        }
        else
        {
            m_art_data->imgData = nullptr;
            m_art_data->imgDataLen = 0;
            m_art_data->gracenoteFileId = m_art_file.data();
            std::wstring ext = GetExtension(m_art_file);
            std::copy_n(ext.begin(), ext.size(), &m_art_data->type[0]);
            std::fill_n(&m_art_data->type[0] + ext.size() + 1, 10 - ext.size(), L'\0');
        }
        s_config.settings.m_artData = m_art_data.get();

        return reinterpret_cast<LRESULT>(m_art_data.get());
    }
#if 0
    else if (lParam == IPC_SETVISWND)
    {
        //MILK2_CONSOLE_LOG("IPC_SETVISWND")
        //SendMessage(m_hwnd_winamp, WM_WA_IPC, (WPARAM)m_hwnd, IPC_SETVISWND);
    }
    else if (lParam == IPC_CB_VISRANDOM)
    {
        //MILK2_CONSOLE_LOG("IPC_CB_VISRANDOM")
        //SendMessage(GetWinampWindow(), WM_WA_IPC, (m_bPresetLockOnAtStartup ? 0 : 1) << 16, IPC_CB_VISRANDOM);
    }
    else if (lParam == IPC_IS_PLAYING_VIDEO)
    {
        //MILK2_CONSOLE_LOG("IPC_IS_PLAYING_VIDEO")
        //if (m_screenmode == FULLSCREEN && SendMessage(GetWinampWindow(), WM_WA_IPC, 0, IPC_IS_PLAYING_VIDEO) > 1)
    }
    else if (lParam == IPC_SET_VIS_FS_FLAG)
    {
        //MILK2_CONSOLE_LOG("IPC_SET_VIS_FS_FLAG")
        //SendMessage(GetWinampWindow(), WM_WA_IPC, 1, IPC_SET_VIS_FS_FLAG);
    }
    else if (lParam == IPC_GET_D3DX9)
    {
        //MILK2_CONSOLE_LOG("IPC_GET_D3DX9")
        //HMODULE d3dx9 = (HMODULE)SendMessage(winamp, WM_WA_IPC, 0, IPC_GET_D3DX9);
    }
    else if (lParam == IPC_GET_API_SERVICE)
    {
        //MILK2_CONSOLE_LOG("IPC_GET_API_SERVICE")
        //WASABI_API_SVC = (api_service*)SendMessage(hwndParent, WM_WA_IPC, 0, IPC_GET_API_SERVICE);
    }
    else if (lParam == IPC_GETDIALOGBOXPARENT)
    {
        //MILK2_CONSOLE_LOG("IPC_GETDIALOGBOXPARENT")
        //HWND parent = (HWND)SendMessage(winamp, WM_WA_IPC, 0, IPC_GETDIALOGBOXPARENT);
    }
    else if (lParam == IPC_GET_RANDFUNC)
    {
        //MILK2_CONSOLE_LOG("IPC_GET_RANDFUNC")
        //int warand();
        //warand = (int (*)(void))SendMessage(hwndParent, WM_WA_IPC, 0, IPC_GET_RANDFUNC);
    }
#endif

    return 1;
}

LRESULT milk2_ui_element::OnRenderMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    if (uMsg != WM_MILK2_RENDER)
        return -1;

#ifdef TIMER_TP
    m_renderPending = false;
    m_renderPostTick = 0;
#endif
    LONGLONG renderQpc = 0;
    bool rendered = false;
    if (m_milk2)
    {
#ifdef TIMER_TP
        if (!TryBeginFrame(renderQpc))
            return 0;
#endif
        Tick();
        rendered = true;
    }
#ifdef TIMER_TP
    if (m_milk2 && rendered)
        FinishFrameTiming(renderQpc);
    if (m_milk2)
        ScheduleNextFrameTimer();
#endif

    return 0;
}

LRESULT milk2_ui_element::OnRestoreWindowed(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    if (uMsg != WM_MILK2_RESTORE_WINDOWED)
        return -1;

    if (s_fullscreen || s_popout_fullscreen || m_milk2)
        return 0;

    int w, h;
    GetDefaultSize(w, h);

    CRect r{};
    if (::GetClientRect(get_wnd(), &r) && r.right - r.left > 0 && r.bottom - r.top > 0)
    {
        w = r.right - r.left;
        h = r.bottom - r.top;
    }

    if (!Initialize(get_wnd(), w, h))
    {
        FB2K_console_print(core_api::get_my_file_name(), ": Could not restore MilkDrop after fullscreen");
        return 0;
    }

    InvalidateRect(nullptr, FALSE);
    return 0;
}

LRESULT milk2_ui_element::OnRepairWindowedD3D12(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    if (uMsg != WM_MILK2_REPAIR_WINDOWED_DX12)
        return -1;

    if (s_fullscreen || s_popout_fullscreen || !m_milk2 || !g_plugin.IsD3D12Active())
        return 0;

    CRect r{};
    if (!::GetClientRect(get_wnd(), &r) || r.right - r.left <= 0 || r.bottom - r.top <= 0)
        return 0;

    const int width = std::max<int>(r.right - r.left, 128);
    const int height = std::max<int>(r.bottom - r.top, 128);

#ifdef TIMER_TP
    if (TryEnterCriticalSection(&s_cs) == 0)
        return 0;
#endif
    g_plugin.CaptureD3D12VisualState();
    if (g_plugin.RestartD3D12ForWindow(get_wnd(), width, height, WINDOWED))
    {
        g_plugin.RestoreD3D12VisualState();
        g_plugin.ResumeD3D12AfterWindowSwap();
    }
#ifdef TIMER_TP
    LeaveCriticalSection(&s_cs);
#endif

    ::PostMessage(get_wnd(), WM_MILK2_RENDER, 0, 0);
    InvalidateRect(nullptr, FALSE);
    return 0;
}

LRESULT milk2_ui_element::OnConfigurationChange(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    MILK2_CONSOLE_LOG("OnConfigurationChange ", GetWnd())
    if (uMsg != WM_CONFIG_CHANGE)
        return 1;
    switch (wParam)
    {
        case 0: // Preferences Dialog
        case 1: // Advanced Preferences
            {
                s_config.reset();
                m_script.reset();
                g_plugin.PanelSettings(&s_config.settings);
                ApplyFrameRateLimit();
                if (s_popout && !s_popout_fullscreen)
                {
                    const DWORD baseStyle = WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
                    const DWORD style = s_config.settings.m_bPopoutBorderless ? (WS_POPUP | baseStyle) : (WS_OVERLAPPEDWINDOW | baseStyle);
                    ::SetWindowLongPtr(get_wnd(), GWL_STYLE, static_cast<LONG_PTR>(style));
                    ::SetWindowLongPtr(get_wnd(), GWL_EXSTYLE, static_cast<LONG_PTR>(WS_EX_APPWINDOW));
                    ::SetWindowPos(get_wnd(), nullptr, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                }
#ifdef TIMER_TP
                StartTimer();
#endif
                UpdateChannelMode();
                break;
            }
    }

    if (s_milk2 && m_milk2)
    {
#ifdef TIMER_TP
        EnterCriticalSection(&s_cs);
#endif
        RECT rect{};
        GetClientRect(&rect);
        g_plugin.OnWindowSizeChanged(rect.right - rect.left, rect.bottom - rect.top);
#ifdef TIMER_TP
        LeaveCriticalSection(&s_cs);
#endif
    }
    SetMsgHandled(TRUE);

    return 0;
}

// Initialize the Direct3D resources required to run.
bool milk2_ui_element::Initialize(HWND window, int width, int height)
{
    const auto initializeFreshPlugin = [&]() -> bool
    {
        if (FALSE == g_plugin.PluginPreInitialize(window, core_api::get_my_instance()))
            return false;
        if (!g_plugin.PanelSettings(&s_config.settings))
            return false;

        swprintf_s(g_plugin.m_szComponentDirPath, L"%ls", const_cast<wchar_t*>(m_pwd.c_str()));
        g_plugin.SetWinampWindow(window);
        g_plugin.SetScreenMode((s_fullscreen || s_popout_fullscreen) ? FULLSCREEN : WINDOWED);
        ApplyFrameRateLimit();

        if (FALSE == g_plugin.PluginInitialize(width, height))
            return false;
        g_plugin.SetFoobarPlaybackActive(m_playback_control->is_playing() && !m_playback_control->is_paused());
        if (g_plugin.IsD3D12Active())
        {
            g_plugin.RestoreD3D12VisualState();
            g_plugin.ResumeD3D12AfterWindowSwap();
        }

        HICON hIcon = ::LoadIcon(_AtlBaseModule.GetResourceInstance(), MAKEINTRESOURCE(IDI_MILK2_ICON));
        HWND parent = GetRealParent(get_wnd());
        ::SetClassLongPtr(parent, GCLP_HICON, (LONG_PTR)hIcon);
        ::SetClassLongPtr(parent, GCLP_HICONSM, (LONG_PTR)hIcon);

        s_milk2 = true;
        return true;
    };

    if (!s_milk2)
    {
        if (!s_fullscreen && !s_popout_fullscreen)
            g_hWindow = get_wnd();
    }

    if (!s_milk2)
    {
        if (!initializeFreshPlugin())
            return false;
    }
    else
    {
        const bool rebuildD3D12 = g_plugin.IsD3D12Active();
#ifdef TIMER_TP
        EnterCriticalSection(&s_cs);
#endif
        if (rebuildD3D12)
        {
            g_plugin.CaptureD3D12VisualState();
            if (!g_plugin.RestartD3D12ForWindow(window, width, height, (s_fullscreen || s_popout_fullscreen) ? FULLSCREEN : WINDOWED))
            {
#ifdef TIMER_TP
                LeaveCriticalSection(&s_cs);
#endif
                return false;
            }
            g_plugin.RestoreD3D12VisualState();
            g_plugin.ResumeD3D12AfterWindowSwap();
        }
        else
        {
            g_plugin.SetWinampWindow(window);
            g_plugin.SetScreenMode((s_fullscreen || s_popout_fullscreen) ? FULLSCREEN : WINDOWED);
            g_plugin.OnWindowSwap(window, width, height);
            g_plugin.ResumeD3D12AfterWindowSwap();
        }
        ApplyFrameRateLimit();
#ifdef TIMER_TP
        LeaveCriticalSection(&s_cs);
#endif
    }

    m_milk2 = true;
    ApplyFrameRateLimit();
    UpdateFoobarPlaybackState();
#ifdef TIMER_TP
    m_renderPending = false;
    m_renderPostTick = 0;
    StartTimer();
    ::PostMessage(get_wnd(), WM_MILK2_RENDER, 0, 0);
#endif
    if (!s_fullscreen && !s_popout_fullscreen && g_plugin.IsD3D12Active())
        ::SetTimer(get_wnd(), WM_MILK2_REPAIR_WINDOWED_DX12, 250, nullptr);

    return true;
}

#pragma region Frame Update
// Executes the render.
void milk2_ui_element::Tick()
{
    if (!m_milk2)
        return;

#ifdef TIMER_TP
    if (TryEnterCriticalSection(&s_cs) == 0)
        return;
    if (!m_milk2)
    {
        LeaveCriticalSection(&s_cs);
        return;
    }
#endif

#ifdef TIMER_DX
    m_timer.Tick([&]() { Update(m_timer); });
#endif

    FlushPendingAnimatedText();
    Render();

#ifdef TIMER_TP
    LeaveCriticalSection(&s_cs);

    InvalidateRect(NULL, TRUE);
#endif
}

#ifdef TIMER_DX
// Updates the world.
void milk2_ui_element::Update(DX::StepTimer const& timer)
{
}
#endif
#pragma endregion

#pragma region Frame Render
// Draws the scene.
HRESULT milk2_ui_element::Render()
{
    // Do not try to render anything before the first `Update()`.
#ifdef TIMER_DX
    if (m_timer.GetFrameCount() == 0)
    {
        return S_OK;
    }
#else
    if (g_plugin.GetFrame() == 0)
    {
    }
#endif

    Clear();

    return g_plugin.PluginRender(waves[0].data(), waves[1].data());
}

// Clears the back buffers and the window contents.
void milk2_ui_element::Clear()
{
#if 0
    HDC hdc = GetDC();
    RECT rect{};
    GetClientRect(&rect);
    FillRect(hdc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
    ReleaseDC(hdc);
#endif
}

void milk2_ui_element::BuildWaves()
{
    //if (!m_vis_stream.is_valid())
    //    return;

    if (!m_playback_control->is_playing() || m_playback_control->is_paused())
    {
        std::fill(waves[0].begin(), waves[0].end(), 0.0f);
        std::fill(waves[1].begin(), waves[1].end(), 0.0f);
        return;
    }

    double time;
    if (!m_vis_stream->get_absolute_time(time))
    {
        std::fill(waves[0].begin(), waves[0].end(), 0.0f);
        std::fill(waves[1].begin(), waves[1].end(), 0.0f);
        return;
    }

    double dt = time - m_last_time;
    m_last_time = time;

    constexpr double min_time = 1.0 / 1000.0;
    constexpr double max_time = 1.0 / 10.0;

    bool use_fake = false;

    if (dt < min_time)
    {
        dt = min_time;
        use_fake = true;
    }
    else if (dt > max_time)
        dt = max_time;

    audio_chunk_impl chunk;
    if (use_fake || !m_vis_stream->get_chunk_absolute(chunk, time - dt, dt))
    {
        //m_vis_stream->make_fake_chunk_absolute(chunk, time - dt, dt);
        for (uint32_t i = 0; i < static_cast<uint32_t>(NUM_AUDIO_BUFFER_SAMPLES); ++i)
        {
            waves[0][i] = waves[1][i] = 0U;
        }
        return;
    }
    auto count = chunk.get_sample_count();
    //auto sample_rate = chunk.get_srate();
    auto channels = chunk.get_channel_count();
    audio_sample* audio_data = chunk.get_data();

    size_t top = std::min(count / channels, static_cast<size_t>(NUM_AUDIO_BUFFER_SAMPLES));
    for (size_t i = 0; i < top; ++i)
    {
        waves[0][i] = static_cast<float>(audio_data[i * channels] * 128.0f);
        if (channels >= 2)
            waves[1][i] = static_cast<float>(audio_data[i * channels + 1] * 128.0f);
        else
            waves[1][i] = waves[0][i];
    }
}
#pragma endregion

#pragma region Message Handlers
void milk2_ui_element::OnActivated()
{
}

void milk2_ui_element::OnDeactivated()
{
}

void milk2_ui_element::OnSuspending()
{
}

void milk2_ui_element::OnResuming()
{
#ifdef TIMER_DX
    m_timer.ResetElapsedTime();
#endif
}
#pragma endregion

// Properties
void milk2_ui_element::GetDefaultSize(int& width, int& height) const noexcept
{
    width = 640;
    height = 480;
}

void milk2_ui_element::SetPwd(std::wstring pwd) noexcept
{
    m_pwd.assign(pwd);
}

void milk2_ui_element::ApplyFrameRateLimit() noexcept
{
    uint32_t maxFps = normalize_frame_limit(s_config.settings.m_max_fps_fs);
    if (s_config.settings.m_max_fps_fs != maxFps)
        s_config.settings.m_max_fps_fs = maxFps;

    m_refresh_interval = get_refresh_interval_ms(maxFps);
#ifdef TIMER_TP
    LONGLONG frameInterval = 0;
    if (m_frameTimerFrequencyQpc > 0)
    {
        if (maxFps != 0)
        {
            const LONGLONG fps = static_cast<LONGLONG>(maxFps);
            frameInterval = (std::max)(1LL, (m_frameTimerFrequencyQpc + fps / 2) / fps);
        }
    }
    m_frameIntervalQpc = frameInterval;
    m_nextFrameQpc = 0;
    m_lastRenderQpc = 0;
    ResetFrameTimingStats();
    SetFrameTimerResolution(m_milk2 && (maxFps == 0 || frameInterval > 0));

    wchar_t logLine[256]{};
    swprintf_s(logLine,
               L"frame limiter mode=%ls target=%u interval_qpc=%lld timer_resolution=%d",
               L"capped",
               maxFps,
               frameInterval,
               m_timerResolutionActive ? 1 : 0);
    write_dx12_ui_log_line(logLine);
#endif

    // The foobar UI scheduler owns frame pacing. Keep the legacy shell sleep
    // limiter disabled so it cannot double-throttle fullscreen frames.
    g_plugin.SetFoobarFullscreenFrameLimit(0);
}

void milk2_ui_element::ResizePluginToCurrentClient(const char* context, bool wait_for_lock) noexcept
{
    UNREFERENCED_PARAMETER(context);

    RECT rect{};
    if (!::GetClientRect(get_wnd(), &rect))
        return;

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
        return;

    width = std::max<int>(width, 128);
    height = std::max<int>(height, 128);
    const eScrMode screenMode = (s_fullscreen || s_popout_fullscreen) ? FULLSCREEN : WINDOWED;

#ifdef TIMER_TP
    bool lockHeld = false;
    if (wait_for_lock)
    {
        EnterCriticalSection(&s_cs);
        lockHeld = true;
    }
    else if (TryEnterCriticalSection(&s_cs) != 0)
    {
        lockHeld = true;
    }
    else
    {
        return;
    }
#endif

    g_plugin.SetWinampWindow(get_wnd());
    g_plugin.SetScreenMode(screenMode);
    if (g_plugin.IsD3D12Active())
    {
        g_plugin.CaptureD3D12VisualState();
        if (g_plugin.RestartD3D12ForWindow(get_wnd(), width, height, screenMode))
        {
            g_plugin.RestoreD3D12VisualState();
            g_plugin.ResumeD3D12AfterWindowSwap();
        }
        else
        {
            g_plugin.OnWindowSizeChanged(width, height);
        }
    }
    else
    {
        g_plugin.OnWindowMoved();
        g_plugin.OnWindowSizeChanged(width, height);
    }

#ifdef TIMER_TP
    if (lockHeld)
        LeaveCriticalSection(&s_cs);
#endif
}

void milk2_ui_element::ToggleFullScreen()
{
    MILK2_CONSOLE_LOG("ToggleFullScreen0 ", GetWnd())
    if (s_popout)
    {
        TogglePopoutFullscreen();
        return;
    }

    if (m_milk2)
    {
        const bool enteringFullscreen = !s_fullscreen;
        if (enteringFullscreen)
            g_hWindow = get_wnd();

        m_milk2 = false;
#ifdef TIMER_TP
        m_renderPending = false;
        m_renderPostTick = 0;
        StopTimer();
#endif
#if 0
        if (s_fullscreen)
        {
            SetWindowLongPtr(GWL_STYLE, WS_OVERLAPPEDWINDOW);
            SetWindowLongPtr(GWL_EXSTYLE, 0);

            int width = 800;
            int height = 600;

            ShowWindow(SW_SHOWNORMAL);

            SetWindowPos(HWND_TOP, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        else
        {
            SetWindowLongPtr(GWL_STYLE, WS_POPUP);
            SetWindowLongPtr(GWL_EXSTYLE, WS_EX_TOPMOST);

            SetWindowPos(HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

            ShowWindow(SW_SHOWMAXIMIZED);
        }
#endif
        s_in_toggle = true;
        s_fullscreen = enteringFullscreen;
        ApplyFrameRateLimit();
        SetTopMost();
        static_api_ptr_t<ui_element_common_methods_v2>()->toggle_fullscreen(g_get_guid(), core_api::get_main_window());
        MILK2_CONSOLE_LOG("ToggleFullScreen1 ", GetWnd())
    }
}

void milk2_ui_element::ToggleHelp()
{
    g_plugin.ToggleHelp();
}

void milk2_ui_element::TogglePlaylist()
{
    g_plugin.TogglePlaylist();
}

void milk2_ui_element::ToggleSongTitle()
{
    s_config.settings.m_bShowSongTitle = !s_config.settings.m_bShowSongTitle;
    g_plugin.m_bShowSongTitle = s_config.settings.m_bShowSongTitle;
    s_config.persist_runtime_settings();
}

void milk2_ui_element::ToggleSongLength()
{
    if (s_config.settings.m_bShowSongTime && s_config.settings.m_bShowSongLen)
    {
        s_config.settings.m_bShowSongTime = false;
        s_config.settings.m_bShowSongLen = false;
    }
    else if (s_config.settings.m_bShowSongTime && !s_config.settings.m_bShowSongLen)
    {
        s_config.settings.m_bShowSongLen = true;
    }
    else
    {
        s_config.settings.m_bShowSongTime = true;
        s_config.settings.m_bShowSongLen = false;
    }
    g_plugin.m_bShowSongTime = s_config.settings.m_bShowSongTime;
    g_plugin.m_bShowSongLen = s_config.settings.m_bShowSongLen;
    s_config.persist_runtime_settings();
}

void milk2_ui_element::TogglePresetInfo()
{
    s_config.settings.m_bShowPresetInfo = !s_config.settings.m_bShowPresetInfo;
    g_plugin.m_bShowPresetInfo = s_config.settings.m_bShowPresetInfo;
    s_config.persist_runtime_settings();
}

void milk2_ui_element::ToggleFps()
{
    s_config.settings.m_bShowFPS = !s_config.settings.m_bShowFPS;
    g_plugin.m_bShowFPS = s_config.settings.m_bShowFPS;
    s_config.persist_runtime_settings();
}

void milk2_ui_element::ToggleRating()
{
    s_config.settings.m_bShowRating = !s_config.settings.m_bShowRating;
    g_plugin.m_bShowRating = s_config.settings.m_bShowRating;
    s_config.persist_runtime_settings();
}

void milk2_ui_element::ToggleShaderHelp()
{
    s_config.settings.m_bShowShaderHelp = !s_config.settings.m_bShowShaderHelp;
    g_plugin.m_bShowShaderHelp = s_config.settings.m_bShowShaderHelp;
    s_config.persist_runtime_settings();
}

const char* milk2_ui_element::ToggleShuffle(bool forward = true)
{
    auto api = playlist_manager::get();
    size_t nModeCount = api->playback_order_get_count();
    size_t nCurrentMode = api->playback_order_get_active();
    size_t nNewMode = (forward ? ++nCurrentMode % nModeCount : (nCurrentMode == 0 ? nModeCount - 1 : --nCurrentMode));
    api->playback_order_set_active(nNewMode);
    return api->playback_order_get_name(nNewMode);
}

void milk2_ui_element::NextPreset(float fBlendTime)
{
    g_plugin.NextPreset(fBlendTime);
}

void milk2_ui_element::PrevPreset(float fBlendTime)
{
    g_plugin.PrevPreset(fBlendTime);
}

bool milk2_ui_element::LoadPreset(int select)
{
#if 0
    g_plugin.m_nCurrentPreset = select + g_plugin.m_nDirs;

    wchar_t szFile[MAX_PATH] = {0};
    wcscpy_s(szFile, g_plugin.m_szPresetDir); // Note: m_szPresetDir always ends with '\'
    wcscat_s(szFile, g_plugin.m_presets[g_plugin.m_nCurrentPreset].szFilename.c_str());

    g_plugin.LoadPreset(szFile, 1.0f);
#else
    UNREFERENCED_PARAMETER(select);
#endif
    return true;
}

void milk2_ui_element::LoadPresetFromFile()
{
    wchar_t selectedFile[MAX_PATH] = {0};
    std::wstring currentPath = g_plugin.GetCurrentPresetPath();
    if (!currentPath.empty() && currentPath.size() < std::size(selectedFile))
    {
        wcscpy_s(selectedFile, currentPath.c_str());
    }

    wchar_t initialDir[MAX_PATH] = {0};
    const wchar_t* presetDir = g_plugin.GetPresetDir();
    if (presetDir && presetDir[0])
    {
        wcscpy_s(initialDir, presetDir);
    }

    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFile = selectedFile;
    ofn.nMaxFile = static_cast<DWORD>(std::size(selectedFile));
    ofn.lpstrInitialDir = initialDir[0] ? initialDir : nullptr;
    ofn.lpstrFilter = L"MilkDrop presets (*.milk)\0*.milk\0All files (*.*)\0*.*\0";
    ofn.lpstrDefExt = L"milk";
    ofn.lpstrTitle = L"Load MilkDrop preset";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    if (!GetOpenFileName(&ofn))
        return;

    wchar_t logLine[MAX_PATH + 64]{};
    swprintf_s(logLine, L"manual preset load file=\"%ls\"", selectedFile);
    write_dx12_ui_log_line(logLine);

    g_plugin.LoadPreset(selectedFile, 0.0f);
    g_plugin.SetPresetListPosition(selectedFile);
}

std::wstring milk2_ui_element::GetCurrentPreset()
{
    return g_plugin.GetCurrentPresetFilename();
}

void milk2_ui_element::OpenCurrentPresetLocation()
{
    const std::wstring presetPath = g_plugin.GetCurrentPresetPath();
    if (presetPath.empty())
        return;

    const std::wstring arguments = L"/select,\"" + presetPath + L"\"";
    ShellExecute(m_hWnd, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

void milk2_ui_element::BlacklistCurrentPreset()
{
    const std::wstring presetName = g_plugin.GetCurrentPresetFilename();
    if (presetName.empty())
        return;

    if (!g_plugin.AddPresetToBlacklist(presetName))
        return;

    KillTimer(ID_BLACKLIST_TIMER);
    m_blacklist_load_retries = 0;
    SetTimer(ID_BLACKLIST_TIMER, 1, nullptr);
}

void milk2_ui_element::LockPreset(bool lockUnlock)
{
    g_plugin.m_bPresetLockedByUser = lockUnlock;
}

bool milk2_ui_element::IsPresetLock()
{
    return g_plugin.m_bPresetLockedByUser || g_plugin.m_bPresetLockedByCode;
}

void milk2_ui_element::RandomPreset(float fBlendTime)
{
    g_plugin.LoadRandomPreset(fBlendTime);
}

void milk2_ui_element::SetPresetRating(float inc_dec)
{
    g_plugin.SetCurrentPresetRating(g_plugin.m_pState->m_fRating + inc_dec);
}

void milk2_ui_element::Seek(UINT nRepCnt, bool bShiftHeldDown, double seekDelta)
{
    int reps = (bShiftHeldDown) ? 6 * nRepCnt : 1 * nRepCnt;
    if (seekDelta > 0.0)
    {
        if (!m_playback_control->playback_can_seek())
        {
            m_playback_control->next();
            return;
        }
    }
    else
    {
        if (!m_playback_control->playback_can_seek())
        {
            m_playback_control->previous();
            return;
        }
        double p = m_playback_control->playback_get_position();
        if (p < 0.0)
            return;
        if (p < (seekDelta * -1.0))
        {
            if (p < (seekDelta * -1.0) / 3.0)
            {
                if (m_playback_control->is_playing() && p > (seekDelta * -1.0))
                    m_playback_control->playback_seek(0.0);
                else
                    m_playback_control->previous();
                return;
            }
            else
            {
                m_playback_control->playback_seek(0.0);
                return;
            }
        }
    }
    for (int i = 0; i < reps; ++i)
        m_playback_control->playback_seek_delta(seekDelta);
}

// clang-format off
void milk2_ui_element::UpdateChannelMode()
{
    if (m_vis_stream.is_valid())
    {
        m_vis_stream->set_channel_mode(s_config.settings.m_bEnableDownmix ? visualisation_stream_v3::channel_mode_mono : visualisation_stream_v3::channel_mode_default);
    }
}

void milk2_ui_element::TogglePopout()
{
    if (s_popout)
        ReturnPopoutToPanel();
    else
        OpenPopoutWindow();
}

void milk2_ui_element::OpenPopoutWindow()
{
    if (!m_milk2 || s_popout || s_fullscreen || !::IsWindow(get_wnd()))
        return;

    HWND hwnd = get_wnd();
    HWND parent = ::GetParent(hwnd);
    if (!parent || !::IsWindow(parent))
        return;

    RECT windowRect{};
    RECT parentRect{};
    if (!::GetWindowRect(hwnd, &windowRect))
        return;

    parentRect = windowRect;
    ::MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&parentRect), 2);

    int width = windowRect.right - windowRect.left;
    int height = windowRect.bottom - windowRect.top;
    width = (std::max)(width, 800);
    height = (std::max)(height, 600);

    HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!::GetMonitorInfo(monitor, &monitorInfo))
        monitorInfo.rcWork = {100, 100, 100 + width, 100 + height};

    const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    width = (std::min)(width, workWidth);
    height = (std::min)(height, workHeight);
    const int x = monitorInfo.rcWork.left + (workWidth - width) / 2;
    const int y = monitorInfo.rcWork.top + (workHeight - height) / 2;

    s_popout_parent = parent;
    s_popout_panel_style = ::GetWindowLongPtr(hwnd, GWL_STYLE);
    s_popout_panel_exstyle = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    s_popout_panel_rect = parentRect;
    s_popout_window_rect = {x, y, x + width, y + height};
    s_popout = true;
    s_popout_fullscreen = false;

#ifdef TIMER_TP
    m_renderPending = false;
    m_renderPostTick = 0;
    StopTimer();
#endif

    const DWORD baseStyle = WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    const DWORD style = s_config.settings.m_bPopoutBorderless ? (WS_POPUP | baseStyle) : (WS_OVERLAPPEDWINDOW | baseStyle);
    const DWORD exstyle = WS_EX_APPWINDOW;

    ::SetParent(hwnd, nullptr);
    ::SetWindowLongPtr(hwnd, GWL_STYLE, static_cast<LONG_PTR>(style));
    ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(exstyle));
    ::SetWindowText(hwnd, L"MilkDrop");
    ::SetWindowPos(hwnd, HWND_TOP, x, y, width, height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    ::ShowWindow(hwnd, SW_SHOWNORMAL);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);

    ApplyFrameRateLimit();
    ResizePluginToCurrentClient("OpenPopoutWindow", true);

#ifdef TIMER_TP
    StartTimer();
    ::PostMessage(get_wnd(), WM_MILK2_RENDER, 0, 0);
#endif
    InvalidateRect(nullptr, FALSE);
}

void milk2_ui_element::ReturnPopoutToPanel()
{
    if (!s_popout || !::IsWindow(get_wnd()))
        return;

    HWND hwnd = get_wnd();
    if (!s_popout_fullscreen)
        ::GetWindowRect(hwnd, &s_popout_window_rect);

    HWND parent = s_popout_parent;
    if (!parent || !::IsWindow(parent))
    {
        s_popout = false;
        s_popout_fullscreen = false;
        s_popout_parent = nullptr;
        return;
    }

#ifdef TIMER_TP
    m_renderPending = false;
    m_renderPostTick = 0;
    StopTimer();
#endif

    s_popout = false;
    s_popout_fullscreen = false;
    ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::SetWindowLongPtr(hwnd, GWL_STYLE, s_popout_panel_style);
    ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, s_popout_panel_exstyle);
    ::SetParent(hwnd, parent);
    ::SetWindowText(hwnd, SHORTNAME);

    const int width = (std::max)(1L, s_popout_panel_rect.right - s_popout_panel_rect.left);
    const int height = (std::max)(1L, s_popout_panel_rect.bottom - s_popout_panel_rect.top);
    ::SetWindowPos(hwnd,
                   HWND_TOP,
                   s_popout_panel_rect.left,
                   s_popout_panel_rect.top,
                   width,
                   height,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    s_popout_parent = nullptr;

    ApplyFrameRateLimit();
    ResizePluginToCurrentClient("ReturnPopoutToPanel", true);

#ifdef TIMER_TP
    StartTimer();
    ::PostMessage(get_wnd(), WM_MILK2_RENDER, 0, 0);
#endif
    InvalidateRect(nullptr, FALSE);
}

void milk2_ui_element::TogglePopoutFullscreen()
{
    if (!s_popout || !m_milk2 || !::IsWindow(get_wnd()))
        return;

    HWND hwnd = get_wnd();

#ifdef TIMER_TP
    m_renderPending = false;
    m_renderPostTick = 0;
    StopTimer();
#endif

    const DWORD baseStyle = WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    if (!s_popout_fullscreen)
    {
        ::GetWindowRect(hwnd, &s_popout_window_rect);
        HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!::GetMonitorInfo(monitor, &monitorInfo))
            monitorInfo.rcMonitor = s_popout_window_rect;

        s_popout_fullscreen = true;
        ::SetWindowLongPtr(hwnd, GWL_STYLE, static_cast<LONG_PTR>(WS_POPUP | baseStyle));
        ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(WS_EX_APPWINDOW));
        ::SetWindowPos(hwnd,
                       HWND_TOPMOST,
                       monitorInfo.rcMonitor.left,
                       monitorInfo.rcMonitor.top,
                       monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                       monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                       SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    else
    {
        s_popout_fullscreen = false;
        const DWORD style = s_config.settings.m_bPopoutBorderless ? (WS_POPUP | baseStyle) : (WS_OVERLAPPEDWINDOW | baseStyle);
        ::SetWindowLongPtr(hwnd, GWL_STYLE, static_cast<LONG_PTR>(style));
        ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(WS_EX_APPWINDOW));
        ::SetWindowPos(hwnd,
                       HWND_NOTOPMOST,
                       s_popout_window_rect.left,
                       s_popout_window_rect.top,
                       s_popout_window_rect.right - s_popout_window_rect.left,
                       s_popout_window_rect.bottom - s_popout_window_rect.top,
                       SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    ApplyFrameRateLimit();
    ResizePluginToCurrentClient("TogglePopoutFullscreen", true);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);

#ifdef TIMER_TP
    StartTimer();
    ::PostMessage(get_wnd(), WM_MILK2_RENDER, 0, 0);
#endif
    InvalidateRect(nullptr, FALSE);
}
// clang-format on

void milk2_ui_element::UpdateFoobarPlaybackState()
{
    const bool active = m_playback_control->is_playing() && !m_playback_control->is_paused();
#ifdef TIMER_TP
    const bool lock = s_cs_initialized;
    if (lock)
        EnterCriticalSection(&s_cs);
#endif
    g_plugin.SetFoobarPlaybackActive(active);
#ifdef TIMER_TP
    if (lock)
        LeaveCriticalSection(&s_cs);
#endif
}

void milk2_ui_element::on_playback_starting(play_control::t_track_command /*p_command*/, bool /*p_paused*/)
{
    MILK2_CONSOLE_LOG("+ PlaybackStart")
    UpdateFoobarPlaybackState();
    UpdateTrack();
}

void milk2_ui_element::on_playback_new_track(metadb_handle_ptr p_track)
{
    MILK2_CONSOLE_LOG("+ PlaybackNew")
    UpdateFoobarPlaybackState();
    UpdateTrack(p_track);
    QueueSongTitle();
}

void milk2_ui_element::on_playback_stop(play_control::t_stop_reason /*p_reason*/)
{
    MILK2_CONSOLE_LOG("+ PlaybackStop")
    UpdateFoobarPlaybackState();
    UpdateTrack();
}

void milk2_ui_element::on_playback_pause(bool p_state)
{
    MILK2_CONSOLE_LOG("+ PlaybackPause")
    UpdateFoobarPlaybackState();
    UpdateTrack();
    if (p_state)
        QueueStatusText(L"Paused", 1.6f, 0.35f, SONGTITLE_FONT);
    else if (m_playback_control->is_playing())
        QueueSongTitle();
}

void milk2_ui_element::UpdateTrack()
{
    if (m_script.is_empty())
    {
        pfc::string8 pattern = pfc::utf8FromWide(s_config.settings.m_szTitleFormat);
        static_api_ptr_t<titleformat_compiler>()->compile_safe_ex(m_script, pattern);
    }

    if (m_playback_control->playback_format_title(NULL, m_state, m_script, NULL, playback_control::display_level_all))
    {
        // Succeeded already.
    }
    else if (m_playback_control->is_playing())
    {
        // Starting playback but not done opening the first track yet.
        m_state = "Opening...";
    }
    else
    {
        m_state = "Stopped.";
    }
}

void milk2_ui_element::UpdateTrack(metadb_handle_ptr p_track)
{
    UpdateTrack();

    if (!p_track.is_valid())
        return;

    // Load the album art.
    if (wcsnlen_s(s_config.settings.m_szArtworkFormat, 256) != 0)
    {
        titleformat_object::ptr script;
        pfc::string8 pattern = pfc::utf8FromWide(s_config.settings.m_szArtworkFormat);
        bool success = titleformat_compiler::get()->compile(script, pattern);

        pfc::string result;
        if (success && script.is_valid() && p_track->format_title(nullptr, result, script, nullptr))
        {
            m_art_file.clear();
            std::vector<uint8_t> empty;
            m_raster.swap(empty);
            m_art_file = pfc::wideFromUTF8(result).c_str();
            m_raster.clear();
        }
    }
    else
    {
        LoadAlbumArt(p_track, fb2k::noAbort);
        ShowAlbumArt();
    }
}

void milk2_ui_element::ShowAlbumArt()
{
    // Kill all existing sprites.
    for (int x = 0; x < NUM_TEX; x++)
        g_plugin.KillSprite(x);

    if (s_config.settings.m_bShowAlbum)
    {
        if (!m_art_file.empty()) // file
        {
            // Check if file exists.
            pfc::string8 artFile = pfc::utf8FromWide(m_art_file.c_str());
            if (filesystem::g_exists(artFile, fb2k::noAbort))
            {
                return;
            }

            g_plugin.LaunchSprite(100, -1, m_art_file);
        }
        else if (m_raster.size() > 0) // memory
        {
            g_plugin.LaunchSprite(100, -1, L"", m_raster);
        }
        else // nothing
        {
            return;
        }
    }
}

void milk2_ui_element::UpdatePlaylist()
{
    auto api = playlist_manager::get();
    size_t total = api->activeplaylist_get_item_count();
    for (size_t i = 0; i < total; ++i)
    {
        if (api->activeplaylist_is_item_selected(i))
        {
            g_plugin.m_playlist_pos = i;
        }
    }
    g_plugin.m_playlist_top_idx = -1;
}

void milk2_ui_element::LaunchSongTitle()
{
    g_plugin.LaunchSongTitleAnim();
}

void milk2_ui_element::QueueStatusText(const wchar_t* text, float duration, float fadeTime, int fontIndex)
{
    if (!text || text[0] == L'\0')
        return;

    std::lock_guard<std::mutex> lock(m_pending_animated_text_mutex);
    m_pending_animated_text_kind = pending_animated_text_kind::status;
    m_pending_animated_status_text = text;
    m_pending_animated_status_duration = duration;
    m_pending_animated_status_fade_time = fadeTime;
    m_pending_animated_status_font = fontIndex;
}

bool milk2_ui_element::ConsumeClickPauseConfirmation(DWORD now) noexcept
{
    if (!m_click_pause_confirmation_pending)
        return false;

    if (now - m_click_pause_confirmation_tick > ID_CLICK_CONFIRM_TIMEOUT_MS)
    {
        m_click_pause_confirmation_pending = false;
        return false;
    }

    m_click_pause_confirmation_required = false;
    m_click_pause_confirmation_pending = false;
    return true;
}

void milk2_ui_element::QueueClickPauseConfirmation(DWORD now)
{
    m_click_pause_confirmation_required = false;
    m_click_pause_confirmation_pending = true;
    m_click_pause_confirmation_tick = now;

    if (!m_playback_control->is_playing())
        QueueStatusText(L"Click again to play", 2.4f, 0.35f, SONGTITLE_FONT);
    else if (m_playback_control->is_paused())
        QueueStatusText(L"Click again to resume", 2.4f, 0.35f, SONGTITLE_FONT);
    else
        QueueStatusText(L"Click again to pause", 2.4f, 0.35f, SONGTITLE_FONT);
}

void milk2_ui_element::QueueSongTitle()
{
    std::lock_guard<std::mutex> lock(m_pending_animated_text_mutex);
    m_pending_animated_text_kind = pending_animated_text_kind::song_title;
    m_pending_animated_status_text.clear();
}

void milk2_ui_element::FlushPendingAnimatedText()
{
    pending_animated_text_kind kind = pending_animated_text_kind::none;
    std::wstring statusText;
    float duration = 1.6f;
    float fadeTime = 0.35f;
    int fontIndex = SONGTITLE_FONT;

    {
        std::lock_guard<std::mutex> lock(m_pending_animated_text_mutex);
        kind = m_pending_animated_text_kind;
        if (kind == pending_animated_text_kind::none)
            return;

        statusText = m_pending_animated_status_text;
        duration = m_pending_animated_status_duration;
        fadeTime = m_pending_animated_status_fade_time;
        fontIndex = m_pending_animated_status_font;
        m_pending_animated_text_kind = pending_animated_text_kind::none;
        m_pending_animated_status_text.clear();
    }

    if (kind == pending_animated_text_kind::song_title)
        g_plugin.LaunchSongTitleAnim();
    else if (!statusText.empty())
        g_plugin.LaunchStatusText(statusText.c_str(), duration, fadeTime, static_cast<eFontIndex>(fontIndex));
}

bool milk2_ui_element::LaunchStatusText(const wchar_t* text, float duration, float fadeTime, int fontIndex)
{
    if (!text || text[0] == L'\0')
        return false;

    g_plugin.LaunchStatusText(text, duration, fadeTime, static_cast<eFontIndex>(fontIndex));
    return true;
}

#ifdef TIMER_TP
// Starts the timer.
void milk2_ui_element::StartTimer() noexcept
{
    MILK2_CONSOLE_LOG_LIMIT("StartTimer ", GetWnd())
    if (!m_milk2)
        return;

    SetFrameTimerResolution(true);
    if (m_framePacerStopEvent == nullptr)
        m_framePacerStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_framePacerWakeEvent == nullptr)
        m_framePacerWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_framePacerStopEvent == nullptr || m_framePacerWakeEvent == nullptr)
    {
        return;
    }

    ResetEvent(m_framePacerStopEvent);
    if (m_framePacerThread == nullptr)
        m_framePacerThread = CreateThread(nullptr, 0, FramePacerThreadProc, this, 0, nullptr);

    ScheduleNextFrameTimer();
}

// Stops the timer.
void milk2_ui_element::StopTimer() noexcept
{
    MILK2_CONSOLE_LOG_LIMIT("StopTimer ", GetWnd())
    if (m_framePacerStopEvent != nullptr)
        SetEvent(m_framePacerStopEvent);
    if (m_framePacerWakeEvent != nullptr)
        SetEvent(m_framePacerWakeEvent);

    if (m_framePacerThread != nullptr)
    {
        WaitForSingleObject(m_framePacerThread, INFINITE);
        CloseHandle(m_framePacerThread);
        m_framePacerThread = nullptr;
    }
    if (m_framePacerWakeEvent != nullptr)
    {
        CloseHandle(m_framePacerWakeEvent);
        m_framePacerWakeEvent = nullptr;
    }
    if (m_framePacerStopEvent != nullptr)
    {
        CloseHandle(m_framePacerStopEvent);
        m_framePacerStopEvent = nullptr;
    }
    m_nextFrameQpc = 0;
    m_lastRenderQpc = 0;
    m_renderPending = false;
    m_renderPostTick = 0;
    ResetFrameTimingStats();
    SetFrameTimerResolution(false);
}

void milk2_ui_element::ScheduleNextFrameTimer() noexcept
{
    if (m_framePacerWakeEvent != nullptr)
        SetEvent(m_framePacerWakeEvent);
}

DWORD WINAPI milk2_ui_element::FramePacerThreadProc(LPVOID context) noexcept
{
    auto* element = static_cast<milk2_ui_element*>(context);
    if (element != nullptr)
        element->FramePacerThreadLoop();
    return 0;
}

void milk2_ui_element::FramePacerThreadLoop() noexcept
{
    HANDLE waits[2] = {m_framePacerStopEvent, m_framePacerWakeEvent};
    if (waits[0] == nullptr || waits[1] == nullptr)
        return;

    for (;;)
    {
        if (WaitForSingleObject(waits[0], 0) == WAIT_OBJECT_0)
            return;

        if (!m_milk2 || !::IsWindow(get_wnd()))
        {
            const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, 50);
            if (waitResult == WAIT_OBJECT_0)
                return;
            continue;
        }

        const LONGLONG frameInterval = m_frameIntervalQpc.load(std::memory_order_relaxed);
        if (frameInterval > 0 && m_frameTimerFrequencyQpc > 0)
        {
            for (;;)
            {
                LARGE_INTEGER nowValue{};
                if (!QueryPerformanceCounter(&nowValue))
                    break;

                const LONGLONG now = nowValue.QuadPart;
                LONGLONG nextFrame = m_nextFrameQpc.load(std::memory_order_relaxed);
                if (nextFrame <= 0 || nextFrame < now - frameInterval)
                {
                    nextFrame = now + frameInterval;
                    m_nextFrameQpc.store(nextFrame, std::memory_order_relaxed);
                }

                const LONGLONG ticksUntilFrame = nextFrame - now;
                if (ticksUntilFrame <= 0)
                    break;

                const DWORD waitMs = static_cast<DWORD>((ticksUntilFrame * 1000LL) / m_frameTimerFrequencyQpc);
                if (waitMs > 2)
                {
                    const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, waitMs - 1);
                    if (waitResult == WAIT_OBJECT_0)
                        return;
                    if (waitResult == WAIT_OBJECT_0 + 1)
                        break;
                    continue;
                }

                SwitchToThread();
            }
        }
        else
        {
            m_nextFrameQpc = 0;
            const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, 1);
            if (waitResult == WAIT_OBJECT_0)
                return;
        }

        if (!QueueRenderMessage())
        {
            const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, 1);
            if (waitResult == WAIT_OBJECT_0)
                return;
        }
    }
}

bool milk2_ui_element::TryBeginFrame(LONGLONG& renderQpc) noexcept
{
    renderQpc = 0;
    if (m_frameTimerFrequencyQpc > 0)
    {
        LARGE_INTEGER nowValue{};
        if (QueryPerformanceCounter(&nowValue))
            renderQpc = nowValue.QuadPart;
    }

    const LONGLONG frameInterval = m_frameIntervalQpc.load(std::memory_order_relaxed);
    if (frameInterval <= 0 || m_frameTimerFrequencyQpc <= 0)
    {
        m_nextFrameQpc = 0;
        return true;
    }

    if (renderQpc <= 0)
    {
        m_nextFrameQpc = 0;
        return true;
    }

    LONGLONG nextFrame = m_nextFrameQpc.load(std::memory_order_relaxed);
    if (nextFrame > 0 && renderQpc < nextFrame)
    {
        ScheduleNextFrameTimer();
        return false;
    }

    return true;
}

void milk2_ui_element::FinishFrameTiming(LONGLONG renderQpc) noexcept
{
    if (renderQpc <= 0 && m_frameTimerFrequencyQpc > 0)
    {
        LARGE_INTEGER nowValue{};
        if (QueryPerformanceCounter(&nowValue))
            renderQpc = nowValue.QuadPart;
    }

    if (renderQpc > 0)
    {
        m_lastRenderQpc.store(renderQpc, std::memory_order_relaxed);
        RecordFrameCadence(renderQpc);
    }

    const LONGLONG frameInterval = m_frameIntervalQpc.load(std::memory_order_relaxed);
    if (frameInterval <= 0 || m_frameTimerFrequencyQpc <= 0 || renderQpc <= 0)
    {
        m_nextFrameQpc = 0;
        return;
    }

    LONGLONG nextFrame = renderQpc + frameInterval;
    LARGE_INTEGER nowValue{};
    if (QueryPerformanceCounter(&nowValue))
    {
        while (nextFrame <= nowValue.QuadPart)
            nextFrame += frameInterval;
    }
    m_nextFrameQpc.store(nextFrame, std::memory_order_relaxed);
}

void milk2_ui_element::ResetFrameTimingStats() noexcept
{
    m_frameStatsStartQpc = 0;
    m_frameStatsCount = 0;
}

void milk2_ui_element::RecordFrameCadence(LONGLONG renderQpc) noexcept
{
    if (renderQpc <= 0 || m_frameTimerFrequencyQpc <= 0)
        return;

    const LONGLONG startQpc = m_frameStatsStartQpc.load(std::memory_order_relaxed);
    if (startQpc <= 0 || renderQpc <= startQpc)
    {
        m_frameStatsStartQpc.store(renderQpc, std::memory_order_relaxed);
        m_frameStatsCount = 0;
        return;
    }

    const unsigned int renderedFrames = m_frameStatsCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const LONGLONG elapsedQpc = renderQpc - startQpc;
    if (elapsedQpc < m_frameTimerFrequencyQpc)
        return;

    const double actualFps = static_cast<double>(renderedFrames) * static_cast<double>(m_frameTimerFrequencyQpc) / static_cast<double>(elapsedQpc);
    const uint32_t targetFps = normalize_frame_limit(s_config.settings.m_max_fps_fs);
    wchar_t logLine[256]{};
    swprintf_s(logLine,
               L"render cadence actual=%.1f target=%u frames=%u screen=%ls",
               actualFps,
               targetFps,
               renderedFrames,
               (s_fullscreen || s_popout_fullscreen) ? L"fullscreen" : L"windowed");
    write_dx12_ui_log_line(logLine);

    m_frameStatsStartQpc.store(renderQpc, std::memory_order_relaxed);
    m_frameStatsCount = 0;
}

bool milk2_ui_element::QueueRenderMessage() noexcept
{
    if (!m_milk2 || !::IsWindow(get_wnd()))
        return false;

    const DWORD now = GetTickCount();
    const DWORD postedAt = m_renderPostTick.load(std::memory_order_relaxed);
    if (m_renderPending && postedAt != 0 && now - postedAt > 250)
    {
        m_renderPending = false;
        m_renderPostTick = 0;
    }

    bool expected = false;
    if (!m_renderPending.compare_exchange_strong(expected, true))
        return false;

    m_renderPostTick = now;
    if (!::PostMessage(get_wnd(), WM_MILK2_RENDER, 0, 0))
    {
        m_renderPending = false;
        m_renderPostTick = 0;
        return false;
    }
    return true;
}

void milk2_ui_element::SetFrameTimerResolution(bool enabled) noexcept
{
    if (enabled == m_timerResolutionActive)
        return;

    if (enabled)
    {
        if (call_winmm_timer_period("timeBeginPeriod"))
            m_timerResolutionActive = true;
    }
    else
    {
        if (m_timerResolutionActive)
            call_winmm_timer_period("timeEndPeriod");
        m_timerResolutionActive = false;
    }
}

#endif

#ifdef TIMER_DX
bool milk2_ui_element::on_idle()
{
    Tick();
    //MILK2_CONSOLE_LOG("on_idle ", GetWnd())
    return true;
}
#endif

// Sets and unsets foobar2000's "Always on Top" setting (if main window is
// `TOPMOST`) so that visualization window becomes `TOPMOST` on entering
// fullscreen.
// Note: Using `SetWindowPos` does not work because foobar2000 appears to
//       reset itself to `TOPMOST` forcefully when this setting is enabled.
//       So unsetting and resetting the program's setting is a workaround.
bool milk2_ui_element::SetTopMost() noexcept
{
    LONG_PTR ptr = ::GetWindowLongPtr(core_api::get_main_window(), GWL_EXSTYLE);
    bool topmost = static_cast<bool>(ptr & WS_EX_TOPMOST);
    if (topmost)
    {
        if (s_fullscreen)
        {
            MILK2_CONSOLE_LOG("SetTopMost unsetting main window ", GetWnd())
            standard_commands::main_always_on_top(); //::SetWindowPos(get_wnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW /*| SWP_NOZORDER | SWP_FRAMECHANGED*/);
            s_was_topmost = true;
            return true;
        }
    }
    else
    {
        if (s_was_topmost)
        {
            MILK2_CONSOLE_LOG("SetTopMost resetting main window ", GetWnd())
            s_was_topmost = false;
            standard_commands::main_always_on_top(); //::SetWindowPos(get_wnd(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW /*| SWP_NOZORDER | SWP_FRAMECHANGED*/);
            return true;
        }
    }
    return false;
}

void milk2_ui_element::ShowPreferencesPage()
{
    ui_control::get()->show_preferences(guid_milk2_preferences);
}

void milk2_ui_element::SetSelectionSingle(size_t idx)
{
    SetSelectionSingle(idx, false, true, true);
}

void milk2_ui_element::SetSelectionSingle(size_t idx, bool toggle, bool focus, bool single_only)
{
    auto api = playlist_manager::get();
    const size_t total = api->activeplaylist_get_item_count();
    const size_t idx_focus = api->activeplaylist_get_focus_item();
    //if (idx_focus == pfc::infinite_size)
    //    return;

    bit_array_bittable mask(total);
    mask.set(idx, toggle ? !api->activeplaylist_is_item_selected(idx) : true);

    if (single_only || toggle || !api->activeplaylist_is_item_selected(idx))
    {
        pfc::bit_array_true baT;
        pfc::bit_array_one baO(idx);
        api->activeplaylist_set_selection(single_only ? (pfc::bit_array&)baT : (pfc::bit_array&)baO, mask);
    }
    if (focus && idx_focus != idx)
        api->activeplaylist_set_focus_item(idx);
}

// Resolves PWD, taking care of the case where the path contains non-ASCII
// characters.
void milk2_ui_element::ResolvePwd()
{
    // Get profile directory path through foobar2000 API.
    pfc::string8 full_path = filesystem::g_get_native_path(core_api::get_my_full_path());
    size_t t = full_path.lastIndexOf(L'\\');
    if (t != SIZE_MAX)
        full_path = full_path.subString(0, t + 1);
    size_t path_length = full_path.get_length();
    std::wstring base_path(path_length + 1, L'\0');
    path_length = pfc::stringcvt::convert_utf8_to_wide(const_cast<wchar_t*>(base_path.c_str()), base_path.size(), full_path.get_ptr(), path_length);
    base_path = base_path.erase(base_path.find(L'\0'));

#if 0
    // Get PWD path through Win32 API.
    wchar_t path[MAX_PATH];
    HMODULE hm = NULL;
    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)&TEXT(APPLICATION_FILE_NAME),
                          &hm) == 0)
    {
        DWORD ret = GetLastError();
        MILK2_CONSOLE_LOG("GetModuleHandleEx failed, error = %d\n", ret);
    }
    if (GetModuleFileName(hm, path, MAX_PATH) == 0)
    {
        DWORD ret = GetLastError();
        MILK2_CONSOLE_LOG("GetModuleFileName failed, error = %d\n", ret);
    }
    std::wstring paths(path);
    size_t p = paths.rfind(L'\\');
    if (p != std::wstring::npos)
        paths.erase(p + 1);

    // Use Win32 string it mismatches with the foobar2000 string.
    if (paths != base_path)
    {
        s_pwd = paths;
    }
    else
    {
        s_pwd = base_path;
    }
#else
    s_pwd = base_path;
#endif
}

// Retrieves image raster data and clears the file path.
void milk2_ui_element::ExtractRasterData(const uint8_t* data, size_t size) noexcept
{
    m_art_file.clear();
    std::vector<uint8_t> empty;
    m_raster.swap(empty);
    if ((data != nullptr) && (size != 0))
    {
        m_raster.assign(data, data + size);
    }
}

// Registers with the album art notifier.
void milk2_ui_element::RegisterForArtwork()
{
#if 0
    // Register with the album art notification manager.
    auto AlbumArtNotificationManager = now_playing_album_art_notify_manager_v2::tryGet();

    if (AlbumArtNotificationManager.is_valid())
        AlbumArtNotificationManager->add(this);

    // Get the artwork data from the album art.
    if (wcsnlen_s(s_config.settings.m_szArtworkFormat, 256) == 0)
    {
        auto aanm = now_playing_album_art_notify_manager_v2::get();

        if (aanm != nullptr)
        {
            album_art_data_ptr aad = aanm->current();

            if (aad.is_valid())
            {
                ExtractRasterData(static_cast<const uint8_t*>(aad->data()), aad->size());
            }
        }
    }
#endif
}

// Loads embedded album art.
void milk2_ui_element::LoadAlbumArt(const metadb_handle_ptr& track, abort_callback& abort)
{
    static_api_ptr_t<album_art_manager_v2> aam;

    auto extractor = aam->open(pfc::list_single_ref_t(track), pfc::list_single_ref_t(album_art_ids::cover_front), abort);

    try
    {
        auto aad = extractor->query(album_art_ids::cover_front, abort);

        if (aad.is_valid())
        {
            ExtractRasterData(static_cast<const uint8_t*>(aad->data()), aad->size());
        }
    }
    catch (const exception_album_art_not_found&)
    {
        return;
    }
    catch (const exception_aborted&)
    {
        throw;
    }
    catch (...)
    {
        return;
    }
}

// Service factory publishes the class.
static service_factory_single_t<ui_element_milk2> g_ui_element_milk2_factory;
#pragma endregion

#pragma region Initialize/Quit
class milk2_initquit : public initquit
{
  public:
    void on_init()
    {
        MILK2_CONSOLE_LOG("on_init")
        if (core_api::is_quiet_mode_enabled())
            return;
        //create_first_run();
    }

    void on_quit()
    {
        MILK2_CONSOLE_LOG("on_quit")
        //NSEEL_quit();
        if (core_api::is_quiet_mode_enabled())
            return;
        //delete_first_run();
    }

    void create_first_run()
    {
        pfc::string8 milkdrop2_path = filesystem::g_get_native_path(core_api::get_profile_path());
        milkdrop2_path.end_with_slash();
        milkdrop2_path.add_string("milkdrop2\\", 10);
        if (!filesystem::g_exists(milkdrop2_path, fb2k::noAbort))
        {
            filesystem::g_create_directory(milkdrop2_path, fb2k::noAbort);
        }
        pfc::string8 presets_path = milkdrop2_path;
        presets_path.add_string("presets\\", 8);
        if (!filesystem::g_exists(presets_path, fb2k::noAbort))
        {
            filesystem::g_create_directory(presets_path, fb2k::noAbort);
        }
        if (filesystem::g_is_empty_directory(presets_path, fb2k::noAbort))
        {
            pfc::string8 preset_file = presets_path;
            preset_file.add_string("!.milk", 6);
            wchar_t szPresetFile[MAX_PATH];
            pfc::stringcvt::convert_utf8_to_wide(szPresetFile, MAX_PATH, preset_file, preset_file.length());
            char data_buffer[] =
                "MILKDROP_PRESET_VERSION=201\r\nPSVERSION=3\r\nPSVERSION_WARP=3\r\nPSVERSION_COMP=3\r\n[preset00]\r\nfRating=1.000\r\n";
            WriteMilkDropFile(szPresetFile, data_buffer, 106);
        }
    }

    void delete_first_run()
    {
        pfc::string8 preset_file = filesystem::g_get_native_path(core_api::get_profile_path());
        preset_file.end_with_slash();
        preset_file.add_string("milkdrop2\\presets\\!.milk", 24);
        if (filesystem::g_exists(preset_file, fb2k::noAbort))
        {
            filesystem::g_remove(preset_file, fb2k::noAbort);
        }
    }

    void WriteMilkDropFile(LPCWSTR szFile, LPCSTR szDataBuffer, CONST SIZE_T nDataSize)
    {
        HANDLE hFile;
        DWORD dwBytesToWrite = (DWORD)strnlen_s(szDataBuffer, nDataSize);
        DWORD dwBytesWritten = 0;
        BOOL bErrorFlag = FALSE;
        hFile = CreateFile(szFile, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            DisplayError(TEXT("CreateFile"));
            wprintf_s(TEXT("Terminal failure: Unable to open file \"%s\" for write.\n"), szFile);
            wchar_t buf[512] = {0}, title[64] = {0};
            swprintf_s(buf, L"Unable to open `%ls` for writing.", szFile);
            MessageBox(NULL, buf, WASABI_API_LNGSTRINGW_BUF(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
            return;
        }

        wprintf_s(TEXT("Writing %d bytes to %s.\n"), dwBytesToWrite, szFile);

        bErrorFlag = WriteFile(hFile, szDataBuffer, dwBytesToWrite, &dwBytesWritten, NULL);

        if (FALSE == bErrorFlag)
        {
            DisplayError(TEXT("WriteFile"));
            wprintf_s(TEXT("Terminal failure: Unable to write to file.\n"));
        }
        else
        {
            // This is an error because a synchronous write that results in
            // success (`WriteFile()` returns `TRUE`) should write all data as
            // requested. This would not necessarily be the case for
            // asynchronous writes.
            if (dwBytesWritten != dwBytesToWrite)
            {
                wprintf_s(TEXT("Error: dwBytesWritten != dwBytesToWrite.\n"));
            }
            else
            {
                wprintf_s(TEXT("Wrote %d bytes to %s successfully.\n"), dwBytesWritten, szFile);
            }
        }

        CloseHandle(hFile);
    }
};

FB2K_SERVICE_FACTORY(milk2_initquit);
#pragma endregion
} // namespace

void milk2_sync_runtime_config_from_cfg() noexcept
{
    s_config.reset();
}
