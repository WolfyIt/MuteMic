#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace mutemic {

// Un shortcut = una card en la UI. El tipo se detecta al capturar (lo que
// el usuario presione primero: tecla, botón de mouse o botón de gamepad).
struct Shortcut {
    std::wstring name;   // editable por el usuario
    UINT type = 0;       // 0 teclado, 1 mouse, 2 gamepad
    UINT vk = 0;         // teclado
    UINT scan = 0;
    UINT mods = 0;
    UINT code = 0;       // mouse: VK del botón; gamepad: bit XINPUT
    UINT mode = 0;       // 0 toggle, 1 push-to-talk, 2 push-to-mute
    bool Bound() const { return type == 0 ? vk != 0 : code != 0; }
};

// Configuración persistida en HKCU\Software\MuteMic.
struct Settings {
    // Hotkey: VK + scancode (para nombre) + modificadores (MOD_CONTROL etc.).
    UINT hotkeyVk = 'M';
    UINT hotkeyScan = 0;
    UINT hotkeyMods = MOD_CONTROL | MOD_ALT;

    // Nombre custom del atajo (para teclas sin nombre estándar, p. ej. la
    // tecla NZXT que aparece como "VK 0x7D"). Vacío = usar nombre derivado.
    std::wstring hotkeyName;

    // Modo del atajo: 0 = toggle, 1 = push-to-talk (mantener = mic abierto),
    // 2 = push-to-mute (mantener = mic silenciado).
    UINT hotkeyMode = 0;

    // LEGACY (solo para migrar instalaciones previas a las shortcut cards):
    UINT padButton = 0;
    UINT mouseButton = 0;

    // V2: lista de shortcuts (cards). Serializada en el registro.
    std::vector<Shortcut> shortcuts;

    // Id del endpoint de captura elegido. Vacío = predeterminado del sistema.
    std::wstring deviceId;

    bool playSound = true;
    bool tooltipShowDevice = true;
    bool tooltipShowShortcut = true;

    // Arrancar minimizado al tray (sin abrir la ventana). Pensado para el
    // autostart con Windows.
    bool startInTray = false;

    // Cues de sonido: nombre de archivo dentro de Sounds\mute / Sounds\unmute
    // (junto al exe) y volumen 0..100.
    std::wstring soundMuteFile = L"default.wav";
    std::wstring soundUnmuteFile = L"default.wav";
    UINT soundVolume = 60;

    // Apariencia: 0 = Night (dark), 1 = Light. Glass es independiente.
    UINT theme = 0;
    bool glass = false;
    // Frosting (blur) del liquid glass: off = cristal transparente nítido.
    bool frost = true;

    // Glass "share-friendly": la ventana SÍ aparece en capturas/compartir
    // pantalla; a cambio el cristal usa un snapshot congelado del fondo
    // (se refresca al mover/mostrar la ventana) en vez de video en vivo.
    // Visual cue al mutear/desmutear: 0=off, 1=bordes, 2=esquinas, 3=notch.
    // cueEdge: orientación del notch (0=top, 1=bottom, 2=left, 3=right).
    UINT visualCue = 0;
    UINT cueEdge = 1;
};

class SettingsStore {
public:
    static Settings Load();
    static void Save(const Settings& s);
};

}  // namespace mutemic
