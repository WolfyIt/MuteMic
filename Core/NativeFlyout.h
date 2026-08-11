#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace mutemic {

// ── Flyout del tray, nativo ──
//
// PORQUÉ EXISTE: el flyout anterior era WinUI 3, lo que obligaba a tener
// vivo el runtime de XAML solo para mostrar un menú de cinco filas. Medido
// en esta máquina: 125 MB de memoria comprometida que nunca se devuelven,
// contra ~5 MB del núcleo Win32. Como el flyout es la superficie que más
// se usa a diario, no puede ser la que obligue a cargar el proceso caro.
//
// CÓMO: ventana sin superficie de redirección + swapchain de composición,
// pintada con Direct2D y DirectWrite. Mismo camino que los cues visuales,
// así que ya está probado: sin bitmap de redirección, elegible para plano
// de hardware, y con esquinas redondeadas reales.
//
// Sin dependencias de WinRT/XAML: puede vivir en el proceso ligero.
class NativeFlyout {
public:
    enum class Action {
        ToggleMute,
        OpenSettings,
        Quit,
        SelectDevice,   // payload = índice en la lista de dispositivos
    };

    struct DeviceEntry {
        std::wstring name;
        std::wstring id;
        bool current = false;
    };

    // Estado que el flyout pinta. Se pasa entero al abrir: el flyout no
    // consulta nada por su cuenta (así no depende del core y se puede
    // probar aislado).
    struct Model {
        bool muted = false;
        bool serviceActive = true;
        std::wstring deviceName;
        std::vector<DeviceEntry> devices;
        UINT theme = 0;        // 0 = night, 1 = light
        bool glass = false;    // afecta transparencias
    };

    // Muestra el flyout anclado al icono del tray (coordenadas de pantalla
    // del click). Si ya estaba abierto, lo actualiza.
    static void Show(int anchorX, int anchorY, Model const& model,
                     std::function<void(Action, int payload)> onAction);

    // Lo esconde (click fuera, Escape, o acción elegida).
    static void Hide();

    static bool IsOpen();

    // Repinta con un modelo nuevo sin reabrir (cambió el estado del micro).
    static void Update(Model const& model);

    // Libera device D3D/D2D y la ventana. Llamar en el teardown.
    static void Term();
};

}  // namespace mutemic
