#include "tray.h"

#include <shellapi.h>
#include <cstring>

#include "autostart.h"
#include "icons.h"

namespace {

constexpr UINT kTrayIconId = 1;

const wchar_t* TooltipFor(MicState state) {
    switch (state) {
        case MicState::Unmuted:  return L"MuteMic — Micrófono activo (Ctrl+Alt+M)";
        case MicState::Muted:    return L"MuteMic — Micrófono MUTEADO (Ctrl+Alt+M)";
        case MicState::NoDevice: return L"MuteMic — Sin micrófono detectado";
    }
    return L"MuteMic";
}

NOTIFYICONDATAW MakeBaseNid(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    return nid;
}

}  // namespace

TrayIcon::TrayIcon(HWND hwnd) : hwnd_(hwnd) {}

TrayIcon::~TrayIcon() {
    if (added_) {
        NOTIFYICONDATAW nid = MakeBaseNid(hwnd_);
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    if (currentIcon_) DestroyIcon(currentIcon_);
}

void TrayIcon::SetIconAndTip(NOTIFYICONDATAW& nid, MicState state) {
    HICON newIcon = CreateStateIcon(state);
    if (currentIcon_) DestroyIcon(currentIcon_);
    currentIcon_ = newIcon;

    nid.uFlags |= NIF_ICON | NIF_TIP;
    nid.hIcon = currentIcon_;
    lstrcpynW(nid.szTip, TooltipFor(state),
              sizeof(nid.szTip) / sizeof(nid.szTip[0]));
}

bool TrayIcon::Add(MicState state) {
    NOTIFYICONDATAW nid = MakeBaseNid(hwnd_);
    nid.uFlags = NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    SetIconAndTip(nid, state);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    return added_;
}

void TrayIcon::Update(MicState state) {
    if (!added_) return;
    NOTIFYICONDATAW nid = MakeBaseNid(hwnd_);
    nid.uFlags = 0;
    SetIconAndTip(nid, state);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* text) {
    if (!added_) return;
    NOTIFYICONDATAW nid = MakeBaseNid(hwnd_);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, title, sizeof(nid.szInfoTitle) / sizeof(nid.szInfoTitle[0]));
    lstrcpynW(nid.szInfo, text, sizeof(nid.szInfo) / sizeof(nid.szInfo[0]));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

UINT TrayIcon::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;

    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_MUTE, L"Mutear/Desmutear\tCtrl+Alt+M");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Iniciar con Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Salir");

    POINT pt;
    GetCursorPos(&pt);
    // Requerido para que el menú se cierre al hacer click fuera.
    SetForegroundWindow(hwnd_);
    UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);  // workaround clásico de TrackPopupMenu

    DestroyMenu(menu);
    return cmd;
}

void TrayIcon::ReAdd(MicState state) {
    added_ = false;
    Add(state);
}
