/// MainWindow.xaml.cpp — MuteMic settings window.
/// Acrylic dark (receta NUCS), tamaño auto-medido, dos vistas (main/gear),
/// shortcut cards dinámicas construidas en código.

#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Microsoft.UI.Input.h>
#include <microsoft.ui.xaml.window.h>   // IWindowNative
#include <dwmapi.h>
#include <cmath>
#include <limits>

#include "Core/MuteMicCore.h"
#include "Core/Autostart.h"
#include "Core/LiquidGlassBackdrop.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using mutemic::MuteMicCore;
using mutemic::MicState;

namespace {

winrt::Windows::UI::Color Argb(uint8_t r, uint8_t g, uint8_t b) {
    return winrt::Windows::UI::ColorHelper::FromArgb(255, r, g, b);
}

// Tamaño lógico: el alto se auto-mide del contenido (fallbacks abajo).
constexpr int kLogicalW = 500;
constexpr int kLogicalHMain = 640;
constexpr int kLogicalHSettings = 726;

bool IsChecked(winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton const& t) {
    auto v = t.IsChecked();
    return v && v.Value();
}

// Brush compartido del tema (mutado en vivo por ApplyTheme).
Media::Brush ThemeBrush(wchar_t const* key) {
    auto res = Application::Current().Resources();
    auto boxed = box_value(hstring(key));
    if (res.HasKey(boxed))
        if (auto b = res.Lookup(boxed).try_as<Media::Brush>())
            return b;
    return Media::SolidColorBrush{ Argb(255, 255, 255) };
}

}  // namespace

namespace winrt::MuteMic::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        SetupWindowChrome();
        SetupBackdrop();

        if (auto root = Content().try_as<FrameworkElement>())
        {
            root.SizeChanged([this](auto&&, auto&&) { UpdateDragRegion(); });
            root.Loaded([this](auto&&, auto&&) { UpdateDragRegion(); });
        }

        // Disposal ordenado al morir la ventana: el DesktopAcrylicController
        // enganchado a una ventana destruida es causa DOCUMENTADA del AV
        // 0xC0000005 al salir en WinUI 3 — hay que cerrarlo explícitamente.
        this->Closed([this](auto&&, auto&&)
        {
            m_renderHook.revoke();
            if (m_acrylic)
            {
                try {
                    if (m_acrylicAttached)
                        if (auto target = this->try_as<winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>())
                            m_acrylic.RemoveSystemBackdropTarget(target);
                } catch (...) {}
                m_acrylicAttached = false;
                try { m_acrylic.Close(); } catch (...) {}
                m_acrylic = nullptr;
            }
            m_backdropConfig = nullptr;
        });

        // Cerrar la ventana NO cierra la app: se esconde al tray.
        // EXCEPTO durante el teardown o el apagado del sistema — cancelar
        // el cierre ahí era lo que hacía que la compu tardara en apagarse.
        AppWindow().Closing([](winrt::Microsoft::UI::Windowing::AppWindow const& sender,
                               winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
        {
            if (MuteMicCore::Exiting()) return;   // dejar morir la ventana
            args.Cancel(true);
            sender.Hide();
        });

        // El backdrop de refracción muestrea lo que hay DETRÁS de la ventana.
        AppWindow().Changed([](winrt::Microsoft::UI::Windowing::AppWindow const& sender,
                               winrt::Microsoft::UI::Windowing::AppWindowChangedEventArgs const& e)
        {
            if (MuteMicCore::Exiting()) return;
            if (e.DidPositionChange() || e.DidSizeChange())
                mutemic::LiquidGlassBackdrop::RequestRedraw();
            if (e.DidVisibilityChange())
                mutemic::LiquidGlassBackdrop::OnWindowVisibility(sender.IsVisible());
        });

        // ── Wiring con el core ──
        auto& core = MuteMicCore::Get();
        core.AddStateListener([this] { UpdateStateUI(); UpdateCardPresence(); });
        core.onShortcutsChanged = [this] { RebuildShortcutCards(); };

        m_loading = true;
        PopulateDevices();
        PopulateSounds();
        LoadFromSettings();
        RebuildShortcutCards();
        m_loading = false;
        UpdateStateUI();
        ApplyTheme();

        // Medidor por frame (v-sync): pico directo del endpoint.
        m_renderHook = winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(
            winrt::auto_revoke, [this](auto&&, auto&&)
        {
            // Guard de teardown: este evento es estático y puede disparar
            // mientras la ventana muere (era una fuente del 0xC0000005).
            if (MuteMicCore::Exiting()) return;
            if (AppWindow() && AppWindow().IsVisible()) UpdateLevelBar();
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    // Chrome / backdrop

    void MainWindow::SetupWindowChrome()
    {
        try
        {
            if (auto p = AppWindow().Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
            {
                p.SetBorderAndTitleBar(true, false);
                p.IsResizable(false);
                p.IsMaximizable(false);
            }
        }
        catch (...) {}

        HWND hwnd{};
        if (auto native = this->try_as<::IWindowNative>())
            native->get_WindowHandle(&hwnd);
        if (hwnd)
        {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
            COLORREF none = 0xFFFFFFFE;
            DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &none, sizeof(none));
            ResizeForView(false);
        }

        // Ícono de ventana/taskbar (el del exe lo pone MuteMic.rc).
        try
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring ico = path;
            ico = ico.substr(0, ico.find_last_of(L'\\')) + L"\\app.ico";
            AppWindow().SetIcon(ico);
        }
        catch (...) {}
    }

    void MainWindow::ResizeForView(bool settingsView)
    {
        HWND hwnd{};
        if (auto native = this->try_as<::IWindowNative>())
            native->get_WindowHandle(&hwnd);
        const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;

        // Altura AUTO: medir el contenido real de la vista.
        int h = settingsView ? kLogicalHSettings : kLogicalHMain;  // fallback
        auto view = settingsView ? SettingsView().as<FrameworkElement>()
                                 : MainView().as<FrameworkElement>();
        if (view && view.Visibility() == Visibility::Visible)
        {
            view.Measure({ static_cast<float>(kLogicalW),
                           std::numeric_limits<float>::infinity() });
            const float content = view.DesiredSize().Height;
            if (content > 100.0f)
                h = static_cast<int>(content) + 40 /*title bar*/;
        }

        AppWindow().Resize({ MulDiv(kLogicalW, static_cast<int>(dpi), 96),
                             MulDiv(h, static_cast<int>(dpi), 96) });
    }

    void MainWindow::SetupBackdrop()
    {
        try
        {
            namespace BD = winrt::Microsoft::UI::Composition::SystemBackdrops;
            m_acrylic = BD::DesktopAcrylicController();
            m_acrylic.TintColor(Argb(10, 10, 10));
            m_acrylic.TintOpacity(0.55f);
            m_acrylic.LuminosityOpacity(0.85f);
            m_acrylic.FallbackColor(Argb(12, 12, 12));

            m_backdropConfig = BD::SystemBackdropConfiguration();
            m_backdropConfig.IsInputActive(true);
            m_backdropConfig.Theme(BD::SystemBackdropTheme::Dark);

            if (auto target = this->try_as<winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>())
            {
                m_acrylic.AddSystemBackdropTarget(target);
                m_acrylic.SetSystemBackdropConfiguration(m_backdropConfig);
                m_acrylicAttached = true;
            }
            this->Activated([this](auto&&, auto&&) {
                if (m_backdropConfig) m_backdropConfig.IsInputActive(true);
            });
        }
        catch (...)
        {
            try { SystemBackdrop(DesktopAcrylicBackdrop()); } catch (...) {}
        }
    }

    void MainWindow::UpdateDragRegion()
    {
        auto root = this->Content();
        if (!root || !root.XamlRoot()) return;
        double scale = root.XamlRoot().RasterizationScale();
        double w = root.XamlRoot().Size().Width;
        const int barH = 40, btnCluster = 44 * 3;  // gear + min + close
        auto src = winrt::Microsoft::UI::Input::InputNonClientPointerSource::GetForWindowId(AppWindow().Id());
        winrt::Windows::Graphics::RectInt32 drag{
            0, 0,
            static_cast<int>((w - btnCluster) * scale),
            static_cast<int>(barH * scale) };
        src.SetRegionRects(winrt::Microsoft::UI::Input::NonClientRegionKind::Caption, { drag });
    }

    // ─────────────────────────────────────────────────────────────────────
    // Shortcut cards (código puro)

    void MainWindow::RebuildShortcutCards()
    {
        auto& core = MuteMicCore::Get();
        auto& list = core.Shortcuts();
        ShortcutList().Children().Clear();
        m_padCards.clear();

        for (size_t i = 0; i < list.size(); ++i)
        {
            auto const& sc = list[i];
            const size_t idx = i;

            Border card;
            card.Background(ThemeBrush(L"NssControlFillBrush"));
            card.BorderBrush(ThemeBrush(L"NssBorderBrush"));
            card.BorderThickness({ 1, 1, 1, 1 });
            card.CornerRadius({ 10, 10, 10, 10 });
            card.Padding({ 12, 10, 12, 10 });

            StackPanel v;
            v.Spacing(8);

            // ── Fila 1: [icono tipo] [nombre editable] [presencia] [✕] ──
            Grid r1;
            r1.ColumnSpacing(8);
            for (int c = 0; c < 4; ++c)
            {
                ColumnDefinition col;
                col.Width((c == 1) ? GridLength{ 1, GridUnitType::Star }
                                   : GridLength{ 0, GridUnitType::Auto });
                r1.ColumnDefinitions().Append(col);
            }

            FontIcon icon;
            static const wchar_t kGlyphKb[] = { 0xE92E, 0 };     // teclado
            static const wchar_t kGlyphMouse[] = { 0xE962, 0 };  // mouse
            static const wchar_t kGlyphPad[] = { 0xE7FC, 0 };    // gamepad
            icon.Glyph(sc.type == 2 ? kGlyphPad : sc.type == 1 ? kGlyphMouse : kGlyphKb);
            icon.FontSize(15);
            icon.VerticalAlignment(VerticalAlignment::Center);
            r1.Children().Append(icon);

            TextBox name;
            name.Text(sc.name);
            name.FontSize(13);
            name.Background(nullptr);
            name.BorderThickness({ 0, 0, 0, 0 });
            name.Padding({ 2, 2, 2, 2 });
            Grid::SetColumn(name, 1);
            name.LostFocus([idx](winrt::Windows::Foundation::IInspectable const& s, auto&&)
            {
                if (auto tb = s.try_as<TextBox>())
                    MuteMicCore::Get().RenameShortcut(idx, std::wstring(tb.Text()));
            });
            r1.Children().Append(name);

            if (sc.type == 2)
            {
                winrt::Microsoft::UI::Xaml::Shapes::Ellipse dot;
                dot.Width(8);
                dot.Height(8);
                dot.VerticalAlignment(VerticalAlignment::Center);
                dot.Fill(SolidColorBrush{ MuteMicCore::Get().AnyPadConnected()
                                              ? Argb(0x39, 0xFF, 0x14)
                                              : Argb(0x6A, 0x6A, 0x6A) });
                ToolTipService::SetToolTip(dot, box_value(
                    hstring(L"Controller presence: green = connected.")));
                Grid::SetColumn(dot, 2);
                r1.Children().Append(dot);
            }

            Button remove;
            remove.Content(box_value(L"✕"));
            remove.FontSize(11);
            remove.Padding({ 8, 3, 8, 3 });
            remove.Background(nullptr);
            remove.BorderThickness({ 0, 0, 0, 0 });
            Grid::SetColumn(remove, 3);
            remove.Click([this, idx](auto&&, auto&&)
            {
                MuteMicCore::Get().RemoveShortcut(idx);
                RebuildShortcutCards();
                ResizeForView(false);
            });
            r1.Children().Append(remove);
            v.Children().Append(r1);

            // ── Fila 2: [binding mono] [Bind] [Test] ──
            Grid r2;
            r2.ColumnSpacing(8);
            for (int c = 0; c < 3; ++c)
            {
                ColumnDefinition col;
                col.Width((c == 0) ? GridLength{ 1, GridUnitType::Star }
                                   : GridLength{ 0, GridUnitType::Auto });
                r2.ColumnDefinitions().Append(col);
            }

            TextBlock binding;
            binding.Text(core.BindingName(sc));
            binding.FontFamily(Media::FontFamily{ L"Consolas" });
            binding.FontSize(13);
            binding.Foreground(sc.Bound() ? ThemeBrush(L"NssCyanBrush")
                                          : ThemeBrush(L"NssTextDimBrush"));
            binding.VerticalAlignment(VerticalAlignment::Center);
            binding.TextTrimming(TextTrimming::CharacterEllipsis);
            r2.Children().Append(binding);

            Button bind;
            bind.Content(box_value(L"Bind"));
            bind.FontSize(12);
            bind.Padding({ 10, 4, 10, 4 });
            Grid::SetColumn(bind, 1);
            bind.Click([this, idx](winrt::Windows::Foundation::IInspectable const& s, auto&&)
            {
                if (auto b = s.try_as<Button>())
                    b.Content(box_value(L"Press any input…"));
                MuteMicCore::Get().StartBind(idx);
            });
            r2.Children().Append(bind);

            Button test;
            test.Content(box_value(L"Test"));
            test.FontSize(12);
            test.Padding({ 10, 4, 10, 4 });
            Grid::SetColumn(test, 2);
            test.Click([idx](auto&&, auto&&)
            {
                MuteMicCore::Get().TestShortcut(idx);
            });
            r2.Children().Append(test);
            v.Children().Append(r2);

            // ── Fila 3: modo por card ──
            ComboBox mode;
            mode.FontSize(12);
            mode.MinWidth(150);
            for (auto const* m : { L"Toggle", L"Push-to-talk", L"Push-to-mute" })
            {
                ComboBoxItem it;
                it.Content(box_value(m));
                mode.Items().Append(it);
            }
            mode.SelectedIndex(static_cast<int>(sc.mode));
            mode.SelectionChanged([this, idx](winrt::Windows::Foundation::IInspectable const& s, auto&&)
            {
                if (m_loading) return;
                if (auto cb = s.try_as<ComboBox>())
                    if (cb.SelectedIndex() >= 0)
                        MuteMicCore::Get().SetShortcutMode(
                            idx, static_cast<UINT>(cb.SelectedIndex()));
            });
            v.Children().Append(mode);

            card.Child(v);
            if (sc.type == 2)
                m_padCards.push_back({ idx, card });
            ShortcutList().Children().Append(card);
        }

        UpdateCardPresence();
        ResizeForView(SettingsView().Visibility() == Visibility::Visible);
    }

    void MainWindow::UpdateCardPresence()
    {
        // Cards de gamepad se atenúan sin pad conectado (siguen editables).
        const bool pad = MuteMicCore::Get().AnyPadConnected();
        for (auto& [idx, card] : m_padCards)
            card.Opacity(pad ? 1.0 : 0.45);
    }

    void MainWindow::OnAddShortcut(winrt::Windows::Foundation::IInspectable const&,
                                   RoutedEventArgs const&)
    {
        MuteMicCore::Get().AddShortcut();
        RebuildShortcutCards();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Datos → UI

    void MainWindow::PopulateDevices()
    {
        auto& core = MuteMicCore::Get();
        DeviceCombo().Items().Clear();

        auto def = ComboBoxItem();
        def.Content(box_value(L"System default"));
        def.Tag(box_value(L""));
        DeviceCombo().Items().Append(def);

        int selected = 0, index = 1;
        const auto current = core.GetSettings().deviceId;
        for (auto const& d : MuteMicCore::EnumerateDevices())
        {
            auto item = ComboBoxItem();
            item.Content(box_value(d.name));
            item.Tag(box_value(d.id));
            DeviceCombo().Items().Append(item);
            if (!current.empty() && d.id == current) selected = index;
            ++index;
        }
        DeviceCombo().SelectedIndex(selected);
    }

    void MainWindow::PopulateSounds()
    {
        auto& s = MuteMicCore::Get().GetSettings();
        auto fill = [](ComboBox const& combo, bool muteCue, std::wstring const& current)
        {
            combo.Items().Clear();
            int selected = 0, index = 0;
            for (auto const& f : MuteMicCore::EnumerateSounds(muteCue))
            {
                auto item = ComboBoxItem();
                std::wstring label = f.size() > 4 ? f.substr(0, f.size() - 4) : f;
                item.Content(box_value(label));
                item.Tag(box_value(f));
                combo.Items().Append(item);
                if (f == current) selected = index;
                ++index;
            }
            if (combo.Items().Size() > 0) combo.SelectedIndex(selected);
        };
        fill(MuteSoundCombo(), true, s.soundMuteFile);
        fill(UnmuteSoundCombo(), false, s.soundUnmuteFile);
    }

    void MainWindow::LoadFromSettings()
    {
        auto& core = MuteMicCore::Get();
        auto& s = core.GetSettings();

        const bool autostart = mutemic::IsAutostartEnabled();
        AutostartToggle().IsChecked(autostart);
        StartTrayToggle().IsChecked(s.startInTray);
        StartTrayToggle().IsEnabled(autostart);
        StartTrayLabel().Opacity(autostart ? 1.0 : 0.4);
        SoundToggle().IsChecked(s.playSound);
        TipDeviceToggle().IsChecked(s.tooltipShowDevice);
        TipShortcutToggle().IsChecked(s.tooltipShowShortcut);
        PauseToggle().IsChecked(core.ServicePaused());
        VolumeSlider().Value(static_cast<double>(s.soundVolume));

        UpdateCueButtons();
        CuePosCombo().Items().Clear();
        for (auto const* name : { L"Top", L"Bottom", L"Left", L"Right" })
        {
            auto item = ComboBoxItem();
            item.Content(box_value(name));
            CuePosCombo().Items().Append(item);
        }
        CuePosCombo().SelectedIndex(static_cast<int>(s.cueEdge));
    }

    void MainWindow::UpdateCueButtons()
    {
        auto& s = MuteMicCore::Get().GetSettings();
        const UINT cue = s.visualCue;
        CueOffBtn().IsChecked(cue == 0);
        CueEdgesBtn().IsChecked(cue == 1);
        CueCornersBtn().IsChecked(cue == 2);
        CueNotchBtn().IsChecked(cue == 3);
        CuePosCombo().IsEnabled(cue == 3);

        auto glyph = CueNotchGlyph();
        const bool vertical = (s.cueEdge >= 2);
        glyph.Width(vertical ? 3.5 : 9.0);
        glyph.Height(vertical ? 9.0 : 3.5);
        switch (s.cueEdge) {
            case 0: glyph.HorizontalAlignment(HorizontalAlignment::Center);
                    glyph.VerticalAlignment(VerticalAlignment::Top); break;
            case 1: glyph.HorizontalAlignment(HorizontalAlignment::Center);
                    glyph.VerticalAlignment(VerticalAlignment::Bottom); break;
            case 2: glyph.HorizontalAlignment(HorizontalAlignment::Left);
                    glyph.VerticalAlignment(VerticalAlignment::Center); break;
            case 3: glyph.HorizontalAlignment(HorizontalAlignment::Right);
                    glyph.VerticalAlignment(VerticalAlignment::Center); break;
        }
    }

    void MainWindow::UpdateStateUI()
    {
        auto& core = MuteMicCore::Get();
        const MicState state = core.State();
        const bool paused = core.ServicePaused();

        SolidColorBrush green{ Argb(0x39, 0xFF, 0x14) };
        SolidColorBrush red{ Argb(0xFF, 0x31, 0x31) };
        SolidColorBrush gray{ Argb(0x6A, 0x6A, 0x6A) };

        if (paused)
        {
            StateDot().Fill(gray);
            StateText().Text(L"Service paused");
            TitleStateBadge().Text(L"● PAUSED");
            TitleStateBadge().Foreground(gray);
        }
        else switch (state)
        {
            case MicState::Unmuted:
                StateDot().Fill(green);
                StateText().Text(L"Mic live");
                TitleStateBadge().Text(L"● LIVE");
                TitleStateBadge().Foreground(green);
                break;
            case MicState::Muted:
                StateDot().Fill(red);
                StateText().Text(L"Muted");
                TitleStateBadge().Text(L"● MUTE");
                TitleStateBadge().Foreground(red);
                break;
            case MicState::NoDevice:
                StateDot().Fill(gray);
                StateText().Text(L"No microphone");
                TitleStateBadge().Text(L"● N/A");
                TitleStateBadge().Foreground(gray);
                break;
        }

        const auto name = core.CurrentDeviceName();
        DeviceText().Text(name.empty() ? L"No capture device detected" : name);

        if (state == MicState::Muted) LevelFill().Background(red);
        else LevelFill().Background(green);

        // Botón consciente del estado: la acción que hará, con su color.
        // El contenido es un TextBlock con Foreground PROPIO (el template
        // del Button pisa el del control en cada visual state).
        {
            const bool willUnmute = (state == MicState::Muted);
            TextBlock label;
            label.Text(willUnmute ? L"Unmute" : L"Mute");
            label.FontSize(12);
            label.Foreground(willUnmute ? green : red);
            MuteBtn().Content(label);
            MuteBtn().BorderBrush(SolidColorBrush{
                willUnmute ? winrt::Windows::UI::ColorHelper::FromArgb(0x59, 0x39, 0xFF, 0x14)
                           : winrt::Windows::UI::ColorHelper::FromArgb(0x59, 0xFF, 0x31, 0x31) });
        }
        MuteBtn().IsEnabled(state != MicState::NoDevice && !paused);
    }

    void MainWindow::UpdateLevelBar()
    {
        auto& core = MuteMicCore::Get();

        double raw = core.ServicePaused() ? 0.0 : core.PollPeak();
        if (raw < 0.0) raw = 0.0;
        if (raw > 1.0) raw = 1.0;
        const double target = std::sqrt(raw);

        if (target > m_displayLevel) m_displayLevel = target;
        else m_displayLevel += (target - m_displayLevel) * 0.22;
        if (m_displayLevel < 0.002) m_displayLevel = 0.0;

        LevelScale().ScaleX(m_displayLevel);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Tema

    void MainWindow::ApplyTheme()
    {
        auto& s = MuteMicCore::Get().GetSettings();
        const bool light = (s.theme == 1);
        const bool glass = s.glass;

        struct Surface {
            winrt::Windows::UI::Color card, cardHover, borderSubtle, border,
                borderHover, text, textSec, textDim, fill, track;
        };
        auto C = [](uint32_t argb) {
            return winrt::Windows::UI::ColorHelper::FromArgb(
                (argb >> 24) & 0xFF, (argb >> 16) & 0xFF,
                (argb >> 8) & 0xFF, argb & 0xFF);
        };
        Surface surf;
        if (!light && !glass) {
            surf = { C(0xFF0E0E0E), C(0xFF161616), C(0x0AFFFFFF), C(0x14FFFFFF),
                     C(0x1FFFFFFF), C(0xFFE8E8F0), C(0x66FFFFFF), C(0x40FFFFFF),
                     C(0x10FFFFFF), C(0x14FFFFFF) };
        } else if (!light && glass) {
            surf = { C(0x4A15171C), C(0x5F1D2026), C(0x24FFFFFF), C(0x3CFFFFFF),
                     C(0x5CFFFFFF), C(0xFFEDEFF4), C(0x8AFFFFFF), C(0x55FFFFFF),
                     C(0x2EFFFFFF), C(0x30FFFFFF) };
        } else if (light && !glass) {
            surf = { C(0xF0FFFFFF), C(0xFFFFFFFF), C(0x10000000), C(0x1E000000),
                     C(0x30000000), C(0xFF17181C), C(0x8A17181C), C(0x5517181C),
                     C(0x12000000), C(0x1E000000) };
        } else {
            surf = { C(0x38FFFFFF), C(0x50FFFFFF), C(0x40FFFFFF), C(0x78FFFFFF),
                     C(0xA8FFFFFF), C(0xFF17181C), C(0x8A17181C), C(0x5517181C),
                     C(0x48FFFFFF), C(0x3AFFFFFF) };
        }

        auto setBrushes = [&](winrt::Microsoft::UI::Xaml::ResourceDictionary const& dict)
        {
            auto set = [&](wchar_t const* key, winrt::Windows::UI::Color c)
            {
                auto boxed = box_value(hstring(key));
                if (dict.HasKey(boxed))
                    if (auto b = dict.Lookup(boxed).try_as<Media::SolidColorBrush>())
                        b.Color(c);
            };
            set(L"NssCardBrush", surf.card);
            set(L"NssCardHoverBrush", surf.cardHover);
            set(L"NssBorderSubtleBrush", surf.borderSubtle);
            set(L"NssBorderBrush", surf.border);
            set(L"NssBorderHoverBrush", surf.borderHover);
            set(L"NssTextBrush", surf.text);
            set(L"NssTextSecondaryBrush", surf.textSec);
            set(L"NssTextDimBrush", surf.textDim);
            set(L"NssControlFillBrush", surf.fill);
            set(L"NssTrackBrush", surf.track);
            set(L"NssToggleOnBrush", glass ? C(0xFF34C759) : C(0xFF39FF14));
        };
        for (auto const& merged : Application::Current().Resources().MergedDictionaries())
        {
            auto themes = merged.ThemeDictionaries();
            for (auto const& key : { hstring(L"Default"), hstring(L"Light") })
            {
                auto boxed = box_value(key);
                if (themes.HasKey(boxed))
                    if (auto dict = themes.Lookup(boxed).try_as<winrt::Microsoft::UI::Xaml::ResourceDictionary>())
                        setBrushes(dict);
            }
        }

        RootGrid().RequestedTheme(light ? ElementTheme::Light : ElementTheme::Dark);

        // ── Backdrop ──
        const bool frost = s.frost;
        auto target = this->try_as<winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>();

        if (glass && target)
        {
            // LIQUID GLASS REAL (shader): frost = full effect; clear = solo
            // refracción. Siempre share-friendly (visible en capturas).
            if (m_acrylic && m_acrylicAttached)
            {
                m_acrylic.RemoveSystemBackdropTarget(target);
                m_acrylicAttached = false;
            }
            HWND hwnd{};
            if (auto native = this->try_as<::IWindowNative>())
                native->get_WindowHandle(&hwnd);

            if (!hwnd || !mutemic::LiquidGlassBackdrop::Start(hwnd, RootGrid(), frost, true))
            {
                if (m_acrylic && !m_acrylicAttached)
                {
                    m_acrylic.AddSystemBackdropTarget(target);
                    m_acrylicAttached = true;
                    m_acrylic.TintOpacity(frost ? 0.15f : 0.03f);
                    m_acrylic.LuminosityOpacity(frost ? 0.40f : 0.10f);
                }
            }
        }
        else if (m_acrylic && target)
        {
            mutemic::LiquidGlassBackdrop::Stop();
            if (!m_acrylicAttached)
            {
                target.SystemBackdrop(nullptr);
                m_acrylic.AddSystemBackdropTarget(target);
                if (m_backdropConfig) m_acrylic.SetSystemBackdropConfiguration(m_backdropConfig);
                m_acrylicAttached = true;
            }
            if (!light) {
                m_acrylic.TintColor(Argb(10, 10, 10));
                m_acrylic.TintOpacity(0.55f);
                m_acrylic.LuminosityOpacity(0.85f);
                m_acrylic.FallbackColor(Argb(12, 12, 12));
            } else {
                m_acrylic.TintColor(Argb(243, 243, 243));
                m_acrylic.TintOpacity(0.50f);
                m_acrylic.LuminosityOpacity(0.90f);
                m_acrylic.FallbackColor(Argb(238, 240, 244));
            }
        }

        ThemeModeToggle().IsChecked(light);
        GlassToggle().IsChecked(glass);
        FrostToggle().IsChecked(frost);
        FrostToggle().IsEnabled(glass);
        FrostLabel().Opacity(glass ? 1.0 : 0.4);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Handlers

    void MainWindow::OnAppearanceToggled(winrt::Windows::Foundation::IInspectable const&,
                                         RoutedEventArgs const&)
    {
        if (m_loading) return;
        auto& core = MuteMicCore::Get();
        core.GetSettings().theme = IsChecked(ThemeModeToggle()) ? 1u : 0u;
        core.SaveSettings();
        ApplyTheme();
    }

    void MainWindow::OnGlassToggled(winrt::Windows::Foundation::IInspectable const&,
                                    RoutedEventArgs const&)
    {
        if (m_loading) return;
        auto& core = MuteMicCore::Get();
        core.GetSettings().glass = IsChecked(GlassToggle());
        core.SaveSettings();
        ApplyTheme();
    }

    void MainWindow::OnFrostToggled(winrt::Windows::Foundation::IInspectable const&,
                                    RoutedEventArgs const&)
    {
        if (m_loading) return;
        auto& core = MuteMicCore::Get();
        core.GetSettings().frost = IsChecked(FrostToggle());
        core.SaveSettings();
        ApplyTheme();
    }

    void MainWindow::OnCueStyleSelected(winrt::Windows::Foundation::IInspectable const& sender,
                                        RoutedEventArgs const&)
    {
        if (m_loading) return;
        auto& core = MuteMicCore::Get();
        auto btn = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton>();
        if (!btn) return;

        UINT style = 0;
        if (btn == CueEdgesBtn()) style = 1;
        else if (btn == CueCornersBtn()) style = 2;
        else if (btn == CueNotchBtn()) style = 3;

        core.GetSettings().visualCue = style;
        core.SaveSettings();
        UpdateCueButtons();
        if (style != 0) core.PreviewCue();
    }

    void MainWindow::OnCuePosChanged(winrt::Windows::Foundation::IInspectable const&,
                                     SelectionChangedEventArgs const&)
    {
        if (m_loading) return;
        auto& core = MuteMicCore::Get();
        const int idx = CuePosCombo().SelectedIndex();
        if (idx < 0) return;
        core.GetSettings().cueEdge = static_cast<UINT>(idx);
        core.SaveSettings();
        UpdateCueButtons();
        if (core.GetSettings().visualCue == 3) core.PreviewCue();
    }

    void MainWindow::OnToggleSettings(winrt::Windows::Foundation::IInspectable const&,
                                      RoutedEventArgs const&)
    {
        const bool showSettings = SettingsView().Visibility() == Visibility::Collapsed;
        if (showSettings)
        {
            m_loading = true;
            PopulateSounds();
            m_loading = false;
        }
        SettingsView().Visibility(showSettings ? Visibility::Visible : Visibility::Collapsed);
        MainView().Visibility(showSettings ? Visibility::Collapsed : Visibility::Visible);
        ResizeForView(showSettings);
        // Engranaje (E713) <-> flecha de volver (E72B).
        static const wchar_t kBack[] = { 0xE72B, 0 };
        static const wchar_t kGear[] = { 0xE713, 0 };
        SettingsGlyph().Glyph(showSettings ? kBack : kGear);
    }

    void MainWindow::OnMinimize(winrt::Windows::Foundation::IInspectable const&,
                                RoutedEventArgs const&)
    {
        if (auto p = AppWindow().Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
            p.Minimize();
    }

    void MainWindow::OnCloseToTray(winrt::Windows::Foundation::IInspectable const&,
                                   RoutedEventArgs const&)
    {
        AppWindow().Hide();
    }

    void MainWindow::OnToggleMute(winrt::Windows::Foundation::IInspectable const&,
                                  RoutedEventArgs const&)
    {
        MuteMicCore::Get().ToggleMute();
    }

    void MainWindow::OnDeviceChanged(winrt::Windows::Foundation::IInspectable const&,
                                     SelectionChangedEventArgs const&)
    {
        if (m_loading) return;
        if (auto item = DeviceCombo().SelectedItem().try_as<ComboBoxItem>())
        {
            auto id = unbox_value<hstring>(item.Tag());
            MuteMicCore::Get().SetDeviceId(std::wstring(id));
        }
    }

    void MainWindow::OnSoundChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                    SelectionChangedEventArgs const&)
    {
        if (m_loading) return;
        auto combo = sender.try_as<ComboBox>();
        if (!combo) return;
        auto item = combo.SelectedItem().try_as<ComboBoxItem>();
        if (!item) return;

        const bool muteCue = (combo == MuteSoundCombo());
        auto file = unbox_value<hstring>(item.Tag());
        auto& core = MuteMicCore::Get();
        core.SetSoundFile(muteCue, std::wstring(file));
        core.PreviewSound(muteCue);
    }

    void MainWindow::OnVolumeChanged(winrt::Windows::Foundation::IInspectable const&,
                                     winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (m_loading) return;
        MuteMicCore::Get().SetSoundVolume(static_cast<UINT>(e.NewValue()));
    }

    void MainWindow::OnOptionToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                     RoutedEventArgs const&)
    {
        if (m_loading) return;
        auto& core = MuteMicCore::Get();
        auto& s = core.GetSettings();
        auto toggle = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton>();
        if (!toggle) return;

        if (toggle == AutostartToggle())
        {
            const bool on = IsChecked(toggle);
            mutemic::SetAutostart(on);
            StartTrayToggle().IsEnabled(on);
            StartTrayLabel().Opacity(on ? 1.0 : 0.4);
        }
        else if (toggle == StartTrayToggle())
        {
            s.startInTray = IsChecked(toggle);
            core.SaveSettings();
        }
        else if (toggle == SoundToggle())
        {
            s.playSound = IsChecked(toggle);
            core.SaveSettings();
        }
        else if (toggle == TipDeviceToggle())
        {
            s.tooltipShowDevice = IsChecked(toggle);
            core.SaveSettings();
        }
        else if (toggle == TipShortcutToggle())
        {
            s.tooltipShowShortcut = IsChecked(toggle);
            core.SaveSettings();
        }
        else if (toggle == PauseToggle())
        {
            core.SetServicePaused(IsChecked(toggle));
        }
    }

    void MainWindow::OnExit(winrt::Windows::Foundation::IInspectable const&,
                            RoutedEventArgs const&)
    {
        m_renderHook.revoke();
        MuteMicCore::Get().RequestExit();
    }
}
