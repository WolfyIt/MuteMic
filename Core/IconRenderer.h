#pragma once
#include <windows.h>

namespace mutemic {

// Las 5 caras del ícono de tray:
//   NoDevice    — símbolo gris apagado, sin tubo
//   GreenIdle   — tubo neón verde completo (mic abierto, en silencio)
//   GreenActive — tubo verde reactivo: brillo/grosor según nivel de entrada
//   RedIdle     — tubo rojo (muteado)
//   RedActive   — tubo rojo reactivo (hablando estando muteado)
enum class TrayFace {
    NoDevice,
    GreenIdle,
    GreenActive,
    RedIdle,
    RedActive,
};

// Renderiza el ícono con GDI+ (antialiasing real, canal alfa) en runtime.
// level: 0..1, solo relevante para caras *Active.
// sizePx: tamaño físico en píxeles (DPI-aware; usar TrayIconSizeForDpi).
// El caller libera el HICON con DestroyIcon.
class IconRenderer {
public:
    // Inicializa/apaga GDI+ (una vez por proceso).
    static void Startup();
    static void Shutdown();

    static HICON Render(TrayFace face, float level, int sizePx);

    // Tamaño correcto del ícono de tray para un DPI dado.
    static int TrayIconSizeForDpi(UINT dpi);
};

}  // namespace mutemic
