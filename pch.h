// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H

#pragma once

#include <windows.h>
#include <DispatcherQueue.h>
#include <algorithm>
#include <map>
#include <atomic>
#include <utility>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <roapi.h>
#include <winstring.h>
#include <dwmapi.h>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <stdio.h>
#include <stdarg.h>

// C++/WinRT Headers
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>

// Composition
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>

// WGC API
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

// Linker Pragmas
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "runtimeobject.lib")

#endif //PCH_H
