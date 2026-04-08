# foo_vis_milk2_dx12

Experimental DirectX 12 fork of MilkDrop 2 for foobar2000.

This repository is not a stable replacement for the normal DX11 component. It is a separate renderer bring-up and backend prototyping effort.

## Current status

- DirectX 12 device probing works
- hosted foobar2000 UI element loads
- embedded DX12 presentation path has been brought up
- this is still experimental and not ready for general users

## Important warning

Do not treat this repository as a production-ready component.

- the stable user-facing component is still the normal DX11 `foo_vis_milk2.dll`
- the DX12 component here is for renderer experimentation
- behavior may vary across systems, drivers, layouts, and foobar2000 configurations

## Scope

This repo is intended to track:

- DX12 renderer scaffolding
- hosted swap chain experiments
- backend abstraction work
- early foobar2000 integration for the DX12 path

It is not yet intended to provide:

- full rendering parity with the DX11 component
- a drop-in replacement for the existing MilkDrop component
- a generally supported end-user release

## Stable component

If you want the working DX11 component with the latest dialog/title fixes, use the stable fork/release work in:

- `Ci303/foo_vis_milk2`

## Build notes

This codebase currently shares the same external dependency expectations as the original project:

- foobar2000 SDK
- pfc
- libPPUI
- projectM EEL
- DirectXTK
- WTL
- Visual Studio 2022

## Testing note

This work has been tested on the maintainer's local foobar2000 x64 setup. Other users may have different presets, drivers, components, or environment issues and may need to troubleshoot those differences themselves.
