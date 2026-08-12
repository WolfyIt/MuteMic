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
#include "Core/LiquidGlassBackdrop.h"
#include "Core/Telemetry.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
namespace MUXC = winrt::Microsoft::UI::Xaml::Controls;
using mutemic::MuteMicCore;
using mutemic::MicState;

namespace {

static UINT g_startupVerb = 0xFFFF;

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
        // Este proceso SOLO existe para mostrar los ajustes: el daemon es
        // quien tiene tray, hooks y bindings. Al cerrar la ventana, el
        // proceso muere y devuelve al sistema los ~162 MB de WinUI.
        auto& core = MuteMicCore::Get();
        if (!core.Init(MuteMicCore::Mode::SettingsUi))
        {
            Exit();
            return;
        }

        core.onOpenSettings = [this] { ShowSettings(); };

        ShowSettings();
    }

    // Reentrada tras cerrar la ventana = AV 0xC0000005.
    //
    // `m_window` no se limpiaba nunca: al cerrar, la referencia WinRT seguía
    // siendo no-nula pero la ventana ya estaba destruida, así que el `if
    // (!m_window)` no reconstruía nada y `AppWindow()` devolvía un objeto COM
    // muerto. Cerrar la ventana además dispara `RequestExit()`, que es
    // DIFERIDO: queda una ventana de tiempo en la que el core todavía puede
    // invocar `onOpenSettings` sobre un proceso que se está muriendo.
    //
    // Dos defensas, ambas baratas: no tocar nada durante el teardown (mismo
    // guard que ya usan el handler de Changed y el hook de Rendering en
    // MainWindow), y soltar la referencia al cerrar para que una apertura
    // legítima posterior reconstruya en vez de resucitar un cadáver.
    void App::ShowSettings()
    {
        if (MuteMicCore::Exiting()) return;

        if (!m_window)
        {
            m_window = make<MainWindow>();
            m_window.Closed([this](auto&&, auto&&) { m_window = nullptr; });
        }
        m_window.AppWindow().Show();
        m_window.Activate();
    }

}

// Verbo de línea de comandos (CLI): puente universal para Stream Deck,
// AutoHotkey, scripts, etc. — sin puertos ni servidores.
//   MuteMic.exe --toggle | --mute | --unmute | --show
// Log de crash: módulo + offset + código a mutemic-crash.log junto al exe.
// Corre antes del diálogo "System Error" de Windows — nos dice el culpable.
static LONG WINAPI CrashLogger(EXCEPTION_POINTERS* ep)
{
    // Que el crash quede también en la telemetría: así el contexto previo
    // (qué se estaba haciendo) y el fallo viven en el MISMO archivo.
    {
        char buf[128];
        sprintf_s(buf, "code=0x%08X addr=%p",
                  ep->ExceptionRecord->ExceptionCode,
                  ep->ExceptionRecord->ExceptionAddress);
        mutemic::Telemetry::Event("CRASH", "unhandled", buf);
    }

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

    const wchar_t* cmd = GetCommandLineW();

    // ── Dos procesos, un solo binario ──
    // Sin bandera: DAEMON, Win32 puro, ~5 MB residentes de por vida.
    // Con --settings: proceso EFÍMERO que levanta XAML solo para la ventana
    // de ajustes y muere al cerrarla. Medido: WinUI deja ~162 MB
    // comprometidos que no se recuperan dentro del mismo proceso; matarlo
    // es la única forma real de devolverlos.
    const bool settingsMode = wcsstr(cmd, L"--settings") != nullptr;

    // CLI: si hay verbo y YA corre el daemon, mandárselo y salir al tiro
    // (sin levantar XAML — milisegundos).
    {
        if (wcsstr(cmd, L"--toggle")) g_startupVerb = 1;
        else if (wcsstr(cmd, L"--unmute")) g_startupVerb = 3;
        else if (wcsstr(cmd, L"--mute")) g_startupVerb = 2;
        else if (wcsstr(cmd, L"--show")) g_startupVerb = 0;

        // Telemetría: siempre en Debug, en Release solo con --diag.
        mutemic::Telemetry::Init(wcsstr(cmd, L"--diag") != nullptr);

        // El proceso de ajustes NO compite con el daemon: lo lanza él.
        if (!settingsMode) {
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
    }

    // ── Camino del DAEMON: sin WinAppSDK, sin XAML, sin bootstrap ──
    if (!settingsMode) {
        winrt::init_apartment();
        auto& core = mutemic::MuteMicCore::Get();
        if (!core.Init(mutemic::MuteMicCore::Mode::Daemon)) {
            mutemic::Telemetry::Shutdown("daemon-init-failed");
            winrt::uninit_apartment();
            return 0;
        }
        if (g_startupVerb != 0xFFFF && g_startupVerb != 0)
            core.ExecuteVerb(g_startupVerb);
        // Sin "start in tray", el daemon abre la ventana de ajustes en su
        // proceso aparte nada más arrancar.
        if (!core.GetSettings().startInTray || g_startupVerb == 0)
            core.LaunchSettingsProcess();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        core.Term();
        mutemic::Telemetry::Shutdown("daemon-exit");
        winrt::uninit_apartment();
        return static_cast<int>(msg.wParam);
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

    mutemic::Telemetry::Shutdown("normal");
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
