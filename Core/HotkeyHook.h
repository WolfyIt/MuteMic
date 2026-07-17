#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace mutemic {

// Combinación de teclas: VK principal + modificadores (MOD_CONTROL|MOD_ALT|
// MOD_SHIFT|MOD_WIN). La tecla principal puede ser CUALQUIER VK que el
// stack de teclado reporte — incluidas teclas raras/macro (p. ej. el botón
// NZXT) siempre que Windows las vea como entrada de teclado.
struct KeyCombo {
    UINT vk = 0;
    UINT scan = 0;   // scancode + flag extendida (para el nombre)
    UINT mods = 0;
};

// Hook global de teclado (WH_KEYBOARD_LL). A diferencia de RegisterHotKey,
// acepta cualquier tecla, con o sin modificadores, y permite un modo captura
// para grabar el atajo que el usuario presione.
//
// Debe instalarse en un hilo con message loop (el hilo de UI sirve).
// El callback del hook NO hace trabajo pesado: postea un mensaje a la
// ventana destino y retorna de inmediato.
class HotkeyHook {
public:
    // targetWindow recibe:
    //  - WM_APP_HOTKEY / WM_APP_HOTKEY_UP con wParam = id de la card
    //  - WM_APP_CAPTURED (wParam=vk, lParam=MAKELONG(mods, scan)) en captura
    //  - WM_APP_MOUSE_CAPTURED (wParam = VK del botón de mouse)
    static constexpr UINT WM_APP_HOTKEY = WM_APP + 10;
    static constexpr UINT WM_APP_CAPTURED = WM_APP + 11;
    static constexpr UINT WM_APP_HOTKEY_UP = WM_APP + 12;
    static constexpr UINT WM_APP_MOUSE_CAPTURED = WM_APP + 13;

    static bool Install(HWND targetWindow);
    static void Uninstall();
    static bool IsInstalled();

    // ── Multi-binding (una entrada por card) ──
    static void ClearBindings();                       // solo teclado
    static void AddKeyBinding(int id, const KeyCombo& combo);
    static void AddMouseBinding(int id, UINT vkButton);
    // (mouse se limpia con UninstallMouse, abajo)

    // Mientras está activo, la próxima tecla no-modificadora presionada se
    // reporta vía WM_APP_CAPTURED (y se consume). Esc cancela (vk=0).
    static void BeginCapture();
    static void CancelCapture();
    static bool IsCapturing();

    // "Ctrl + Alt + M" — nombre legible de una combinación.
    static std::wstring ComboName(const KeyCombo& combo);

    // Captura de mouse (medio / X1 / X2): el próximo botón se reporta.
    static void BeginMouseCapture();
    static void CancelMouseCapture();
    static void UninstallMouse();                    // al pausar servicio
    static std::wstring MouseButtonName(UINT vkButton);
};

}  // namespace mutemic
