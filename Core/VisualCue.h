#pragma once
#include <windows.h>

namespace mutemic {

// Overlay de confirmación visual al mutear/desmutear. Pensado para juegos /
// apps a pantalla completa (borderless): ventana layered TOPMOST +
// TRANSPARENT + NOACTIVATE — click-through total y JAMÁS roba el foco.
// (Sobre fullscreen EXCLUSIVO ningún overlay puede dibujar sin inyectarse
// en el juego; en borderless/ventana funciona siempre.)
//
// Estilos (settings.visualCue):
//   1 = Edges   — tiras de glow cortas en los 4 bordes + mic mini top-left
//   2 = Corners — el mismo glow pero solo en las esquinas + mic mini
//   3 = Notch   — "gota" estilo Dynamic Island que emerge del borde
//                 (cueEdge: 0=top, 1=bottom, 2=left, 3=right; en bottom
//                 sale del taskbar si está visible). Cuerpo negro/blanco
//                 según tema; translúcido si Liquid Glass (más aún sin
//                 frost). El mic entra con un pequeño retraso (lag suave).
//
// Verde neón = desmuteado, rojo neón = muteado. ~1.1 s y el overlay se
// destruye (cero costo en reposo).
class VisualCue {
public:
    static void Show(bool muted, UINT style, UINT edge,
                     bool lightTheme, bool glass, bool frost);
    static void Term();
};

}  // namespace mutemic
