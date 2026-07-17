#pragma once

#include "MainWindow.g.h"
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <vector>

namespace winrt::MuteMic::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        // Handlers referenciados desde MainWindow.xaml
        void OnToggleSettings(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMinimize(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCloseToTray(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnToggleMute(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAddShortcut(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDeviceChanged(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnSoundChanged(winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnVolumeChanged(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void OnOptionToggled(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAppearanceToggled(winrt::Windows::Foundation::IInspectable const&,
                                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnGlassToggled(winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnFrostToggled(winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCueStyleSelected(winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCuePosChanged(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnExit(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void SetupBackdrop();
        void SetupWindowChrome();
        void UpdateDragRegion();
        void PopulateDevices();
        void PopulateSounds();
        void LoadFromSettings();
        void UpdateStateUI();
        void UpdateLevelBar();
        void ApplyTheme();
        void UpdateCueButtons();
        void ResizeForView(bool settingsView);

        // Shortcut cards (construidas en código — sin XAML generado nuevo).
        void RebuildShortcutCards();
        void UpdateCardPresence();

        winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_acrylic{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_backdropConfig{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering_revoker m_renderHook{};

        // Presencia (cards de gamepad se atenúan sin pad conectado).
        std::vector<std::pair<size_t, winrt::Microsoft::UI::Xaml::Controls::Border>> m_padCards;

        bool m_loading = false;
        bool m_acrylicAttached = false;
        double m_displayLevel = 0.0;
    };
}

namespace winrt::MuteMic::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
