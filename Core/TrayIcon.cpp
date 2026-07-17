#include "pch.h"
#include "TrayIcon.h"

namespace mutemic {
namespace {

constexpr UINT kTrayIconId = 1;
// Buckets de nivel: cuantizamos 0..1 en 8 pasos para no re-renderizar el
// ícono en cada tick del medidor (solo cuando cruza un umbral perceptible).
constexpr int kLevelBuckets = 8;

int BucketFor(TrayFace face, float level) {
    if (face != TrayFace::GreenActive && face != TrayFace::RedActive) return 0;
    int b = static_cast<int>(level * kLevelBuckets);
    return (b >= kLevelBuckets) ? kLevelBuckets - 1 : b;
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
    if (icon_) DestroyIcon(icon_);
}

UINT TrayIcon::CurrentDpi() const {
    // El tray vive en el monitor primario.
    const UINT dpi = GetDpiForSystem();
    return dpi ? dpi : 96;
}

void TrayIcon::Render(TrayFace face, float level) {
    HICON newIcon = IconRenderer::Render(
        face, level, IconRenderer::TrayIconSizeForDpi(CurrentDpi()));
    if (icon_) DestroyIcon(icon_);
    icon_ = newIcon;
}

bool TrayIcon::Add(TrayFace face, float level, const std::wstring& tooltip) {
    lastFace_ = face;
    lastBucket_ = BucketFor(face, level);
    lastDpi_ = CurrentDpi();
    lastTip_ = tooltip;
    Render(face, level);

    NOTIFYICONDATAW nid = MakeBaseNid(hwnd_);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = icon_;
    lstrcpynW(nid.szTip, tooltip.c_str(), ARRAYSIZE(nid.szTip));
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    return added_;
}

void TrayIcon::Update(TrayFace face, float level, const std::wstring& tooltip) {
    if (!added_) return;

    const int bucket = BucketFor(face, level);
    const UINT dpi = CurrentDpi();
    const bool iconChanged =
        face != lastFace_ || bucket != lastBucket_ || dpi != lastDpi_;
    const bool tipChanged = tooltip != lastTip_;
    if (!iconChanged && !tipChanged) return;  // no-op: nada que redibujar

    NOTIFYICONDATAW nid = MakeBaseNid(hwnd_);
    nid.uFlags = 0;

    if (iconChanged) {
        lastFace_ = face;
        lastBucket_ = bucket;
        lastDpi_ = dpi;
        Render(face, level);
        nid.uFlags |= NIF_ICON;
        nid.hIcon = icon_;
    }
    if (tipChanged) {
        lastTip_ = tooltip;
        nid.uFlags |= NIF_TIP;
        lstrcpynW(nid.szTip, tooltip.c_str(), ARRAYSIZE(nid.szTip));
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* text) {
    if (!added_) return;
    NOTIFYICONDATAW nid = MakeBaseNid(hwnd_);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text, ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

UINT TrayIcon::ShowMenu(bool autostartOn, bool servicePaused) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;

    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open MuteMic");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_MUTE, L"Mute / Unmute");
    AppendMenuW(menu, MF_STRING | (servicePaused ? MF_CHECKED : 0),
                IDM_PAUSE_SERVICE, L"Pause service");
    AppendMenuW(menu, MF_STRING | (autostartOn ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    DestroyMenu(menu);
    return cmd;
}

void TrayIcon::ReAdd() {
    added_ = false;
    // Forzar re-render en el próximo Add.
    NOTIFYICONDATAW nid = MakeBaseNid(hwnd_);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    Render(lastFace_, 0.0f);
    nid.hIcon = icon_;
    lstrcpynW(nid.szTip, lastTip_.c_str(), ARRAYSIZE(nid.szTip));
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

}  // namespace mutemic
