#include "pch.h"
#include "HotkeyHook.h"

namespace mutemic {
namespace {

HHOOK g_hook = nullptr;
HWND g_target = nullptr;
std::atomic<bool> g_capturing{false};

// Multi-binding: una entrada por card. held = anti-spam por card (una
// acción por pulsación, se rearma al soltar).
struct KeyBind { int id; KeyCombo combo; bool held; };
std::vector<KeyBind> g_keyBinds;

bool IsModifierVk(UINT vk) {
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_LWIN: case VK_RWIN:
            return true;
    }
    return false;
}

// Estado actual de modificadores. En un hook LL, GetAsyncKeyState refleja
// el estado físico real en el momento del evento.
UINT CurrentMods() {
    UINT mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
        mods |= MOD_WIN;
    return mods;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        // Soltar: rearma el anti-spam y notifica el UP de cada card que
        // estuviera "held" con esa tecla (PTT/PTM actúan al soltar).
        if (up) {
            for (auto& b : g_keyBinds) {
                if (b.held && kb->vkCode == b.combo.vk) {
                    b.held = false;
                    PostMessageW(g_target, HotkeyHook::WM_APP_HOTKEY_UP,
                                 static_cast<WPARAM>(b.id), 0);
                }
            }
        }

        // Ignorar eventos inyectados (SendInput de otras apps / de nosotros).
        if (down && !(kb->flags & LLKHF_INJECTED)) {
            const UINT vk = kb->vkCode;
            const UINT scan = kb->scanCode |
                              ((kb->flags & LLKHF_EXTENDED) ? 0x100u : 0u);

            if (g_capturing.load(std::memory_order_relaxed)) {
                if (vk == VK_ESCAPE) {
                    g_capturing = false;
                    PostMessageW(g_target, HotkeyHook::WM_APP_CAPTURED, 0, 0);
                    return 1;  // consumir
                }
                if (!IsModifierVk(vk)) {
                    g_capturing = false;
                    const UINT mods = CurrentMods();
                    PostMessageW(g_target, HotkeyHook::WM_APP_CAPTURED,
                                 static_cast<WPARAM>(vk),
                                 static_cast<LPARAM>(MAKELONG(mods, scan)));
                    return 1;  // consumir
                }
                // Modificador solo: dejar pasar, esperar la tecla principal.
            } else {
                const UINT mods = CurrentMods();
                for (auto& b : g_keyBinds) {
                    if (b.combo.vk == vk && b.combo.mods == mods) {
                        // Auto-repeat: una acción por pulsación por card.
                        if (!b.held) {
                            b.held = true;
                            PostMessageW(g_target, HotkeyHook::WM_APP_HOTKEY,
                                         static_cast<WPARAM>(b.id), 0);
                        }
                        return 1;  // consumir: es nuestro atajo
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

}  // namespace

bool HotkeyHook::Install(HWND targetWindow) {
    if (g_hook) return true;
    g_target = targetWindow;
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                               GetModuleHandleW(nullptr), 0);
    return g_hook != nullptr;
}

void HotkeyHook::Uninstall() {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    g_capturing = false;
}

bool HotkeyHook::IsInstalled() {
    return g_hook != nullptr;
}

void HotkeyHook::ClearBindings() {
    g_keyBinds.clear();
    // El mouse se limpia en su propia sección (abajo).
}

void HotkeyHook::AddKeyBinding(int id, const KeyCombo& combo) {
    if (combo.vk != 0)
        g_keyBinds.push_back({ id, combo, false });
}

void HotkeyHook::BeginCapture() {
    g_capturing = true;
}

void HotkeyHook::CancelCapture() {
    g_capturing = false;
}

bool HotkeyHook::IsCapturing() {
    return g_capturing.load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────
// Binding de MOUSE (hook LL propio, independiente del de teclado)

namespace {
HHOOK g_mouseHook = nullptr;
std::atomic<bool> g_mouseCapturing{ false };

struct MouseBind { int id; UINT vk; bool held; };
std::vector<MouseBind> g_mouseBinds;

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(l);
        UINT vk = 0;
        bool down = false, up = false;
        switch (w) {
            case WM_MBUTTONDOWN: vk = VK_MBUTTON; down = true; break;
            case WM_MBUTTONUP:   vk = VK_MBUTTON; up = true; break;
            case WM_XBUTTONDOWN:
                vk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
                down = true;
                break;
            case WM_XBUTTONUP:
                vk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
                up = true;
                break;
        }
        if (vk != 0) {
            if (g_mouseCapturing.load(std::memory_order_relaxed) && down) {
                g_mouseCapturing = false;
                PostMessageW(g_target, HotkeyHook::WM_APP_MOUSE_CAPTURED, vk, 0);
                return 1;  // consumir
            }
            bool consumed = false;
            for (auto& b : g_mouseBinds) {
                if (b.vk != vk) continue;
                // Mismo pipeline que el teclado: modos y debounce aplican.
                if (down && !b.held) {
                    b.held = true;
                    PostMessageW(g_target, HotkeyHook::WM_APP_HOTKEY,
                                 static_cast<WPARAM>(b.id), 0);
                }
                if (up && b.held) {
                    b.held = false;
                    PostMessageW(g_target, HotkeyHook::WM_APP_HOTKEY_UP,
                                 static_cast<WPARAM>(b.id), 0);
                }
                consumed = true;
            }
            if (consumed) return 1;  // consumir: es nuestro botón
        }
    }
    return CallNextHookEx(g_mouseHook, code, w, l);
}

void EnsureMouseHook() {
    const bool need = !g_mouseBinds.empty() ||
                      g_mouseCapturing.load(std::memory_order_relaxed);
    if (need && !g_mouseHook) {
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                        GetModuleHandleW(nullptr), 0);
    } else if (!need && g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}
}  // namespace

void HotkeyHook::AddMouseBinding(int id, UINT vkButton) {
    if (vkButton != 0)
        g_mouseBinds.push_back({ id, vkButton, false });
    EnsureMouseHook();
}

void HotkeyHook::BeginMouseCapture() {
    g_mouseCapturing = true;
    EnsureMouseHook();
}

void HotkeyHook::CancelMouseCapture() {
    g_mouseCapturing = false;
    EnsureMouseHook();
}

void HotkeyHook::UninstallMouse() {
    g_mouseCapturing = false;
    g_mouseBinds.clear();
    EnsureMouseHook();
}

std::wstring HotkeyHook::MouseButtonName(UINT vkButton) {
    switch (vkButton) {
        case VK_MBUTTON:  return L"Middle click";
        case VK_XBUTTON1: return L"Mouse 4 (X1)";
        case VK_XBUTTON2: return L"Mouse 5 (X2)";
    }
    return L"None";
}

std::wstring HotkeyHook::ComboName(const KeyCombo& combo) {
    if (combo.vk == 0) return L"(none)";

    std::wstring name;
    if (combo.mods & MOD_CONTROL) name += L"Ctrl + ";
    if (combo.mods & MOD_ALT) name += L"Alt + ";
    if (combo.mods & MOD_SHIFT) name += L"Shift + ";
    if (combo.mods & MOD_WIN) name += L"Win + ";

    wchar_t keyName[64] = {};
    // GetKeyNameText espera el scancode en bits 16-23 y el flag extendida
    // en el bit 24.
    LONG lparam = static_cast<LONG>((combo.scan & 0xFF) << 16);
    if (combo.scan & 0x100) lparam |= (1 << 24);
    if (GetKeyNameTextW(lparam, keyName, 64) > 0) {
        name += keyName;
    } else {
        wchar_t fallback[16];
        swprintf_s(fallback, L"VK 0x%02X", combo.vk);
        name += fallback;
    }
    return name;
}

}  // namespace mutemic
