# DX12 Investigation

## Current state

- The installed foobar2000 x64 build is `2.26 preview 2026-03-23`.
- The current official foobar2000 x64 preview installer downloaded to the Desktop is `2.26 preview 2026-04-30`.
- The installed `foo_vis_milk2` x64 DLL is `0.2.1.26`.
- The downloaded official `foo_vis_milk2` package is `0.8.8.0`.
- The public GitHub source cloned here is `v0.2.0.0`, which is older than the installed component and much older than the current official component.

## DX11 usage found in source

The renderer is not a simple shader-only layer. Direct3D 11 types and calls are part of several core modules:

- `vis_milk2/deviceresources.cpp`: creates the D3D11 device, DXGI swap chain, D2D device/context, DWrite factory, render targets, and viewport resources.
- `vis_milk2/d3d11shim.cpp`: wraps D3D11 buffers, textures, shader creation, state objects, draw calls, render target binding, sampler state, and texture loading.
- `vis_milk2/constanttable.cpp`: uses D3D11 shader reflection and dynamic constant buffers.
- `vis_milk2/texmgr.cpp`: stores `ID3D11Resource` texture handles and queries D3D11 resource descriptors.
- `vis_milk2/textmgr.cpp`: renders DirectWrite/Direct2D text onto D3D11-backed surfaces.
- `vis_milk2/plugin.cpp` and headers: store D3D11 texture, shader, resource, and shim pointers throughout the visualization state.

## Practical DX12 paths

1. Update first to the current DX11 component.
   This is the lowest-risk step because `0.8.8.0` contains many DirectX rendering fixes over the currently installed `0.2.1.26`.

2. Use D3D11On12 only as a source-level bridge.
   Windows provides `d3d11on12.dll`, but an existing D3D11 app/component does not automatically run over D3D12 just because the DLL exists. The code must create a D3D12 device and command queue, then call `D3D11On12CreateDevice`, and resource acquire/release calls must be inserted around interop rendering.

3. Native D3D12 port.
   This requires replacing the D3D11 backend with explicit D3D12 device, command queue, command allocator/list, descriptor heap, root signature, pipeline state object, resource barrier, upload heap, and swap-chain handling. The HLSL shader files can mostly remain HLSL, but effect/reflection and binding code need to change.

## First implementation target

Do not port from this cloned `v0.2.0.0` source unless newer source is unavailable. It is too far behind the current component. If current source is obtained, start by splitting the renderer behind an interface:

- `DeviceResources11` remains the current implementation.
- `DeviceResources12` owns D3D12 device/swap-chain/command objects.
- `D3D11Shim` becomes a backend-neutral render shim or gets a parallel `D3D12Shim`.
- Keep text rendering on Direct2D via D3D11On12 initially, then move it to native D2D/D3D12 interop once the frame graph is stable.

## Install notes

The component DLL is loaded while foobar2000 is running. Close foobar2000 before installing the downloaded foobar2000 preview or `foo_vis_milk2-0.8.8.fb2k-component`.
