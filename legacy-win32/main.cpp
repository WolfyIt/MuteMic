// MuteMic — toggle de micrófono por hotkey global (Ctrl+Alt+M) con tray icon.
// C++ puro / Win32, sin dependencias externas.

#include <windows.h>
#include <memory>

#include "audio.h"
#include "autostart.h"
#include "tray.h"

namespace {

constexpr int kHotkeyId = 1;
constexpr wchar_t kWindowClass[] = L"MuteMicHiddenWindow";
constexpr wchar_t kMutexName[] = L"Local\\MuteMic_SingleInstance";

MicController* g_mic = nullptr;
TrayIcon* g_tray = nullptr;
UINT g_taskbarCreatedMsg = 0;

void PlayFeedback(MicState state) {
    // Tonos distintos para mute/unmute; silencio si no hay dispositivo.
    if (state == MicState::Muted) {
        MessageBeep(MB_ICONHAND);
    } else if (state == MicState::Unmuted) {
        MessageBeep(MB_ICONASTERISK);
    }
}

void ToggleMute() {
    MicState state = g_mic->Toggle();
    g_tray->Update(state);
    PlayFeedback(state);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_HOTKEY:
            if (wParam == kHotkeyId) ToggleMute();
            return 0;

        case WM_TRAYICON:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                    ToggleMute();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: {
                    UINT cmd = g_tray->ShowMenu();
                    switch (cmd) {
                        case IDM_TOGGLE_MUTE:
                            ToggleMute();
                            break;
                        case IDM_AUTOSTART:
                            SetAutostart(!IsAutostartEnabled());
                            break;
                        case IDM_EXIT:
                            PostQuitMessage(0);
                            break;
                    }
                    break;
                }
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            // Explorer se reinició: re-agregar el tray icon.
            if (msg == g_taskbarCreatedMsg && g_tray) {
                g_tray->ReAdd(g_mic->GetState());
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Instancia única.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);

    // Ventana message-only: recibe mensajes sin aparecer en ningún lado...
    // pero HWND_MESSAGE no recibe broadcasts (TaskbarCreated), así que
    // usamos una ventana top-level oculta normal.
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"MuteMic", 0,
                                0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        CloseHandle(mutex);
        return 1;
    }

    MicController mic;
    TrayIcon tray(hwnd);
    g_mic = &mic;
    g_tray = &tray;

    MicState initial = mic.GetState();
    tray.Add(initial);

    // Hotkey global: Ctrl+Alt+M. MOD_NOREPEAT evita ráfagas al mantener.
    if (RegisterHotKey(hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M')) {
        tray.ShowBalloon(L"MuteMic activo",
                         L"Ctrl+Alt+M para mutear/desmutear el micrófono.");
    } else {
        tray.ShowBalloon(L"MuteMic — hotkey no disponible",
                         L"Ctrl+Alt+M ya está en uso por otra app. "
                         L"El toggle sigue disponible desde el tray icon.");
    }

    // 0% CPU en reposo: GetMessage bloquea hasta que llegue un evento.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(hwnd, kHotkeyId);
    g_tray = nullptr;
    g_mic = nullptr;
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
