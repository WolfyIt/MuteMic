#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>
#include "IconRenderer.h"

namespace mutemic {

constexpr UINT WM_TRAYICON = WM_APP + 1;

// IDs del menú contextual del tray.
constexpr UINT IDM_OPEN = 100;
constexpr UINT IDM_TOGGLE_MUTE = 101;
constexpr UINT IDM_AUTOSTART = 102;
constexpr UINT IDM_PAUSE_SERVICE = 103;
constexpr UINT IDM_EXIT = 104;

class TrayIcon {
public:
    explicit TrayIcon(HWND hwnd);
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Add(TrayFace face, float level, const std::wstring& tooltip);

    // Re-renderiza solo si (cara, bucket de nivel o DPI) cambió — el poll de
    // 80 ms llama esto en cada tick y casi siempre es no-op.
    void Update(TrayFace face, float level, const std::wstring& tooltip);

    void ShowBalloon(const wchar_t* title, const wchar_t* text);

    // Menú contextual. servicePaused/autostart controlan los checkmarks.
    // Devuelve el comando elegido (0 = ninguno).
    UINT ShowMenu(bool autostartOn, bool servicePaused);

    // Re-agrega tras reinicio de explorer (TaskbarCreated).
    void ReAdd();

private:
    void Render(TrayFace face, float level);
    UINT CurrentDpi() const;

    HWND hwnd_;
    HICON icon_ = nullptr;
    bool added_ = false;

    TrayFace lastFace_ = TrayFace::NoDevice;
    int lastBucket_ = -1;
    UINT lastDpi_ = 0;
    std::wstring lastTip_;
};

}  // namespace mutemic
