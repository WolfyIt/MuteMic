/// App.xaml.cpp — Application + entry point manual (DISABLE_XAML_GENERATED_MAIN).
/// Incluye el tray flyout estilo Win11 construido en código (sin XAML nuevo,
/// así no hay más *.xaml.g.h que mantener a mano).

#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

#include <WindowsAppSDK-VersionInfo.h>
#include <MddBootstrap.h>
#include <microsoft.ui.xaml.window.h>   // IWindowNative
#include <dwmapi.h>
#include <commctrl.h>                   // subclass del flyout
#include <winrt/Windows.UI.Text.h>      // FontWeights
#include <cstdio>                       // CrashLogger

#include "Core/MuteMicCore.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
namespace MUXC = winrt::Microsoft::UI::Xaml::Controls;
using mutemic::MuteMicCore;
using mutemic::MicState;

namespace {

static UINT g_startupVerb = 0xFFFF;

// Glifos Segoe Fluent (escapes para no depender del encoding del archivo).
constexpr wchar_t kGlyphMic[] = L"";
constexpr wchar_t kGlyphMicOff[] = L"";
constexpr wchar_t kGlyphSettings[] = L"";
constexpr wchar_t kGlyphPower[] = L"";

winrt::Windows::UI::Color Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b);
}

Media::SolidColorBrush Brush(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return Media::SolidColorBrush{ Argb(a, r, g, b) };
}

HWND HwndOf(winrt::Microsoft::UI::Xaml::Window const& w) {
    HWND hwnd{};
    if (auto native = w.try_as<::IWindowNative>())
        native->get_WindowHandle(&hwnd);
    return hwnd;
}

// Paleta del flyout: root, texto, ícono, separadores y borde exterior.
struct FlyoutPalette {
    winrt::Windows::UI::Color root, text, icon, divider, border;
};

// (theme: 0 night / 1 light) × (glass on/off) → 4 paletas.
// Night = negro de verdad, sin borde blanco (espejo del light, que sí gusta).
// Los modos glass dejan root translúcido para que el acrylic de atrás
// haga de cristal (frost) — la refracción real del shader es solo de la
// ventana principal; en un menú transitorio el acrylic translúcido cumple.
FlyoutPalette PaletteFor(UINT theme, bool glass) {
    const bool light = (theme == 1);
    if (light && glass)
        return { Argb(0x8E, 0xF2, 0xF4, 0xF8), Argb(0xFF, 0x17, 0x18, 0x1C),
                 Argb(0xFF, 0x17, 0x18, 0x1C), Argb(0x1E, 0x00, 0x00, 0x00),
                 Argb(0x30, 0xFF, 0xFF, 0xFF) };
    if (light)
        return { Argb(0xF5, 0xF6, 0xF7, 0xFA), Argb(0xFF, 0x17, 0x18, 0x1C),
                 Argb(0xFF, 0x17, 0x18, 0x1C), Argb(0x22, 0x00, 0x00, 0x00),
                 Argb(0x22, 0x00, 0x00, 0x00) };
    if (glass)
        return { Argb(0x74, 0x0E, 0x10, 0x14), Argb(0xFF, 0xED, 0xEF, 0xF4),
                 Argb(0xFF, 0xED, 0xEF, 0xF4), Argb(0x16, 0xFF, 0xFF, 0xFF),
                 Argb(0x1C, 0xFF, 0xFF, 0xFF) };
    // Night sólido: negro profundo, hairline apenas perceptible.
    return { Argb(0xF7, 0x0D, 0x0E, 0x11), Argb(0xFF, 0xE8, 0xE8, 0xF0),
             Argb(0xFF, 0xE8, 0xE8, 0xF0), Argb(0x12, 0xFF, 0xFF, 0xFF),
             Argb(0x0C, 0xFF, 0xFF, 0xFF) };
}

// Fila estilo Win11: [glyph] [texto], fondo transparente, hover del tema.
// outIcon/outText opcionales para poder actualizarlos después.
MUXC::Button MakeFlyoutRow(winrt::hstring const& glyph, winrt::hstring const& text,
                           FlyoutPalette const& pal,
                           MUXC::FontIcon* outIcon = nullptr,
                           MUXC::TextBlock* outText = nullptr) {
    MUXC::StackPanel content;
    content.Orientation(MUXC::Orientation::Horizontal);
    content.Spacing(14);

    MUXC::FontIcon icon;
    icon.Glyph(glyph);
    icon.FontSize(16);
    icon.Foreground(Media::SolidColorBrush{ pal.icon });
    MUXC::TextBlock label;
    label.Text(text);
    label.FontSize(13);
    label.VerticalAlignment(VerticalAlignment::Center);
    label.Foreground(Media::SolidColorBrush{ pal.text });
    content.Children().Append(icon);
    content.Children().Append(label);

    MUXC::Button row;
    row.Content(content);
    row.HorizontalAlignment(HorizontalAlignment::Stretch);
    row.HorizontalContentAlignment(HorizontalAlignment::Left);
    row.Background(Media::SolidColorBrush{ Argb(0, 0, 0, 0) });
    row.BorderThickness({ 0, 0, 0, 0 });
    row.CornerRadius({ 4, 4, 4, 4 });
    row.Padding({ 12, 9, 12, 9 });

    if (outIcon) *outIcon = icon;
    if (outText) *outText = label;
    return row;
}

// ── Cierre del flyout al click fuera ──
// WM_ACTIVATE no basta: el taskbar NO roba activación por diseño, así que
// clickearlo nunca dispara WA_INACTIVE y el menú quedaba plantado. Los
// flyouts nativos usan lo mismo que esto: un hook de mouse mientras el
// menú está visible — cualquier botón fuera del rect lo cierra.
HHOOK g_flyMouseHook = nullptr;
HWND g_flyHwnd = nullptr;

void UninstallFlyoutMouseHook() {
    if (g_flyMouseHook) {
        UnhookWindowsHookEx(g_flyMouseHook);
        g_flyMouseHook = nullptr;
    }
}

LRESULT CALLBACK FlyoutMouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION &&
        (w == WM_LBUTTONDOWN || w == WM_RBUTTONDOWN || w == WM_MBUTTONDOWN ||
         w == WM_NCLBUTTONDOWN || w == WM_NCRBUTTONDOWN)) {
        if (g_flyHwnd && IsWindowVisible(g_flyHwnd)) {
            const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(l);
            RECT r{};
            GetWindowRect(g_flyHwnd, &r);
            if (!PtInRect(&r, ms->pt)) {
                ShowWindow(g_flyHwnd, SW_HIDE);
                UninstallFlyoutMouseHook();
            }
        } else {
            UninstallFlyoutMouseHook();
        }
    }
    return CallNextHookEx(g_flyMouseHook, code, w, l);
}

void InstallFlyoutMouseHook() {
    if (!g_flyMouseHook)
        g_flyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, FlyoutMouseProc,
                                           GetModuleHandleW(nullptr), 0);
}

LRESULT CALLBACK FlyoutSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l,
                                    UINT_PTR id, DWORD_PTR) noexcept {
    if (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) {
        ShowWindow(h, SW_HIDE);
        UninstallFlyoutMouseHook();
    }
    if (m == WM_NCDESTROY) {
        UninstallFlyoutMouseHook();
        RemoveWindowSubclass(h, FlyoutSubclassProc, id);
    }
    return DefSubclassProc(h, m, w, l);
}

MUXC::Border MakeDivider(FlyoutPalette const& pal) {
    MUXC::Border d;
    d.Height(1);
    d.Background(Media::SolidColorBrush{ pal.divider });
    d.Margin({ 0, 2, 0, 2 });
    return d;
}

}  // namespace

namespace winrt::MuteMic::implementation
{
    App::App()
    {
        InitializeComponent();

        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            OutputDebugStringW(L"[MuteMic] Unhandled exception: ");
            OutputDebugStringW(e.Message().c_str());
            OutputDebugStringW(L"\n");
        });
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& args)
    {
        auto& core = MuteMicCore::Get();
        if (!core.Init())
        {
            Exit();
            return;
        }

        core.onOpenSettings = [this] { ShowSettings(); };
        core.onTrayFlyout = [this](int x, int y) { ShowTrayFlyout(x, y); };
        core.AddStateListener([this] { RefreshTrayFlyout(); });

        // Start in tray: arranca silencioso (solo el ícono); la ventana se
        // crea recién cuando el usuario la abre.
        if (!core.GetSettings().startInTray)
            ShowSettings();

        // Verbo CLI en el primer arranque (p. ej. "MuteMic.exe --mute" sin
        // instancia previa): aplicarlo ya inicializado el core.
        if (g_startupVerb != 0xFFFF)
            core.ExecuteVerb(g_startupVerb);
    }

    void App::ShowSettings()
    {
        if (!m_window)
        {
            m_window = make<MainWindow>();
        }
        m_window.AppWindow().Show();
        m_window.Activate();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Tray flyout (estilo del flyout de red de Win11)

    void App::BuildTrayFlyout()
    {
        // Se reconstruye si cambió el tema/glass (paleta baked en los controles).
        static UINT builtKey = 0xFFFF;
        auto& s = MuteMicCore::Get().GetSettings();
        const UINT theme = s.theme;
        const UINT key = theme | (s.glass ? 0x10u : 0u);
        if (m_flyout && builtKey == key) return;
        builtKey = key;

        const FlyoutPalette pal = PaletteFor(theme, s.glass);
        const bool firstBuild = !m_flyout;

        MUXC::StackPanel panel;
        panel.Padding({ 6, 8, 6, 8 });
        panel.Spacing(2);

        // ── Estado: LED + texto (se actualiza en vivo) ──
        MUXC::StackPanel stateRow;
        stateRow.Orientation(MUXC::Orientation::Horizontal);
        stateRow.Spacing(10);
        stateRow.Padding({ 12, 6, 12, 6 });
        m_flyLed = winrt::Microsoft::UI::Xaml::Shapes::Ellipse();
        m_flyLed.Width(10);
        m_flyLed.Height(10);
        m_flyLed.VerticalAlignment(VerticalAlignment::Center);
        m_flyState = MUXC::TextBlock();
        m_flyState.FontSize(13);
        m_flyState.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        m_flyState.Foreground(Media::SolidColorBrush{ pal.text });
        m_flyState.VerticalAlignment(VerticalAlignment::Center);
        stateRow.Children().Append(m_flyLed);
        stateRow.Children().Append(m_flyState);
        panel.Children().Append(stateRow);

        panel.Children().Append(MakeDivider(pal));

        // ── Mute/Unmute (label dinámico según estado actual) ──
        auto muteRow = MakeFlyoutRow(kGlyphMic, L"Mute microphone", pal,
                                     &m_flyMuteIcon, &m_flyMuteLabel);
        muteRow.Click([](auto&&, auto&&) { MuteMicCore::Get().ToggleMute(); });
        panel.Children().Append(muteRow);

        // ── Quick switch de dispositivo ──
        m_flyCombo = MUXC::ComboBox();
        m_flyCombo.HorizontalAlignment(HorizontalAlignment::Stretch);
        m_flyCombo.FontSize(12);
        m_flyCombo.Margin({ 12, 4, 12, 4 });
        m_flyCombo.SelectionChanged([this](auto&&, auto&&)
        {
            if (m_flyLoading) return;
            if (auto item = m_flyCombo.SelectedItem().try_as<MUXC::ComboBoxItem>())
            {
                auto id = winrt::unbox_value<winrt::hstring>(item.Tag());
                MuteMicCore::Get().SetDeviceId(std::wstring(id));
            }
        });
        panel.Children().Append(m_flyCombo);

        panel.Children().Append(MakeDivider(pal));

        // ── Abrir / salir ──
        auto openRow = MakeFlyoutRow(kGlyphSettings, L"MuteMic settings", pal);
        openRow.Click([this](auto&&, auto&&)
        {
            m_flyout.AppWindow().Hide();
            ShowSettings();
        });
        panel.Children().Append(openRow);

        auto quitRow = MakeFlyoutRow(kGlyphPower, L"Quit MuteMic", pal);
        quitRow.Click([](auto&&, auto&&) { MuteMicCore::Get().RequestExit(); });
        panel.Children().Append(quitRow);

        // Root con fondo del tema, borde hairline y radio — look nativo.
        m_flyRoot = MUXC::Border();
        m_flyRoot.Background(Media::SolidColorBrush{ pal.root });
        m_flyRoot.BorderBrush(Media::SolidColorBrush{ pal.border });
        m_flyRoot.BorderThickness({ 1, 1, 1, 1 });
        m_flyRoot.CornerRadius({ 8, 8, 8, 8 });
        m_flyRoot.Child(panel);

        if (firstBuild)
        {
            m_flyout = winrt::Microsoft::UI::Xaml::Window();
            m_flyout.Content(m_flyRoot);

            // Acrylic manual (el DesktopAcrylicBackdrop de XAML fallaba
            // silencioso en esta ventana): mismo patrón que MainWindow.
            try
            {
                namespace BD = winrt::Microsoft::UI::Composition::SystemBackdrops;
                m_flyAcrylic = BD::DesktopAcrylicController();
                m_flyBackdropConfig = BD::SystemBackdropConfiguration();
                m_flyBackdropConfig.IsInputActive(true);
                if (auto t = m_flyout.try_as<winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>())
                {
                    m_flyAcrylic.AddSystemBackdropTarget(t);
                    m_flyAcrylic.SetSystemBackdropConfiguration(m_flyBackdropConfig);
                }
            }
            catch (...) {}

            if (auto p = m_flyout.AppWindow().Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
            {
                p.SetBorderAndTitleBar(false, false);
                p.IsResizable(false);
                p.IsMaximizable(false);
                p.IsMinimizable(false);
                p.IsAlwaysOnTop(true);
            }
            m_flyout.AppWindow().IsShownInSwitchers(false);

            if (HWND hwnd = HwndOf(m_flyout))
            {
                BOOL dark = TRUE;
                DwmSetWindowAttribute(hwnd, 20 /*IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
                DWORD corner = 2 /*DWMWCP_ROUND*/;
                DwmSetWindowAttribute(hwnd, 33 /*WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
                COLORREF none = 0xFFFFFFFE /*DWMWA_COLOR_NONE*/;
                DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &none, sizeof(none));

                // El "borde blanco" que sobrevivía era el frame win32 clásico
                // (WS_BORDER/THICKFRAME + WS_EX_WINDOWEDGE): fuera todo.
                LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
                style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME);
                SetWindowLongPtrW(hwnd, GWL_STYLE, style);
                LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME);
                ex |= WS_EX_TOOLWINDOW;   // sin taskbar/alt-tab
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOACTIVATE | SWP_FRAMECHANGED);

                // Cierre al click fuera vía WM_ACTIVATE (fiable).
                SetWindowSubclass(hwnd, FlyoutSubclassProc, 1, 0);
            }

            // Disposal del acrylic al morir el flyout: un controller vivo
            // sobre una ventana destruida es causa documentada del AV
            // 0xC0000005 al salir en WinUI 3.
            m_flyout.Closed([this](auto&&, auto&&)
            {
                if (m_flyAcrylic)
                {
                    try {
                        if (auto t = m_flyout.try_as<winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>())
                            m_flyAcrylic.RemoveSystemBackdropTarget(t);
                    } catch (...) {}
                    try { m_flyAcrylic.Close(); } catch (...) {}
                    m_flyAcrylic = nullptr;
                }
                m_flyBackdropConfig = nullptr;
            });
        }
        else
        {
            m_flyout.Content(m_flyRoot);
        }

        // Glass: los rows heredan tema claro para el hover correcto.
        m_flyRoot.RequestedTheme(theme == 1 ? ElementTheme::Light : ElementTheme::Dark);

        // Tinte del acrylic acorde al modo (glass = más transparente).
        if (m_flyAcrylic)
        {
            const bool light = (theme == 1);
            const bool glassMode = s.glass;
            if (light) {
                m_flyAcrylic.TintColor(Argb(0xFF, 246, 247, 250));
                m_flyAcrylic.TintOpacity(glassMode ? 0.18f : 0.70f);
                m_flyAcrylic.LuminosityOpacity(glassMode ? 0.50f : 0.85f);
                m_flyAcrylic.FallbackColor(Argb(0xFF, 240, 242, 246));
            } else {
                m_flyAcrylic.TintColor(Argb(0xFF, 12, 13, 16));
                m_flyAcrylic.TintOpacity(glassMode ? 0.25f : 0.75f);
                m_flyAcrylic.LuminosityOpacity(glassMode ? 0.45f : 0.90f);
                m_flyAcrylic.FallbackColor(Argb(0xFF, 14, 15, 18));
            }
        }
    }

    void App::RefreshTrayFlyout()
    {
        if (!m_flyout || !m_flyLed) return;

        auto& core = MuteMicCore::Get();
        const MicState state = core.State();

        if (core.ServicePaused() || state == MicState::NoDevice)
        {
            m_flyLed.Fill(Brush(0xFF, 0x8A, 0x8A, 0x8A));
            m_flyState.Text(core.ServicePaused() ? L"Service paused" : L"No microphone");
        }
        else if (state == MicState::Muted)
        {
            m_flyLed.Fill(Brush(0xFF, 0xFF, 0x31, 0x31));
            m_flyState.Text(L"Muted");
        }
        else
        {
            m_flyLed.Fill(Brush(0xFF, 0x39, 0xFF, 0x14));
            m_flyState.Text(L"Mic live");
        }

        // La acción dice lo que va a HACER (estado actual → acción inversa).
        if (m_flyMuteLabel)
        {
            const bool muted = (state == MicState::Muted);
            m_flyMuteLabel.Text(muted ? L"Unmute microphone" : L"Mute microphone");
            if (m_flyMuteIcon)
                m_flyMuteIcon.Glyph(muted ? kGlyphMic : kGlyphMicOff);
        }
    }

    void App::ShowTrayFlyout(int anchorX, int anchorY)
    {
        BuildTrayFlyout();

        // Poblar dispositivos (rápido, y así siempre está fresco).
        m_flyLoading = true;
        m_flyCombo.Items().Clear();
        auto def = MUXC::ComboBoxItem();
        def.Content(winrt::box_value(L"System default"));
        def.Tag(winrt::box_value(L""));
        m_flyCombo.Items().Append(def);
        int selected = 0, index = 1;
        const auto current = MuteMicCore::Get().GetSettings().deviceId;
        for (auto const& d : MuteMicCore::EnumerateDevices())
        {
            auto item = MUXC::ComboBoxItem();
            item.Content(winrt::box_value(d.name));
            item.Tag(winrt::box_value(d.id));
            m_flyCombo.Items().Append(item);
            if (!current.empty() && d.id == current) selected = index;
            ++index;
        }
        m_flyCombo.SelectedIndex(selected);
        m_flyLoading = false;

        RefreshTrayFlyout();

        // Posicionar como los flyouts nativos: pegado ARRIBA del taskbar
        // (borde inferior = tope del work area, con gap), nunca tapándolo.
        // El work area ya excluye el taskbar.
        HWND hwnd = HwndOf(m_flyout);
        const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
        const int w = MulDiv(280, static_cast<int>(dpi), 96);
        const int h = MulDiv(212, static_cast<int>(dpi), 96);
        const int gap = MulDiv(12, static_cast<int>(dpi), 96);

        RECT work = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int x = anchorX - w / 2;
        if (x + w > work.right - gap) x = work.right - w - gap;
        if (x < work.left + gap) x = work.left + gap;
        int y = work.bottom - h - gap;   // siempre sobre el taskbar
        if (y < work.top) y = work.top + gap;

        m_flyout.AppWindow().MoveAndResize({ x, y, w, h });
        m_flyout.AppWindow().Show();
        m_flyout.Activate();
        g_flyHwnd = hwnd;
        InstallFlyoutMouseHook();
        (void)anchorY;
    }
}

// Verbo de línea de comandos (CLI): puente universal para Stream Deck,
// AutoHotkey, scripts, etc. — sin puertos ni servidores.
//   MuteMic.exe --toggle | --mute | --unmute | --show
// Log de crash: módulo + offset + código a mutemic-crash.log junto al exe.
// Corre antes del diálogo "System Error" de Windows — nos dice el culpable.
static LONG WINAPI CrashLogger(EXCEPTION_POINTERS* ep)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *(slash + 1) = 0;
    wcscat_s(path, L"mutemic-crash.log");

    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    wchar_t modName[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) && mod)
        GetModuleFileNameW(mod, modName, MAX_PATH);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") == 0 && f)
    {
        SYSTEMTIME st; GetLocalTime(&st);
        fwprintf(f, L"[%02u:%02u:%02u] code=0x%08X addr=%p module=%s offset=+0x%llX tid=%lu\n",
                 st.wHour, st.wMinute, st.wSecond,
                 ep->ExceptionRecord->ExceptionCode, addr, modName,
                 mod ? static_cast<unsigned long long>(
                           reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)) : 0ULL,
                 GetCurrentThreadId());
        // Un par de frames del stack (best effort, sin dbghelp).
        void* frames[16] = {};
        USHORT n = RtlCaptureStackBackTrace(0, 16, frames, nullptr);
        for (USHORT i = 0; i < n; ++i)
        {
            HMODULE fm = nullptr; wchar_t fn[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(frames[i]), &fm) && fm)
                GetModuleFileNameW(fm, fn, MAX_PATH);
            fwprintf(f, L"    frame %02u: %s+0x%llX\n", i, fn,
                     fm ? static_cast<unsigned long long>(
                              reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(fm)) : 0ULL);
        }
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // deja que Windows siga (diálogo/WER)
}

/// Entry point manual.
int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int)
{
    SetUnhandledExceptionFilter(CrashLogger);

    // PRIMERA línea: DPI awareness antes de todo.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // CLI: si hay verbo y YA corre una instancia, mandárselo y salir al tiro
    // (sin levantar XAML — milisegundos).
    {
        const wchar_t* cmd = GetCommandLineW();
        if (wcsstr(cmd, L"--toggle")) g_startupVerb = 1;
        else if (wcsstr(cmd, L"--unmute")) g_startupVerb = 3;
        else if (wcsstr(cmd, L"--mute")) g_startupVerb = 2;
        else if (wcsstr(cmd, L"--show")) g_startupVerb = 0;

        HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE,
                                     L"Local\\MuteMic_SingleInstance");
        if (existing) {
            CloseHandle(existing);
            const UINT verb = (g_startupVerb == 0xFFFF) ? 0 : g_startupVerb;
            PostMessageW(HWND_BROADCAST,
                         RegisterWindowMessageW(L"MuteMic.Cmd"), verb, 0);
            return 0;
        }
    }

    winrt::init_apartment();

    UINT32 majorMinor = WINDOWSAPPSDK_RELEASE_MAJORMINOR;
    PCWSTR versionTag = WINDOWSAPPSDK_RELEASE_VERSION_TAG_W;
    PACKAGE_VERSION minVersion{};

    HRESULT hr = MddBootstrapInitialize(majorMinor, versionTag, minVersion);
    if (FAILED(hr))
    {
        WCHAR buf[256];
        swprintf_s(buf, L"Windows App Runtime 1.6 not found (0x%08X).\n"
                        L"Install it with: winget install Microsoft.WindowsAppRuntime.1.6",
                   static_cast<UINT32>(hr));
        MessageBoxW(nullptr, buf, L"MuteMic", MB_ICONERROR);
        return 1;
    }

    winrt::Microsoft::UI::Xaml::Application::Start(
        [](auto&&)
        {
            winrt::make<winrt::MuteMic::implementation::App>();
        });

    MddBootstrapShutdown();
    winrt::uninit_apartment();
    return 0;
}

// Exports WinRT del módulo (normalmente en module.g.cpp).
int32_t __stdcall WINRT_CanUnloadNow() noexcept
{
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

int32_t __stdcall WINRT_GetActivationFactory(void* classId, void** factory) noexcept
{
    try
    {
        auto factoryObj = winrt::get_activation_factory<winrt::Windows::Foundation::IActivationFactory>(
            *reinterpret_cast<winrt::hstring const*>(&classId));
        *factory = winrt::get_abi(factoryObj);
        return 0;
    }
    catch (winrt::hresult_error const& e) { return e.to_abi(); }
    catch (...) { return static_cast<int32_t>(E_FAIL); }
}
