#pragma once

#include "App.g.h"
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>

namespace winrt::MuteMic::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

        // Muestra (o re-muestra) la ventana de configuración.
        void ShowSettings();

    private:
        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
    };
}
