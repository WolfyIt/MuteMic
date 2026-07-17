#pragma once
#include <windows.h>
#include <shellapi.h>
#include "audio.h"

// Mensaje custom que el tray icon envía a la ventana oculta.
constexpr UINT WM_TRAYICON = WM_APP + 1;

// IDs del menú contextual.
constexpr UINT IDM_TOGGLE_MUTE = 100;
constexpr UINT IDM_AUTOSTART = 101;
constexpr UINT IDM_EXIT = 102;

class TrayIcon {
public:
    // hwnd: ventana oculta que recibe WM_TRAYICON.
    explicit TrayIcon(HWND hwnd);
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Agrega el ícono al tray con el estado inicial.
    bool Add(MicState state);

    // Actualiza ícono + tooltip según el estado.
    void Update(MicState state);

    // Muestra una balloon notification.
    void ShowBalloon(const wchar_t* title, const wchar_t* text);

    // Muestra el menú contextual en la posición del cursor.
    // Devuelve el ID del comando elegido (0 si nada).
    UINT ShowMenu();

    // Re-agrega el ícono (para TaskbarCreated, cuando explorer se reinicia).
    void ReAdd(MicState state);

private:
    void SetIconAndTip(NOTIFYICONDATAW& nid, MicState state);

    HWND hwnd_;
    HICON currentIcon_ = nullptr;
    bool added_ = false;
};
