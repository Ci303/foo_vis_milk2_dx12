# foo_vis_milk2_dx12

Experimental DirectX 12 development fork of `foo_vis_milk2`, the MilkDrop 2 visualization component for foobar2000.

This repository is not a finished replacement for the normal DirectX 11 component. It is a live renderer bring-up branch used to move the existing MilkDrop/foobar2000 component toward a native DirectX 12 path while preserving the working DX11 code as a fallback.

## Current Status

The current DX12 path is enabled only when this environment variable is set:

```powershell
$env:FOO_VIS_MILK2_DX12_DEV = "1"
```

Without that variable, the component follows the existing DirectX 11 path.

Optional DX12 development flags used during portable testing:

```powershell
$env:FOO_VIS_MILK2_DX12_TEXTURE_CYCLE = "1"
$env:FOO_VIS_MILK2_DX12_TEXTURE_CYCLE_MS = "3000"
$env:FOO_VIS_MILK2_DX12_POSTPROCESS = "1"
```

The current DX12 development path can:

- create a native Direct3D 12 device, command queue, swap chain, descriptor heaps, command lists, fences, root signatures, and pipeline states;
- present inside the foobar2000 hosted visualization element;
- render beat-responsive waveform lines using native D3D12;
- import `.milk` preset state without invoking the old D3D11 shader compile path;
- drive DX12 waveform color, alpha, scale, position, decay, zoom, rotation, mode, mystery/param, dots, thick, additive, and brighten/maximize controls from imported preset values;
- load WIC, DDS, and TGA textures from `milkdrop2\textures` through native D3D12 SRV paths;
- draw blended texture-backed visuals behind the waveform;
- cycle texture-backed visuals during portable testing;
- parse preset `sampler_*` texture requests and try to load matching texture-pack files;
- support random texture requests such as `sampler_rand00_prefix`;
- render native D3D12 feedback/trails, preset echo controls, darken-center, borders, motion vectors, custom shapes, and custom waves;
- run preset per-frame equations on the DX12 path;
- apply an experimental texture/feedback warp mesh from the MilkDrop per-pixel grid;
- apply an experimental postprocess pass for gamma, brighten, darken, solarize, invert, and first `fShader` approximation when `FOO_VIS_MILK2_DX12_POSTPROCESS=1`;
- enter fullscreen and return to the hosted windowed foobar2000 panel without leaving the panel render loop stopped.

The current DX12 path is still incomplete. It does not yet provide full MilkDrop rendering parity and should be treated as active renderer development.

## What Is Not Finished

The following areas still need renderer work:

- full MilkDrop warp/composite shader parity on D3D12;
- full shader reflection and resource binding replacement for the old D3D11 constant-table path;
- complete render-to-texture feedback, blur chain, and video echo parity;
- complete MilkDrop shape, border, motion vector, custom wave, and sprite rendering parity;
- DirectWrite/Direct2D text rendering in the DX12 path;
- compressed and less common DDS/TGA texture variants;
- multi-texture preset sampler binding beyond the current first usable texture selection;
- feature parity with the current working live DX11 component.

This repo should be treated as development source, not a stable release.

## Repository Shape

The previous separate DX12 scaffold projects have been removed. DX12 work now lives in the normal component source tree:

```text
foo_vis_milk2\          foobar2000 component entry/UI code
vis_milk2\              MilkDrop renderer/library code
vis_milk2\d3d12resources.*  native D3D12 bring-up path
docs\                   investigation notes
external\               external dependencies, populated locally
```

The current DX12 code is deliberately integrated behind runtime gating instead of replacing the normal DX11 path outright.

## Build Requirements

Expected build environment:

- Windows 10/11
- Visual Studio 2022
- MSVC v143 toolset
- Windows SDK
- foobar2000 SDK under `external\foobar2000`
- foobar2000 SDK helper dependencies under `external\pfc` and `external\libppui`
- projectM EEL under `external\projectm-eval`
- DirectXTK and WTL packages restored through NuGet

Import `.vsconfig` in Visual Studio if required.

## Build

From a Visual Studio Developer PowerShell:

```powershell
msbuild .\foo_vis_milk2.sln -m:4 -t:Build -p:Configuration=Release -p:Platform=x64
```

The resulting component DLL is written to:

```text
Bin\x64\Release\foo_vis_milk2.dll
```

The current development work has been build-checked with:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" .\foo_vis_milk2.sln -m:4 -t:Build -p:Configuration=Release -p:Platform=x64
```

## Portable DX12 Test Setup

The recommended test workflow is a separate portable foobar2000 install, not a live install.

Example local layout used during development:

```text
%USERPROFILE%\Desktop\foobar2000-portable-dx12-dev\
```

Install the built DLL to the portable component folder:

```text
profile\user-components-x64\foo_vis_milk2\foo_vis_milk2.dll
```

Launch the portable build with DX12 mode enabled:

```powershell
$env:FOO_VIS_MILK2_DX12_DEV = "1"
Start-Process "$env:USERPROFILE\Desktop\foobar2000-portable-dx12-dev\foobar2000.exe" -WorkingDirectory "$env:USERPROFILE\Desktop\foobar2000-portable-dx12-dev"
```

During development, this has been launched through:

```text
%USERPROFILE%\Desktop\foobar2000-portable-dx12-dev\Launch DX12 dev portable.ps1
```

## Presets And Textures

Presets should be placed under:

```text
<foobar2000 profile>\milkdrop2\presets
```

Textures should be placed under:

```text
<foobar2000 profile>\milkdrop2\textures
```

The current DX12 texture loader supports WIC-backed formats:

- `jpg`
- `jpeg`
- `png`
- `bmp`
- `gif`
- `jfif`
- `dds` where a Windows WIC DDS decoder can decode the file

It also includes a native loader for common uncompressed and RLE true-color TGA files:

- `tga`

Some DDS/TGA variants may still fail if they use compression or pixel formats outside those currently decoded paths.

## Safety Notes

Do not test this by replacing a working live foobar2000 install unless you have a backup and intentionally want to test the experimental component there.

The current postprocess and texture/feedback warp work is experimental. If DX12 rendering behaves unexpectedly, disable `FOO_VIS_MILK2_DX12_POSTPROCESS`, use the safe portable launcher, or restart the affected process/session before continuing.

Fullscreen transitions are supported in the DX12 development path. The component explicitly restores the original hosted panel after foobar2000 destroys the temporary fullscreen visualization window, then rebuilds the DX12 runtime for that windowed panel.

The current recommended workflow is:

1. keep the working live DX11 install untouched;
2. test DX12 only in a portable foobar2000 folder;
3. back up the portable `foo_vis_milk2.dll` before each replacement;
4. check `profile\crash reports` after test launches.

## Development Notes

The old D3D11 renderer is tightly coupled through:

- `vis_milk2\deviceresources.*`
- `vis_milk2\d3d11shim.*`
- `vis_milk2\constanttable.*`
- `vis_milk2\texmgr.*`
- `vis_milk2\textmgr.*`
- large sections of `vis_milk2\plugin.cpp` and `vis_milk2\milkdropfs.cpp`

The current DX12 bring-up avoids pretending those paths are already ported. It adds a native D3D12 rendering path for startup, presentation, waveform rendering, preset state import, and initial texture handling, then bypasses old D3D11-only allocations and shader compilation while DX12 mode is active.

See `docs\dx12-investigation.md` for earlier investigation notes.

## Credits

This work is based on the MilkDrop 2 visualization source and the existing `foo_vis_milk2` foobar2000 component work. The DX12 path in this repository is experimental development work on top of that foundation.

## License

See `LICENSE.txt` and `LICENSES.md`.
