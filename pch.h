#pragma once
/// pch.h — Precompiled header for MuteMic

// Force desktop API surface BEFORE <windows.h> (WinAppSDK targets set
// WINAPI_FAMILY to PC_APP which hides desktop-only APIs we need).
#ifdef WINAPI_FAMILY
#undef WINAPI_FAMILY
#endif
#define WINAPI_FAMILY 100  /* WINAPI_FAMILY_DESKTOP_APP */

// Windows SDK
#include <windows.h>
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

// C++/WinRT base
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>

// WinUI 3
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Navigation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Dispatching.h>

// STL
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <chrono>

// ══ Implementation types ══
// Required by XamlTypeInfo.g.cpp (winrt::make<implementation::T>()).
// MUST come after all WinRT/WinUI headers above.
#include "App.xaml.h"
#include "MainWindow.xaml.h"
