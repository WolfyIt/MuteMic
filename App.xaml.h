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

        // Flyout estilo Win11 (como el de red) para el right-click del tray.
        void ShowTrayFlyout(int anchorX, int anchorY);

    private:
        void BuildTrayFlyout();
        void RefreshTrayFlyout();

        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };

        winrt::Microsoft::UI::Xaml::Window m_flyout{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_flyAcrylic{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_flyBackdropConfig{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_flyRoot{ nullptr };
        winrt::Microsoft::UI::Xaml::Shapes::Ellipse m_flyLed{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_flyState{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_flyMuteLabel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::FontIcon m_flyMuteIcon{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ComboBox m_flyCombo{ nullptr };
        bool m_flyLoading = false;
    };
}
