/*
 * pch.h - Precompiled header for the experimental foobar2000 DX12 component.
 */

#pragma once

#include <winsdkver.h>
#define _WIN32_WINNT 0x0A00
#include <sdkddkver.h>

#define NOMINMAX
#define NOMCX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4245)
#endif
#include <helpers/foobar2000-lite+atl.h>
#include <sdk/componentversion.h>
#include <sdk/coreversion.h>
#include <sdk/initquit.h>
#include <sdk/ui.h>
#include <sdk/ui_element.h>
#include <helpers/atl-misc.h>
#include <helpers/BumpableElem.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <vis_milk2_dx12/render_backend.h>

#ifndef HINST_THISCOMPONENT
extern "C" IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE)&__ImageBase)
#endif
