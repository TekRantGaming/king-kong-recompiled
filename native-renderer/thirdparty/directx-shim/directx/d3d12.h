// Stand-in for Microsoft's DirectX-Headers <directx/d3d12.h>, which NVRHI
// includes. This machine builds against the Windows SDK, whose own d3d12.h
// (10.0.26100) has every interface NVRHI uses, so the shim just forwards to it.
// If a newer NVRHI needs something the Windows SDK lacks, add it here or vendor
// microsoft/DirectX-Headers (MIT) instead.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
