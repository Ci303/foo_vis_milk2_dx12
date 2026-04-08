/*
 * main.cpp - Component metadata for the experimental DX12 component.
 */

#include "pch.h"
#include "version.h"

DECLARE_COMPONENT_VERSION("MilkDrop 2 DX12 Experimental",
                          APPLICATION_VERSION,
                          "Experimental DirectX 12 scaffold for MilkDrop 2 on foobar2000.\n\n"
                          APPLICATION_FILE_NAME " " APPLICATION_VERSION "\n"
                          "This component currently probes DX12 support and exposes a separate UI element.\n"
                          "It does not yet implement MilkDrop rendering parity.\n\n"
                          "Source: " APPLICATION_SOURCE_URL)

VALIDATE_COMPONENT_FILENAME(APPLICATION_FILE_NAME ".dll")

