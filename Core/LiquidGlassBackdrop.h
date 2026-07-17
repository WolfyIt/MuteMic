#pragma once
#include <windows.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace mutemic {

// Backdrop "Liquid Glass" real: captura el monitor (excluyendo esta ventana
// vía WDA_EXCLUDEFROMCAPTURE), recorta la región detrás del hwnd, aplica el
// shader de refracción del repo LiquidGlass (portado a HLSL/D2D) y lo pinta
// en un CanvasControl de Win2D insertado como capa INFERIOR del grid raíz.
//
// (Se usa CanvasControl y no SystemBackdrop porque la composición de Win2D
// vive en Windows.UI.Composition y el backdrop de WinUI 3 en
// Microsoft.UI.Composition — árboles incompatibles. CanvasControl es la vía
// soportada de Win2D dentro de XAML/WinUI 3.)
//
//   frost = true  → "Full effect": blur gaussiano + noise + glow
//   frost = false → "Without blur or noise": refracción limpia
//
// Tradeoffs: la ventana queda excluida de capturas del usuario mientras el
// modo está activo; costo GPU moderado.
class LiquidGlassBackdrop {
public:
    // Inicia (o reconfigura si solo cambió frost). host = grid raíz de la
    // ventana; el canvas se inserta como primer hijo (detrás de todo).
    // Devuelve false si el sistema no soporta captura o falta el shader.
    // shareFriendly: la ventana queda VISIBLE en capturas/screen share; el
    // cristal usa un snapshot congelado que se refresca en cada movimiento
    // (ciclo: excluir → capturar 1 frame → des-excluir). Con false, video
    // en vivo pero la ventana no aparece en capturas.
    static bool Start(HWND hwnd,
                      winrt::Microsoft::UI::Xaml::Controls::Grid const& host,
                      bool frost, bool shareFriendly);

    // Quita el canvas, detiene la captura y restaura display affinity.
    static void Stop();

    // Redibujar con el último frame (mover/redimensionar la ventana).
    static void RequestRedraw();

    // Ventana oculta en el tray → suelta captura, frames y grafo de efectos
    // (decenas de MB de GPU/RAM que nadie ve). Al volver a mostrarse, se
    // rearma solo. El modo glass sigue "activo" lógicamente.
    static void OnWindowVisibility(bool visible);

    static bool IsActive();
};

}  // namespace mutemic
