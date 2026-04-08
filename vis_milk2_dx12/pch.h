/*
 * pch.h - Precompiled header for the experimental DX12 renderer scaffold.
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

#include <memory>
#include <optional>
#include <string>

